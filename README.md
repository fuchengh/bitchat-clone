# BitChat-clone

Minimal BLE 1:1 E2E-encrypted messenger (Linux + BlueZ).
With features:

- CLI <-> daemon IPC
- Fragmentation over small MTU-links
- AEAD (XChaCha20-Poly1305)
- BlueZ/BLE wiring (TODO)

## Build

Install libsodium first:

- **MacOS**

```bash
brew install libsodium
```

- **Ubuntu**

```bash
sudo apt-get update
sudo apt-get install -y libsodium-dev pkg-config
```

Configure and build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

PS: BlueZ transport is Linux-only and currently not built by default

## What runs

- Daemon: `build/bin/bitchatd`
- CLI: `build/bin/bitchatctl`

```bash
Usage:
  bitchatctl [--sock <path>] <command> [args]

Commands:
  send <text...>   # send one line of text
  tail on|off      # toggle printing of received messages
  quit             # ask daemon to exit
```

- Default socket path: `~/.cache/bitchat-clone/ctl.sock`
  - Created by the daemon automatically; CLI can override via `--sock <file>`

## Quickstart

```bash
# 1. Terminal A — start daemon:
./build/bin/bitchatd

# 2. Terminal B — talk to it:
./build/bin/bitchatctl tail off
./build/bin/bitchatctl send "should be silent" # no [RECV]
./build/bin/bitchatctl tail on
./build/bin/bitchatctl send "now visible"      # daemon prints: [RECV] now visible
./build/bin/bitchatctl quit
```

Expected daemon logs (abridged):

```text
[INFO] main: Using {SodiumPskAead (key from BITCHAT_PSK_HEX) | NoopPskAead (plaintext)}
[INFO] start_server: Listening on ~/.cache/bitchat-clone/ctl.sock
[INFO] on_line: TAIL Disabled
[INFO] on_line: CMD: SEND should be silent
# (no [RECV])
[INFO] on_line: TAIL Enabled
[INFO] on_line: CMD: SEND now visible
[INFO] on_rx: [RECV] now visible
[INFO] on_line: Received QUIT command, exiting...
```

Note: printing of [RECV] ... is gated by tail on.

## Security (AEAD)

By default the app uses a Noop AEAD (plaintext) so you can test the full pipeline without keys.

Set a PSK to enable XChaCha20-Poly1305:

```bash
export BITCHAT_PSK_HEX="$(openssl rand -hex 32)"
./build/bin/bitchatd
```

- Env var: BITCHAT_PSK_HEX = 64-hex chars (32 bytes)
- Ciphertext wire format: `[24B nonce | ciphertext||tag]`
- AEAD is applied **before** fragmentation; reassembly happens **before** decryption.

> ⚠️ Without BITCHAT_PSK_HEX, traffic is not encrypted (dev mode only).

## Design sketch

```text
bitchatctl
   │  (AF_UNIX line-based IPC)
   ▼
bitchatd on_line()
   │
   ▼
ChatService
   ├─ AEAD.seal/open (default XChaCha20-Poly1305)
   ├─ frag.make_chunks / Reassembler (12-byte header)
   └─ transport (loopback for dev, BlueZ on Linux)
```

## Tests

Build and run test

```bash
ctest --test-dir build --output-on-failure
```
