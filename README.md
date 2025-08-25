# BitChat-clone

Minimal BLE 1:1 E2E-encrypted messenger (Linux + BlueZ).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

## CLI

Server `build/bin/bitcahtd`

```bash
Usage:
  bitcahtd
```

Client `build/bin/bitchatctl`

```bash
Usage:
  bitchatctl [--sock <path>] <command> [args]

Commands:
  send <text...>
  tail on|off
  quit
```

## Quickstart

```bash
# 1 Prepare cache location (required)
mkdir -p ~/.cache/bitchat-clone

# 2 Start a temporary listener (terminal A)
./build/bin/bitcahtd

# 3 Send a command (terminal B)
./build/bin/bitchatctl send "hello world"
# Listener should print: "SEND hello world\n"
./build/bin/bitchatctl quit
# Listener should print: "QUIT\n"

# TODO:
./build/bin/bitchatctl tail on/off
```
