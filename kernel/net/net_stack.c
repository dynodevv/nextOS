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
/* Computes the Internet checksum (RFC 1071) over on-wire (network byte order)
 * data.  The sum is byte-order independent because the folding arithmetic
 * produces the same result regardless of host endianness.  The odd-byte case
 * adds the last byte in the low position, which matches RFC 1071 behaviour. */
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
/* IPs (our_ip, dst_ip) are stored in network byte order throughout the stack,
 * so the pseudo-header words are already in the correct on-wire order. */
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
    if (total < 60) {
        int pad_start = total;
        total = 60;  /* Minimum Ethernet frame size */
        mem_zero(pkt_buf + pad_start, total - pad_start);
    }
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
    /* IPs are stored in network byte order throughout the stack */
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
    if (hdr_len < 20 || hdr_len > len)
        return;
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
    uint16_t udp_len = ntohs(udp->length);
    if (udp_len < 8 || udp_len > (uint16_t)len)
        return;
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t dst_port = ntohs(udp->dst_port);
    int payload_len = (int)udp_len - 8;
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
    if (hdr_len < 20 || hdr_len > len) return;
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
            /* Got some data and buffer empty, wait for more but respect timeout */
            uint64_t remaining_ms = 0;
            uint64_t elapsed = timer_get_ticks() - start;
            if (elapsed < (uint64_t)timeout_ms)
                remaining_ms = (uint64_t)timeout_ms - elapsed;
            uint64_t inter_wait = (remaining_ms < 500) ? remaining_ms : 500;
            uint64_t wait_start = timer_get_ticks();
            while (timer_get_ticks() - wait_start < inter_wait) {
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
static int str_len_ns(const char *s) { int n = 0; while (s[n]) n++; return n; }

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
    int max_rpos = (int)(sizeof(request) - 1);
    const char *get = "GET ";
    for (int i = 0; get[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = get[i];
    }
    for (int i = 0; path[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = path[i];
    }
    const char *ver = " HTTP/1.1\r\nHost: ";
    for (int i = 0; ver[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = ver[i];
    }
    for (int i = 0; host[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = host[i];
    }
    const char *hdr = "\r\nConnection: close\r\nUser-Agent: nextOS/2.5.0\r\n\r\n";
    for (int i = 0; hdr[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = hdr[i];
    }
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

/* ── TLS 1.2 Client ──────────────────────────────────────────────────── */
/*
 * Full TLS 1.2 implementation for HTTPS support.
 * Uses TLS_RSA_WITH_AES_128_CBC_SHA256 (0x003C) cipher suite.
 *
 * Implements complete handshake:
 *   ClientHello -> ServerHello,Certificate,ServerHelloDone ->
 *   ClientKeyExchange,ChangeCipherSpec,Finished ->
 *   ChangeCipherSpec,Finished
 * Then encrypted application data for HTTP request/response.
 */
#include "tls_crypto.h"

/* TLS record types */
#define TLS_CHANGE_CIPHER  20
#define TLS_ALERT          21
#define TLS_HANDSHAKE      22
#define TLS_APPLICATION    23

/* Handshake types */
#define TLS_CLIENT_HELLO    1
#define TLS_SERVER_HELLO    2
#define TLS_CERTIFICATE    11
#define TLS_SERVER_DONE    14
#define TLS_CLIENT_KEY_EX  16
#define TLS_FINISHED       20

/* TLS 1.2 version */
#define TLS_VER_MAJOR 3
#define TLS_VER_MINOR 3

/* TLS session state */
static uint8_t tls_client_random[32];
static uint8_t tls_server_random[32];
static uint8_t tls_master_secret[48];
static uint8_t tls_client_write_key[16];
static uint8_t tls_server_write_key[16];
static uint8_t tls_client_write_iv[16];
static uint8_t tls_server_write_iv[16];
static uint8_t tls_client_write_mac_key[32];
static uint8_t tls_server_write_mac_key[32];
static uint64_t tls_client_seq;
static uint64_t tls_server_seq;

/* Negotiated cipher suite info */
static uint16_t tls_cipher_suite;   /* Selected cipher suite ID */
static int tls_mac_len;             /* MAC length: 20 for SHA-1, 32 for SHA-256 */

/* Handshake message accumulator for Finished verification */
#define TLS_HS_BUF_SIZE 8192
static uint8_t tls_hs_buf[TLS_HS_BUF_SIZE];
static int tls_hs_len;

static void tls_hs_accumulate(const uint8_t *data, int len)
{
    if (tls_hs_len + len <= TLS_HS_BUF_SIZE) {
        mem_copy(tls_hs_buf + tls_hs_len, data, len);
        tls_hs_len += len;
    }
}

/* Server certificate buffer */
#define TLS_CERT_BUF_SIZE 4096
static uint8_t tls_cert_buf[TLS_CERT_BUF_SIZE];
static int tls_cert_len;

/* Large receive buffer for TLS records */
#define TLS_RECV_BUF_SIZE 8192
static uint8_t tls_recv_buf[TLS_RECV_BUF_SIZE];

/* Build and send TLS ClientHello */
static int tls_send_client_hello(const char *host)
{
    uint8_t msg[512];
    int pos = 0;
    int host_len = str_len_ns(host);

    /* Generate client random */
    uint32_t ts = (uint32_t)(timer_get_ticks() / 1000);
    tls_client_random[0] = (ts >> 24) & 0xFF;
    tls_client_random[1] = (ts >> 16) & 0xFF;
    tls_client_random[2] = (ts >> 8) & 0xFF;
    tls_client_random[3] = ts & 0xFF;
    tls_random_bytes(tls_client_random + 4, 28);

    /* TLS Record Header */
    msg[pos++] = TLS_HANDSHAKE;
    msg[pos++] = TLS_VER_MAJOR;
    msg[pos++] = 1;  /* TLS 1.0 in record layer for compat */
    int rec_len_pos = pos;
    pos += 2;  /* Length placeholder */

    /* Handshake header */
    int hs_start = pos;
    msg[pos++] = TLS_CLIENT_HELLO;
    int hs_len_pos = pos;
    pos += 3;  /* Length placeholder */

    /* Client Version: TLS 1.2 */
    int hello_start = pos;
    msg[pos++] = TLS_VER_MAJOR;
    msg[pos++] = TLS_VER_MINOR;

    /* Random (32 bytes) */
    mem_copy(msg + pos, tls_client_random, 32);
    pos += 32;

    /* Session ID length = 0 */
    msg[pos++] = 0;

    /* Cipher suites - offer AES_128_CBC_SHA256 primarily */
    msg[pos++] = 0; msg[pos++] = 4;  /* 2 cipher suites = 4 bytes */
    msg[pos++] = 0x00; msg[pos++] = 0x3C;  /* TLS_RSA_WITH_AES_128_CBC_SHA256 */
    msg[pos++] = 0x00; msg[pos++] = 0x2F;  /* TLS_RSA_WITH_AES_128_CBC_SHA */

    /* Compression methods */
    msg[pos++] = 1;  /* 1 method */
    msg[pos++] = 0;  /* NULL compression */

    /* Extensions */
    int ext_len_pos = pos;
    pos += 2;
    int ext_start = pos;

    /* SNI extension */
    msg[pos++] = 0x00; msg[pos++] = 0x00;
    int sni_len_pos = pos;
    pos += 2;
    int sni_start = pos;
    msg[pos++] = (uint8_t)((host_len + 3) >> 8);
    msg[pos++] = (uint8_t)((host_len + 3) & 0xFF);
    msg[pos++] = 0;
    msg[pos++] = (uint8_t)(host_len >> 8);
    msg[pos++] = (uint8_t)(host_len & 0xFF);
    for (int i = 0; i < host_len && pos < (int)sizeof(msg) - 30; i++)
        msg[pos++] = (uint8_t)host[i];
    int sni_data_len = pos - sni_start;
    msg[sni_len_pos] = (uint8_t)(sni_data_len >> 8);
    msg[sni_len_pos + 1] = (uint8_t)(sni_data_len & 0xFF);

    /* Signature algorithms extension (required for TLS 1.2) */
    msg[pos++] = 0x00; msg[pos++] = 0x0d;  /* signature_algorithms */
    msg[pos++] = 0x00; msg[pos++] = 0x08;  /* extension length */
    msg[pos++] = 0x00; msg[pos++] = 0x06;  /* list length */
    msg[pos++] = 0x04; msg[pos++] = 0x01;  /* RSA/PKCS1/SHA256 */
    msg[pos++] = 0x05; msg[pos++] = 0x01;  /* RSA/PKCS1/SHA384 */
    msg[pos++] = 0x02; msg[pos++] = 0x01;  /* RSA/PKCS1/SHA1 */

    /* EC point formats extension (some servers require this) */
    msg[pos++] = 0x00; msg[pos++] = 0x0b;  /* ec_point_formats */
    msg[pos++] = 0x00; msg[pos++] = 0x02;  /* extension length */
    msg[pos++] = 0x01;                      /* list length */
    msg[pos++] = 0x00;                      /* uncompressed */

    /* Renegotiation info extension (empty, signals initial handshake) */
    msg[pos++] = 0xFF; msg[pos++] = 0x01;  /* renegotiation_info */
    msg[pos++] = 0x00; msg[pos++] = 0x01;  /* extension length */
    msg[pos++] = 0x00;                      /* empty renegotiated_connection */

    /* Fill extensions length */
    int ext_len = pos - ext_start;
    msg[ext_len_pos] = (uint8_t)(ext_len >> 8);
    msg[ext_len_pos + 1] = (uint8_t)(ext_len & 0xFF);

    /* Fill handshake length */
    int hello_len = pos - hello_start;
    msg[hs_len_pos] = 0;
    msg[hs_len_pos + 1] = (uint8_t)(hello_len >> 8);
    msg[hs_len_pos + 2] = (uint8_t)(hello_len & 0xFF);

    /* Fill record length */
    int rec_payload_len = pos - hs_start;
    msg[rec_len_pos] = (uint8_t)(rec_payload_len >> 8);
    msg[rec_len_pos + 1] = (uint8_t)(rec_payload_len & 0xFF);

    /* Accumulate handshake message (without record header) */
    tls_hs_len = 0;
    tls_cipher_suite = 0x003C;  /* Default: AES_128_CBC_SHA256 */
    tls_mac_len = 32;
    tls_hs_accumulate(msg + hs_start, rec_payload_len);

    return tcp_send_data(msg, pos);
}

/* Read a full TLS record from TCP. Returns record body length or -1 */
static int tls_read_record(uint8_t *out_type, uint8_t *buf, int buf_size)
{
    uint8_t hdr[5];
    int got = tcp_receive_data(hdr, 5, 8000);
    if (got < 5) return -1;

    *out_type = hdr[0];
    int rec_len = (hdr[3] << 8) | hdr[4];
    if (rec_len > buf_size) return -1;
    if (rec_len <= 0) return 0;

    /* Read record body */
    int total = 0;
    int remaining = rec_len;
    while (remaining > 0) {
        int chunk = tcp_receive_data(buf + total, remaining, 5000);
        if (chunk <= 0) break;
        total += chunk;
        remaining -= chunk;
    }
    if (remaining != 0) return -1;
    return total;
}

/* Process server handshake: ServerHello, Certificate, ServerHelloDone */
static int tls_process_server_handshake(rsa_pubkey_t *server_key)
{
    int got_hello = 0, got_cert = 0, got_done = 0;
    tls_cert_len = 0;

    while (!got_done) {
        uint8_t rec_type;
        int rec_len = tls_read_record(&rec_type, tls_recv_buf, TLS_RECV_BUF_SIZE);
        if (rec_len < 0) return -1;

        if (rec_type == TLS_ALERT) return -1;

        if (rec_type == TLS_HANDSHAKE) {
            /* Accumulate for Finished hash */
            tls_hs_accumulate(tls_recv_buf, rec_len);

            /* Parse handshake messages within record */
            int hpos = 0;
            while (hpos + 4 <= rec_len) {
                uint8_t hs_type = tls_recv_buf[hpos];
                int hs_len = ((int)tls_recv_buf[hpos+1] << 16) |
                             ((int)tls_recv_buf[hpos+2] << 8) |
                             tls_recv_buf[hpos+3];

                if (hpos + 4 + hs_len > rec_len) break;

                if (hs_type == TLS_SERVER_HELLO) {
                    /* Extract server random (bytes 2-33 of hello body) */
                    if (hs_len >= 34) {
                        mem_copy(tls_server_random, tls_recv_buf + hpos + 6, 32);
                    }
                    /* Extract selected cipher suite */
                    if (hs_len >= 37) {
                        int body = hpos + 4;
                        int sid_len = tls_recv_buf[body + 34];
                        int cs_off = body + 35 + sid_len;
                        if (cs_off + 2 <= hpos + 4 + hs_len) {
                            tls_cipher_suite = ((uint16_t)tls_recv_buf[cs_off] << 8) |
                                                tls_recv_buf[cs_off + 1];
                        }
                    }
                    got_hello = 1;
                } else if (hs_type == TLS_CERTIFICATE) {
                    /* Extract first certificate */
                    if (hs_len > 6) {
                        int certs_total = ((int)tls_recv_buf[hpos+4] << 16) |
                                          ((int)tls_recv_buf[hpos+5] << 8) |
                                          tls_recv_buf[hpos+6];
                        (void)certs_total;
                        /* First cert length */
                        if (hs_len > 9) {
                            int cert_len = ((int)tls_recv_buf[hpos+7] << 16) |
                                           ((int)tls_recv_buf[hpos+8] << 8) |
                                           tls_recv_buf[hpos+9];
                            if (cert_len > 0 && cert_len <= TLS_CERT_BUF_SIZE &&
                                hpos + 10 + cert_len <= rec_len) {
                                mem_copy(tls_cert_buf, tls_recv_buf + hpos + 10, cert_len);
                                tls_cert_len = cert_len;
                            }
                        }
                    }
                    got_cert = 1;
                } else if (hs_type == TLS_SERVER_DONE) {
                    got_done = 1;
                }

                hpos += 4 + hs_len;
            }
        }
    }

    if (!got_hello || !got_cert) return -1;
    if (tls_cert_len == 0) return -1;

    /* Extract RSA public key from certificate
     * NOTE: This implementation does NOT validate the certificate chain,
     * expiration, or hostname. HTTPS connections are therefore vulnerable
     * to man-in-the-middle attacks. Full certificate validation (chain of
     * trust, CRL/OCSP, hostname matching) is not yet implemented. */
    if (rsa_extract_pubkey(tls_cert_buf, tls_cert_len, server_key) != 0)
        return -1;
    if (server_key->mod_len == 0)
        return -1;

    return 0;
}

/* Derive master secret and key material */
static void tls_derive_keys(const uint8_t *pre_master_secret)
{
    /* Determine MAC length from negotiated cipher suite:
     * 0x002F = AES_128_CBC_SHA    -> HMAC-SHA-1   (20 bytes)
     * 0x003C = AES_128_CBC_SHA256 -> HMAC-SHA-256 (32 bytes) */
    if (tls_cipher_suite == 0x003C)
        tls_mac_len = 32;
    else
        tls_mac_len = 20;

    /* master_secret = PRF(pre_master_secret, "master secret",
     *                     ClientHello.random + ServerHello.random) */
    uint8_t seed[64];
    mem_copy(seed, tls_client_random, 32);
    mem_copy(seed + 32, tls_server_random, 32);
    tls_prf_sha256(pre_master_secret, 48, "master secret", seed, 64,
                   tls_master_secret, 48);

    /* key_block = PRF(master_secret, "key expansion",
     *                 server_random + client_random) */
    uint8_t ks_seed[64];
    mem_copy(ks_seed, tls_server_random, 32);
    mem_copy(ks_seed + 32, tls_client_random, 32);

    /* Key material: 2*(mac_key + enc_key(16) + IV(16))
     * SHA-256: 2*(32+16+16) = 128,  SHA-1: 2*(20+16+16) = 104 */
    int kb_len = 2 * (tls_mac_len + 16 + 16);
    uint8_t key_block[128];
    tls_prf_sha256(tls_master_secret, 48, "key expansion", ks_seed, 64,
                   key_block, kb_len);

    int off = 0;
    mem_copy(tls_client_write_mac_key, key_block + off, tls_mac_len); off += tls_mac_len;
    mem_copy(tls_server_write_mac_key, key_block + off, tls_mac_len); off += tls_mac_len;
    mem_copy(tls_client_write_key, key_block + off, 16); off += 16;
    mem_copy(tls_server_write_key, key_block + off, 16); off += 16;
    mem_copy(tls_client_write_iv, key_block + off, 16); off += 16;
    mem_copy(tls_server_write_iv, key_block + off, 16);

    tls_client_seq = 0;
    tls_server_seq = 0;
}

/* Send ClientKeyExchange: encrypt pre-master secret with server's RSA key */
static int tls_send_client_key_exchange(const rsa_pubkey_t *server_key)
{
    /* Generate 48-byte pre-master secret */
    uint8_t pms[48];
    pms[0] = TLS_VER_MAJOR;
    pms[1] = TLS_VER_MINOR;
    tls_random_bytes(pms + 2, 46);

    /* RSA PKCS#1 encrypt */
    uint8_t encrypted[RSA_MAX_MOD_BYTES];
    int enc_len = rsa_pkcs1_encrypt(server_key, pms, 48, encrypted, sizeof(encrypted));
    if (enc_len < 0) return -1;

    /* Derive keys from pre-master secret */
    tls_derive_keys(pms);

    /* Build ClientKeyExchange handshake message */
    int msg_len = 4 + 2 + enc_len;  /* HS header(4) + length prefix(2) + encrypted */
    uint8_t *msg = (uint8_t *)kmalloc(5 + msg_len);
    if (!msg) return -1;

    int pos = 0;
    /* TLS record header */
    msg[pos++] = TLS_HANDSHAKE;
    msg[pos++] = TLS_VER_MAJOR;
    msg[pos++] = TLS_VER_MINOR;
    msg[pos++] = (uint8_t)(msg_len >> 8);
    msg[pos++] = (uint8_t)(msg_len & 0xFF);

    int hs_start = pos;
    /* Handshake type */
    msg[pos++] = TLS_CLIENT_KEY_EX;
    /* Length (3 bytes) */
    int body_len = 2 + enc_len;
    msg[pos++] = 0;
    msg[pos++] = (uint8_t)(body_len >> 8);
    msg[pos++] = (uint8_t)(body_len & 0xFF);

    /* Encrypted pre-master secret length (2 bytes) */
    msg[pos++] = (uint8_t)(enc_len >> 8);
    msg[pos++] = (uint8_t)(enc_len & 0xFF);
    mem_copy(msg + pos, encrypted, enc_len);
    pos += enc_len;

    /* Accumulate handshake message */
    tls_hs_accumulate(msg + hs_start, msg_len);

    int ret = tcp_send_data(msg, pos);
    kfree(msg);
    return ret;
}

/* Send ChangeCipherSpec */
static int tls_send_change_cipher_spec(void)
{
    uint8_t msg[6];
    msg[0] = TLS_CHANGE_CIPHER;
    msg[1] = TLS_VER_MAJOR;
    msg[2] = TLS_VER_MINOR;
    msg[3] = 0;
    msg[4] = 1;  /* Length = 1 */
    msg[5] = 1;  /* ChangeCipherSpec message */
    return tcp_send_data(msg, 6);
}

/* Compute MAC for a TLS record (HMAC-SHA-256 or HMAC-SHA-1) */
static void tls_compute_mac(const uint8_t *mac_key,
                            uint64_t seq_num, uint8_t rec_type,
                            const uint8_t *data, int data_len,
                            uint8_t *mac_out)
{
    /* MAC input: seq_num(8) + type(1) + version(2) + length(2) + data */
    uint8_t header[13];
    int hi = 0;
    for (int i = 7; i >= 0; i--)
        header[hi++] = (seq_num >> (i * 8)) & 0xFF;
    header[hi++] = rec_type;
    header[hi++] = TLS_VER_MAJOR;
    header[hi++] = TLS_VER_MINOR;
    header[hi++] = (uint8_t)(data_len >> 8);
    header[hi++] = (uint8_t)(data_len & 0xFF);

    uint8_t k_pad[64];

    if (tls_mac_len == 20) {
        /* HMAC-SHA-1 */
        sha1_ctx_t ctx;
        mem_zero(k_pad, 64);
        mem_copy(k_pad, mac_key, tls_mac_len);
        for (int i = 0; i < 64; i++) k_pad[i] ^= 0x36;
        sha1_init(&ctx);
        sha1_update(&ctx, k_pad, 64);
        sha1_update(&ctx, header, 13);
        sha1_update(&ctx, data, data_len);
        uint8_t inner[20];
        sha1_final(&ctx, inner);
        mem_zero(k_pad, 64);
        mem_copy(k_pad, mac_key, tls_mac_len);
        for (int i = 0; i < 64; i++) k_pad[i] ^= 0x5c;
        sha1_init(&ctx);
        sha1_update(&ctx, k_pad, 64);
        sha1_update(&ctx, inner, 20);
        sha1_final(&ctx, mac_out);
    } else {
        /* HMAC-SHA-256 */
        sha256_ctx_t ctx;
        mem_zero(k_pad, 64);
        mem_copy(k_pad, mac_key, tls_mac_len);
        for (int i = 0; i < 64; i++) k_pad[i] ^= 0x36;
        sha256_init(&ctx);
        sha256_update(&ctx, k_pad, 64);
        sha256_update(&ctx, header, 13);
        sha256_update(&ctx, data, data_len);
        uint8_t inner[32];
        sha256_final(&ctx, inner);
        mem_zero(k_pad, 64);
        mem_copy(k_pad, mac_key, tls_mac_len);
        for (int i = 0; i < 64; i++) k_pad[i] ^= 0x5c;
        sha256_init(&ctx);
        sha256_update(&ctx, k_pad, 64);
        sha256_update(&ctx, inner, 32);
        sha256_final(&ctx, mac_out);
    }
}

/* Send encrypted TLS record */
static int tls_send_encrypted(uint8_t rec_type, const uint8_t *data, int data_len)
{
    /* Build plaintext: data + MAC + padding */
    uint8_t mac[32];
    tls_compute_mac(tls_client_write_mac_key, tls_client_seq,
                    rec_type, data, data_len, mac);

    int plain_len = data_len + tls_mac_len;  /* data + MAC */
    /* TLS padding for AES-CBC */
    int pad = 16 - (plain_len % 16);
    int total_plain = plain_len + pad;

    /* Generate random IV (16 bytes) */
    uint8_t iv[16];
    tls_random_bytes(iv, 16);

    /* Plaintext buffer: data + mac + padding */
    uint8_t *pt = (uint8_t *)kmalloc(total_plain);
    if (!pt) return -1;
    mem_copy(pt, data, data_len);
    mem_copy(pt + data_len, mac, tls_mac_len);
    for (int i = 0; i < pad; i++)
        pt[plain_len + i] = (uint8_t)(pad - 1);  /* TLS padding value = pad_len - 1 */

    /* Encrypt with AES-128-CBC (no PKCS#7 since we added TLS padding) */
    uint8_t *ct = (uint8_t *)kmalloc(total_plain);
    if (!ct) { kfree(pt); return -1; }

    /* Manual CBC encrypt (TLS padding is different from PKCS#7) */
    aes128_ctx_t aes_ctx;
    aes128_init(&aes_ctx, tls_client_write_key);
    uint8_t prev[16];
    mem_copy(prev, iv, 16);
    for (int i = 0; i < total_plain; i += 16) {
        uint8_t block[16];
        mem_copy(block, pt + i, 16);
        for (int j = 0; j < 16; j++) block[j] ^= prev[j];
        aes128_encrypt_block(&aes_ctx, block, ct + i);
        mem_copy(prev, ct + i, 16);
    }

    /* TLS record: record header(5) + IV(16) + ciphertext */
    int rec_payload = 16 + total_plain;
    uint8_t *rec = (uint8_t *)kmalloc(5 + rec_payload);
    if (!rec) { kfree(pt); kfree(ct); return -1; }

    rec[0] = rec_type;
    rec[1] = TLS_VER_MAJOR;
    rec[2] = TLS_VER_MINOR;
    rec[3] = (uint8_t)(rec_payload >> 8);
    rec[4] = (uint8_t)(rec_payload & 0xFF);
    mem_copy(rec + 5, iv, 16);
    mem_copy(rec + 5 + 16, ct, total_plain);

    int ret = tcp_send_data(rec, 5 + rec_payload);

    tls_client_seq++;
    kfree(pt);
    kfree(ct);
    kfree(rec);
    return ret;
}

/* Decrypt a TLS record */
static int tls_decrypt_record(const uint8_t *data, int data_len,
                              uint8_t rec_type, uint8_t *out, int max_out)
{
    if (data_len < 32) return -1;  /* Need at least IV(16) + one block(16) */

    const uint8_t *iv = data;
    const uint8_t *ct = data + 16;
    int ct_len = data_len - 16;

    if (ct_len % 16 != 0) return -1;
    if (ct_len > max_out) return -1;

    /* Decrypt AES-128-CBC */
    aes128_ctx_t aes_ctx;
    aes128_init(&aes_ctx, tls_server_write_key);
    const uint8_t *prev = iv;
    for (int i = 0; i < ct_len; i += 16) {
        uint8_t block[16];
        aes128_decrypt_block(&aes_ctx, ct + i, block);
        for (int j = 0; j < 16; j++) block[j] ^= prev[j];
        mem_copy(out + i, block, 16);
        prev = ct + i;
    }

    /* Remove TLS padding */
    int pad_val = out[ct_len - 1];
    int pad_count = pad_val + 1;
    if (pad_count > ct_len) return -1;

    int content_plus_mac = ct_len - pad_count;
    /* MAC length depends on cipher suite */
    if (content_plus_mac < tls_mac_len) return -1;
    int content_len = content_plus_mac - tls_mac_len;

    /* Verify MAC: compute expected MAC over decrypted content */
    uint8_t expected_mac[32];
    tls_compute_mac(tls_server_write_mac_key, tls_server_seq,
                    rec_type, out, content_len, expected_mac);
    const uint8_t *received_mac = out + content_len;
    int mac_ok = 1;
    for (int i = 0; i < tls_mac_len; i++) {
        if (received_mac[i] != expected_mac[i]) { mac_ok = 0; break; }
    }
    if (!mac_ok) return -1;

    tls_server_seq++;
    return content_len;
}

/* Send Finished message */
static int tls_send_finished(void)
{
    /* Compute verify_data = PRF(master_secret, "client finished",
     *                           SHA-256(handshake_messages))[0..11] */
    uint8_t hs_hash[32];
    sha256(tls_hs_buf, tls_hs_len, hs_hash);

    uint8_t verify_data[12];
    tls_prf_sha256(tls_master_secret, 48, "client finished",
                   hs_hash, 32, verify_data, 12);

    /* Build Finished handshake message */
    uint8_t finished[16];
    finished[0] = TLS_FINISHED;
    finished[1] = 0;
    finished[2] = 0;
    finished[3] = 12;
    mem_copy(finished + 4, verify_data, 12);

    /* Accumulate client Finished in HS hash (needed for server Finished) */
    tls_hs_accumulate(finished, 16);

    return tls_send_encrypted(TLS_HANDSHAKE, finished, 16);
}

/* Wait for server's ChangeCipherSpec + Finished */
static int tls_receive_server_finished(void)
{
    int got_ccs = 0, got_finished = 0;

    while (!got_finished) {
        uint8_t rec_type;
        int rec_len = tls_read_record(&rec_type, tls_recv_buf, TLS_RECV_BUF_SIZE);
        if (rec_len < 0) return -1;

        if (rec_type == TLS_ALERT) return -1;

        if (rec_type == TLS_CHANGE_CIPHER) {
            got_ccs = 1;
        } else if (rec_type == TLS_HANDSHAKE && got_ccs) {
            /* Encrypted handshake - decrypt it */
            uint8_t pt[256];
            int pt_len = tls_decrypt_record(tls_recv_buf, rec_len, TLS_HANDSHAKE, pt, sizeof(pt));
            if (pt_len < 4) return -1;

            uint8_t hs_type = pt[0];

            /* Skip NewSessionTicket (type 4) - many servers send this */
            if (hs_type == 0x04) {
                /* NewSessionTicket: ignore but continue waiting for Finished */
                continue;
            }

            /* Verify: should be Finished(20) + verify_data(12) */
            if (hs_type != TLS_FINISHED) return -1;
            if (pt_len < 16) return -1;

            /* Compute expected server verify_data */
            uint8_t hs_hash[32];
            sha256(tls_hs_buf, tls_hs_len, hs_hash);
            uint8_t expected[12];
            tls_prf_sha256(tls_master_secret, 48, "server finished",
                           hs_hash, 32, expected, 12);

            /* Compare (skip HS header: type(1) + length(3) = 4 bytes) */
            int ok = 1;
            for (int i = 0; i < 12; i++) {
                if (pt[4 + i] != expected[i]) { ok = 0; break; }
            }
            if (!ok) return -1;

            got_finished = 1;
        }
    }
    return 0;
}

int https_get(const char *host, uint16_t port, const char *path,
              char *response_buf, int buf_size)
{
    if (!net_is_available()) return -1;

    /* Resolve hostname */
    uint32_t ip = dns_resolve(host);
    if (!ip) return -1;

    /* Connect TCP to HTTPS port */
    if (tcp_connect(ip, port) != 0)
        return -1;

    /* Step 1: Send ClientHello */
    if (tls_send_client_hello(host) < 0) {
        tcp_close();
        return -1;
    }

    /* Step 2: Receive ServerHello, Certificate, ServerHelloDone */
    rsa_pubkey_t server_key;
    if (tls_process_server_handshake(&server_key) != 0) {
        tcp_close();
        /* Return error page */
        int len = 0;
        const char *err =
            "<html><body bgcolor=\"#FFFFF0\">"
            "<h1>HTTPS Handshake Failed</h1>"
            "<p>Could not complete the TLS handshake with the server.</p>"
            "<p>The server may require cipher suites or TLS extensions "
            "that nextOS does not support.</p>"
            "<p>Try using <b>http://</b> instead if available.</p>"
            "</body></html>";
        for (int i = 0; err[i] && len < buf_size - 1; i++)
            response_buf[len++] = err[i];
        response_buf[len] = 0;
        return len;
    }

    /* Step 3: Send ClientKeyExchange (RSA encrypted pre-master secret) */
    if (tls_send_client_key_exchange(&server_key) < 0) {
        tcp_close();
        return -1;
    }

    /* Step 4: Send ChangeCipherSpec */
    if (tls_send_change_cipher_spec() < 0) {
        tcp_close();
        return -1;
    }

    /* Step 5: Send Finished */
    if (tls_send_finished() < 0) {
        tcp_close();
        return -1;
    }

    /* Step 6: Receive server's ChangeCipherSpec + Finished */
    if (tls_receive_server_finished() != 0) {
        tcp_close();
        int len = 0;
        const char *err =
            "<html><body bgcolor=\"#FFFFF0\">"
            "<h1>HTTPS Encryption Failed</h1>"
            "<p>TLS handshake was completed but the server's Finished "
            "message could not be verified.</p>"
            "<p>Try using <b>http://</b> instead if available.</p>"
            "</body></html>";
        for (int i = 0; err[i] && len < buf_size - 1; i++)
            response_buf[len++] = err[i];
        response_buf[len] = 0;
        return len;
    }

    /* Step 7: Send HTTP request over TLS */
    char request[512];
    int rpos = 0;
    int max_rpos = (int)(sizeof(request) - 1);
    const char *get = "GET ";
    for (int i = 0; get[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = get[i];
    }
    for (int i = 0; path[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = path[i];
    }
    const char *ver = " HTTP/1.1\r\nHost: ";
    for (int i = 0; ver[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = ver[i];
    }
    for (int i = 0; host[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = host[i];
    }
    const char *hdr = "\r\nConnection: close\r\nUser-Agent: nextOS/2.5.0\r\nAccept: text/html,*/*\r\n\r\n";
    for (int i = 0; hdr[i]; i++) {
        if (rpos >= max_rpos) { tcp_close(); return -1; }
        request[rpos++] = hdr[i];
    }
    request[rpos] = 0;

    if (tls_send_encrypted(TLS_APPLICATION, (uint8_t *)request, rpos) < 0) {
        tcp_close();
        return -1;
    }

    /* Step 8: Receive encrypted HTTP response */
    int total_body = 0;
    int header_done = 0;
    int body_start_off = 0;

    for (int attempt = 0; attempt < 20 && total_body < buf_size - 1; attempt++) {
        uint8_t rec_type;
        int rec_len = tls_read_record(&rec_type, tls_recv_buf, TLS_RECV_BUF_SIZE);
        if (rec_len <= 0) break;

        if (rec_type == TLS_ALERT) break;

        if (rec_type == TLS_APPLICATION) {
            uint8_t pt[TLS_RECV_BUF_SIZE];
            int pt_len = tls_decrypt_record(tls_recv_buf, rec_len, TLS_APPLICATION, pt, sizeof(pt));
            if (pt_len <= 0) break;

            if (!header_done) {
                /* Find end of HTTP headers */
                int temp_start = total_body;
                int copy_len = pt_len;
                if (temp_start + copy_len >= buf_size) copy_len = buf_size - temp_start - 1;
                mem_copy(response_buf + temp_start, pt, copy_len);
                total_body += copy_len;
                response_buf[total_body] = 0;

                /* Search for \r\n\r\n in accumulated data */
                for (int i = 0; i < total_body - 3; i++) {
                    if (response_buf[i] == '\r' && response_buf[i+1] == '\n' &&
                        response_buf[i+2] == '\r' && response_buf[i+3] == '\n') {
                        body_start_off = i + 4;
                        header_done = 1;
                        /* Move body to start */
                        int body_len = total_body - body_start_off;
                        for (int j = 0; j < body_len; j++)
                            response_buf[j] = response_buf[body_start_off + j];
                        total_body = body_len;
                        response_buf[total_body] = 0;
                        break;
                    }
                }
            } else {
                /* Append decrypted data */
                int copy_len = pt_len;
                if (total_body + copy_len >= buf_size) copy_len = buf_size - total_body - 1;
                if (copy_len > 0) {
                    mem_copy(response_buf + total_body, pt, copy_len);
                    total_body += copy_len;
                }
            }
        }
    }

    response_buf[total_body] = 0;
    tcp_close();

    /* If we got no body at all, return error */
    if (total_body == 0) {
        const char *err =
            "<html><body bgcolor=\"#FFFFF0\">"
            "<h1>Empty HTTPS Response</h1>"
            "<p>The server did not return any content.</p>"
            "</body></html>";
        int len = 0;
        for (int i = 0; err[i] && len < buf_size - 1; i++)
            response_buf[len++] = err[i];
        response_buf[len] = 0;
        return len;
    }

    return total_body;
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
