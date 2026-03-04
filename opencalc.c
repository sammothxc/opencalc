#include <math.h>
#include <stdlib.h>
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

#include "version.h"

// ── Line input buffer ─────────────────────────────────────────────────────────
#define LINE_BUF_MAX 256

static char line_buf[LINE_BUF_MAX];
static int  line_len    = 0;   // number of chars in buffer
static int  cursor_pos  = 0;   // insertion point (0 = before first char)
static int  line_start_x, line_start_y;
static int  fw, fh, ncols;     // font width/height, columns per row
static int  scr_w, scr_h;     // screen dimensions in pixels
static int  caps_on = 0;
static int  bottom_toolbar_y;

// ── Command history ────────────────────────────────────────────────────────────
#define HISTORY_MAX 5
static char history[HISTORY_MAX][LINE_BUF_MAX];
static int  history_count = 0;   // number of valid entries (0..HISTORY_MAX)
static int  history_head  = 0;   // index of most-recently-added entry (ring)
static int  history_pos   = -1;  // -1 = not browsing; 0 = newest, 1 = older...
static char history_draft[LINE_BUF_MAX]; // saved live input while browsing

static void line_goto(int i);  // forward declaration

static void history_push(const char *cmd, int len) {
    if (len == 0) return;
    // Don't store duplicates of the most recent entry
    if (history_count > 0) {
        int last = (history_head - 1 + HISTORY_MAX) % HISTORY_MAX;
        if (strncmp(history[last], cmd, len) == 0 && history[last][len] == '\0')
            return;
    }
    memcpy(history[history_head], cmd, len);
    history[history_head][len] = '\0';
    history_head = (history_head + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX) history_count++;
}

// Replace the current input line with s (redraws from start).
static void line_replace(const char *s) {
    int new_len = (int)strlen(s);
    // Erase old content by overwriting with spaces
    int old_len = line_len;
    line_goto(0);
    for (int i = 0; i < old_len; i++) lcd_putc(0, ' ');
    // Write new content
    line_len   = new_len;
    cursor_pos = new_len;
    memcpy(line_buf, s, new_len);
    line_buf[new_len] = '\0';
    line_goto(0);
    for (int i = 0; i < new_len; i++) lcd_putc(0, (uint8_t)line_buf[i]);
    line_goto(cursor_pos);
}

static void history_navigate(int dir) {
    // dir: -1 = newer (down), +1 = older (up)
    if (history_count == 0) return;

    if (history_pos == -1) {
        // Save draft before starting to browse
        if (dir != 1) return;
        memcpy(history_draft, line_buf, line_len);
        history_draft[line_len] = '\0';
        history_pos = 0;
    } else {
        int new_pos = history_pos + dir;
        if (new_pos < 0) {
            // Back to draft
            history_pos = -1;
            line_replace(history_draft);
            return;
        }
        if (new_pos >= history_count) return; // can't go further back
        history_pos = new_pos;
    }

    // Entry at history_pos: 0 = newest
    int idx = (history_head - 1 - history_pos + HISTORY_MAX * 2) % HISTORY_MAX;
    line_replace(history[idx]);
}

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

static void input_delete(void) {
    if (cursor_pos >= line_len) return;
    memmove(&line_buf[cursor_pos], &line_buf[cursor_pos + 1],
            line_len - cursor_pos - 1);
    line_len--;
    line_redraw_from(cursor_pos, 1);
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

static void input_home(void) {
    cursor_pos = 0;
    line_goto(0);
}

static void input_end(void) {
    cursor_pos = line_len;
    line_goto(line_len);
}

// ── Expression evaluator ──────────────────────────────────────────────────────
// Grammar (low to high precedence):
//   expr    = term   (('+' | '-') term)*
//   term    = power  (('*' | '/') power)*
//   power   = unary  ('^' unary)*          right-associative
//   unary   = ('-' | '+') unary | primary
//   primary = NUMBER | '(' expr ')'

typedef struct { const char *p; int err; } Parser;

static void ps_skip(Parser *ps) { while (*ps->p == ' ') ps->p++; }

static double parse_expr(Parser *ps);

static double parse_primary(Parser *ps) {
    ps_skip(ps);
    if (*ps->p == '(') {
        ps->p++;
        double v = parse_expr(ps);
        ps_skip(ps);
        if (*ps->p == ')') ps->p++; else ps->err = 1;
        return v;
    }
    char *end;
    double v = strtod(ps->p, &end);
    if (end == ps->p) { ps->err = 1; return 0; }
    ps->p = end;
    return v;
}

static double parse_unary(Parser *ps) {
    ps_skip(ps);
    if (*ps->p == '-') { ps->p++; return -parse_unary(ps); }
    if (*ps->p == '+') { ps->p++; return  parse_unary(ps); }
    return parse_primary(ps);
}

static double parse_power(Parser *ps) {
    double b = parse_unary(ps);
    ps_skip(ps);
    if (*ps->p == '^') { ps->p++; return pow(b, parse_power(ps)); }
    return b;
}

static double parse_term(Parser *ps) {
    double v = parse_power(ps);
    for (;;) {
        ps_skip(ps);
        if (*ps->p == '*') { ps->p++; v *= parse_power(ps); }
        else if (*ps->p == '(') { v *= parse_power(ps); }
        else if (*ps->p == '/') {
            ps->p++;
            double d = parse_power(ps);
            if (d == 0.0) { ps->err = 1; return 0; }
            v /= d;
        } else break;
    }
    return v;
}

static double parse_expr(Parser *ps) {
    double v = parse_term(ps);
    for (;;) {
        ps_skip(ps);
        if      (*ps->p == '+') { ps->p++; v += parse_term(ps); }
        else if (*ps->p == '-') { ps->p++; v -= parse_term(ps); }
        else break;
    }
    return v;
}

static int eval_expr(const char *s, double *out) {
    Parser ps = { s, 0 };
    *out = parse_expr(&ps);
    ps_skip(&ps);
    if (ps.err || *ps.p != '\0') return 0;
    return 1;
}

// ── Toolbar ───────────────────────────────────────────────────────────────────
static void draw_toolbar(void) {
    lcd_fill_rect(0, 0, ncols * fw - 1, fh - 1, BLACK);
    lcd_set_fg_colour(GREEN);
    lcd_set_xy(0, 0);
    lcd_print_string("OpenCalc v" APP_VERSION);
    if (caps_on) {
        lcd_set_bg_colour(YELLOW);
        lcd_set_fg_colour(BLACK);
        lcd_set_xy((ncols - 4) * fw, 0);
        lcd_print_string("CAPS");
        lcd_set_bg_colour(BLACK);
        lcd_set_fg_colour(GREEN);
    }
    lcd_fill_rect(0, fh, ncols * fw - 1, fh + 1, GREEN);
}

static const char * const fn_labels[] = {
    "Equations", "Graph", "Apps", "Settings"
};

static void draw_bottom_toolbar(void) {
    int box_w = scr_w / 4;   // 80px per box on a 320px screen
    int sep_y  = bottom_toolbar_y;
    int label_y = sep_y + 2;

    // Horizontal separator line (2px, mirrors top toolbar)
    lcd_fill_rect(0, sep_y, scr_w - 1, sep_y + 1, GREEN);
    // Clear label row
    lcd_fill_rect(0, label_y, scr_w - 1, label_y + fh - 1, BLACK);

    // Centered labels
    lcd_set_fg_colour(GREEN);
    for (int i = 0; i < 4; i++) {
        int label_px = (int)strlen(fn_labels[i]) * fw;
        int x = i * box_w + (box_w - label_px) / 2;
        lcd_set_xy(x, label_y);
        lcd_print_string((char *)fn_labels[i]);
    }

    // Vertical separators between boxes
    for (int i = 1; i < 4; i++) {
        lcd_fill_rect(i * box_w, sep_y, i * box_w, label_y + fh - 1, GREEN);
    }
}

// ── CLI ───────────────────────────────────────────────────────────────────────
#define PROMPT "> "

static void print_prompt(void) {
    lcd_set_fg_colour(GREEN);
    lcd_print_string(PROMPT);
    lcd_get_xy(&line_start_x, &line_start_y);
    lcd_set_fg_colour(WHITE);
}

static void print_right(const char *s) {
    int slen = (int)strlen(s);
    int x, y;
    lcd_get_xy(&x, &y);
    int rx = (ncols - slen) * fw;
    lcd_set_fg_colour(YELLOW);
    lcd_set_xy(rx < 0 ? 0 : rx, y);
    lcd_print_string((char *)s);
    lcd_putc(0, '\n');
}

static void exec_command(const char *cmd, int len) {
    while (len > 0 && cmd[len - 1] == ' ') len--;
    if (len == 0) return;

    char buf[LINE_BUF_MAX];
    memcpy(buf, cmd, len);
    buf[len] = '\0';

    if (strcmp(buf, "cls") == 0) {
        lcd_clear_content();
        return;
    }
    if (strcmp(buf, "bat") == 0) {
        print_right("100%");
        return;
    }
    if (strcmp(buf, "ver") == 0) {
        print_right(APP_VERSION);
        return;
    }

    double result;
    if (eval_expr(buf, &result)) {
        char out[32];
        long long ival = (long long)result;
        if ((double)ival == result)
            snprintf(out, sizeof(out), "%lld", ival);
        else
            snprintf(out, sizeof(out), "%.10g", result);
        print_right(out);
    } else {
        print_right("?");
    }
}

static void input_newline(void) {
    history_push(line_buf, line_len);
    history_pos = -1;
    line_goto(line_len);
    lcd_putc(0, '\n');
    exec_command(line_buf, line_len);
    print_prompt();
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
    lcd_get_size(&scr_w, &scr_h);

    lcd_clear();
    int toolbar_h = fh + 2;
    bottom_toolbar_y = scr_h - (fh + 2);
    lcd_set_content_start(toolbar_h);
    lcd_set_content_end(bottom_toolbar_y);
    draw_toolbar();
    draw_bottom_toolbar();
    lcd_set_xy(0, toolbar_h);

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
    print_prompt();
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
        } else if (c == KEY_UP || c == KEY_DOWN) {
            lcd_cursor_off();
            history_navigate(c == KEY_UP ? 1 : -1);
            lcd_cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_LEFT || c == KEY_RIGHT || c == KEY_HOME || c == KEY_END) {
            lcd_cursor_off();
            if      (c == KEY_LEFT)  input_move_left();
            else if (c == KEY_RIGHT) input_move_right();
            else if (c == KEY_HOME)  input_home();
            else                     input_end();
            lcd_cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_CAPS_TOGGLE) {
            lcd_cursor_off();
            caps_on ^= 1;
            int sx, sy;
            lcd_get_xy(&sx, &sy);
            draw_toolbar();
            lcd_set_xy(sx, sy);
            lcd_set_fg_colour(WHITE);
            lcd_cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_DEL) {
            lcd_cursor_off();
            input_delete();
            lcd_cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c > 0) {
            lcd_cursor_off();
            if      (c == '\b')              { history_pos = -1; input_backspace(); }
            else if (c == '\r' || c == '\n') input_newline();
            else                             { history_pos = -1; input_insert((char)c); }
            lcd_cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        }
    }
#endif
}
