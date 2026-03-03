#include <stdio.h>
#include <pico/stdio.h>
#include "i2ckbd.h"

static uint8_t i2c_inited = 0;

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
    static int ctrlheld = 0;
    static int altheld = 0;
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
            ctrlheld = ((buff & 0xff) != 0x03); // 0xA501/A502=held, 0xA503=released
        } else if ((buff >> 8) == 0xA1) {
            altheld = ((buff & 0xff) != 0x03);  // 0xA101/A102=held, 0xA103=released
        } else if ((buff & 0xff) == 1) {//pressed
            c = buff >> 8;
            if (c == 0xD4 && ctrlheld && altheld) return KEY_REBOOT;
            if (c == 0x46 && ctrlheld && altheld) return KEY_BOOTSEL;
            if (c == 0xB4) return KEY_LEFT;
            if (c == 0xB5) return KEY_UP;
            if (c == 0xB6) return KEY_DOWN;
            if (c == 0xB7) return KEY_RIGHT;
            if (c == 0xA2 || c == 0xA3 || c == 0xC1) return -1; // shift/capslock: suppress
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