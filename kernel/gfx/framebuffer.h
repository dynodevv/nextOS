/*
 * nextOS - framebuffer.h
 * Double-buffered VESA/GOP framebuffer engine with BGA mode switching
 */
#ifndef NEXTOS_FRAMEBUFFER_H
#define NEXTOS_FRAMEBUFFER_H

#include <stdint.h>

typedef struct {
    uint32_t *address;      /* Physical framebuffer address */
    uint32_t *backbuffer;   /* Off-screen back buffer */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;        /* Bytes per scanline */
    uint32_t  bpp;          /* Bits per pixel */
} framebuffer_t;

void      fb_init(uint64_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp);
int       fb_set_resolution(uint32_t w, uint32_t h);
void      fb_swap(void);
void      fb_clear(uint32_t color);
void      fb_putpixel(int x, int y, uint32_t color);
uint32_t  fb_getpixel(int x, int y);
void      fb_putpixel_vram(int x, int y, uint32_t color);
uint32_t  fb_getpixel_vram(int x, int y);
void      fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void      fb_draw_rect(int x, int y, int w, int h, uint32_t color);
void      fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void      fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void      fb_draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg);
void      fb_blit(int dx, int dy, int w, int h, const uint32_t *src);

framebuffer_t *fb_get(void);

/* Color helpers */
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t)((r << 16) | (g << 8) | b);
}

static inline uint32_t rgba_blend(uint32_t bg, uint32_t fg, uint8_t alpha) {
    uint8_t sr = (fg >> 16) & 0xFF, sg = (fg >> 8) & 0xFF, sb = fg & 0xFF;
    uint8_t dr = (bg >> 16) & 0xFF, dg = (bg >> 8) & 0xFF, db = bg & 0xFF;
    uint8_t rr = (sr * alpha + dr * (255 - alpha)) / 255;
    uint8_t rg = (sg * alpha + dg * (255 - alpha)) / 255;
    uint8_t rb = (sb * alpha + db * (255 - alpha)) / 255;
    return rgb(rr, rg, rb);
}

#endif /* NEXTOS_FRAMEBUFFER_H */
