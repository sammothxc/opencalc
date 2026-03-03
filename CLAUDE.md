# PicoCalc Custom Firmware

## Project Goal
Custom firmware for the ClockworkPi PicoCalc. Ultimate goal: TI-BASIC interpreter
with autocomplete, parameter placeholders, and context-sensitive help.
Current phase: minimal typewriter (type → display characters).

## Hardware (ClockworkPi v2.0 + Pico 2W)
- **MCU**: RP2350 (Pico 2W), dual Cortex-M33 @ 150MHz
- **Display**: 320×320 4" IPS, ST7789-compatible (ST7365P), SPI0
  - SCK=18, MOSI=19, CS=17, DC=20, RST=21
  - RGB565, big-endian over SPI
- **Keyboard**: STM32F103R8T6 south-bridge, I2C1 @ 0x1F
  - SDA=6, SCL=7, 10kHz
  - FIFO register 0x09, key data register 0x04
  - Backlight registers: kbd=0x05, lcd=0x0A
  - Write flag: bit 7 of register byte
- **RAM**: 520KB on-chip + 8MB PSRAM onboard
- **Flash**: 4MB + SD card (FAT32)
- **Audio**: PWM on pins 26 (L) and 27 (R)
- **Battery**: 18650 Li-ion, status via I2C register 0x0B

## Build
- Pico SDK + CMake, board = pico2_w
- Build: `cd build && cmake -DPICO_BOARD=pico2_w .. && make -j`
- Flash: Ctrl+B on PicoCalc keyboard → BOOTSEL → drag .uf2
  (picotool -f doesn't work on Windows)

## Architecture (current)
```
main.c              Entry point, main loop, key dispatch
lcdspi/lcdspi.c     ST7789 SPI driver + 6×8 bitmap font
i2ckbd/i2ckbd.c     STM32 keyboard I2C driver
```

## Architecture (planned, build incrementally)
```
UI Layer (REPL, Editor, Graph, Popups, Input Engine)
    ↓
BASIC Engine (Lexer → Parser → AST → Evaluator)
    ↓
Function Registry + Symbol Table + Variable Store
    ↓
HAL (Display, Keyboard, SD, Audio, PSRAM, Wi-Fi)
```

## Coding Conventions
- C11, no C++
- Optimize for size (-Os), must fit in 4MB flash
- Prefer stack allocation over heap where possible
- PSRAM for large buffers (framebuffer, AST nodes, variable store)
- 6×8 font = 53 columns × 40 rows on 320×320 display

## Key Design Decisions
- Function registry: each function has name, signature, params with
  help text, examples, category. Powers autocomplete + help popups.
- Autocomplete: trie-based prefix matching, inline completion in
  accent colour, Tab accepts and creates parameter placeholders.
- Ctrl+B = software reboot to BOOTSEL (reset_usb_boot) for flashing.
- Start minimal, build incrementally. Get each piece working on
  hardware before adding the next.Get each piece working on
  hardware before adding the next. Get each piece working on
  hardware before adding the next.