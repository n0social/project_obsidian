# Warden Findings Knowledge Base (Obsidian / RetroWoW)

Operational findings from making Obsidian’s Classic Warden client survive RetroWoW (`logon.retro-wow.org`).  
Companion docs: `WARDEN_IMPLEMENTATION.md`, `WARDEN_QUICK_REFERENCE.md`.

---

## Symptom map

| Log signal | Meaning |
|---|---|
| `(sync) Parsed 0 checks` / empty result | String-table misparse → RetroWoW `peer_closed` |
| `PAGE_A/PAGE_B detected` then no `Sent async` | Async reply never drained / starved |
| `SUCCESSFULLY ENTERED WORLD` then `peer_closed` ~0.5–1s after `Sent async` | Integrity reply rejected (often checksum/framing or a failed check) |
| Grey Enter World / `DISCONNECTED` at char select | World socket already closed by Warden kick |
| Ambient / terrain stalls without Warden drain | Client response timeout (~10s on VMaNGOS-style cores) |

Device log: `/sdcard/Android/data/com.obsidian.client/files/logs/wowee.log`

---

## Finding 1 — False PAGE_A/PAGE_B detection (critical)

**Bug:** Early code XOR-scanned every byte in the check stream. A MODULE seed byte such as `0x05` decoded as PAGE_B and forced the async path.

**Effect:** Async PATH taken for non-PAGE packets; replies delayed or never sent; kick.

**Fix:** Walk opcodes with known body sizes only (`MEM=6`, `MODULE=24`, `PAGE=29`, …). Set async only when a real PAGE opcode is seen.

**Code:** `warden_handler.cpp` cheat-check pre-scan before async launch.

---

## Finding 2 — Warden packet starvation during world load

**Bug:** `processQueuedIncomingPackets` prioritized `UpdateObject` and burned a tiny IN_WORLD budget, delaying `SMSG_WARDEN_DATA`.

**Fix:** Drain all pending Warden packets first, then the normal budgeted loop; call `WardenHandler::drainPendingResponse()` from `update()`.

**Code:** `game_handler_packets.cpp`, `warden_handler.cpp`.

---

## Finding 3 — Async reply must be framed on the main thread

**Bug:** Async worker built `[0x02][len][checksum][resultData]` including `SHA1` checksum on a background thread, then main thread only RC4-encrypted. OpenSSL SHA1/HMAC are not assumed safe vs concurrent main-thread crypto.

**Observed:** Sync integrity batches (no PAGE) survived; first PAGE batch went async → `Sent async` → `peer_closed` ~0.7s later. Result payload hex looked structurally correct (MEM/PAGE/TIMING/MPQ sizes matched).

**Fix:**
1. Async worker returns **raw `resultData` only**.
2. Main thread runs `frameCheatChecksResult()` (length + checksum) then encrypt/send.
3. Known-fast PAGE scans (packet-process sanity at RVA `0x3620` / offset `13856`) stay on the **sync** path so they never touch the worker.

**Code:** `WardenHandler::frameCheatChecksResult`, `drainPendingResponse`, async pre-scan `isKnownFastPageBody`.

---

## Finding 4 — Main-loop stalls vs Warden client timeout

**Bug:** Ambient WAV load blocked ~1.5–1.7s; VMaNGOS-style response timeout can kick.

**Fix:** `pumpNetworkForWarden()` via `GameHandler::update(0)` inside `AmbientSoundManager::loadSound`.

**Code:** `ambient_sound_manager.cpp`.

---

## Finding 5 — OpenGL vs RetroWoW EndScene walk

**Context:** VMaNGOS reads `g_theGxDevicePtr` → device, then `device+0x1FC` (0=OpenGL, 1=D3D). OpenGL skips EndScene. RetroWoW often still walks D3D (`device+0x38A8` → … → MEM of EndScene) even when API reports OpenGL.

**Fix:** Patch GX pointer to a fake device, `+0x1FC = 0` (OpenGL), and keep an inert EndScene stub in BSS (`0xCE8800`) with a clean prologue (`push ebp; mov ebp,esp; xor eax,eax; pop ebp; ret` + NOPs). `ValidateEndScene` mainly flags INT3/JMP hooks.

**Addresses (Classic 5875):**
- SYSINFO module ptr: `0xCE897C`
- GX device ptr: `0xC0ED38`
- LastHardwareAction: `0xCF0BC8`
- Fake EndScene stub: `0xCE8800`

**Code:** `warden_memory.cpp` `patchRuntimeGlobals()`.

---

## Finding 6 — Anti-AFK = MEM(LastHardware) + TIMING

**Server (VMaNGOS):** One scan emits MEM `@0xCF0BC8` then TIMING. Checker requires:
- MEM status `0x00`
- TIMING status ≠ `0` (soft-fail if clocks disagree)
- `lastHardwareAction <= currentTime`

**Client fix:** One shared tick per reply (`wardenTickMs()`). Write LHA = `ticks - 2000` (saturating), TIMING = `ticks`. Never let LHA underflow below 0 (would appear “in the future” and kick).

**Pattern in failing RetroWoW packet:** `… PAGE_A … MEM 0xCF0BC8 … TIMING … MPQ …`

---

## Finding 7 — PAGE sanity at offset 13856 (`0x3620`)

**Pattern (wanted):** `33 D2 33 C9 E8 87 07 1B 00 E8`  
**Result byte:** `0x4A` (`WindowsCodeScan::PatternFound`) — not classic MaNGOS’s mistaken `0xE9`.

**Note:** Seed is the 4 bytes *after* the PAGE opcode. Mis-including the opcode in the seed breaks HMAC matching.

---

## Finding 8 — PE image selection

| Mode | Expected `SizeOfImage` |
|---|---|
| Turtle | `0x906000` |
| Stock classic (non-Turtle flag) | `0x9FD000` |

Repo ships `Data/expansions/classic/misc/WoW.exe` at **`0x906000`**. RetroWoW integrity MEM bytes we have seen match this PE (and the embedded scan DB). Prefer matching `SizeOfImage` when multiple exes exist.

`verifyWardenScanEntries()` logs whether DB expected bytes match the loaded PE; Turtle mode can patch mismatches into the image.

---

## Finding 9 — MPQ door hashes

`knownDoorHashes()` holds SHA1s for common door M2s (wallhack checks). Prefer known hash, else hash the on-disk classic file.

Verified equal for:
- `deadminedoor02.m2`
- `nox_door_plague.m2`

Wrong “prefer known before file” is only unsafe if the known table drifts from what RetroWoW expects.

---

## Finding 10 — CHEAT_CHECKS_RESULT framing

```
[0x02][uint16 length][uint32 checksum][result bytes…]
```

- `length` = size of result bytes only  
- `checksum` = XOR of five little-endian `uint32`s from `SHA1(result)`  
- Check results are in **request opcode order** (VMaNGOS `ReadScanResults`), not classic “TIMING always first” unless the server builds requests that way

---

## RetroWoW session shape (successful enter)

1. Maiev string-hash  
2. SYSINFO locate chain (MEM `@0xCE897C` → … → SYSINFO blob)  
3. GX / EndScene locate (MEM `@0xC0ED38` → … → EndScene bytes)  
4. Enter world  
5. Integrity batch: MEM × N + optional PAGE + Anti-AFK (LHA+TIMING) + MPQ  

VPN (Thunder) required for `logon.retro-wow.org:3724`.

---

## Debug checklist

1. VPN up → Play → Enter World  
2. Pull log; confirm no `peer_closed` after Warden  
3. Prefer `(sync) Parsed` for known PAGE sanity; if async, confirm `Sent async … framed=`  
4. Dump `RESPONSE_HEX` sizes: each MEM = `1+len`, PAGE = `1`, TIMING = `5`, MPQ found = `21`  
5. For Anti-AFK: LHA `uint32` ≤ TIMING `uint32`  
6. EndScene stub must not start with `CC` / `E9`

---

## Files touched (client)

- `WoWee/src/game/warden_handler.cpp` / `warden_handler.hpp`  
- `WoWee/src/game/warden_memory.cpp`  
- `WoWee/src/game/game_handler.cpp` / `game_handler_packets.cpp`  
- `WoWee/src/rendering/terrain_manager.cpp`  
- `WoWee/src/network/world_socket.cpp` (`dispatchWardenCallbacks`, Warden-first queue)
- `WoWee/include/game/warden_runtime.hpp`

---

## Finding 11 — Post-EndScene kick without integrity batch (2026-08-23)

**Observed (after async/checksum fix):**
- Enter world succeeded; terrain/M2/UI assets loaded (user: “assets rendered”, felt faster).
- Full SYSINFO + EndScene locate chain answered **synchronously** and looked valid.
- EndScene stub reply: `55 8b ec 33 c0 5d c3` + NOPs.
- `peer_closed` ~4s later with **no further `CHEAT_CHECKS` in the client log**.
- CLOSE TRACE: last Warden TX was EndScene reply; then only movement/`COMPRESSED_MOVES` until close.

**Interpretation:** Not the old PAGE/async integrity failure. Likely one of:
1. Server sent the next scan batch and the client never logged/handled it (VPN loss or dispatch gap under load), then **client-response timeout** (~3–5s fits).
2. Server-side penalty from a check that does not require another round-trip (less likely given VMaNGOS `ReadScanResults` kicks on the reply that failed).
3. Non-Warden kick (rarer given timing right after Warden init finished).

**Note:** `Module loaded successfully (image size=0 bytes)` + RSA unpack failure remains; HASH still matches via `.cr`. PAGE “Warden Memory Read” sanity must keep using `isKnownWantedCodeScan` / BSS embed because module image is empty.

**Also:** RetroWoW still skips `device+0x1FC` OpenGL gate and walks D3D EndScene (`+0x38A8` → … → stub).

## Finding 12 — Post-EndScene hardening (client)

Mitigations added after Finding 11:
1. Treat VMaNGOS “Warden Memory Read” 37-byte PAGE_B pattern as **known-fast** (stay sync).
2. Extra socket + packet pumps each frame while Warden is required (chained scans).
3. EndScene pointer prefers a real `.text` `push ebp; mov ebp,esp` prologue instead of BSS stub.

**Still not proof** against VPN-lost follow-up packets.

---

## Finding 14 — Terrain `loadTile` starved Warden (~4s kick)

**Bug:** After a matching integrity reply, RetroWoW often sent the next scan while the main thread was inside synchronous `TerrainManager::loadTile` (CPU prepare + GPU upload). `GameHandler::updateNetworking` did not run, so `SMSG_WARDEN_DATA` sat in the socket until the server's client-response timeout (~3–5s) → `peer_closed` with no further Warden line in the log.

**Fix:** `GameHandler::pumpWardenIo()` drains socket + Warden packets only (re-entrancy guarded). Called from `loadTile` and ambient WAV load instead of a full `update(0)`.

**Code:** `game_handler.cpp` `pumpWardenIo`, `terrain_manager.cpp` `loadTile`, `ambient_sound_manager.cpp`.

---

## Finding 15 — Callback budget hid Warden behind UpdateObject (stay-alive)

**Bug:** Recv is already async, but parsed packets wait in `WorldSocket::pendingPacketCallbacks_`. `dispatchQueuedPackets()` only moved **48** packets per main-thread tick. `SMSG_WARDEN_DATA` (0x2E6) was logged as login-pipeline but **not** drained first. During Enter World, hundreds of `SMSG_UPDATE_OBJECT` sat in front of the next cheat-check. `pumpWardenIo()` → `socket->update()` still hit that budget, so a check could sit unanswered until RetroWoW's ~3–5s client-response timeout (`peer_closed`, no further Warden line).

A Warden kick **destroys the world session**. There is no in-place dungeon failsafe after the server has already closed; the real failsafe is answering on time.

**Fix:**
1. Enqueue 0x2E6 at the **front** of the socket callback queue (stable order for multiple Warden packets).
2. `dispatchQueuedPackets` steals **all** Warden packets before applying the 48-packet budget.
3. `dispatchWardenCallbacks()` + a 25ms **watchdog thread** so `loadTile` / UpdateObject cannot starve checks. Packet queue and Warden RC4 are mutex-protected.
4. `pumpWardenIo` no longer requires `wardenGateSeen()` (MODULE_USE / HASH also time out).
5. On `peer_closed`, classify timeout vs post-result reject vs VPN, write `last_world_error.txt` + `last_auth_error.txt`, and show the exact string in UI/chat.
6. TCP keepalive after `TCP_NODELAY` so VPN RSTs surface instead of hanging.

**Cannot keep the same dungeon connection after a Warden reject.** If the last TX was `CHEAT_CHECKS_RESULT` and the server still closes, that is a result mismatch, not a dispatch gap.

---

## Finding 13 — Kick after matching integrity batch (2026-08-23)

**Observed:** Entered world; EndScene at `.text` `0x401030`; integrity sync batch
(MEM×8 + MPQ diremaul door + PAGE_A `@0x3620`) replied with bytes that match stock
`WoW.exe` + known door SHA1 + HMAC-verified PAGE pattern. `peer_closed` ~3.8s later.
No further `SMSG_WARDEN_DATA` was logged. World/M2 assets were loading (kick cuts
streaming short). Real missing files: `assets/krayonload.png`, classic
`IncompleteQuestIcon.blp`.

**Client changes:**
1. EndScene pointer restored to dedicated BSS stub `0xCE8800` (no random `.text`).
2. Loading screen uses classic Glue BLPs + procedural Obsidian fallback.
3. Incomplete quest marker falls back to Available icon.
4. Integrity `RESPONSE_HEX` logged as one line for full capture.

**Open:** If kick persists with stub EndScene + matching integrity hex, need server-side
Warden fail reason (or proof a follow-up scan was dropped on the wire).

---

## Open risks

- Stock `0x9FD000` PE not in tree; if RetroWoW ever expects that image’s bytes at a DB address our `0x906000` PE differs on, MEM will fail.  
- Brute-force PAGE (non-sanity) still uses async; worker must not call OpenSSL for checksum (fixed) but still uses HMAC for search — keep heavy PAGE rare or add crypto locking if races appear.  
- Module unpack still noisy (RSA / stub); HASH via `.cr` is what RetroWoW accepts.  
- Post-EndScene silent kick: need either server-side Warden logs or client proof a follow-up `SMSG_WARDEN_DATA` was missed.
