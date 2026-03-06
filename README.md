# OpenCalc
 A command line oriented calculator firmware for the [clockwork PicoCalc](https://www.clockworkpi.com/picocalc)

## Functions
| Command | Description | Usage |
| --- | --- | --- |
| ver | Prints the OpenCalc version number |  |
| cls | Clears the screen |  |
| bat | Prints the battery percentage |  |
| name | Renames the PicoCalc | `name(MyCalc)` sets, `name()` clears,  `name` prints |
| +, -, *, /, () | Basic arithmetic | `3(4-5)/7+25*5` |
| ^, !, sqrt, cbrt, log, ln, exp, abs, floor, ceil, round, sign | Advanced arithmetic | `2^3`, `32!`, `sqrt(9)` |
| pi, e, i | Mathematical constants: pi, Euler's number, imaginary unit | `2pi` |
| sin, cos, tan, asin, acos, atan, sinh, cosh, tanh | Trigonometry | `cos(45)` |
| all non-mathematical constant letters, ans | Variables | `2a^2+4b+8^c`, `ans/45`, `s=3`, `t=4x^2+7x-3` > `x=5` > `t` prints `132` |
| cle | Clears all variables |  |

## Keybinds
- <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Del</kbd> does a warm reboot, <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F</kbd> boots into `BOOTSEL` mode
- <kbd>Ctrl</kbd>+<kbd>X</kbd> cuts, <kbd>Ctrl</kbd>+<kbd>C</kbd> copies, and <kbd>Ctrl</kbd>+<kbd>V</kbd> pastes the active line
- Holding <kbd>Alt</kbd> shows battery percentage and OpenCalc version
- Function Keys (specifically <kbd>F6</kbd> thru <kbd>F10</kbd>) are context-aware, and may have different functions depending on the app being used
- <kbd>Esc</kbd> navigates back to the home screen from anywhere

## TODO
- battery level
- command autocompletion
- resistor app
- help function
- advanced graph/table/calculation tools
- file management
- part lookup