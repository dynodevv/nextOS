/*
 * nextOS - net_stack.c
 * Minimal network protocol stack
 *
 * Implements: Ethernet framing, ARP, IPv4, UDP, TCP (basic),
 *             DNS resolution, and HTTP/1.1 GET requests.
 * Uses polling-based approach (no async/interrupt-driven networking).
 */
#include "net_stack.h"
#include "../drivers/net.h"
#include "../drivers/timer.h"
#include "../mem/heap.h"

/* ── String/Memory Helpers (freestanding) ────────────────────────────── */
static void mem_copy(void *dst, const void *src, int n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void mem_zero(void *dst, int n)
{
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = 0;
}

/* ── Byte Order ──────────────────────────────────────────────────────── */
static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* ── Network Configuration ───────────────────────────────────────────── */
static uint32_t our_ip      = 0;
static uint32_t gateway_ip  = 0;
static uint32_t netmask     = 0;
static uint32_t dns_server  = 0;
static uint8_t  our_mac[6];

/* Default DHCP-style config for common QEMU user networking */
#define DEFAULT_IP       0x0A000202   /* 10.0.2.2 - but we use 10.0.2.15 */
#define DEFAULT_GATEWAY  0x0A000202   /* 10.0.2.2 */
#define DEFAULT_NETMASK  0xFFFFFF00   /* 255.255.255.0 */
#define DEFAULT_DNS      0x0A000203   /* 10.0.2.3 */

/* ── ARP Cache ───────────────────────────────────────────────────────── */
#define ARP_CACHE_SIZE 16
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* ── TX/RX packet buffer ─────────────────────────────────────────────── */
static uint8_t pkt_buf[2048];
static uint8_t rx_pkt[2048];

/* ── TCP State (single connection at a time) ─────────────────────────── */
typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
} tcp_state_t;

static tcp_state_t tcp_state = TCP_CLOSED;
static uint32_t tcp_remote_ip;
static uint16_t tcp_local_port;
static uint16_t tcp_remote_port;
static uint32_t tcp_local_seq;
static uint32_t tcp_local_ack;
static uint32_t tcp_remote_seq;
static uint16_t tcp_next_port = 49152;

/* TCP receive buffer */
#define TCP_RX_BUF_SIZE 65536
static uint8_t tcp_rx_buf[TCP_RX_BUF_SIZE];
static int tcp_rx_head = 0;
static int tcp_rx_tail = 0;

/* ── IP checksum ─────────────────────────────────────────────────────── */
static uint16_t ip_checksum(const void *data, int len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    for (int i = 0; i < len / 2; i++)
        sum += p[i];
    if (len & 1)
        sum += ((const uint8_t *)data)[len - 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ── TCP pseudo-header checksum ──────────────────────────────────────── */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void *tcp_data, int tcp_len)
{
    uint32_t sum = 0;
    /* Pseudo-header */
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += htons(IP_PROTO_TCP);
    sum += htons((uint16_t)tcp_len);
    /* TCP segment */
    const uint16_t *p = (const uint16_t *)tcp_data;
    for (int i = 0; i < tcp_len / 2; i++)
        sum += p[i];
    if (tcp_len & 1)
        sum += ((const uint8_t *)tcp_data)[tcp_len - 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ── Build & Send Ethernet Frame ─────────────────────────────────────── */
static void send_eth(const uint8_t dst[6], uint16_t ethertype,
                     const void *payload, int payload_len)
{
    eth_header_t *eth = (eth_header_t *)pkt_buf;
    mem_copy(eth->dst, dst, 6);
    mem_copy(eth->src, our_mac, 6);
    eth->ethertype = htons(ethertype);
    mem_copy(pkt_buf + sizeof(eth_header_t), payload, payload_len);
    int total = (int)sizeof(eth_header_t) + payload_len;
    if (total < 60) total = 60;  /* Minimum Ethernet frame size */
    net_send(pkt_buf, total);
}

/* ── ARP ─────────────────────────────────────────────────────────────── */
static void arp_cache_add(uint32_t ip, const uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            mem_copy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            mem_copy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Cache full: overwrite first entry */
    arp_cache[0].ip = ip;
    mem_copy(arp_cache[0].mac, mac, 6);
    arp_cache[0].valid = 1;
}

static int arp_cache_lookup(uint32_t ip, uint8_t mac_out[6])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            mem_copy(mac_out, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_send_request(uint32_t target_ip)
{
    arp_header_t arp;
    arp.htype = htons(1);
    arp.ptype = htons(0x0800);
    arp.hlen = 6;
    arp.plen = 4;
    arp.oper = htons(1);  /* Request */
    mem_copy(arp.sha, our_mac, 6);
    arp.spa = our_ip;
    mem_zero(arp.tha, 6);
    arp.tpa = target_ip;

    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    send_eth(broadcast, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static void arp_handle(const uint8_t *data, int len)
{
    if (len < (int)sizeof(arp_header_t)) return;
    const arp_header_t *arp = (const arp_header_t *)data;

    arp_cache_add(arp->spa, arp->sha);

    if (ntohs(arp->oper) == 1 && arp->tpa == our_ip) {
        /* ARP Reply */
        arp_header_t reply;
        reply.htype = htons(1);
        reply.ptype = htons(0x0800);
        reply.hlen = 6;
        reply.plen = 4;
        reply.oper = htons(2);
        mem_copy(reply.sha, our_mac, 6);
        reply.spa = our_ip;
        mem_copy(reply.tha, arp->sha, 6);
        reply.tpa = arp->spa;
        send_eth(arp->sha, ETH_TYPE_ARP, &reply, sizeof(reply));
    }
}

/* ── Resolve MAC for an IP (ARP with retry) ──────────────────────────── */
static int resolve_mac(uint32_t ip, uint8_t mac_out[6])
{
    /* If target is not on our subnet, use gateway */
    uint32_t target = ip;
    if ((ip & netmask) != (our_ip & netmask))
        target = gateway_ip;

    if (arp_cache_lookup(target, mac_out))
        return 1;

    /* Send ARP request and wait */
    for (int retry = 0; retry < 3; retry++) {
        arp_send_request(target);
        uint64_t start = timer_get_ticks();
        while (timer_get_ticks() - start < 500) {
            net_stack_process();
            if (arp_cache_lookup(target, mac_out))
                return 1;
        }
    }
    return 0;
}

/* ── Send IPv4 packet ────────────────────────────────────────────────── */
static uint16_t ip_id_counter = 1;

static void send_ipv4(uint32_t dst_ip, uint8_t protocol,
                      const void *payload, int payload_len)
{
    uint8_t dst_mac[6];
    if (!resolve_mac(dst_ip, dst_mac))
        return;

    uint8_t ip_pkt[1500];
    ipv4_header_t *ip = (ipv4_header_t *)ip_pkt;
    ip->ver_ihl = 0x45;  /* IPv4, 5 words header */
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(20 + payload_len));
    ip->id = htons(ip_id_counter++);
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_ip = our_ip;
    ip->dst_ip = dst_ip;
    ip->checksum = ip_checksum(ip, 20);

    mem_copy(ip_pkt + 20, payload, payload_len);
    send_eth(dst_mac, ETH_TYPE_IPV4, ip_pkt, 20 + payload_len);
}

/* ── UDP send ────────────────────────────────────────────────────────── */
static void send_udp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                     const void *data, int data_len)
{
    uint8_t udp_pkt[1472];
    udp_header_t *udp = (udp_header_t *)udp_pkt;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons((uint16_t)(8 + data_len));
    udp->checksum = 0;  /* Optional for UDP over IPv4 */
    mem_copy(udp_pkt + 8, data, data_len);
    send_ipv4(dst_ip, IP_PROTO_UDP, udp_pkt, 8 + data_len);
}

/* ── TCP send ────────────────────────────────────────────────────────── */
static void send_tcp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                     uint32_t seq, uint32_t ack, uint8_t flags,
                     const void *data, int data_len)
{
    uint8_t tcp_pkt[1460 + 20];
    tcp_header_t *tcp = (tcp_header_t *)tcp_pkt;
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq_num = htonl(seq);
    tcp->ack_num = htonl(ack);
    tcp->data_offset = (5 << 4);  /* 20 bytes header, no options */
    tcp->flags = flags;
    tcp->window = htons(8192);
    tcp->checksum = 0;
    tcp->urgent = 0;

    if (data_len > 0)
        mem_copy(tcp_pkt + 20, data, data_len);

    int tcp_total = 20 + data_len;
    tcp->checksum = tcp_checksum(our_ip, dst_ip, tcp_pkt, tcp_total);

    send_ipv4(dst_ip, IP_PROTO_TCP, tcp_pkt, tcp_total);
}

/* ── Handle incoming TCP ─────────────────────────────────────────────── */
static void tcp_handle(uint32_t src_ip, const uint8_t *data, int len)
{
    if (len < 20) return;
    const tcp_header_t *tcp = (const tcp_header_t *)data;

    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    int hdr_len = (tcp->data_offset >> 4) * 4;
    int payload_len = len - hdr_len;

    if (dst_port != tcp_local_port || src_ip != tcp_remote_ip)
        return;

    if (tcp_state == TCP_SYN_SENT) {
        if ((tcp->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            tcp_remote_seq = seq + 1;
            tcp_local_seq = ack;
            tcp_local_ack = tcp_remote_seq;
            /* Send ACK */
            send_tcp(tcp_remote_ip, tcp_local_port, tcp_remote_port,
                     tcp_local_seq, tcp_local_ack, TCP_ACK, 0, 0);
            tcp_state = TCP_ESTABLISHED;
        }
    } else if (tcp_state == TCP_ESTABLISHED) {
        if (payload_len > 0 && seq == tcp_local_ack) {
            /* Store received data */
            const uint8_t *payload = data + hdr_len;
            for (int i = 0; i < payload_len; i++) {
                int next = (tcp_rx_head + 1) % TCP_RX_BUF_SIZE;
                if (next == tcp_rx_tail) break;  /* Buffer full */
                tcp_rx_buf[tcp_rx_head] = payload[i];
                tcp_rx_head = next;
            }
            tcp_local_ack = seq + (uint32_t)payload_len;
            send_tcp(tcp_remote_ip, tcp_local_port, tcp_remote_port,
                     tcp_local_seq, tcp_local_ack, TCP_ACK, 0, 0);
        }
        if (tcp->flags & TCP_FIN) {
            tcp_local_ack = seq + 1;
            if (payload_len > 0)
                tcp_local_ack = seq + (uint32_t)payload_len + 1;
            send_tcp(tcp_remote_ip, tcp_local_port, tcp_remote_port,
                     tcp_local_seq, tcp_local_ack, TCP_ACK | TCP_FIN, 0, 0);
            tcp_state = TCP_CLOSED;
        }
        if (tcp->flags & TCP_RST) {
            tcp_state = TCP_CLOSED;
        }
    } else if (tcp_state == TCP_FIN_WAIT) {
        if (tcp->flags & (TCP_ACK | TCP_FIN)) {
            if (tcp->flags & TCP_FIN) {
                tcp_local_ack = seq + 1;
                send_tcp(tcp_remote_ip, tcp_local_port, tcp_remote_port,
                         tcp_local_seq, tcp_local_ack, TCP_ACK, 0, 0);
            }
            tcp_state = TCP_CLOSED;
        }
    }
}

/* ── Handle incoming UDP ─────────────────────────────────────────────── */
/* DNS response buffer */
static uint8_t dns_response[512];
static int dns_response_len = 0;
static int dns_response_ready = 0;
static uint16_t dns_transaction_id = 0;

static void udp_handle(uint32_t src_ip, const uint8_t *data, int len)
{
    if (len < 8) return;
    const udp_header_t *udp = (const udp_header_t *)data;
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t dst_port = ntohs(udp->dst_port);
    int payload_len = ntohs(udp->length) - 8;
    const uint8_t *payload = data + 8;

    (void)src_ip;
    (void)src_port;

    /* DNS response (port 53 -> our ephemeral port) */
    if (dst_port >= 49152 && payload_len >= (int)sizeof(dns_header_t)) {
        const dns_header_t *dns = (const dns_header_t *)payload;
        if (ntohs(dns->id) == dns_transaction_id) {
            dns_response_len = payload_len;
            if (dns_response_len > (int)sizeof(dns_response))
                dns_response_len = (int)sizeof(dns_response);
            mem_copy(dns_response, payload, dns_response_len);
            dns_response_ready = 1;
        }
    }
}

/* ── Handle incoming IPv4 ────────────────────────────────────────────── */
static void ipv4_handle(const uint8_t *data, int len)
{
    if (len < 20) return;
    const ipv4_header_t *ip = (const ipv4_header_t *)data;
    int hdr_len = (ip->ver_ihl & 0x0F) * 4;
    int total = ntohs(ip->total_len);
    if (total > len) total = len;
    int payload_len = total - hdr_len;
    const uint8_t *payload = data + hdr_len;

    if (ip->protocol == IP_PROTO_UDP) {
        udp_handle(ip->src_ip, payload, payload_len);
    } else if (ip->protocol == IP_PROTO_TCP) {
        tcp_handle(ip->src_ip, payload, payload_len);
    }
}

/* ── Process incoming packets ────────────────────────────────────────── */
void net_stack_process(void)
{
    if (!net_is_available()) return;

    int len = net_receive(rx_pkt, sizeof(rx_pkt));
    while (len > 0) {
        if (len >= (int)sizeof(eth_header_t)) {
            eth_header_t *eth = (eth_header_t *)rx_pkt;
            uint16_t type = ntohs(eth->ethertype);
            int payload_off = (int)sizeof(eth_header_t);
            int payload_len = len - payload_off;

            if (type == ETH_TYPE_ARP)
                arp_handle(rx_pkt + payload_off, payload_len);
            else if (type == ETH_TYPE_IPV4)
                ipv4_handle(rx_pkt + payload_off, payload_len);
        }
        len = net_receive(rx_pkt, sizeof(rx_pkt));
    }
}

/* ── DNS Resolution ──────────────────────────────────────────────────── */
static uint32_t parse_ip_string(const char *s)
{
    uint32_t parts[4] = {0};
    int pi = 0;
    for (int i = 0; s[i] && pi < 4; i++) {
        if (s[i] == '.') { pi++; continue; }
        if (s[i] >= '0' && s[i] <= '9')
            parts[pi] = parts[pi] * 10 + (s[i] - '0');
    }
    /* Network byte order */
    return (parts[0]) | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_ip_address(const char *s)
{
    int dots = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '.') dots++;
        else if (!is_digit(s[i])) return 0;
    }
    return dots == 3;
}

uint32_t dns_resolve(const char *hostname)
{
    if (!net_is_available()) return 0;

    /* Check if it's already an IP address */
    if (is_ip_address(hostname))
        return parse_ip_string(hostname);

    /* Build DNS query */
    uint8_t query[256];
    mem_zero(query, sizeof(query));
    dns_header_t *dns = (dns_header_t *)query;
    dns_transaction_id = (uint16_t)(timer_get_ticks() & 0xFFFF);
    dns->id = htons(dns_transaction_id);
    dns->flags = htons(0x0100);  /* Standard query, recursion desired */
    dns->qdcount = htons(1);

    /* Encode hostname as DNS name */
    int pos = sizeof(dns_header_t);
    const char *p = hostname;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int label_len = (int)(dot - p);
        if (label_len > 63 || pos + label_len + 1 >= (int)sizeof(query))
            return 0;
        query[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++)
            query[pos++] = (uint8_t)p[i];
        p = *dot ? dot + 1 : dot;
    }
    query[pos++] = 0;  /* Root label */
    /* QTYPE A (1) */
    query[pos++] = 0; query[pos++] = 1;
    /* QCLASS IN (1) */
    query[pos++] = 0; query[pos++] = 1;

    /* Send DNS query */
    dns_response_ready = 0;
    uint16_t src_port = tcp_next_port++;
    send_udp(dns_server, src_port, 53, query, pos);

    /* Wait for response */
    uint64_t start = timer_get_ticks();
    while (!dns_response_ready && timer_get_ticks() - start < 3000) {
        net_stack_process();
    }

    if (!dns_response_ready) return 0;

    /* Parse DNS response - find A record */
    const uint8_t *resp = dns_response;
    int rlen = dns_response_len;
    if (rlen < (int)sizeof(dns_header_t)) return 0;

    dns_header_t *rdns = (dns_header_t *)resp;
    int ancount = ntohs(rdns->ancount);
    if (ancount == 0) return 0;

    /* Skip question section */
    int rpos = sizeof(dns_header_t);
    /* Skip QNAME */
    while (rpos < rlen && resp[rpos] != 0) {
        if ((resp[rpos] & 0xC0) == 0xC0) { rpos += 2; goto skip_q_done; }
        rpos += resp[rpos] + 1;
    }
    rpos++;  /* Skip null terminator */
skip_q_done:
    rpos += 4;  /* Skip QTYPE + QCLASS */

    /* Parse answer records */
    for (int a = 0; a < ancount && rpos < rlen; a++) {
        /* Skip NAME (might be compressed) */
        if ((resp[rpos] & 0xC0) == 0xC0) rpos += 2;
        else {
            while (rpos < rlen && resp[rpos] != 0) rpos += resp[rpos] + 1;
            rpos++;
        }
        if (rpos + 10 > rlen) break;
        uint16_t rtype = (uint16_t)((resp[rpos] << 8) | resp[rpos + 1]);
        rpos += 2;  /* TYPE */
        rpos += 2;  /* CLASS */
        rpos += 4;  /* TTL */
        uint16_t rdlength = (uint16_t)((resp[rpos] << 8) | resp[rpos + 1]);
        rpos += 2;

        if (rtype == 1 && rdlength == 4 && rpos + 4 <= rlen) {
            /* A record - return IP in network byte order */
            uint32_t ip;
            mem_copy(&ip, resp + rpos, 4);
            return ip;
        }
        rpos += rdlength;
    }

    return 0;
}

/* ── TCP Connection ──────────────────────────────────────────────────── */
int tcp_connect(uint32_t dst_ip, uint16_t dst_port)
{
    if (!net_is_available()) return -1;
    if (tcp_state != TCP_CLOSED) return -1;

    tcp_remote_ip = dst_ip;
    tcp_remote_port = dst_port;
    tcp_local_port = tcp_next_port++;
    tcp_local_seq = (uint32_t)(timer_get_ticks() & 0xFFFFFFFF);
    tcp_local_ack = 0;
    tcp_rx_head = 0;
    tcp_rx_tail = 0;

    /* Send SYN */
    tcp_state = TCP_SYN_SENT;
    send_tcp(dst_ip, tcp_local_port, dst_port,
             tcp_local_seq, 0, TCP_SYN, 0, 0);
    tcp_local_seq++;  /* SYN consumes one sequence number */

    /* Wait for SYN-ACK */
    uint64_t start = timer_get_ticks();
    while (tcp_state == TCP_SYN_SENT && timer_get_ticks() - start < 5000) {
        net_stack_process();
    }

    return (tcp_state == TCP_ESTABLISHED) ? 0 : -1;
}

int tcp_send_data(const void *data, int len)
{
    if (tcp_state != TCP_ESTABLISHED) return -1;

    const uint8_t *p = (const uint8_t *)data;
    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 1400) chunk = 1400;  /* MSS */
        send_tcp(tcp_remote_ip, tcp_local_port, tcp_remote_port,
                 tcp_local_seq, tcp_local_ack,
                 TCP_ACK | TCP_PSH, p + sent, chunk);
        tcp_local_seq += (uint32_t)chunk;
        sent += chunk;

        /* Brief delay to avoid overwhelming */
        uint64_t t = timer_get_ticks();
        while (timer_get_ticks() - t < 10)
            net_stack_process();
    }
    return sent;
}

int tcp_receive_data(void *buf, int buf_size, int timeout_ms)
{
    uint8_t *dst = (uint8_t *)buf;
    int received = 0;
    uint64_t start = timer_get_ticks();

    while (received < buf_size) {
        net_stack_process();

        while (tcp_rx_tail != tcp_rx_head && received < buf_size) {
            dst[received++] = tcp_rx_buf[tcp_rx_tail];
            tcp_rx_tail = (tcp_rx_tail + 1) % TCP_RX_BUF_SIZE;
        }

        if (received > 0 && tcp_rx_tail == tcp_rx_head) {
            /* Got some data and buffer empty, wait a bit for more */
            uint64_t wait_start = timer_get_ticks();
            while (timer_get_ticks() - wait_start < 100) {
                net_stack_process();
                if (tcp_rx_tail != tcp_rx_head) break;
            }
            if (tcp_rx_tail == tcp_rx_head) break;
        }

        if (tcp_state == TCP_CLOSED && tcp_rx_tail == tcp_rx_head)
            break;

        if (timer_get_ticks() - start > (uint64_t)timeout_ms)
            break;
    }
    return received;
}

void tcp_close(void)
{
    if (tcp_state == TCP_ESTABLISHED) {
        send_tcp(tcp_remote_ip, tcp_local_port, tcp_remote_port,
                 tcp_local_seq, tcp_local_ack, TCP_FIN | TCP_ACK, 0, 0);
        tcp_local_seq++;
        tcp_state = TCP_FIN_WAIT;

        uint64_t start = timer_get_ticks();
        while (tcp_state != TCP_CLOSED && timer_get_ticks() - start < 2000)
            net_stack_process();
    }
    tcp_state = TCP_CLOSED;
}

int tcp_is_connected(void)
{
    return tcp_state == TCP_ESTABLISHED;
}

/* ── HTTP GET ────────────────────────────────────────────────────────── */
int http_get(const char *host, uint16_t port, const char *path,
             char *response_buf, int buf_size)
{
    if (!net_is_available()) return -1;

    /* Resolve hostname */
    uint32_t ip = dns_resolve(host);
    if (!ip) return -1;

    /* Connect */
    if (tcp_connect(ip, port) != 0)
        return -1;

    /* Build HTTP request */
    char request[512];
    int rpos = 0;
    const char *get = "GET ";
    for (int i = 0; get[i]; i++) request[rpos++] = get[i];
    for (int i = 0; path[i]; i++) request[rpos++] = path[i];
    const char *ver = " HTTP/1.1\r\nHost: ";
    for (int i = 0; ver[i]; i++) request[rpos++] = ver[i];
    for (int i = 0; host[i]; i++) request[rpos++] = host[i];
    const char *hdr = "\r\nConnection: close\r\nUser-Agent: nextOS/2.5.0\r\n\r\n";
    for (int i = 0; hdr[i]; i++) request[rpos++] = hdr[i];
    request[rpos] = 0;

    /* Send request */
    tcp_send_data(request, rpos);

    /* Receive response */
    int total = tcp_receive_data(response_buf, buf_size - 1, 10000);
    response_buf[total] = 0;

    tcp_close();

    /* Find end of HTTP headers (blank line \r\n\r\n) */
    int body_start = -1;
    for (int i = 0; i < total - 3; i++) {
        if (response_buf[i] == '\r' && response_buf[i+1] == '\n' &&
            response_buf[i+2] == '\r' && response_buf[i+3] == '\n') {
            body_start = i + 4;
            break;
        }
    }

    if (body_start < 0) return total;  /* No headers found, return raw */

    /* Move body to start of buffer */
    int body_len = total - body_start;
    for (int i = 0; i < body_len; i++)
        response_buf[i] = response_buf[body_start + i];
    response_buf[body_len] = 0;

    return body_len;
}

/* ── Initialization ──────────────────────────────────────────────────── */
void net_stack_init(void)
{
    mem_zero(arp_cache, sizeof(arp_cache));
    tcp_state = TCP_CLOSED;
    tcp_rx_head = 0;
    tcp_rx_tail = 0;

    /* Set defaults for QEMU user networking (SLIRP) */
    our_ip     = parse_ip_string("10.0.2.15");
    gateway_ip = parse_ip_string("10.0.2.2");
    netmask    = parse_ip_string("255.255.255.0");
    dns_server = parse_ip_string("10.0.2.3");

    net_get_mac(our_mac);
}

void net_stack_set_ip(uint32_t ip, uint32_t gw, uint32_t mask, uint32_t dns)
{
    our_ip = ip;
    gateway_ip = gw;
    netmask = mask;
    dns_server = dns;
}

uint32_t net_stack_get_ip(void)
{
    return our_ip;
}
