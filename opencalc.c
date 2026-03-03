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
    lcd_cursor_on();
    int cursor_state = 1;
    uint64_t last_blink = time_us_64();

    while (1) {
        if (time_us_64() - last_blink >= 500000) {  // 500ms blink
            if (cursor_state) lcd_cursor_off();
            else              lcd_cursor_on();
            cursor_state ^= 1;
            last_blink = time_us_64();
        }

        int c = read_i2c_kbd();
        if (c == KEY_BOOTSEL) {
            reset_usb_boot(0, 0);
        } else if (c == KEY_REBOOT) {
            watchdog_reboot(0, 0, 0);
        } else if (c > 0) {
            lcd_cursor_off();
            lcd_putc(0, (uint8_t)c);
            lcd_cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        }
    }
#endif
}
