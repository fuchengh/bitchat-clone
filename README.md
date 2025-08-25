# BitChat-clone

Minimal BLE 1:1 E2E-encrypted messenger (Linux + BlueZ).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

## CLI

Binary: `build/bin/bitchatctl`

```bash
Usage:
  bitchatctl [--sock <path>] <command> [args]

Commands:
  send <text...>
  tail on|off
  quit
```

## Quickstart (no daemon yet, using a fake server)

```bash
# 1 Prepare directory (macOS typically doesn't have ~/.cache by default)
mkdir -p ~/.cache/bitchat-clone

# 2 Start a temporary listener (terminal A)
socat -d -d UNIX-LISTEN:$HOME/.cache/bitchat-clone/ctl.sock,fork -

# 3 Send a command (terminal B)
./build/bin/bitchatctl send "hello world"
# > Listener should print: "SEND hello world\n"

# TODO:
./build/bin/bitchatctl tail on
./build/bin/bitchatctl quit
```
