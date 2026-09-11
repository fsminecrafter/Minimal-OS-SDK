#ifndef MINIMALOS_GRAPHICS_H
#define MINIMALOS_GRAPHICS_H

#include "x86_64/user_graphics.h"

#define graphics_get_width       mos_graphics_get_width
#define graphics_get_height      mos_graphics_get_height
#define graphics_clear           mos_graphics_clear
#define graphics_pixel           mos_graphics_pixel
#define graphics_line            mos_graphics_line
#define graphics_rectangle       mos_graphics_rectangle
#define graphics_fill_rectangle  mos_graphics_fill_rectangle
#define graphics_triangle        mos_graphics_triangle
#define graphics_fill_triangle   mos_graphics_fill_triangle
#define graphics_circle          mos_graphics_circle
#define graphics_fill_circle     mos_graphics_fill_circle
#define graphics_ellipse         mos_graphics_ellipse
#define graphics_text            mos_graphics_text
#define graphics_set_resolution  mos_graphics_set_resolution

#endif