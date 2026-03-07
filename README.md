# OpenCalc
 A command line oriented calculator firmware for the [clockwork PicoCalc](https://www.clockworkpi.com/picocalc)

## Features
- Command history
- Command autocompletion and suggestion
- Parameter hints
- Completion dropdown list

## Keybinds
- <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Del</kbd> does a warm reboot, <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F</kbd> boots into `BOOTSEL` mode
- <kbd>Ctrl</kbd>+<kbd>X</kbd> cuts, <kbd>Ctrl</kbd>+<kbd>C</kbd> copies, and <kbd>Ctrl</kbd>+<kbd>V</kbd> pastes the active line
- Holding <kbd>Alt</kbd> shows OpenCalc version, charging status, and battery percentage
- Function Keys (specifically <kbd>F6</kbd> thru <kbd>F10</kbd>) are context-aware, and may have different functions depending on the app being used
- <kbd>Esc</kbd> navigates back to the home screen from anywhere
- <kbd>Up</kbd> navigates through the command history
- <kbd>Down</kbd> pulls down a list of alphabetically close commands to the one currently being typed

## TODO
- multiline handling
- more binary/hex/logic commands
- help function (F10)
- file management commands
- advanced graph/table/calculation tools
- resistor app
- user manual app (searchable)
- file management app
- part lookup app
