#ifndef MINIMALOS_STDIO_H
#define MINIMALOS_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "minimalos.h"

static inline void mos_printf_putc(char* buffer, size_t* used, long* written, char value) {
    if (*used == 128) {
        *written += (long)mos_write(1, buffer, *used);
        *used = 0;
    }
    buffer[(*used)++] = value;
}

static inline long mos_vprintf(const char* format, va_list args) {
    char buffer[128];
    size_t used = 0;
    long written = 0;

    for (size_t i = 0; format[i]; ++i) {
        if (format[i] != '%') {
            mos_printf_putc(buffer, &used, &written, format[i]);
        } else {
            char conversion = format[++i];
            char number[32];
            const char* text = 0;
            size_t length = 0;
            int base = 10;
            int negative = 0;

            if (conversion == '%') {
                mos_printf_putc(buffer, &used, &written, '%');
            } else if (conversion == 'c') {
                mos_printf_putc(buffer, &used, &written, (char)va_arg(args, int));
            } else if (conversion == 's') {
                text = va_arg(args, const char*);
                if (!text) text = "(null)";
                while (text[length]) ++length;
                for (size_t j = 0; j < length; ++j) {
                    mos_printf_putc(buffer, &used, &written, text[j]);
                }
            } else if (conversion == 'x' || conversion == 'X') {
                uint64_t value = va_arg(args, unsigned int);
                base = 16;
                do {
                    uint64_t digit = value % (uint64_t)base;
                    number[length++] = (char)(digit < 10 ? '0' + digit :
                        (conversion == 'x' ? 'a' : 'A') + digit - 10);
                    value /= (uint64_t)base;
                } while (value);
                for (size_t j = 0; j < length; ++j) {
                    mos_printf_putc(buffer, &used, &written, number[length - j - 1]);
                }
            } else if (conversion == 'd' || conversion == 'i' || conversion == 'u') {
                uint64_t value;
                if (conversion == 'u') {
                    value = va_arg(args, unsigned int);
                } else {
                    int value_signed = va_arg(args, int);
                    if (value_signed < 0) {
                        negative = 1;
                        value = (uint64_t)-(int64_t)value_signed;
                    } else {
                        value = (uint64_t)value_signed;
                    }
                }
                do {
                    number[length++] = (char)('0' + value % 10);
                    value /= 10;
                } while (value);
                if (negative) mos_printf_putc(buffer, &used, &written, '-');
                for (size_t j = 0; j < length; ++j) {
                    mos_printf_putc(buffer, &used, &written, number[length - j - 1]);
                }
            } else if (conversion == 'l') {
                char long_conversion = format[++i];
                uint64_t value;
                if (long_conversion == 'u') {
                    value = va_arg(args, unsigned long);
                } else {
                    long value_signed = va_arg(args, long);
                    if (value_signed < 0) {
                        negative = 1;
                        value = (uint64_t)-value_signed;
                    } else {
                        value = (uint64_t)value_signed;
                    }
                }
                do {
                    number[length++] = (char)('0' + value % 10);
                    value /= 10;
                } while (value);
                if (negative) mos_printf_putc(buffer, &used, &written, '-');
                for (size_t j = 0; j < length; ++j) {
                    mos_printf_putc(buffer, &used, &written, number[length - j - 1]);
                }
            } else {
                mos_printf_putc(buffer, &used, &written, '%');
                if (conversion) mos_printf_putc(buffer, &used, &written, conversion);
            }
        }

        if (used == sizeof(buffer)) {
            written += (long)mos_write(1, buffer, used);
            used = 0;
        }
    }

    if (used) written += (long)mos_write(1, buffer, used);
    return written;
}

static inline long printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    long result = mos_vprintf(format, args);
    va_end(args);
    return result;
}

#endif
