# Design Document for "bitchat-clone"

## 1. Introduction

* Goal: Ship a demo-able one-to-one nearby messenger with end-to-end encryption over BLE (Linux + BlueZ).
* Scope (MVP): Single link; two GATT characteristics (TX=Notify, RX=Write with response); app-level fragmentation (~100 B payload/fragment); **PSK AEAD (XChaCha20-Poly1305)**; minimal IPC/CLI for control; **human-readable ID discovery** (via BLE ServiceData tag).
* Non-goals (for now): ACK/NAK/retransmit, safety numbers/QR, group/mesh, automatic key exchange, role contention.
* Future/Optional: **Single-hop relay (blind forwarder)**, TUI client over IPC, public-key identity & authenticated handshake, PFS.

## 2. Feature description

### Architecture (layers)

* Transport: LoopbackTransport (in-process, same-thread callback). BlueZ GATT on Linux for real BLE.
* Proto: 12-byte fragment header + fragmenter/reassembler (payload <= 100 B/fragment).
* Crypto: **PskAead interface using XChaCha20-Poly1305** (stream-safe nonce). HKDF-based session keys derived per connection.
* App: ChatService — plaintext -> AEAD -> fragment -> transport; reverse on RX.
* Ctl: Minimal AF_UNIX IPC line protocol between CLI and daemon.

### Fixed constants

* Service UUID: 7e0f8f20-cc0b-4c6e-8a3e-5d21b2f8a9c4
* TX (Notify): 7e0f8f21-cc0b-4c6e-8a3e-5d21b2f8a9c4
* RX (Write w/ response): 7e0f8f22-cc0b-4c6e-8a3e-5d21b2f8a9c4
* Default app payload per fragment: 100 B
  * **Discovery ID tag (advertising)**: 8 bytes, `ID_TAG = Truncate_8( SHA-256( UTF8(user_id) ) )`
  * Advertised in **LEAdvertisement1.ServiceData** as a map entry `{ <Service UUID> : ay(ID_TAG) }`.
  * Security note: ID_TAG is **not** secret. It enables human-readable addressing only. Anti-spoof requires public-key identities (see §4.6).


### Roles

* Fixed central <-> peripheral; no dual-role negotiation.

### Identity & Discovery (human-readable IDs)

* **Goal**: Let users connect by a human ID (e.g., `userAA`) instead of raw BLE addresses or object paths.
* **Advertising**:
  * `ServiceUUIDs` includes the BitChat service UUID.
  * `ServiceData` MUST include `{ <Service UUID> : ID_TAG(8B) }` as above.
  * `LocalName` MAY be a generic string (e.g., `BitChat`) and SHOULD NOT leak the full `user_id`.
+* **Resolution**:
  * Central scans for the service UUID; if an operator/UI specifies a `user_id`, the daemon computes its `ID_TAG` and matches against `ServiceData` to pick the device.
  * Optionally, after connect, an **Identity characteristic** (read-only string `user_id`) MAY be added later

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
  * `ver/flags`: format control & future bits without breaking older peers.
  * `msg_id`: groups fragments of one logical message; prevents cross-mix with other messages.
  * `seq/total`: 0-based index and final count; enables complete-only delivery and out-of-order assembly.
  * `len`: exact bytes of payload in this fragment (last chunk is shorter).
* Reassembly rules: Buffer by (sender, msg_id); accept out-of-order; deliver only when all seq in [0,total) arrive. Evict incomplete messages after a timeout to cap memory.

### 4.3 Crypto boundary (PSK AEAD; XChaCha20-Poly1305 later)

* What we do: A small PskAead interface sits between app and proto. Migrating to XChaCha20-Poly1305 requires no changes to transport/proto.
* We fragment ciphertext+tag across chunks; every fragment carries its header and uses identical AAD derivation, so the receiver rejects mismatched or replayed pieces.

### 4.4 Fixed roles (central/peripheral)

* Why fixed roles: Avoids role arbitration and halves state space. Connection logic and error handling stay simple; we can generalize later if needed.

### 4.5 Human-readable identity (discovery)

* Each device has a `user_id` (e.g., alice).
* Advertising includes `ServiceUUIDs` (BitChat UUID) and a non-secret 8-byte `ID_TAG` in ServiceData:
* `ID_TAG` = `Truncate_8( SHA-256( UTF8(user_id) ) )`
* Central that’s given user_id computes ID_TAG and matches during scan.
* LocalName stays generic (e.g., BitChat) to avoid leaking full user_id.

> Note: ID_TAG is not authentication. Preventing spoofing requires public-key identities (see §4.6).

### 4.6 Identity hardening (future)

* Make the displayed ID a Bech32 of Hash(static_public_key).
* On connect, peer proves possession of pubkey via signed transcript over the HKDF nonce exchange.
* PSK becomes optional/legacy; XChaCha framing unchanged.

### 4.7 Relay (single-hop, blind forwarder — optional)

* Node B maintains two links (central to A, peripheral to C) and forwards fragments without decryption.
* Use `HAS_ROUTE_PREFIX` and prepend 8-byte cleartext DST_TAG to each fragment payload; DST_TAG is in AAD.
* B keeps a small `{ID_TAG -> link}` cache; on RX, forward to the matching link, else drop/TTL queue.
* True mesh/multi-hop remains out of scope.

### 4.8 Explicit non-features (by design)

* No ACK/NAK/retransmit layer.
* No storage/outbox, safety numbers/verification, group/mesh, etc.
* No platform-specific MTU tuning in upper layers (fragmenter owns sizing).

