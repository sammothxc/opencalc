#include <math.h>
#include <complex.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "i2ckbd.h"
#include "lcdspi.h"

// DEBUG: show raw hex of every keyboard event to identify correct key codes.
#define KBD_DEBUG 0

#include "version.h"

// ── Line input buffer ─────────────────────────────────────────────────────────
#define LINE_BUF_MAX 256

// ── Clipboard ─────────────────────────────────────────────────────────────────
static char clipboard[LINE_BUF_MAX];
static int  clipboard_len = 0;

static char line_buf[LINE_BUF_MAX];
static int  line_len    = 0;   // number of chars in buffer
static int  cursor_pos  = 0;   // insertion point (0 = before first char)
static int  line_start_x, line_start_y;
static int  fw, fh, ncols;     // font width/height, columns per row
static int  scr_w, scr_h;     // screen dimensions in pixels
static int  caps_on = 0;
static int  bottom_toolbar_y;
#define CALC_NAME_MAX 32
static char calc_name[CALC_NAME_MAX] = {0};

// ── Screen modes ──────────────────────────────────────────────────────────────
typedef enum {
    SCREEN_HOME, SCREEN_EQUATIONS, SCREEN_GRAPH, SCREEN_SETTINGS, SCREEN_APPS,
    SCREEN_TABLE, SCREEN_ZOOM, SCREEN_CALCULATE, SCREEN_3D
} screen_mode_t;
static screen_mode_t screen_mode = SCREEN_HOME;

// ── Equations state ───────────────────────────────────────────────────────────
#define EQ_COUNT 10
static char eq_buf[EQ_COUNT][LINE_BUF_MAX];
static int  eq_len[EQ_COUNT];
static int  eq_cpos[EQ_COUNT];
static int  eq_sel = 0;

// ── Command history ────────────────────────────────────────────────────────────
#define HISTORY_MAX 5
static char history[HISTORY_MAX][LINE_BUF_MAX];
static int  history_count = 0;   // number of valid entries (0..HISTORY_MAX)
static int  history_head  = 0;   // index of most-recently-added entry (ring)
static int  history_pos   = -1;  // -1 = not browsing; 0 = newest, 1 = older...
static char history_draft[LINE_BUF_MAX]; // saved live input while browsing

static void line_goto(int i);                        // forward declaration
static void cursor_on(void);                         // forward declaration
static void eq_goto(int col);                        // forward declaration
static void eq_redraw_from(int row, int from, int clear_tail); // forward declaration

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

static void input_insert_str(const char *s, int len) {
    int avail = LINE_BUF_MAX - 1 - line_len;
    if (len > avail) len = avail;
    if (len <= 0) return;
    memmove(&line_buf[cursor_pos + len], &line_buf[cursor_pos], line_len - cursor_pos);
    memcpy(&line_buf[cursor_pos], s, len);
    cursor_pos += len;
    line_len   += len;
    line_buf[line_len] = '\0';
    line_redraw_from(cursor_pos - len, 0);
    line_goto(cursor_pos);
}

static void eq_insert_str(const char *s, int len) {
    int avail = (ncols - 4) - eq_len[eq_sel];
    if (len > avail) len = avail;
    if (len <= 0) return;
    int p = eq_cpos[eq_sel];
    memmove(&eq_buf[eq_sel][p + len], &eq_buf[eq_sel][p], eq_len[eq_sel] - p);
    memcpy(&eq_buf[eq_sel][p], s, len);
    eq_cpos[eq_sel] += len;
    eq_len[eq_sel]  += len;
    eq_redraw_from(eq_sel, p, 0);
    eq_goto(eq_cpos[eq_sel]);
}

// ── Autocomplete ──────────────────────────────────────────────────────────────
typedef struct { const char *name; int has_parens; } ac_entry_t;
static const ac_entry_t ac_table[] = {
    { "abs",   1 }, { "acos",  1 }, { "asin",  1 }, { "atan",  1 },
    { "bat",   0 }, { "cbrt",  1 }, { "ceil",  1 }, { "cle",   0 },
    { "cls",   0 }, { "cos",   1 }, { "cosh",  1 }, { "exp",   1 },
    { "floor", 1 }, { "ln",    1 }, { "log",   1 }, { "name",  1 },
    { "neg",   0 }, { "round", 1 }, { "sign",  1 }, { "sin",   1 },
    { "sinh",  1 }, { "sqrt",  1 }, { "tan",   1 }, { "tanh",  1 },
    { "ver",   0 },
};
#define AC_COUNT ((int)(sizeof(ac_table)/sizeof(ac_table[0])))

static int  ac_active     = 0;   // 1 while cycling through completions
static int  ac_start      = 0;   // line_buf index where the prefix begins
static char ac_prefix[16];       // original typed prefix
static int  ac_prefix_len = 0;
static int  ac_idx        = 0;   // current match index in ac_table
static int  ac_has_parens = 0;   // whether current insertion includes "()"
static int  ac_old_ins_len = 0;  // length of the previously inserted completion text

static void do_autocomplete(void) {
    char prefix[16];
    int  plen, pstart;
    int  search_from = 0;

    if (ac_active) {
        // Remove the previously inserted completion from the buffer.
        // If has_parens: cursor is between '(' and ')' so old text ends at cursor_pos+1.
        // If no parens:  cursor is right after the name so old text ends at cursor_pos.
        int old_end    = ac_has_parens ? cursor_pos + 1 : cursor_pos;
        int remove_len = old_end - ac_start;
        if (remove_len > 0 && old_end <= line_len) {
            memmove(&line_buf[ac_start], &line_buf[old_end], line_len - old_end);
            line_len   -= remove_len;
            cursor_pos  = ac_start;
            line_buf[line_len] = '\0';
        } else {
            ac_active = 0;  // state mismatch — fall through to fresh start
        }
        pstart = ac_start;
        plen   = ac_prefix_len;
        memcpy(prefix, ac_prefix, plen);
        prefix[plen]  = '\0';
        search_from   = (ac_idx + 1) % AC_COUNT;
    }

    if (!ac_active) {
        // Fresh: find the alphabetic run immediately before the cursor.
        pstart = cursor_pos;
        while (pstart > 0 && isalpha((unsigned char)line_buf[pstart - 1]))
            pstart--;
        plen = cursor_pos - pstart;
        if (plen == 0 || plen >= (int)sizeof(prefix)) return;
        memcpy(prefix, &line_buf[pstart], plen);
        prefix[plen] = '\0';
        // Remove the prefix from the buffer (it will be replaced by the completion).
        memmove(&line_buf[pstart], &line_buf[cursor_pos], line_len - cursor_pos);
        line_len   -= plen;
        cursor_pos  = pstart;
        line_buf[line_len] = '\0';
        search_from    = 0;
        ac_old_ins_len = 0;
    }

    // Two-pass search: forward from search_from, then wrap if cycling.
    int found = -1;
    for (int pass = 0; pass < 2 && found == -1; pass++) {
        int s = (pass == 0) ? search_from : 0;
        int e = (pass == 0) ? AC_COUNT    : search_from;
        for (int i = s; i < e; i++) {
            if (strncmp(ac_table[i].name, prefix, plen) == 0) {
                found = i; break;
            }
        }
        if (!ac_active) break;  // no wrap on first press
    }

    if (found == -1) {
        // No match: restore the prefix so the buffer is unchanged.
        if (line_len + plen < LINE_BUF_MAX) {
            memmove(&line_buf[pstart + plen], &line_buf[pstart], line_len - pstart);
            memcpy(&line_buf[pstart], prefix, plen);
            line_len   += plen;
            cursor_pos  = pstart + plen;
            line_buf[line_len] = '\0';
        }
        ac_active = 0;
        return;
    }

    // Insert the completion.
    const char *match      = ac_table[found].name;
    int         mlen       = (int)strlen(match);
    int         has_parens = ac_table[found].has_parens;
    int         ins_len    = mlen + (has_parens ? 2 : 0);
    if (line_len + ins_len >= LINE_BUF_MAX) { ac_active = 0; return; }

    memmove(&line_buf[pstart + ins_len], &line_buf[pstart], line_len - pstart);
    memcpy(&line_buf[pstart], match, mlen);
    if (has_parens) { line_buf[pstart + mlen] = '('; line_buf[pstart + mlen + 1] = ')'; }
    line_len   += ins_len;
    line_buf[line_len] = '\0';
    cursor_pos  = pstart + mlen + (has_parens ? 1 : 0);

    // Redraw from pstart; if the new completion is shorter than the old, clear the gap.
    line_goto(pstart);
    for (int i = pstart; i < line_len; i++) lcd_putc(0, (uint8_t)line_buf[i]);
    for (int i = line_len; i < pstart + ac_old_ins_len; i++) lcd_putc(0, ' ');
    line_goto(cursor_pos);

    // Save state for the next Tab press.
    ac_active      = 1;
    ac_start       = pstart;
    memcpy(ac_prefix, prefix, plen);
    ac_prefix[plen] = '\0';
    ac_prefix_len  = plen;
    ac_idx         = found;
    ac_has_parens  = has_parens;
    ac_old_ins_len = ins_len;
}

static int settings_sel[5] = { 0, 0, 0, 0, 0 }; // [0]=number fmt, [1]=decimal, [2]=angle, [3]=graph type, [4]=input mode(0=STD,1=RPN)

// ── Expression evaluator ──────────────────────────────────────────────────────
static double complex ans     = 0.0;  // last computed answer
static double complex vars[26] = {0}; // user variables a-z (e, i are read-only constants)
static char var_expr[26][LINE_BUF_MAX]; // formula strings; empty = plain numeric var
static int  cycle_detected = 0;       // set when a circular formula reference is found
// Grammar (low to high precedence):
//   expr    = term   (('+' | '-') term)*
//   term    = power  (('*' | '/') power)*
//   power   = unary  ('^' unary)*          right-associative
//   unary   = ('-' | '+') unary | primary
//   primary = NUMBER | IDENT | IDENT'('expr')' | '('expr')'

typedef struct { const char *p; int err; } Parser;

static void ps_skip(Parser *ps) { while (*ps->p == ' ') ps->p++; }

static double complex parse_expr(Parser *ps);
static int eval_expr(const char *s, double complex *out); // forward decl

static double complex parse_primary(Parser *ps) {
    ps_skip(ps);
    if (*ps->p == '(') {
        ps->p++;
        double complex v = parse_expr(ps);
        ps_skip(ps);
        if (*ps->p == ')') ps->p++; else ps->err = 1;
        return v;
    }
    // Identifier: constant or function call
    if (isalpha((unsigned char)*ps->p)) {
        char name[12]; int n = 0;
        while (n < 11 && isalpha((unsigned char)ps->p[n]))
            name[n] = tolower((unsigned char)ps->p[n]), n++;
        name[n] = '\0';
        ps->p += n;
        // Constants (no parentheses)
        if (!strcmp(name, "pi"))  return M_PI;
        if (!strcmp(name, "e"))   return M_E;
        if (!strcmp(name, "i"))   return I;
        if (!strcmp(name, "ans")) return ans;
        // Single-letter user variable (e and i are caught above as constants)
        if (n == 1) {
            int vi = (unsigned char)name[0] - 'a';
            if (var_expr[vi][0]) {
                static uint32_t eval_mask = 0;
                if (eval_mask & (1u << vi)) {
                    cycle_detected = 1; ps->err = 1; return 0;
                }
                eval_mask |= (1u << vi);
                double complex result = 0;
                int ok = eval_expr(var_expr[vi], &result);
                eval_mask &= ~(1u << vi);
                if (!ok) { ps->err = 1; return 0; }
                return result;
            }
            return vars[vi];
        }
        // Functions — expect '('
        ps_skip(ps);
        if (*ps->p != '(') { ps->err = 1; return 0; }
        ps->p++;
        double complex a = parse_expr(ps);
        ps_skip(ps);
        if (*ps->p == ')') ps->p++; else ps->err = 1;
        if (ps->err) return 0;
        int deg = (settings_sel[2] == 1);
        double to_rad = deg ? M_PI / 180.0 : 1.0;
        double to_deg = deg ? 180.0 / M_PI : 1.0;
        if (!strcmp(name, "sin"))   return csin(a * to_rad);
        if (!strcmp(name, "cos"))   return ccos(a * to_rad);
        if (!strcmp(name, "tan"))   return ctan(a * to_rad);
        if (!strcmp(name, "asin"))  return casin(a) * to_deg;
        if (!strcmp(name, "acos"))  return cacos(a) * to_deg;
        if (!strcmp(name, "atan"))  return catan(a) * to_deg;
        if (!strcmp(name, "sinh"))  return csinh(a);
        if (!strcmp(name, "cosh"))  return ccosh(a);
        if (!strcmp(name, "tanh"))  return ctanh(a);
        if (!strcmp(name, "sqrt"))  return csqrt(a);
        if (!strcmp(name, "cbrt"))  return cpow(a, 1.0/3.0);
        if (!strcmp(name, "log"))   return clog(a) / log(10.0);
        if (!strcmp(name, "ln"))    return clog(a);
        if (!strcmp(name, "exp"))   return cexp(a);
        if (!strcmp(name, "abs"))   return cabs(a);
        if (!strcmp(name, "floor")) return floor(creal(a)) + floor(cimag(a)) * I;
        if (!strcmp(name, "ceil"))  return ceil(creal(a))  + ceil(cimag(a))  * I;
        if (!strcmp(name, "round")) return round(creal(a)) + round(cimag(a)) * I;
        if (!strcmp(name, "sign"))  return (creal(a) > 0) - (creal(a) < 0);
        ps->err = 1; return 0;
    }
    // Numeric literal
    char *end;
    double v = strtod(ps->p, &end);
    if (end == ps->p) { ps->err = 1; return 0; }
    ps->p = end;
    return v;
}

static double complex parse_unary(Parser *ps) {
    ps_skip(ps);
    if (*ps->p == '-') { ps->p++; return -parse_unary(ps); }
    if (*ps->p == '+') { ps->p++; return  parse_unary(ps); }
    double complex v = parse_primary(ps);
    ps_skip(ps);
    while (*ps->p == '!') {
        ps->p++;
        double rv = creal(v);
        if (cimag(v) != 0.0 || rv < 0 || rv != floor(rv) || rv > 170) { ps->err = 1; return 0; }
        double f = 1.0;
        for (long long k = 2; k <= (long long)rv; k++) f *= (double)k;
        v = f;
        ps_skip(ps);
    }
    return v;
}

static double complex parse_power(Parser *ps) {
    double complex b = parse_unary(ps);
    ps_skip(ps);
    if (*ps->p == '^') { ps->p++; return cpow(b, parse_power(ps)); }
    return b;
}

static double complex parse_term(Parser *ps) {
    double complex v = parse_power(ps);
    for (;;) {
        if (ps->err) break;
        ps_skip(ps);
        if (*ps->p == '*') { ps->p++; v *= parse_power(ps); }
        else if (*ps->p == '(' || isalpha((unsigned char)*ps->p)) { v *= parse_power(ps); }
        else if (*ps->p == '/') {
            ps->p++;
            double complex d = parse_power(ps);
            if (cabs(d) == 0.0) { ps->err = 1; return 0; }
            v /= d;
        } else break;
    }
    return v;
}

static double complex parse_expr(Parser *ps) {
    double complex v = parse_term(ps);
    for (;;) {
        if (ps->err) break;
        ps_skip(ps);
        if      (*ps->p == '+') { ps->p++; v += parse_term(ps); }
        else if (*ps->p == '-') { ps->p++; v -= parse_term(ps); }
        else break;
    }
    return v;
}

static int eval_expr(const char *s, double complex *out) {
    Parser ps = { s, 0 };
    *out = parse_expr(&ps);
    ps_skip(&ps);
    if (ps.err || *ps.p != '\0') return 0;
    return 1;
}


// ── Toolbar ───────────────────────────────────────────────────────────────────
static void draw_toolbar(void) {
    int sx, sy; lcd_get_xy(&sx, &sy);
    lcd_fill_rect(0, 0, ncols * fw - 1, fh - 1, BLACK);
    // Title: if a custom name is set, alt temporarily reveals the version string
    int alt_held = (read_modifier_state() & MOD_ALT) != 0;
    const char *title = (calc_name[0] && alt_held)
                        ? "OpenCalc v" APP_VERSION
                        : (calc_name[0] ? calc_name : "OpenCalc v" APP_VERSION);
    lcd_set_fg_colour(GREEN);
    lcd_set_xy(0, 0);
    lcd_print_string((char *)title);
    // Right side: battery percentage when alt held, otherwise indicators
    lcd_set_fg_colour(YELLOW);
    lcd_set_bg_colour(BLACK);
    if (alt_held) {
        char bat[24];
        int raw = read_battery();
        int pct = (raw >> 8) & 0x7F;
        int charging = (raw >> 8) & 0x80;
        if (charging)
            snprintf(bat, sizeof(bat), "Charging, %d%%", pct);
        else
            snprintf(bat, sizeof(bat), "%d%%", pct);
        lcd_set_xy((ncols - (int)strlen(bat)) * fw, 0);
        lcd_print_string(bat);
    } else {
        // Indicators in settings row order: RAD/DEG  FUNC  STD/RPN
        static const char * const graph_labels[] = { "FUNC", "PARA", "POL", "SEQ" };
        const char *angle_label = (settings_sel[2] == 0) ? "RAD" : "DEG";
        const char *graph_label = graph_labels[settings_sel[3]];
        const char *rpn_label   = (settings_sel[4] == 0) ? "STD" : "RPN";
        int rpn_len   = (int)strlen(rpn_label);
        int graph_len = (int)strlen(graph_label);
        int angle_len = (int)strlen(angle_label);
        lcd_set_xy((ncols - rpn_len) * fw, 0);
        lcd_print_string((char *)rpn_label);
        lcd_set_xy((ncols - rpn_len - 1 - graph_len) * fw, 0);
        lcd_print_string((char *)graph_label);
        lcd_set_xy((ncols - rpn_len - 1 - graph_len - 1 - angle_len) * fw, 0);
        lcd_print_string((char *)angle_label);
    }
    lcd_fill_rect(0, fh, ncols * fw - 1, fh + 1, GREEN);
    lcd_set_fg_colour(WHITE);
    lcd_set_xy(sx, sy);
}

static const char * const fn_labels[]  = { "Equations", "Graph", "Apps", "Settings" };
static const char * const fn2_labels[] = { "Table", "Zoom", "Calculate", "3D" };

// True when the secondary toolbar should be shown (shift held on applicable screens).
static int use_secondary_toolbar(void) {
    if (!(read_modifier_state() & MOD_SHIFT)) return 0;
    return screen_mode == SCREEN_GRAPH     || screen_mode == SCREEN_EQUATIONS
        || screen_mode == SCREEN_TABLE     || screen_mode == SCREEN_ZOOM
        || screen_mode == SCREEN_CALCULATE || screen_mode == SCREEN_3D;
}

// Returns the active bottom-toolbar box index for the current screen, or -1.
static int active_toolbar_box(void) {
    if (use_secondary_toolbar()) {
        if (screen_mode == SCREEN_TABLE)     return 0;
        if (screen_mode == SCREEN_ZOOM)      return 1;
        if (screen_mode == SCREEN_CALCULATE) return 2;
        if (screen_mode == SCREEN_3D)        return 3;
        return -1;
    }
    if (screen_mode == SCREEN_EQUATIONS) return 0;
    if (screen_mode == SCREEN_GRAPH)     return 1;
    if (screen_mode == SCREEN_APPS)      return 2;
    if (screen_mode == SCREEN_SETTINGS)  return 3;
    return -1;
}

static void draw_bottom_toolbar(void) {
    int sx, sy; lcd_get_xy(&sx, &sy);
    int box_w = scr_w / 4;   // 80px per box on a 320px screen
    int sep_y  = bottom_toolbar_y;
    int label_y = sep_y + 2;
    int active = active_toolbar_box();
    const char * const *labels = use_secondary_toolbar() ? fn2_labels : fn_labels;

    // Horizontal separator line (2px, mirrors top toolbar)
    lcd_fill_rect(0, sep_y, scr_w - 1, sep_y + 1, GREEN);

    // Draw each box
    for (int i = 0; i < 4; i++) {
        int x0 = i * box_w;
        int x1 = (i + 1) * box_w - 1;
        if (i == active) {
            lcd_fill_rect(x0, label_y, x1, label_y + fh - 1, GREEN);
            lcd_set_bg_colour(GREEN);
            lcd_set_fg_colour(BLACK);
        } else {
            lcd_fill_rect(x0, label_y, x1, label_y + fh - 1, BLACK);
            lcd_set_bg_colour(BLACK);
            lcd_set_fg_colour(GREEN);
        }
        int label_px = (int)strlen(labels[i]) * fw;
        int x = x0 + (box_w - label_px) / 2;
        lcd_set_xy(x, label_y);
        lcd_print_string((char *)labels[i]);
    }
    lcd_set_bg_colour(BLACK);
    lcd_set_fg_colour(GREEN);

    // Vertical separators between boxes (drawn last so they're on top)
    for (int i = 1; i < 4; i++) {
        lcd_fill_rect(i * box_w, sep_y, i * box_w, label_y + fh - 1, GREEN);
    }
    lcd_set_bg_colour(BLACK);
    lcd_set_fg_colour(WHITE);
    lcd_set_xy(sx, sy);
}

// ── Equations screen ──────────────────────────────────────────────────────────
static const int eq_row_colours[EQ_COUNT] = {
    CYAN, YELLOW, RGB(128, 0, 128), RED, GREEN, ORANGE, RGB(165, 42, 42), BLUE,
    RGB(255, 105, 180),   // pink
    RGB(128, 128, 128),   // grey
};

// LCD position at character column within the selected equation's input area.
static void eq_goto(int col) {
    lcd_set_xy((4 + col) * fw, (fh + 2) + eq_sel * fh);
}

// Redraw equation row's input area from column `from` onward.
static void eq_redraw_from(int row, int from, int clear_tail) {
    lcd_set_bg_colour(BLACK);
    lcd_set_fg_colour(WHITE);
    lcd_set_xy((4 + from) * fw, (fh + 2) + row * fh);
    for (int i = from; i < eq_len[row]; i++)
        lcd_putc(0, (uint8_t)eq_buf[row][i]);
    if (clear_tail) lcd_putc(0, ' ');
    lcd_set_fg_colour(WHITE);  // keep input fg white after redraw
}

static void eq_insert(char c) {
    if (eq_len[eq_sel] >= ncols - 4) return;
    int p = eq_cpos[eq_sel];
    memmove(&eq_buf[eq_sel][p + 1], &eq_buf[eq_sel][p], eq_len[eq_sel] - p);
    eq_buf[eq_sel][p] = c;
    eq_cpos[eq_sel]++;
    eq_len[eq_sel]++;
    eq_redraw_from(eq_sel, p, 0);
    eq_goto(eq_cpos[eq_sel]);
}

static void eq_backspace(void) {
    if (eq_cpos[eq_sel] == 0) return;
    int p = eq_cpos[eq_sel];
    memmove(&eq_buf[eq_sel][p - 1], &eq_buf[eq_sel][p], eq_len[eq_sel] - p);
    eq_cpos[eq_sel]--;
    eq_len[eq_sel]--;
    eq_redraw_from(eq_sel, eq_cpos[eq_sel], 1);
    eq_goto(eq_cpos[eq_sel]);
}

static void eq_delete(void) {
    int p = eq_cpos[eq_sel];
    if (p >= eq_len[eq_sel]) return;
    memmove(&eq_buf[eq_sel][p], &eq_buf[eq_sel][p + 1], eq_len[eq_sel] - p - 1);
    eq_len[eq_sel]--;
    eq_redraw_from(eq_sel, p, 1);
    eq_goto(p);
}

static void eq_nav_vertical(int dir) {
    int new_sel = eq_sel + dir;
    if (new_sel < 0 || new_sel >= EQ_COUNT) return;
    int col = eq_cpos[eq_sel];
    eq_sel = new_sel;
    if (col > eq_len[eq_sel]) col = eq_len[eq_sel];
    eq_cpos[eq_sel] = col;
    eq_goto(eq_cpos[eq_sel]);
}

static void draw_equations_screen(void) {
    lcd_clear_content();
    int content_top = fh + 2;  // same as toolbar_h
    for (int i = 0; i < EQ_COUNT; i++) {
        int row_y = content_top + i * fh;
        // Full row: black background
        lcd_fill_rect(0, row_y, scr_w - 1, row_y + fh - 1, BLACK);
        // "Y#" cell highlight (3 chars wide to fit Y10)
        lcd_fill_rect(0, row_y, 3 * fw - 1, row_y + fh - 1, eq_row_colours[i]);
        // Draw "Y#" left-justified in 3-char field — black text on colour
        lcd_set_bg_colour(eq_row_colours[i]);
        lcd_set_fg_colour(BLACK);
        lcd_set_xy(0, row_y);
        char yn[4];
        snprintf(yn, sizeof(yn), "Y%-2d", i + 1);
        lcd_print_string(yn);
        // Draw "=" — white text on black
        lcd_set_bg_colour(BLACK);
        lcd_set_fg_colour(WHITE);
        lcd_print_string("=");
        // Draw any existing equation content
        for (int j = 0; j < eq_len[i]; j++)
            lcd_putc(0, (uint8_t)eq_buf[i][j]);
    }
    lcd_set_bg_colour(BLACK);
    lcd_set_fg_colour(GREEN);
}

static void enter_equations(void) {
    screen_mode = SCREEN_EQUATIONS;
    eq_sel = 0;
    lcd_cursor_off();
    draw_equations_screen();
    draw_bottom_toolbar();
    lcd_set_fg_colour(WHITE);
    eq_goto(eq_cpos[eq_sel]);
    cursor_on();
}

// ── Graph screen ──────────────────────────────────────────────────────────────
static double graph_xmin = -10.0, graph_xmax = 10.0;
static double graph_ymin = -10.0, graph_ymax = 10.0;

// Map math x → pixel x, math y → pixel y within the content area.
static int gpx(double mx, int w) {
    return (int)((mx - graph_xmin) / (graph_xmax - graph_xmin) * (w - 1) + 0.5);
}
static int gpy(double my, int ct, int ch) {
    return ct + ch - 1 - (int)((my - graph_ymin) / (graph_ymax - graph_ymin) * (ch - 1) + 0.5);
}

static void draw_graph_screen(void) {
    int ct = fh + 2;                  // content top (same as toolbar_h)
    int ch = bottom_toolbar_y - ct;   // content height in pixels
    int w  = scr_w;

    lcd_clear_content();

    int ax = gpx(0.0, w);     // pixel x of y-axis
    int ay = gpy(0.0, ct, ch); // pixel y of x-axis

    // Tick marks at each integer on both axes (MYRTLE = subtle dark green)
    for (int i = (int)ceil(graph_xmin); i <= (int)floor(graph_xmax); i++) {
        int px = gpx((double)i, w);
        if (px >= 0 && px < w && ay >= ct && ay < ct + ch) {
            int t0 = ay - 2; if (t0 < ct) t0 = ct;
            int t1 = ay + 2; if (t1 >= ct + ch) t1 = ct + ch - 1;
            lcd_fill_rect(px, t0, px, t1, MYRTLE);
        }
    }
    for (int j = (int)ceil(graph_ymin); j <= (int)floor(graph_ymax); j++) {
        int py = gpy((double)j, ct, ch);
        if (py >= ct && py < ct + ch && ax >= 0 && ax < w) {
            int t0 = ax - 2; if (t0 < 0) t0 = 0;
            int t1 = ax + 2; if (t1 >= w) t1 = w - 1;
            lcd_fill_rect(t0, py, t1, py, MYRTLE);
        }
    }

    // Axes (GREEN, drawn over ticks)
    if (ay >= ct && ay < ct + ch) lcd_fill_rect(0, ay, w - 1, ay, GREEN);
    if (ax >= 0  && ax < w)       lcd_fill_rect(ax, ct, ax, ct + ch - 1, GREEN);

    // Plot each non-empty equation, connecting consecutive points vertically
    // so steep curves don't appear as sparse dots.
    // Temporarily borrow vars['x'-'a'] for the sweep; restore after.
    // Also clear any formula on x so the numeric sweep value is used directly.
    double complex saved_x = vars['x' - 'a'];
    char saved_xexpr[LINE_BUF_MAX];
    memcpy(saved_xexpr, var_expr['x' - 'a'], LINE_BUF_MAX);
    var_expr['x' - 'a'][0] = '\0';
    for (int eq = 0; eq < EQ_COUNT; eq++) {
        if (eq_len[eq] == 0) continue;
        eq_buf[eq][eq_len[eq]] = '\0';
        int colour = eq_row_colours[eq];
        int prev_py = -1;
        int prev_valid = 0;
        for (int px = 0; px < w; px++) {
            vars['x' - 'a'] = graph_xmin + (graph_xmax - graph_xmin) * px / (double)(w - 1);
            double complex y;
            if (!eval_expr(eq_buf[eq], &y)) { prev_valid = 0; continue; }
            int py = gpy(creal(y), ct, ch);
            if (prev_valid) {
                // Connect prev_py to py with a vertical span on column px-1..px
                int y0 = prev_py, y1 = py;
                if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
                // clamp to content area
                if (y0 < ct) y0 = ct;
                if (y1 >= ct + ch) y1 = ct + ch - 1;
                if (y0 <= y1)
                    lcd_fill_rect(px - 1, y0, px, y1, colour);
            } else if (py >= ct && py < ct + ch) {
                lcd_fill_rect(px, py, px, py, colour);
            }
            prev_py = py;
            prev_valid = 1;
        }
    }
    vars['x' - 'a'] = saved_x;
    memcpy(var_expr['x' - 'a'], saved_xexpr, LINE_BUF_MAX); // restore user's x formula
}

static void enter_graph(void) {
    screen_mode = SCREEN_GRAPH;
    lcd_cursor_off();
    draw_graph_screen();
    draw_bottom_toolbar();
    // No cursor on graph screen; leave cursor off.
}

// ── Settings persistence (last flash sector) ──────────────────────────────────
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SETTINGS_MAGIC        0x4F43534E   // bump version when struct changes

typedef struct {
    uint32_t magic;
    int      sel[5];
    char     name[CALC_NAME_MAX];
} settings_data_t;

static void settings_save(const int sel[5]) {
    uint8_t buf[FLASH_PAGE_SIZE];
    memset(buf, 0xff, sizeof(buf));
    settings_data_t *d = (settings_data_t *)buf;
    d->magic = SETTINGS_MAGIC;
    memcpy(d->sel, sel, 5 * sizeof(int));
    memcpy(d->name, calc_name, CALC_NAME_MAX);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

static void settings_load(int sel[5]) {
    const settings_data_t *d =
        (const settings_data_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
    if (d->magic == SETTINGS_MAGIC) {
        memcpy(sel, d->sel, 5 * sizeof(int));
        memcpy(calc_name, d->name, CALC_NAME_MAX);
        calc_name[CALC_NAME_MAX - 1] = '\0';
    }
}

// ── Settings screen ───────────────────────────────────────────────────────────
static const char * const srow0[] = { "Normal", "Sci", "Eng" };
static const char * const srow1[] = { "Float","0","1","2","3","4","5","6","7","8","9" };
static const char * const srow2[] = { "Radian", "Degree" };
static const char * const srow3[] = { "Function", "Parametric", "Polar", "Seq" };
static const char * const srow4[] = { "Standard", "Reverse Polish Notation" };
static const char * const *settings_rows[] = { srow0, srow1, srow2, srow3, srow4 };
static const int settings_row_count[] = { 3, 11, 2, 4, 2 };

static int settings_row = 0;
static int settings_col = 0;

// y pixel of settings row r in content area
static int settings_row_y(int r) {
    return (fh + 2) + fh + r * (2 * fh);
}

// x pixel of option text start for (row, col)
static int settings_opt_x(int row, int col) {
    int x = 4;
    for (int i = 0; i < col; i++)
        x += (int)strlen(settings_rows[row][i]) * fw + 8;
    return x;
}

// Draw one option cell (committed = GREEN text / BLACK bg, else WHITE text / BLACK bg)
static void settings_draw_opt(int row, int col) {
    int y   = settings_row_y(row);
    int xt  = settings_opt_x(row, col);
    int optw = (int)strlen(settings_rows[row][col]) * fw;
    int x0 = xt - 2,  x1 = xt + optw + 1;
    int y0 = y - 1,   y1 = y + fh;
    lcd_fill_rect(x0, y0, x1, y1, BLACK);
    lcd_set_bg_colour(BLACK);
    lcd_set_fg_colour(settings_sel[row] == col ? GREEN : WHITE);
    lcd_set_xy(xt, y);
    lcd_print_string((char *)settings_rows[row][col]);
    lcd_set_bg_colour(BLACK);
    lcd_set_fg_colour(WHITE);
}

// Draw (visible=1) or erase (visible=0) the YELLOW highlight cursor.
static void settings_cursor_draw(int visible) {
    int row = settings_row, col = settings_col;
    int y   = settings_row_y(row);
    int xt  = settings_opt_x(row, col);
    int optw = (int)strlen(settings_rows[row][col]) * fw;
    int x0 = xt - 2,  x1 = xt + optw + 1;
    int y0 = y - 1,   y1 = y + fh;
    if (visible) {
        lcd_fill_rect(x0, y0, x1, y1, YELLOW);
        lcd_set_bg_colour(YELLOW);
        lcd_set_fg_colour(BLACK);
        lcd_set_xy(xt, y);
        lcd_print_string((char *)settings_rows[row][col]);
        lcd_set_bg_colour(BLACK);
        lcd_set_fg_colour(WHITE);
    } else {
        settings_draw_opt(row, col);             // repaint cell without highlight
    }
}

// Move the settings cursor by (drow, dcol), erasing old and drawing new.
static void settings_nav(int drow, int dcol) {
    settings_cursor_draw(0);
    if (drow != 0) {
        int nr = settings_row + drow;
        if (nr < 0 || nr >= 5) { settings_cursor_draw(1); return; }
        settings_row = nr;
        if (settings_col >= settings_row_count[settings_row])
            settings_col = settings_row_count[settings_row] - 1;
    } else {
        int nc = settings_col + dcol;
        if (nc < 0 || nc >= settings_row_count[settings_row]) { settings_cursor_draw(1); return; }
        settings_col = nc;
    }
    settings_cursor_draw(1);
}

static void draw_settings_screen(void) {
    lcd_clear_content();
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < settings_row_count[r]; c++)
            settings_draw_opt(r, c);
    lcd_set_bg_colour(BLACK);
    lcd_set_fg_colour(GREEN);
}

static void enter_settings(void) {
    screen_mode = SCREEN_SETTINGS;
    lcd_cursor_off();
    draw_settings_screen();
    draw_bottom_toolbar();
    settings_cursor_draw(1);
}

// ── Apps screen ───────────────────────────────────────────────────────────────
static const char * const app_names[] = { "TestApp1", "TestApp2", "TestApp3" };
#define APP_COUNT 3
static int apps_sel = 0;

static void apps_draw_row(int i) {
    int y = (fh + 2) + fh + i * (fh * 2);
    int is_sel = (i == apps_sel);
    int x0 = fw * 2;
    int x1 = x0 + (int)strlen(app_names[i]) * fw - 1;
    // Clear the full row to background first, then draw highlight only around the name
    lcd_fill_rect(0, y - 1, scr_w - 1, y + fh, BLACK);
    if (is_sel) lcd_fill_rect(x0 - 2, y - 1, x1 + 2, y + fh, YELLOW);
    lcd_set_fg_colour(is_sel ? BLACK : WHITE);
    lcd_set_bg_colour(is_sel ? YELLOW : BLACK);
    lcd_set_xy(x0, y);
    lcd_print_string((char *)app_names[i]);
    lcd_set_fg_colour(WHITE);
    lcd_set_bg_colour(BLACK);
}

static void draw_apps_screen(void) {
    lcd_clear_content();
    for (int i = 0; i < APP_COUNT; i++)
        apps_draw_row(i);
}

static void enter_apps(void) {
    screen_mode = SCREEN_APPS;
    lcd_cursor_off();
    draw_apps_screen();
    draw_bottom_toolbar();
}

static void apps_nav(int dir) {
    int old = apps_sel;
    apps_sel += dir;
    if (apps_sel < 0) apps_sel = 0;
    if (apps_sel >= APP_COUNT) apps_sel = APP_COUNT - 1;
    if (apps_sel == old) return;
    apps_draw_row(old);
    apps_draw_row(apps_sel);
}

// ── CLI ───────────────────────────────────────────────────────────────────────
#define PROMPT "> "

static void print_prompt(void) {
    lcd_set_fg_colour(GREEN);
    lcd_print_string(PROMPT);
    lcd_get_xy(&line_start_x, &line_start_y);
    lcd_set_fg_colour(WHITE);
}

static void enter_secondary(screen_mode_t mode, const char *name) {
    screen_mode = mode;
    lcd_cursor_off();
    lcd_clear_content();
    lcd_set_fg_colour(WHITE);
    lcd_set_xy(fw, fh + 2 + fh * 2);
    lcd_print_string((char *)name);
    draw_bottom_toolbar();
}

static void enter_home(void) {
    screen_mode = SCREEN_HOME;
    line_len = 0;
    cursor_pos = 0;
    history_pos = -1;
    lcd_clear_content();
    print_prompt();
    draw_bottom_toolbar();
    lcd_set_fg_colour(WHITE);  // restore input fg after toolbar draw
    line_goto(cursor_pos);
    cursor_on();
}

static void print_right_col(const char *s, int colour) {
    int slen = (int)strlen(s);
    int x, y;
    lcd_get_xy(&x, &y);
    int rx = (ncols - slen) * fw;
    lcd_set_fg_colour(colour);
    lcd_set_xy(rx < 0 ? 0 : rx, y);
    lcd_print_string((char *)s);
    lcd_putc(0, '\n');
    lcd_set_fg_colour(WHITE);
}
static void print_right(const char *s) { print_right_col(s, YELLOW); }
static void print_ok(const char *s)    { print_right_col(s, GREEN);  }
static void print_err(const char *s)   { print_right_col(s, RED);    }

static void format_eng(char *out, int out_sz, double val, int dp) {
    if (!isfinite(val)) { snprintf(out, out_sz, "?"); return; }
    if (val == 0.0) { snprintf(out, out_sz, "%.*fE+0", dp, 0.0); return; }
    double av = fabs(val);
    int e  = (int)floor(log10(av));
    int ee = e - ((e % 3 + 3) % 3);   // round down to multiple of 3
    double m = val / pow(10.0, ee);
    // Fix rounding edge cases
    if (fabs(m) >= 999.5) { ee += 3; m /= 1000.0; }
    if (fabs(m) <   0.999 && fabs(m) > 0) { ee -= 3; m *= 1000.0; }
    snprintf(out, out_sz, "%.*fE%+d", dp, m, ee);
}

static void format_real(char *out, int out_sz, double val) {
    int fmt = settings_sel[0]; // 0=Normal, 1=Sci, 2=Eng
    int dec = settings_sel[1]; // 0=Float, 1=0dp, 2=1dp, …, 10=9dp
    int dp  = (dec == 0) ? -1 : dec - 1;  // -1 = auto
    if (fmt == 0 && dp < 0) {
        // Normal Float: show as integer when exact
        long long iv = (long long)val;
        if ((double)iv == val && val > -9e15 && val < 9e15)
            { snprintf(out, out_sz, "%lld", iv); return; }
        snprintf(out, out_sz, "%.10g", val);
    } else if (fmt == 0) {
        snprintf(out, out_sz, "%.*f", dp, val);       // Normal Fixed
    } else if (fmt == 1 && dp < 0) {
        snprintf(out, out_sz, "%.9E", val);            // Sci Float
    } else if (fmt == 1) {
        snprintf(out, out_sz, "%.*E", dp, val);        // Sci Fixed
    } else {
        format_eng(out, out_sz, val, dp < 0 ? 6 : dp); // Eng
    }
}

static void format_result(char *out, int out_sz, double complex val) {
    double re = creal(val);
    double im = cimag(val);
    if (im == 0.0) {
        format_real(out, out_sz, re);
        return;
    }
    char re_s[32], im_s[32];
    format_real(re_s, sizeof(re_s), re);
    format_real(im_s, sizeof(im_s), fabs(im));
    if (im >= 0.0)
        snprintf(out, out_sz, "%s+%si", re_s, im_s);
    else
        snprintf(out, out_sz, "%s-%si", re_s, im_s);
}

// ── RPN (postfix) evaluator ───────────────────────────────────────────────────
#define RPN_STACK_MAX 32

static int rpn_push(double complex *stack, int *sp, double complex v) {
    if (*sp >= RPN_STACK_MAX) return 0;
    stack[(*sp)++] = v;
    return 1;
}

static int exec_rpn(const char *buf) {
    double complex stack[RPN_STACK_MAX];
    int sp = 0;
    const char *p = buf;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        char tok[32];
        int tlen = 0;
        while (*p && *p != ' ' && tlen < 31) tok[tlen++] = *p++;
        tok[tlen] = '\0';

        // Number literal
        char *endp;
        double val = strtod(tok, &endp);
        if (endp != tok && *endp == '\0') {
            if (!rpn_push(stack, &sp, val)) return 0;
            continue;
        }

        // Named constants / ans / variables
        if (!strcmp(tok, "pi"))  { if (!rpn_push(stack, &sp, M_PI)) return 0; continue; }
        if (!strcmp(tok, "e"))   { if (!rpn_push(stack, &sp, M_E))  return 0; continue; }
        if (!strcmp(tok, "i"))   { if (!rpn_push(stack, &sp, I))    return 0; continue; }
        if (!strcmp(tok, "ans")) { if (!rpn_push(stack, &sp, ans))  return 0; continue; }
        if (tlen == 1 && tok[0] >= 'a' && tok[0] <= 'z') {
            if (!rpn_push(stack, &sp, vars[(unsigned char)tok[0] - 'a'])) return 0;
            continue;
        }

        // Binary operators
        if (tlen == 1 && sp >= 2) {
            double complex b = stack[--sp], a = stack[--sp];
            double complex r;
            int ok = 1;
            switch (tok[0]) {
                case '+': r = a + b; break;
                case '-': r = a - b; break;
                case '*': r = a * b; break;
                case '/': if (cabs(b) == 0.0) return 0; r = a / b; break;
                case '^': r = cpow(a, b); break;
                default: ok = 0;
            }
            if (ok) { stack[sp++] = r; continue; }
            // not a binary op, fall through
            stack[sp++] = a; stack[sp++] = b;
        }

        // Unary operators / functions
        if (!strcmp(tok, "neg"))  { if (sp < 1) return 0; stack[sp-1] = -stack[sp-1]; continue; }
        if (!strcmp(tok, "abs"))  { if (sp < 1) return 0; stack[sp-1] = cabs(stack[sp-1]); continue; }
        if (!strcmp(tok, "sqrt")) { if (sp < 1) return 0; stack[sp-1] = csqrt(stack[sp-1]); continue; }
        if (!strcmp(tok, "cbrt")) { if (sp < 1) return 0; stack[sp-1] = cpow(stack[sp-1], 1.0/3.0); continue; }
        if (!strcmp(tok, "sin"))  { if (sp < 1) return 0; stack[sp-1] = csin(stack[sp-1]); continue; }
        if (!strcmp(tok, "cos"))  { if (sp < 1) return 0; stack[sp-1] = ccos(stack[sp-1]); continue; }
        if (!strcmp(tok, "tan"))  { if (sp < 1) return 0; stack[sp-1] = ctan(stack[sp-1]); continue; }
        if (!strcmp(tok, "asin")) { if (sp < 1) return 0; stack[sp-1] = casin(stack[sp-1]); continue; }
        if (!strcmp(tok, "acos")) { if (sp < 1) return 0; stack[sp-1] = cacos(stack[sp-1]); continue; }
        if (!strcmp(tok, "atan")) { if (sp < 1) return 0; stack[sp-1] = catan(stack[sp-1]); continue; }
        if (!strcmp(tok, "sinh")) { if (sp < 1) return 0; stack[sp-1] = csinh(stack[sp-1]); continue; }
        if (!strcmp(tok, "cosh")) { if (sp < 1) return 0; stack[sp-1] = ccosh(stack[sp-1]); continue; }
        if (!strcmp(tok, "tanh")) { if (sp < 1) return 0; stack[sp-1] = ctanh(stack[sp-1]); continue; }
        if (!strcmp(tok, "log"))  { if (sp < 1) return 0; stack[sp-1] = clog(stack[sp-1]) / log(10.0); continue; }
        if (!strcmp(tok, "ln"))   { if (sp < 1) return 0; stack[sp-1] = clog(stack[sp-1]); continue; }
        if (!strcmp(tok, "exp"))  { if (sp < 1) return 0; stack[sp-1] = cexp(stack[sp-1]); continue; }

        // Unknown token
        return 0;
    }

    if (sp != 1) return 0;
    ans = stack[0];
    char out[64];
    format_result(out, sizeof(out), stack[0]);
    print_right(out);
    return 1;
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
        char bat[16];
        snprintf(bat, sizeof(bat), "%d", read_battery());
        print_right(bat);
        return;
    }
    if (strcmp(buf, "ver") == 0) {
        print_right(APP_VERSION);
        return;
    }
    if (strcmp(buf, "cle") == 0) {
        memset(vars, 0, sizeof(vars));
        memset(var_expr, 0, sizeof(var_expr));
        ans = 0.0;
        print_ok("ok");
        return;
    }
    if (strcmp(buf, "name") == 0) {
        print_right(calc_name[0] ? calc_name : "OpenCalc v" APP_VERSION);
        return;
    }
    if (strncmp(buf, "name(", 5) == 0 && buf[len - 1] == ')') {
        int nlen = len - 6; // chars between the parens
        if (nlen <= 0) {
            calc_name[0] = '\0';
        } else {
            int copy = nlen < CALC_NAME_MAX - 1 ? nlen : CALC_NAME_MAX - 1;
            memcpy(calc_name, buf + 5, copy);
            calc_name[copy] = '\0';
        }
        settings_save(settings_sel);
        draw_toolbar();
        print_ok("ok");
        return;
    }

    // Variable assignment: letter=expr (e, i are read-only constants)
    if (isalpha((unsigned char)buf[0]) && buf[1] == '=') {
        char v = buf[0];
        if (v == 'e' || v == 'i') { print_err("constant"); return; }
        const char *rhs_str = buf + 2;
        // Store expression string for deferred evaluation
        strncpy(var_expr[v - 'a'], rhs_str, LINE_BUF_MAX - 1);
        var_expr[v - 'a'][LINE_BUF_MAX - 1] = '\0';
        vars[v - 'a'] = 0; // clear cached numeric value
        print_right(rhs_str);
        return;
    }

    if (settings_sel[4] == 1) {
        // RPN mode: postfix evaluation
        cycle_detected = 0;
        if (!exec_rpn(buf)) print_err(cycle_detected ? "cycle" : "?");
        return;
    }

    double complex result;
    cycle_detected = 0;
    if (eval_expr(buf, &result)) {
        ans = result;
        char out[64];
        format_result(out, sizeof(out), result);
        print_right(out);
    } else {
        print_err(cycle_detected ? "cycle" : "?");
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

// ── Block cursor helpers ───────────────────────────────────────────────────────
// Returns the character currently under the text cursor.
static char get_cursor_char(void) {
    if (screen_mode == SCREEN_HOME)
        return (cursor_pos < line_len) ? line_buf[cursor_pos] : ' ';
    if (screen_mode == SCREEN_EQUATIONS)
        return (eq_cpos[eq_sel] < eq_len[eq_sel]) ? eq_buf[eq_sel][eq_cpos[eq_sel]] : ' ';
    return ' ';
}

// Turn cursor on, selecting shape/color based on modifier state.
static void cursor_on(void) {
    uint8_t mods = read_modifier_state();
    char restore = get_cursor_char();
    if (mods & MOD_CTRL) {
        lcd_set_fg_colour(WHITE);
        lcd_cursor_outline(restore);
    } else {
        int col = (mods & MOD_ALT) ? YELLOW : (mods & MOD_SHIFT) ? GREEN : WHITE;
        lcd_set_fg_colour(col);
        lcd_cursor_block(caps_on ? '^' : 0, restore);
    }
    lcd_cursor_on();
    lcd_set_fg_colour(WHITE);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();
    init_i2c_kbd();
    lcd_init();
    lcd_get_metrics(&fw, &fh, &ncols);
    lcd_get_size(&scr_w, &scr_h);

    settings_load(settings_sel);

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
    cursor_on();
    int cursor_state = 1;
    uint64_t last_blink = time_us_64();

    while (1) {
        if (time_us_64() - last_blink >= 500000) {  // 500ms blink
            if (screen_mode == SCREEN_SETTINGS || screen_mode == SCREEN_APPS
             || screen_mode == SCREEN_TABLE    || screen_mode == SCREEN_ZOOM
             || screen_mode == SCREEN_CALCULATE|| screen_mode == SCREEN_3D) {
                // no blink on these screens
            } else if (screen_mode != SCREEN_GRAPH) {
                if (cursor_state) lcd_cursor_off();
                else              cursor_on();
                cursor_state ^= 1;
            }
            last_blink = time_us_64();
        }

        int c = read_i2c_kbd();
        if (c == KEY_MOD_CHANGED) {
            uint8_t mod = read_modifier_state();
            static uint8_t last_mod = 0;
            if (mod != last_mod) {
                last_mod = mod;
                draw_toolbar();
                // Redraw bottom toolbar when shift toggles on applicable screens
                if (screen_mode == SCREEN_GRAPH     || screen_mode == SCREEN_EQUATIONS
                 || screen_mode == SCREEN_TABLE     || screen_mode == SCREEN_ZOOM
                 || screen_mode == SCREEN_CALCULATE || screen_mode == SCREEN_3D)
                    draw_bottom_toolbar();
            }
        } else if (c == KEY_BOOTSEL) {
            reset_usb_boot(0, 0);
        } else if (c == KEY_REBOOT) {
            watchdog_reboot(0, 0, 0);
        } else if (c == KEY_ESC) {
            if (screen_mode != SCREEN_HOME) {
                enter_home();
                cursor_state = 1;
                last_blink = time_us_64();
            }
        } else if (c == KEY_F1) {
            if (screen_mode == SCREEN_EQUATIONS) enter_home();
            else enter_equations();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_F2) {
            if (screen_mode == SCREEN_GRAPH) enter_home();
            else enter_graph();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_F3) {
            if (screen_mode == SCREEN_APPS) enter_home();
            else enter_apps();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_F4) {
            if (screen_mode == SCREEN_SETTINGS) enter_home();
            else enter_settings();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_F6 || c == KEY_F7 || c == KEY_F8 || c == KEY_F9) {
            if (use_secondary_toolbar()) {
                if      (c == KEY_F6) enter_secondary(SCREEN_TABLE,     "Table");
                else if (c == KEY_F7) enter_secondary(SCREEN_ZOOM,      "Zoom");
                else if (c == KEY_F8) enter_secondary(SCREEN_CALCULATE, "Calculate");
                else                  enter_secondary(SCREEN_3D,        "3D");
                cursor_state = 1; last_blink = time_us_64();
            }
        } else if (c == KEY_UP || c == KEY_DOWN) {
            if (screen_mode == SCREEN_SETTINGS) {
                settings_nav(c == KEY_UP ? -1 : 1, 0);
            } else if (screen_mode == SCREEN_APPS) {
                apps_nav(c == KEY_UP ? -1 : 1);
            } else {
                lcd_cursor_off();
                if (screen_mode == SCREEN_HOME) {
                    ac_active = 0;
                    history_navigate(c == KEY_UP ? 1 : -1);
                }
                else if (screen_mode == SCREEN_EQUATIONS)
                    eq_nav_vertical(c == KEY_UP ? -1 : 1);
                cursor_on();
            }
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_LEFT || c == KEY_RIGHT || c == KEY_HOME || c == KEY_END) {
            if (screen_mode == SCREEN_SETTINGS) {
                if (c == KEY_LEFT)  settings_nav(0, -1);
                else if (c == KEY_RIGHT) settings_nav(0,  1);
                // HOME/END: ignore on settings
            } else {
                lcd_cursor_off();
                if (screen_mode == SCREEN_HOME) {
                    ac_active = 0;
                    if      (c == KEY_LEFT)  input_move_left();
                    else if (c == KEY_RIGHT) input_move_right();
                    else if (c == KEY_HOME)  input_home();
                    else                     input_end();
                } else if (screen_mode == SCREEN_EQUATIONS) {
                    if (c == KEY_LEFT) {
                        if (eq_cpos[eq_sel] > 0) { eq_cpos[eq_sel]--; eq_goto(eq_cpos[eq_sel]); }
                    } else if (c == KEY_RIGHT) {
                        if (eq_cpos[eq_sel] < eq_len[eq_sel]) { eq_cpos[eq_sel]++; eq_goto(eq_cpos[eq_sel]); }
                    } else if (c == KEY_HOME) {
                        eq_cpos[eq_sel] = 0; eq_goto(0);
                    } else {
                        eq_cpos[eq_sel] = eq_len[eq_sel]; eq_goto(eq_len[eq_sel]);
                    }
                }
                cursor_on();
            }
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_CAPS_TOGGLE) {
            lcd_cursor_off();
            caps_on ^= 1;
            cursor_on();
            cursor_state = 1;
            last_blink = time_us_64();
        } else if (c == KEY_DEL) {
            if (screen_mode != SCREEN_SETTINGS) {
                lcd_cursor_off();
                if (screen_mode == SCREEN_HOME) { ac_active = 0; input_delete(); }
                else if (screen_mode == SCREEN_EQUATIONS) eq_delete();
                cursor_on();
                cursor_state = 1;
                last_blink = time_us_64();
            }
        } else if (c == KEY_TAB) {
            if (screen_mode == SCREEN_HOME) {
                lcd_cursor_off();
                do_autocomplete();
                cursor_on();
                cursor_state = 1;
                last_blink = time_us_64();
            }
        } else if (c > 0) {
            if (screen_mode == SCREEN_SETTINGS) {
                if (c == '\r' || c == '\n') {
                    // Select the current option for this row
                    int old = settings_sel[settings_row];
                    settings_sel[settings_row] = settings_col;
                    settings_cursor_draw(0);
                    settings_draw_opt(settings_row, old);
                    settings_draw_opt(settings_row, settings_col);
                    settings_cursor_draw(1);
                    settings_save(settings_sel);
                    draw_toolbar(); // refresh top toolbar (e.g. RAD/DEG indicator)
                }
                // All other keypresses ignored on settings screen
            } else {
                lcd_cursor_off();
                if (c == 3 || c == 24) {
                    // Copy the whole current line/equation to clipboard
                    if (screen_mode == SCREEN_HOME) {
                        ac_active = 0;
                        clipboard_len = line_len;
                        memcpy(clipboard, line_buf, line_len);
                    } else if (screen_mode == SCREEN_EQUATIONS) {
                        clipboard_len = eq_len[eq_sel];
                        memcpy(clipboard, eq_buf[eq_sel], clipboard_len);
                    }
                        if (c == 24) {
                        // Cut: also clear the buffer
                        if (screen_mode == SCREEN_HOME)
                            line_replace("");
                        else if (screen_mode == SCREEN_EQUATIONS) {
                            eq_len[eq_sel] = 0; eq_cpos[eq_sel] = 0;
                            eq_redraw_from(eq_sel, 0, 1);
                            eq_goto(0);
                        }
                    }
                } else if (c == 22) {
                    // Paste at cursor
                    if (clipboard_len > 0) {
                        if (screen_mode == SCREEN_HOME) {
                            history_pos = -1;
                            ac_active = 0;
                            input_insert_str(clipboard, clipboard_len);
                        } else if (screen_mode == SCREEN_EQUATIONS) {
                            eq_insert_str(clipboard, clipboard_len);
                        }
                    }
                } else if (screen_mode == SCREEN_HOME) {
                    if      (c == '\b')              { history_pos = -1; ac_active = 0; input_backspace(); }
                    else if (c == '\r' || c == '\n') { ac_active = 0; input_newline(); }
                    else                             { history_pos = -1; ac_active = 0; input_insert((char)c); }
                } else if (screen_mode == SCREEN_EQUATIONS) {
                    if      (c == '\b') eq_backspace();
                    else if (c == '\r' || c == '\n') { /* enter: no-op for now */ }
                    else                             eq_insert((char)c);
                }
                cursor_on();
            }
            cursor_state = 1;
            last_blink = time_us_64();
        }
    }
#endif
}
