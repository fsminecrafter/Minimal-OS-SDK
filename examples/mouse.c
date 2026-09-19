// examples/mouse.c — polls the USB mouse and prints movement/buttons.
//
// Build: ./build.sh examples/mouse.c
// Run:   run 0:/programs/mouse.run

#include "minimalos.h"
#include "stdio.h"
#include "x86_64/user_mouse.h"

int main(void) {
    mos_mouse_init();

    if (!mos_mouse_has_mouse()) {
        printf("No USB mouse detected.\n");
    }

    for (int i = 0; i < 500; i++) {
        mos_mouse_poll();

        syscall_mouse_state_t state = {0};
        mos_mouse_get_state(&state);

        if (state.dx || state.dy || state.wheel || state.buttons) {
            printf("dx=%ld dy=%ld wheel=%ld L=%ld R=%ld M=%ld\n",
                   (long)state.dx, (long)state.dy, (long)state.wheel,
                   (long)(state.buttons & SYSCALL_MOUSE_BTN_LEFT   ? 1 : 0),
                   (long)(state.buttons & SYSCALL_MOUSE_BTN_RIGHT  ? 1 : 0),
                   (long)(state.buttons & SYSCALL_MOUSE_BTN_MIDDLE ? 1 : 0));
        }

        mos_sleep(20);
    }

    return 0;
}
