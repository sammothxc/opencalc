#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "i2ckbd.h"
#include "lcdspi.h"

// DEBUG: show raw hex of every keyboard event to identify correct key codes.
#define KBD_DEBUG 0

// ── Line input buffer ─────────────────────────────────────────────────────────
#define LINE_BUF_MAX 256

static char line_buf[LINE_BUF_MAX];
static int  line_len    = 0;   // number of chars in buffer
static int  cursor_pos  = 0;   // insertion point (0 = before first char)
static int  line_start_x, line_start_y;
static int  fw, fh, ncols;     // font width/height, columns per row

// Position the LCD draw cursor at character index i in the current line.
static void line_goto(int i) {
    int abs_col = (line_start_x / fw) + i;
    lcd_set_xy((abs_col % ncols) * fw,
               line_start_y + (abs_col / ncols) * fh);
}

// Redraw line_buf[from .. line_len), then clear one extra cell if clear_tail.
static void line_redraw_from(int from, int clear_tail) {
    line_goto(from);
    for (int i = from; i < line_len; i++)
        lcd_putc(0, (uint8_t)line_buf[i]);
    if (clear_tail)
        lcd_putc(0, ' ');   // clears the now-vacant trailing cell
}

static void input_insert(char c) {
    if (line_len >= LINE_BUF_MAX - 1) return;
    memmove(&line_buf[cursor_pos + 1], &line_buf[cursor_pos],
            line_len - cursor_pos);
    line_buf[cursor_pos] = c;
    cursor_pos++;
    line_len++;
    line_redraw_from(cursor_pos - 1, 0);
    line_goto(cursor_pos);
}

static void input_backspace(void) {
    if (cursor_pos == 0) return;
    memmove(&line_buf[cursor_pos - 1], &line_buf[cursor_pos],
            line_len - cursor_pos);
    cursor_pos--;
    line_len--;
    line_redraw_from(cursor_pos, 1);
    line_goto(cursor_pos);
}

static void input_move_left(void) {
    if (cursor_pos == 0) return;
    cursor_pos--;
    line_goto(cursor_pos);
}

static void input_move_right(void) {
    if (cursor_pos >= line_len) return;
    cursor_pos++;
    line_goto(cursor_pos);
}

static void input_newline(void) {
    line_goto(line_len);
    lcd_putc(0, '\n');
    lcd_get_xy(&line_start_x, &line_start_y);
    line_len   = 0;
    cursor_pos = 0;
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();
    init_i2c_kbd();
    lcd_init();
    lcd_get_metrics(&fw, &fh, &ncols);

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
    lcd_get_xy(&line_start_x, &line_start_y);
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
        } else if (c == KEY_LEFT || c == KEY_RIGHT) {
            lcd_cursor_off();
            if (c == KEY_LEFT) input_move_left();
            else               input_move_right();
            lcd_cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c > 0) {
            lcd_cursor_off();
            if      (c == '\b')              input_backspace();
            else if (c == '\r' || c == '\n') input_newline();
            else                             input_insert((char)c);
            lcd_cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        }
    }
#endif
}
