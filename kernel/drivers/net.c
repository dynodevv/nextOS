/*
 * nextOS - net.c
 * Intel E1000 NIC driver (for QEMU e1000 emulation)
 *
 * Supports basic packet send/receive using polling mode.
 * PCI device: vendor 0x8086, device 0x100E (82540EM)
 */
#include "net.h"
#include "../arch/x86_64/idt.h"
#include "../mem/paging.h"

/* PCI Configuration Space */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                    (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                      uint32_t val)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                    (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* ── E1000 Register Offsets ──────────────────────────────────────────── */
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_EERD      0x0014
#define E1000_ICR       0x00C0
#define E1000_IMS       0x00D0
#define E1000_IMC       0x00D8
#define E1000_RCTL      0x0100
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TCTL      0x0400
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_RAL0      0x5400
#define E1000_RAH0      0x5404
#define E1000_MTA       0x5200

/* CTRL bits */
#define E1000_CTRL_SLU  (1u << 6)   /* Set Link Up */
#define E1000_CTRL_RST  (1u << 26)  /* Device Reset */

/* RCTL bits */
#define E1000_RCTL_EN   (1u << 1)
#define E1000_RCTL_BAM  (1u << 15)  /* Broadcast Accept */
#define E1000_RCTL_BSIZE_2048 0     /* Buffer size 2048 */
#define E1000_RCTL_SECRC (1u << 26) /* Strip Ethernet CRC */

/* TCTL bits */
#define E1000_TCTL_EN   (1u << 1)
#define E1000_TCTL_PSP  (1u << 3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

/* TX descriptor command bits */
#define E1000_TXD_CMD_EOP  (1u << 0)
#define E1000_TXD_CMD_IFCS (1u << 1)
#define E1000_TXD_CMD_RS   (1u << 3)
#define E1000_TXD_STAT_DD  (1u << 0)

/* RX descriptor status bits */
#define E1000_RXD_STAT_DD  (1u << 0)
#define E1000_RXD_STAT_EOP (1u << 1)

/* Descriptor counts (must be multiple of 8) */
#define NUM_RX_DESC  32
#define NUM_TX_DESC  8

/* ── Descriptor Structures ───────────────────────────────────────────── */
typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

/* ── Static Buffers (BSS, identity-mapped for DMA) ───────────────────── */
static e1000_rx_desc_t rx_descs[NUM_RX_DESC] __attribute__((aligned(16)));
static e1000_tx_desc_t tx_descs[NUM_TX_DESC] __attribute__((aligned(16)));
static uint8_t rx_buffers[NUM_RX_DESC][2048] __attribute__((aligned(16)));
static uint8_t tx_buffer[2048] __attribute__((aligned(16)));

/* ── Device State ────────────────────────────────────────────────────── */
static uint64_t mmio_base = 0;
static int      net_present = 0;
static uint8_t  mac_addr[6];
static uint32_t rx_cur = 0;
static uint32_t tx_cur = 0;

/* ── MMIO Access ─────────────────────────────────────────────────────── */
static uint32_t e1000_read(uint32_t reg)
{
    return *(volatile uint32_t *)(mmio_base + reg);
}

static void e1000_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(mmio_base + reg) = val;
}

/* ── Read MAC from EEPROM ────────────────────────────────────────────── */
static uint16_t e1000_eeprom_read(uint8_t addr)
{
    e1000_write(E1000_EERD, (1u) | ((uint32_t)addr << 8));
    uint32_t val;
    for (int i = 0; i < 100000; i++) {
        val = e1000_read(E1000_EERD);
        if (val & (1u << 4))
            return (uint16_t)(val >> 16);
    }
    return 0;
}

static void e1000_read_mac(void)
{
    /* Try EEPROM first */
    uint16_t w0 = e1000_eeprom_read(0);
    uint16_t w1 = e1000_eeprom_read(1);
    uint16_t w2 = e1000_eeprom_read(2);
    if (w0 || w1 || w2) {
        mac_addr[0] = w0 & 0xFF;
        mac_addr[1] = (w0 >> 8) & 0xFF;
        mac_addr[2] = w1 & 0xFF;
        mac_addr[3] = (w1 >> 8) & 0xFF;
        mac_addr[4] = w2 & 0xFF;
        mac_addr[5] = (w2 >> 8) & 0xFF;
    } else {
        /* Fallback: read from RAL/RAH */
        uint32_t ral = e1000_read(E1000_RAL0);
        uint32_t rah = e1000_read(E1000_RAH0);
        mac_addr[0] = ral & 0xFF;
        mac_addr[1] = (ral >> 8) & 0xFF;
        mac_addr[2] = (ral >> 16) & 0xFF;
        mac_addr[3] = (ral >> 24) & 0xFF;
        mac_addr[4] = rah & 0xFF;
        mac_addr[5] = (rah >> 8) & 0xFF;
    }
}

/* ── Initialize RX ring ──────────────────────────────────────────────── */
static void e1000_init_rx(void)
{
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_descs[i].addr = (uint64_t)(uintptr_t)&rx_buffers[i][0];
        rx_descs[i].status = 0;
    }

    uint64_t rx_addr = (uint64_t)(uintptr_t)rx_descs;
    e1000_write(E1000_RDBAL, (uint32_t)(rx_addr & 0xFFFFFFFF));
    e1000_write(E1000_RDBAH, (uint32_t)(rx_addr >> 32));
    e1000_write(E1000_RDLEN, NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, NUM_RX_DESC - 1);
    rx_cur = 0;

    e1000_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                             E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);
}

/* ── Initialize TX ring ──────────────────────────────────────────────── */
static void e1000_init_tx(void)
{
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_descs[i].addr = 0;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = E1000_TXD_STAT_DD;
    }

    uint64_t tx_addr = (uint64_t)(uintptr_t)tx_descs;
    e1000_write(E1000_TDBAL, (uint32_t)(tx_addr & 0xFFFFFFFF));
    e1000_write(E1000_TDBAH, (uint32_t)(tx_addr >> 32));
    e1000_write(E1000_TDLEN, NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    tx_cur = 0;

    e1000_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                             (15u << E1000_TCTL_CT_SHIFT) |
                             (64u << E1000_TCTL_COLD_SHIFT));
}

/* ── PCI Scan for E1000 ──────────────────────────────────────────────── */
static int find_e1000(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read(bus, slot, func, 0);
                if (id == 0xFFFFFFFF) continue;
                uint16_t vendor = id & 0xFFFF;
                uint16_t device = (id >> 16) & 0xFFFF;
                /* Intel 82540EM (QEMU default e1000) */
                if (vendor == 0x8086 && (device == 0x100E || device == 0x100F ||
                    device == 0x10D3 || device == 0x153A)) {
                    *out_bus = bus;
                    *out_slot = slot;
                    *out_func = func;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */
void net_init(void)
{
    uint8_t bus, slot, func;
    if (!find_e1000(&bus, &slot, &func)) {
        net_present = 0;
        return;
    }

    /* Enable bus mastering + memory space */
    uint32_t cmd = pci_read(bus, slot, func, 0x04);
    cmd |= (1u << 2) | (1u << 1);  /* bus master + memory space */
    pci_write(bus, slot, func, 0x04, cmd);

    /* Read BAR0 (memory-mapped I/O) */
    uint32_t bar0 = pci_read(bus, slot, func, 0x10);
    mmio_base = bar0 & ~0xFu;

    /* Map MMIO region (128KB should be enough for E1000 registers) */
    for (uint64_t off = 0; off < 0x20000; off += 4096)
        paging_map(mmio_base + off, mmio_base + off, 0x03);

    /* Reset the device */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
    for (volatile int i = 0; i < 100000; i++);  /* brief delay */

    /* Set link up */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU);

    /* Disable interrupts (we use polling) */
    e1000_write(E1000_IMC, 0xFFFFFFFF);

    /* Clear multicast table */
    for (int i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0);

    /* Read MAC address */
    e1000_read_mac();

    /* Initialize RX and TX */
    e1000_init_rx();
    e1000_init_tx();

    net_present = 1;
}

int net_is_available(void)
{
    return net_present;
}

void net_get_mac(uint8_t mac[6])
{
    for (int i = 0; i < 6; i++)
        mac[i] = mac_addr[i];
}

int net_send(const void *data, uint32_t len)
{
    if (!net_present || len > 1518) return -1;

    /* Wait for previous TX to complete */
    while (!(tx_descs[tx_cur].status & E1000_TXD_STAT_DD)) {
        for (volatile int i = 0; i < 1000; i++);
    }

    /* Copy data to TX buffer */
    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++)
        tx_buffer[i] = src[i];

    tx_descs[tx_cur].addr = (uint64_t)(uintptr_t)tx_buffer;
    tx_descs[tx_cur].length = (uint16_t)len;
    tx_descs[tx_cur].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    tx_descs[tx_cur].status = 0;

    uint32_t old = tx_cur;
    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    e1000_write(E1000_TDT, tx_cur);

    /* Wait for completion */
    for (int timeout = 0; timeout < 100000; timeout++) {
        if (tx_descs[old].status & E1000_TXD_STAT_DD)
            return 0;
    }
    return -1;
}

int net_receive(void *buf, uint32_t buf_size)
{
    if (!net_present) return -1;

    if (!(rx_descs[rx_cur].status & E1000_RXD_STAT_DD))
        return 0;  /* No packet available */

    uint16_t len = rx_descs[rx_cur].length;
    if (len > buf_size) len = (uint16_t)buf_size;

    uint8_t *dst = (uint8_t *)buf;
    uint8_t *src = rx_buffers[rx_cur];
    for (uint16_t i = 0; i < len; i++)
        dst[i] = src[i];

    rx_descs[rx_cur].status = 0;
    uint32_t old = rx_cur;
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    e1000_write(E1000_RDT, old);

    return len;
}

void net_poll(void)
{
    if (!net_present) return;
    /* Clear interrupt causes (for polling mode) */
    e1000_read(E1000_ICR);
}
