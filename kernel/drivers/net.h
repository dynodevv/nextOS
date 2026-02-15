/*
 * nextOS - net.h
 * Network driver interface (Intel E1000 for QEMU)
 */
#ifndef NEXTOS_NET_H
#define NEXTOS_NET_H

#include <stdint.h>

#define ETH_FRAME_MAX  1518
#define ETH_ALEN       6

void net_init(void);
int  net_is_available(void);
void net_get_mac(uint8_t mac[6]);
int  net_send(const void *data, uint32_t len);
int  net_receive(void *buf, uint32_t buf_size);
void net_poll(void);

#endif /* NEXTOS_NET_H */
