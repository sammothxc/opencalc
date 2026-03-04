#ifndef I2C_KEYBOARD_H
#define I2C_KEYBOARD_H
#include <pico/stdlib.h>
#include <pico/platform.h>
#include <hardware/gpio.h>
#include <hardware/i2c.h>

#define I2C_KBD_MOD i2c1
#define I2C_KBD_SDA 6
#define I2C_KBD_SCL 7

#define I2C_KBD_SPEED  10000 // if dual i2c, then the speed of keyboard i2c should be 10khz

#define I2C_KBD_ADDR 0x1F

#define KEY_REBOOT   (-2)
#define KEY_BOOTSEL  (-3)
#define KEY_LEFT     (-4)
#define KEY_RIGHT    (-5)
#define KEY_UP       (-6)
#define KEY_DOWN     (-7)
#define KEY_HOME     (-8)
#define KEY_END      (-9)
#define KEY_TAB      (-10)
#define KEY_DEL          (-11)
#define KEY_CAPS_TOGGLE  (-12)
#define KEY_F1           (-13)
#define KEY_F2           (-14)
#define KEY_F3           (-15)
#define KEY_F4           (-16)
#define KEY_MOD_CHANGED  (-17)

#define MOD_SHIFT  0x01
#define MOD_ALT    0x02
#define MOD_CTRL   0x04

void init_i2c_kbd();
int read_i2c_kbd();
uint16_t read_i2c_kbd_raw();
int read_battery();
uint8_t read_modifier_state(void);

#endif