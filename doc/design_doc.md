# Design Document for "bitchat-clone"

## 1. Introduction

* Goal: Ship a demo-able one-to-one nearby messenger with end-to-end encryption over BLE (Linux + BlueZ). Development uses an in-process loopback transport to bring up the pipeline quickly.
* Scope: Single link; two GATT characteristics (TX=Notify, RX=Write with response); app-level fragmentation (~100 B payload/fragment); PSK AEAD; minimal IPC/CLI for control.
* Non-goals: ACK/NAK/retransmit, safety numbers/QR, group/mesh, key exchange, role contention. (future work)

## 2. Feature description

### Architecture (layers)

* Transport: LoopbackTransport (in-process, same-thread callback). BlueZ GATT on Linux for real BLE.
* Proto: 12-byte fragment header + fragmenter/reassembler (payload <= 100 B/fragment).
* Crypto: PskAead interface. Recommended production AEAD: XChaCha20-Poly1305 (libsodium).
* App: ChatService — plaintext -> AEAD -> fragment -> transport; reverse on RX.
* Ctl: Minimal AF_UNIX IPC line protocol between CLI and daemon.

### Fixed constants

* Service UUID: 7e0f8f20-cc0b-4c6e-8a3e-5d21b2f8a9c4
* TX (Notify): 7e0f8f21-cc0b-4c6e-8a3e-5d21b2f8a9c4
* RX (Write w/ response): 7e0f8f22-cc0b-4c6e-8a3e-5d21b2f8a9c4
* Default app payload per fragment: 100 B

### Roles

* Fixed central <-> peripheral; no dual-role negotiation.

### Fragment header (12 bytes, big-endian)

```txt
|-------|---------|----------|-------|---------|-------|
|  1 B  |    1 B  |   4 B    |  2 B  |   2 B   |  2 B  |
|-------|---------|----------|-------|---------|-------|
| ver:1 | flags:1 | msg_id:4 | seq:2 | total:2 | len:2 |
|-------|---------|----------|-------|---------|-------|
```

* msg_id groups fragments into a message.
* seq/total bound each fragment’s slot and message length.
* len is payload bytes following the header.

## 3. CLI

Protocol (one line per connection):

```bash
SEND <text…>
TAIL on|off
QUIT
```

Usage:
```bash
bitchatctl send <text...>
bitchatctl tail on|off
bitchatctl quit
  [--sock <path>]    # default: ~/.cache/bitchat-clone/ctl.sock
```

## 4. Documentation — Design rationale

### 4.1 TX/RX split (Notify vs. Write-with-response)

* What we do: Two characteristics: TX = Notify, RX = Write(with response).
* Why:
  * Fast outbound: Notifications are fire-and-forget (no app-level ACK), ideal for pushing chat payloads with low latency.
  * Confirmed inbound: Writes with response give a delivery confirmation when we receive control/user data.
  * Simple mapping: This splits concerns cleanly and matches BlueZ APIs (StartNotify, WriteValue), so implementation stays small and predictable.
* Failure model: Notifications can drop; we accept best-effort in the MVP. If a reliable path is needed later, switch specific flows to indications or add an app-layer retry.

### 4.2 App-level fragmentation (100-byte payload + 12-byte header)

* What we do: Always fragment ciphertext into <= 100 B payload chunks, each prefixed by a 12-byte header:

```txt
|-------|---------|----------|-------|---------|-------|
| ver:1 | flags:1 | msg_id:4 | seq:2 | total:2 | len:2 |
|-------|---------|----------|-------|---------|-------|
```

* Why:
  * MTU portability: BLE’s usable payload per notification is ATT_MTU − 3 and varies widely by device/OS. Fixing 100 B avoids edge-case failures without per-device tuning.
  * Deterministic plumbing: Upper layers never care about platform MTU; fragmenter owns sizing.
* Header fields (practical intent):
  * ver/flags -> format control & future bits without breaking older peers.
  * msg_id -> groups fragments of one logical message; prevents cross-mix with other messages.
  * seq/total -> 0-based index and final count; enables complete-only delivery and out-of-order assembly.
  * len -> exact bytes of payload in this fragment (last chunk is shorter).
* Reassembly rules: Buffer by (sender, msg_id); accept out-of-order; deliver only when all seq in [0,total) arrive. Evict incomplete messages after a timeout to cap memory.

### 4.3 Crypto boundary (PSK AEAD; XChaCha20-Poly1305 later)

* What we do: A small PskAead interface sits between app and proto. During bring-up we use a placeholder; swapping in XChaCha20-Poly1305 (libsodium) requires no changes to transport/proto.
* We fragment ciphertext+tag across chunks; every fragment carries its header and uses identical AAD derivation, so the receiver rejects mismatched or replayed pieces.

### 4.4 Fixed roles (central/peripheral)

* Why fixed: Avoids role arbitration and halves state space. Connection logic and error handling stay lean; we can generalize later if needed.

### 4.5 Explicit non-features (by design)

* No ACK/NAK/retransmit layer.
* No storage/outbox, safety numbers/verification, group/mesh, etc.
* No platform-specific MTU tuning in upper layers (fragmenter owns sizing).

