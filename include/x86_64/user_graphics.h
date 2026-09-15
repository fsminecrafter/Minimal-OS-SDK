#ifndef MINIMALOS_X86_64_USER_GRAPHICS_H
#define MINIMALOS_X86_64_USER_GRAPHICS_H

#include <stdint.h>
#include "x86_64/user_syscalls.h"

static inline uint32_t mos_graphics_get_width(void) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_GET_WIDTH;
    return (uint32_t)mos_graphics(&request);
}

static inline uint32_t mos_graphics_get_height(void) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_GET_HEIGHT;
    return (uint32_t)mos_graphics(&request);
}

static inline void mos_graphics_clear(uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_CLEAR;
    request.args[0] = r;
    request.args[1] = g;
    request.args[2] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_pixel(int32_t x, int32_t y,
                                      uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_PIXEL;
    request.args[0] = x;
    request.args[1] = y;
    request.args[2] = r;
    request.args[3] = g;
    request.args[4] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_line(int32_t x1, int32_t y1, int32_t x2,
                                     int32_t y2, uint8_t r, uint8_t g,
                                     uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_LINE;
    request.args[0] = x1; request.args[1] = y1;
    request.args[2] = x2; request.args[3] = y2;
    request.args[4] = r; request.args[5] = g; request.args[6] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_rectangle(int32_t x, int32_t y,
                                          uint32_t width, uint32_t height,
                                          uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_RECTANGLE;
    request.args[0] = x; request.args[1] = y;
    request.args[2] = width; request.args[3] = height;
    request.args[4] = r; request.args[5] = g; request.args[6] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_fill_rectangle(int32_t x, int32_t y,
                                               uint32_t width, uint32_t height,
                                               uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_FILL_RECTANGLE;
    request.args[0] = x; request.args[1] = y;
    request.args[2] = width; request.args[3] = height;
    request.args[4] = r; request.args[5] = g; request.args[6] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_triangle(int32_t x1, int32_t y1,
                                         int32_t x2, int32_t y2,
                                         int32_t x3, int32_t y3,
                                         uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_TRIANGLE;
    request.args[0] = x1; request.args[1] = y1;
    request.args[2] = x2; request.args[3] = y2;
    request.args[4] = x3; request.args[5] = y3;
    request.args[6] = r; request.args[7] = g; request.args[8] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_fill_triangle(int32_t x1, int32_t y1,
                                              int32_t x2, int32_t y2,
                                              int32_t x3, int32_t y3,
                                              uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_FILL_TRIANGLE;
    request.args[0] = x1; request.args[1] = y1;
    request.args[2] = x2; request.args[3] = y2;
    request.args[4] = x3; request.args[5] = y3;
    request.args[6] = r; request.args[7] = g; request.args[8] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_circle(int32_t x, int32_t y, uint32_t radius,
                                       uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_CIRCLE;
    request.args[0] = x; request.args[1] = y; request.args[2] = radius;
    request.args[3] = r; request.args[4] = g; request.args[5] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_fill_circle(int32_t x, int32_t y,
                                            uint32_t radius, uint8_t r,
                                            uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_FILL_CIRCLE;
    request.args[0] = x; request.args[1] = y; request.args[2] = radius;
    request.args[3] = r; request.args[4] = g; request.args[5] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_ellipse(int32_t x, int32_t y,
                                        uint32_t radius_x, uint32_t radius_y,
                                        uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_ELLIPSE;
    request.args[0] = x; request.args[1] = y;
    request.args[2] = radius_x; request.args[3] = radius_y;
    request.args[4] = r; request.args[5] = g; request.args[6] = b;
    mos_graphics(&request);
}

static inline void mos_graphics_text(const char* text, int32_t x, int32_t y,
                                     uint8_t r, uint8_t g, uint8_t b) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_TEXT;
    request.args[0] = x; request.args[1] = y;
    request.args[2] = r; request.args[3] = g; request.args[4] = b;
    request.text = text;
    mos_graphics(&request);
}

static inline void mos_graphics_set_resolution(uint32_t columns,
                                               uint32_t rows) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_SET_RESOLUTION;
    request.args[0] = columns;
    request.args[1] = rows;
    mos_graphics(&request);
}

static inline void mos_graphics_measure_text(const char* text,
                                             uint32_t* out_width,
                                             uint32_t* out_height) {
    syscall_graphics_request_t request = { .op = SYS_GRAPHICS_MEASURE_TEXT,
                                           .text = text,
                                           .out_width = out_width,
                                           .out_height = out_height };
    mos_graphics(&request);
}

#endif