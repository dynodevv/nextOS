/*
 * nextOS - net_stack.h
 * Minimal network protocol stack (Ethernet, ARP, IP, UDP, TCP, DNS, HTTP)
 */
#ifndef NEXTOS_NET_STACK_H
#define NEXTOS_NET_STACK_H

#include <stdint.h>

/* ── Ethernet ────────────────────────────────────────────────────────── */
#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IPV4 0x0800

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
} __attribute__((packed)) eth_header_t;

/* ── ARP ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed)) arp_header_t;

/* ── IPv4 ────────────────────────────────────────────────────────────── */
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ipv4_header_t;

/* ── UDP ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

/* ── TCP ─────────────────────────────────────────────────────────────── */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_header_t;

/* ── DNS ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_header_t;

/* ── Network stack configuration ─────────────────────────────────────── */
#define NET_MAX_HTTP_RESPONSE 32768

void     net_stack_init(void);

/* Configuration */
void     net_stack_set_ip(uint32_t ip, uint32_t gateway, uint32_t netmask, uint32_t dns);
uint32_t net_stack_get_ip(void);

/* DNS resolution */
uint32_t dns_resolve(const char *hostname);

/* HTTP GET - returns response body length, or -1 on error */
int      http_get(const char *host, uint16_t port, const char *path,
                  char *response_buf, int buf_size);

/* HTTPS GET - returns response body length, or -1 on error */
int      https_get(const char *host, uint16_t port, const char *path,
                   char *response_buf, int buf_size);

/* Process incoming packets */
void     net_stack_process(void);

/* TCP connection (simple blocking) */
int      tcp_connect(uint32_t dst_ip, uint16_t dst_port);
int      tcp_send_data(const void *data, int len);
int      tcp_receive_data(void *buf, int buf_size, int timeout_ms);
void     tcp_close(void);
int      tcp_is_connected(void);

#endif /* NEXTOS_NET_STACK_H */
