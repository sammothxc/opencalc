#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "i2ckbd.h"
#include "lcdspi.h"

const uint LEDPIN = 25;
bool led = true;
char buffer[10];

int main() {
    set_sys_clock_khz(133000, true);
    stdio_init_all();
    init_i2c_kbd();
    lcd_init();

    lcd_clear();
    lcd_print_string("OpenCalc birth\n");
    while (1) {
        snprintf(buffer, sizeof(buffer), "%u", read_battery());
        lcd_print_string(buffer);
        lcd_print_string("\n");
        sleep_ms(3000);
    }
}
