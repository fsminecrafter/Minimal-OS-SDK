#ifndef MINIMALOS_WINDOW_H
#define MINIMALOS_WINDOW_H

#include <stdint.h>
#include <stdbool.h>
#include "syscall.h"
#include "vm.h"

typedef struct {
    void (*onwindowclose)(uint32_t id);
    void (*onwindowminimize)(uint32_t id);
    void (*onwindowmaximise)(uint32_t id);
    void (*windowsizechange_recall)(uint32_t id, uint32_t width,
                                     uint32_t height);
    void (*onwindowfocus)(uint32_t id, int focused);
} mos_window_callbacks_t;

static inline uint64_t mos_window_call(syscall_window_request_t* request) {
    uint64_t result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"((uint64_t)SYS_WINDOW),
                       "D"((uint64_t)(uintptr_t)request),
                       "S"(0), "d"(0)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

static inline syscall_window_request_t mos_window_request(uint32_t op,
                                                          uint32_t id) {
    syscall_window_request_t request = {
        .size = sizeof(syscall_window_request_t),
        .version = SYS_WINDOW_ABI_VERSION,
        .op = op,
        .id = id
    };
    return request;
}

static inline uint32_t mos_window_create(uint32_t parent_id, uint32_t x,
                                         uint32_t y, uint32_t width,
                                         uint32_t height, const char* title) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_CREATE, 0);
    request.parent_id = parent_id;
    request.x = x;
    request.y = y;
    request.width = width;
    request.height = height;
    request.flags = SYSCALL_WINDOW_VISIBLE | SYSCALL_WINDOW_RESIZABLE;
    if (title) {
        uint32_t i = 0;
        while (i + 1u < SYSCALL_WINDOW_MAX_TITLE && title[i]) {
            request.title[i] = title[i];
            i++;
        }
        request.title[i] = '\0';
    }
    uint64_t result = mos_window_call(&request);
    return result <= SYSCALL_WINDOW_MAX_WINDOWS ? (uint32_t)result : 0;
}

static inline uint64_t mos_window_close(uint32_t id) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_CLOSE, id);
    return mos_window_call(&request);
}

static inline uint64_t mos_window_set_bounds(uint32_t id, uint32_t x,
                                            uint32_t y, uint32_t width,
                                            uint32_t height) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_SET_BOUNDS, id);
    request.x = x;
    request.y = y;
    request.width = width;
    request.height = height;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_set_title(uint32_t id, const char* title) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_SET_TITLE, id);
    if (title) {
        uint32_t i = 0;
        while (i + 1u < SYSCALL_WINDOW_MAX_TITLE && title[i]) {
            request.title[i] = title[i];
            i++;
        }
        request.title[i] = '\0';
    }
    return mos_window_call(&request);
}

static inline uint64_t mos_window_set_flags(uint32_t id, uint32_t flags) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_SET_FLAGS, id);
    request.flags = flags;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_get_info(uint32_t id,
                                           syscall_window_info_t* info);

static inline uint64_t mos_window_set_fullscreen(uint32_t id, bool enabled) {
    syscall_window_info_t info;
    uint64_t result = mos_window_get_info(id, &info);
    if (result != SYS_SUCCESS) return result;
    uint32_t flags = info.flags & ~(SYSCALL_WINDOW_FULLSCREEN |
                                    SYSCALL_WINDOW_MAXIMIZED |
                                    SYSCALL_WINDOW_MINIMIZED);
    if (enabled) flags |= SYSCALL_WINDOW_FULLSCREEN;
    return mos_window_set_flags(id, flags);
}

static inline uint64_t mos_window_minimize(uint32_t id) {
    syscall_window_info_t info;
    uint64_t result = mos_window_get_info(id, &info);
    if (result != SYS_SUCCESS) return result;
    uint32_t flags = info.flags & ~(SYSCALL_WINDOW_FULLSCREEN |
                                    SYSCALL_WINDOW_MAXIMIZED);
    flags |= SYSCALL_WINDOW_MINIMIZED;
    return mos_window_set_flags(id, flags);
}

static inline uint64_t mos_window_maximize(uint32_t id) {
    syscall_window_info_t info;
    uint64_t result = mos_window_get_info(id, &info);
    if (result != SYS_SUCCESS) return result;
    uint32_t flags = info.flags & ~(SYSCALL_WINDOW_FULLSCREEN |
                                    SYSCALL_WINDOW_MINIMIZED);
    flags |= SYSCALL_WINDOW_MAXIMIZED;
    return mos_window_set_flags(id, flags);
}

static inline uint64_t mos_window_restore(uint32_t id) {
    syscall_window_info_t info;
    uint64_t result = mos_window_get_info(id, &info);
    if (result != SYS_SUCCESS) return result;
    uint32_t flags = info.flags & ~(SYSCALL_WINDOW_FULLSCREEN |
                                    SYSCALL_WINDOW_MAXIMIZED |
                                    SYSCALL_WINDOW_MINIMIZED);
    return mos_window_set_flags(id, flags);
}

static inline uint64_t mos_window_set_style(uint32_t id,
                                           syscall_window_style_t style) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_SET_STYLE, id);
    request.style = style;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_present(uint32_t id, const uint32_t* pixels,
                                          uint32_t pitch, uint32_t x,
                                          uint32_t y, uint32_t width,
                                          uint32_t height) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_PRESENT, id);
    request.pixels = pixels;
    request.pitch = pitch;
    request.x = x;
    request.y = y;
    request.width = width;
    request.height = height;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_get_info(uint32_t id,
                                           syscall_window_info_t* info) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_GET_INFO, id);
    request.info = info;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_set_resizable(uint32_t id, bool resizable) {
    syscall_window_info_t info;
    uint64_t result = mos_window_get_info(id, &info);
    if (result != SYS_SUCCESS) return result;
    uint32_t flags = resizable
        ? info.flags | SYSCALL_WINDOW_RESIZABLE
        : info.flags & ~SYSCALL_WINDOW_RESIZABLE;
    return mos_window_set_flags(id, flags);
}

// Takes the mouse while the window is focused (games): the cursor is
// hidden and the window gets SYSCALL_WINDOW_EVENT_MOUSE_RELATIVE events
// (x, y = movement). Turn it off again to give the mouse back.
static inline uint64_t mos_window_set_pointer_lock(uint32_t id, bool locked) {
    syscall_window_info_t info;
    uint64_t result = mos_window_get_info(id, &info);
    if (result != SYS_SUCCESS) return result;
    uint32_t flags = locked
        ? info.flags | SYSCALL_WINDOW_POINTER_LOCK
        : info.flags & ~SYSCALL_WINDOW_POINTER_LOCK;
    // The focus bit belongs to the window manager.
    return mos_window_set_flags(id, flags & ~SYSCALL_WINDOW_FOCUSED);
}

static inline uint64_t mos_window_set_visible(uint32_t id, bool visible) {
    syscall_window_info_t info;
    uint64_t result = mos_window_get_info(id, &info);
    if (result != SYS_SUCCESS) return result;
    uint32_t flags = visible
        ? info.flags | SYSCALL_WINDOW_VISIBLE
        : info.flags & ~SYSCALL_WINDOW_VISIBLE;
    return mos_window_set_flags(id, flags);
}

static inline uint64_t mos_window_list(syscall_window_info_t* windows,
                                       uint32_t capacity) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_LIST, 0);
    request.windows = windows;
    request.capacity = capacity;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_get_frame(uint32_t id,
                                            syscall_window_frame_t* frame) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_GET_FRAME, id);
    request.frame = frame;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_next_event(uint32_t id,
                                             syscall_window_event_t* event) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_NEXT_EVENT, id);
    request.event = event;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_dispatch_events(
        uint32_t id, const mos_window_callbacks_t* callbacks) {
    if (!callbacks) return SYS_ERR_INVAL;
    syscall_window_event_t event;
    uint64_t count = 0;
    for (;;) {
        uint64_t result = mos_window_next_event(id, &event);
        if (result == SYS_ERR_AGAIN) return count;
        if (result != SYS_SUCCESS) return result;
        count++;
        switch (event.type) {
            case SYSCALL_WINDOW_EVENT_CLOSE_REQUEST:
                if (callbacks->onwindowclose)
                    callbacks->onwindowclose(id);
                break;
            case SYSCALL_WINDOW_EVENT_MINIMIZED:
                if (callbacks->onwindowminimize)
                    callbacks->onwindowminimize(id);
                break;
            case SYSCALL_WINDOW_EVENT_MAXIMIZED:
                if (callbacks->onwindowmaximise)
                    callbacks->onwindowmaximise(id);
                break;
            case SYSCALL_WINDOW_EVENT_RESIZED:
                if (callbacks->windowsizechange_recall)
                    callbacks->windowsizechange_recall(
                        id, (uint32_t)event.width, (uint32_t)event.height);
                break;
            case SYSCALL_WINDOW_EVENT_FOCUS_GAINED:
                if (callbacks->onwindowfocus)
                    callbacks->onwindowfocus(id, 1);
                break;
            case SYSCALL_WINDOW_EVENT_FOCUS_LOST:
                if (callbacks->onwindowfocus)
                    callbacks->onwindowfocus(id, 0);
                break;
            default:
                break;
        }
    }
}

static inline uint64_t mos_window_post_event(uint32_t id,
                                             syscall_window_event_t* event) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_POST_EVENT, id);
    request.event = event;
    return mos_window_call(&request);
}

static inline uint64_t mos_window_register_manager(void) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_REGISTER_MANAGER, 0);
    return mos_window_call(&request);
}

/* ------------------------------------------------------------------
 * Shared frames: draw into the window without a copy.
 *
 * mos_window_map_frame() maps the window's two buffers into your address
 * space (writable for the owner, read-only for the window manager). Draw
 * into the back buffer, then tell the kernel it is ready with
 * mos_window_present_shared() (buffer number + damage rectangle); the
 * window manager shows the front buffer and recomposites only the damage.
 * The old mos_window_present() keeps working on the same window.
 *
 * Each buffer holds the whole window (width * 4 bytes per row, pixels are
 * 0xAARRGGBB), the two are `buffer_stride` bytes apart. The damage you
 * present is the part that differs from the buffer shown before, so if you
 * redraw only a rectangle, copy the previous front buffer into the back
 * buffer first (or present the whole window).
 *
 * Resizing replaces the buffers: after SYSCALL_WINDOW_EVENT_RESIZED (or
 * whenever `generation` changed) the old mapping still works but is stale
 * and no longer shown; map again for the new buffers (new address, new
 * size) and unmap the old range yourself (mos_vm_unmap, two strides). The
 * frames are freed when the last process that mapped them unmaps them or
 * ends, whichever comes last.
 *
 * Errors: SYS_ERR_NOMEM (no contiguous memory / over the 128 MiB limit),
 * SYS_ERR_PERM (not your window and not the manager), SYS_ERR_AGAIN (the
 * manager asked for a window that is not shared), SYS_ERR_INVAL (bad
 * buffer number or rectangle, window not mapped yet).
 * ------------------------------------------------------------------ */
static inline uint64_t mos_window_map_frame(uint32_t id,
                                            syscall_window_shared_t* shared) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_MAP_FRAME, id);
    request.shared = shared;
    return mos_window_call(&request);
}

/* Buffer `buffer` is ready; x/y/width/height is what changed (0 width and
 * height: all of it). Becomes the front buffer; draw into the other one. */
static inline uint64_t mos_window_present_shared(uint32_t id, uint32_t buffer,
                                                 uint32_t x, uint32_t y,
                                                 uint32_t width,
                                                 uint32_t height) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_PRESENT_SHARED, id);
    request.flags = buffer;
    request.x = x;
    request.y = y;
    request.width = width;
    request.height = height;
    return mos_window_call(&request);
}

/* Window manager only: what changed in the window since the last call,
 * and which buffer is in front. buffer_count 0: the window uses the copy
 * path (damage is still reported). */
static inline uint64_t mos_window_take_damage(uint32_t id,
                                              syscall_window_shared_t* shared) {
    syscall_window_request_t request =
        mos_window_request(SYS_WINDOW_TAKE_DAMAGE, id);
    request.shared = shared;
    return mos_window_call(&request);
}

/* The convenient form for an application. */
typedef struct {
    syscall_window_shared_t info;     /* sizes, generation, front */
    uint32_t* buffer[SYSCALL_WINDOW_SHARED_BUFFERS];
} mos_window_surface_t;

static inline uint64_t mos_window_surface_map(uint32_t id,
                                              mos_window_surface_t* surface) {
    uint64_t result = mos_window_map_frame(id, &surface->info);
    if (result != SYS_SUCCESS) return result;
    for (uint32_t i = 0; i < SYSCALL_WINDOW_SHARED_BUFFERS; i++)
        surface->buffer[i] = (uint32_t*)(uintptr_t)(surface->info.address +
                                         (uint64_t)i * surface->info.buffer_stride);
    return SYS_SUCCESS;
}

/* Pixels to draw into: the buffer that is not shown. Rows are
 * info.pitch bytes apart. */
static inline uint32_t* mos_window_surface_back(const mos_window_surface_t* surface) {
    return surface->buffer[surface->info.front ^ 1u];
}

/* Presents the back buffer (damage rectangle as above) and swaps. */
static inline uint64_t mos_window_surface_present(uint32_t id,
                                                  mos_window_surface_t* surface,
                                                  uint32_t x, uint32_t y,
                                                  uint32_t width,
                                                  uint32_t height) {
    uint32_t next = surface->info.front ^ 1u;
    uint64_t result = mos_window_present_shared(id, next, x, y, width, height);
    if (result == SYS_SUCCESS) surface->info.front = next;
    return result;
}

/* Unmaps the buffers of this surface (also the stale ones after a resize). */
static inline uint64_t mos_window_surface_unmap(mos_window_surface_t* surface) {
    if (!surface->info.address) return SYS_ERR_INVAL;
    uint64_t result = mos_vm_unmap((void*)(uintptr_t)surface->info.address,
        (size_t)(surface->info.buffer_stride * SYSCALL_WINDOW_SHARED_BUFFERS));
    surface->info.address = 0;
    surface->buffer[0] = surface->buffer[1] = 0;
    return result;
}

#endif
