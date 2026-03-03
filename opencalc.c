#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "i2ckbd.h"
#include "lcdspi.h"

// DEBUG: show raw hex of every keyboard event to identify correct key codes.
// Remove once Ctrl+B value is confirmed.
#define KBD_DEBUG 0

int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();
    init_i2c_kbd();
    lcd_init();

    lcd_clear();
#if KBD_DEBUG
    lcd_print_string("KBD DEBUG\nPress keys to see raw hex\n\n");
    while (1) {
        uint16_t raw = read_i2c_kbd_raw();
        if (raw != 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%04X\n", raw);
            lcd_print_string(buf);
        }
    }
#else
    lcd_print_string("OpenCalc\n");
    while (1) {
        int c = read_i2c_kbd();
        if (c == KEY_BOOTSEL) {       // Ctrl+Alt+B -> BOOTSEL for flashing
            reset_usb_boot(0, 0);
        } else if (c == KEY_REBOOT) { // Ctrl+Alt+Del -> warm reboot into firmware
            watchdog_reboot(0, 0, 0);
        } else if (c > 0) {
            lcd_putc(0, (uint8_t)c);
        }
    }
#endif
}
