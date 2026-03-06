## Functions
| Command | Description | Example Usage | Returns |
| --- | --- | --- | --- |
| ! | Returns the factorial | `9!` | `362880` |
| +, -, *, /, () | Basic arithmetic operators | `3(4-5)/7+25*5` | `24.57142857` |
| a-d, f-h, j-z | Variables that support deferred assignment | `t=4x^2+7x-3` then `x=5`then `t` | `132` |
| abs() | Returns the absolute value | `abs(-36)` | `36` |
| acos(), asin(), atan() | Inverse trignometric functions |  |  |
| acosh(), asinh(), atanh() | Inverse hyperbolic functions |  |  |
| ans | Variable referencing the previous output | `44+1` then `ans+1` | `46` |
| bat | Returns the 16-bit integer of battery data from the STM32 southbridge | when battery is 96% and not charging, `bat` | `24587` |
| cbrt() | Returns the cube root | `cbrt(27)` | `3` |
| ceil() | Rounds up to the nearest integer | `ceil(4.1)` or `ceil(4.9)` | `5` |
| cle | Clears all variables | `cle` | `ok` |
| cls | Clears the screen | `cls` | `ok` |
| cos(), sin(), tan() | Trignometric function |  |  |
| cosh(), sinh(), tanh() | Hyperbolic functions |  |  |
| e | Euler's number, 2.718281828 | `2e` | `5.436563657` |
| exp() |  |  |  |
| floor() | Rounds down to the nearest integer | `floor(4.1)` or `floor(4.9)` | `4` |
| i | Imaginary unit, sqrt(-1) | `i^2+2i` | `-1+2i` |
| ln() |  |  |  |
| log() |  |  |  |
| name | Returns the current calculator name | `name` | `MyCalc` |
| name() | Renames the PicoCalc | `name(MyCalc)` sets, `name()` clears | `ok` |
| neg | RPN negation operator | `5 3 neg +` | `2` |
| pi | Pi, 3.141592654 | `2pi` | `6.283185307` |
| round() | Rounds to the nearest integer | `round(4.6)` or `round(5.4)` | `5` |
| sign() | Returns the sign | `sign(-4523)` or `sign(-1.1)` | `-1` |
| sqrt() | Returns the square root | `sqrt(9)` | `3` |
| ver | Returns the OpenCalc version number | `ver` | `0.5.40` |