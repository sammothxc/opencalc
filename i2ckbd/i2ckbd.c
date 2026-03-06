#include <stdio.h>
#include <pico/stdio.h>
#include "i2ckbd.h"

static uint8_t i2c_inited = 0;
static int ctrlheld  = 0;
static int altheld   = 0;
static int shiftheld = 0;

uint8_t read_modifier_state(void) {
    return (shiftheld ? MOD_SHIFT : 0)
         | (altheld   ? MOD_ALT   : 0)
         | (ctrlheld  ? MOD_CTRL  : 0);
}

void init_i2c_kbd() {
    gpio_set_function(I2C_KBD_SCL, GPIO_FUNC_I2C);
    gpio_set_function(I2C_KBD_SDA, GPIO_FUNC_I2C);
    i2c_init(I2C_KBD_MOD, I2C_KBD_SPEED);
    gpio_pull_up(I2C_KBD_SCL);
    gpio_pull_up(I2C_KBD_SDA);

    i2c_inited = 1;
}

int read_i2c_kbd() {
    int retval;
    uint16_t buff = 0;
    unsigned char msg[2];
    int c = -1;
    msg[0] = 0x09;

    if (i2c_inited == 0) return -1;

    retval = i2c_write_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, msg, 1, false, 500000);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("read_i2c_kbd i2c write error\n");
        return -1;
    }

    sleep_ms(16);
    retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, (unsigned char *) &buff, 2, false, 500000);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("read_i2c_kbd i2c read error read\n");
        return -1;
    }

    if (buff != 0) {
        if ((buff >> 8) == 0xA5) {
            ctrlheld  = ((buff & 0xff) != 0x03); // 0xA501/A502=held, 0xA503=released
            return KEY_MOD_CHANGED;
        } else if ((buff >> 8) == 0xA1) {
            altheld   = ((buff & 0xff) != 0x03); // 0xA101/A102=held, 0xA103=released
            return KEY_MOD_CHANGED;
        } else if ((buff >> 8) == 0xA2 || (buff >> 8) == 0xA3) {
            shiftheld = ((buff & 0xff) != 0x03); // shift press/release
            return KEY_MOD_CHANGED;
        } else if ((buff & 0xff) == 1) {//pressed
            c = buff >> 8;
            if (c == 0xD4 && ctrlheld && altheld) return KEY_REBOOT;
            if (c == 0x46 && ctrlheld && altheld) return KEY_BOOTSEL;
            if (c == 0xB4) return KEY_LEFT;
            if (c == 0xB5) return KEY_UP;
            if (c == 0xB6) return KEY_DOWN;
            if (c == 0xB7) return KEY_RIGHT;
            if (c == 0xD2) return KEY_HOME;  // Shift+Tab
            if (c == 0xD5) return KEY_END;   // Shift+Del
            if (c == 0x09) return KEY_TAB;   // Tab (reserved)
            // Suppress: Shift/CapsLock, F1-F9, F10, Esc, BRK, plain Del
            if (c == 0xA2 || c == 0xA3) return -1;
            if (c == 0xC1) return KEY_CAPS_TOGGLE;
            if (c == 0x81) return KEY_F1;
            if (c == 0x82) return KEY_F2;
            if (c == 0x83) return KEY_F3;
            if (c == 0x84) return KEY_F4;
            if ((c >= 0x85 && c <= 0x89) || c == 0x90) return -1; // F5-F10
            if (c == 0xB1 || c == 0xD0) return -1;                 // Esc, BRK
            if (c == 0xD4) return KEY_DEL;                          // Del alone
            int realc = -1;
            switch (c) {
                default:
                    realc = c;
                    break;
            }
            c = realc;
            if (c >= 'a' && c <= 'z' && ctrlheld)c = c - 'a' + 1;
        }
        return c;
    }
    return -1;
}

uint16_t read_i2c_kbd_raw() {
    int retval;
    uint16_t buff = 0;
    unsigned char msg[1];
    msg[0] = 0x09;

    if (i2c_inited == 0) return 0;

    retval = i2c_write_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, msg, 1, false, 500000);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) return 0;

    sleep_ms(16);
    retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, (unsigned char *)&buff, 2, false, 500000);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) return 0;

    return buff;
}

int read_battery() {
    int retval;
    uint16_t buff = 0;
    unsigned char msg[2];
    msg[0] = 0x0b;

    if (i2c_inited == 0) return -1;

    retval = i2c_write_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, msg, 1, false, 500000);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("read_battery i2c write error\n");
        return -1;
    }
    sleep_ms(16);
    retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, (unsigned char *) &buff, 2, false, 500000);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf("read_battery i2c read error read\n");
        return -1;
    }

    if (buff != 0) {
        return buff;
    }
    return -1;
}