# AirPlay HomeKit Pairing — Parked Investigation

Branch: `feat/airplay-homekit-pairing`
Last active: June 2026
Status: **Parked.** AirPlay receiver advertises and accepts connections from iPhone and
iPad, but pair-setup (the password exchange) fails. Android (scrcpy/Miracast) and Google
Cast paths are unaffected.

---

## TL;DR

We implemented the modern AirPlay 2 / HomeKit pairing flow (HAP TLV8 `/pair-setup` with
SRP-3072-SHA512) from scratch. Our implementation passes a self-consistent round-trip
test and matches the published RFC 5054 / Apple HAP spec byte-for-byte. **But the
client M1 proof produced by iOS does not match the M1 we compute** — and we've ruled out
every obvious variable. The most likely remaining cause is a non-public modification
Apple made to the M1 transcript, the K derivation, or the password encoding. Diagnosing
further requires diffing against a known-working open-source receiver
(UxPlay) under a live capture from the same iPhone.

---

## What works today

- mDNS advertisement (Bonjour + fallback responder) on `_airplay._tcp` / `_raop._tcp`
- `/info` bplist returns 1128 bytes with the HK feature mask
  `features = 0x527FFEE6,0x94D40` (clears bit 27 `SupportsLegacyPairing`, sets bits
  38/40/41/43/46/48/51 for HK pairing/transient/PTP/encryption)
- `/pair-setup` M1 → M2 (server returns salt + B)
- `/pair-verify` correctly refuses when no controllers are paired so iOS doesn't
  loop forever on the verify path
- HAP infrastructure exists end-to-end (`hap_srp`, `hap_pair_setup`, `hap_pair_verify`,
  `hap_session`, `tlv8`, `hap_crypto`, Ed25519 device identity, ChaCha20-Poly1305
  framing). Self-test (`--srp-self-test`) passes.

## What fails

- `/pair-setup` M3: the M1 proof iOS sends is never the M1 our server computes
  from the same `(salt, A, B, K)`.
- iPad MDM-managed devices skip the HAP path entirely and force the legacy
  `/pair-pin-start` SRP-6a flow. The prior author burned **4320 SRP variants**
  (commit `fe2e0f4`) on that path without success — Apple's published AirPlay 1
  SRP-6a/SHA-1/AES-CBC PIN flow no longer matches modern iPadOS. We deliberately
  404 those endpoints now so iOS doesn't fall back to a broken path.

---

## How we know our SRP implementation is spec-correct

We built a capture harness:

- C++ side: [src/airplay/hap_srp.cpp](src/airplay/hap_srp.cpp) dumps every
  intermediate (`I`, `setup_code`, `salt`, `B`, `v`, `A`, `S`, `K`, `M1_server`,
  `M1_client`) as `[HAP-SRP-DUMP]` lines.
- Reference side: [scripts/srp_check.ps1](scripts/srp_check.ps1) (PowerShell) and
  [scripts/srp_check.py](scripts/srp_check.py) (Python) each implement a clean
  RFC 5054 SRP-3072-SHA512 from scratch and replay the captured values.

Reference replay against two real iPhone captures produced:

| Check | Result |
|---|---|
| `K = SHA512(PAD(S))` | matches our C++ |
| `v = g^x mod N`, with `x = H(salt \| H(I:p))` | matches our C++ |
| Server `M1 = H(H(N) ⊕ H(g) \| H(I) \| salt \| A \| B \| K)` | matches our C++ (both N-padded and N-minimal) |
| iPhone client M1 | **does not match** with `I="Pair-Setup"` + password `"940-70-414"` |
| iPhone client M1 against any of `{I="", I="admin", I=lowercase, pw without dashes, all 4 padding variants}` | **no match** |

So: our math is right per the spec; iPhone is computing M1 over genuinely
different bytes.

---

## Hypotheses that remain

In rough order of likelihood:

1. **Different K derivation.** Apple may not use `K = SHA512(PAD(S))`. Possible
   variants: HKDF-Extract over `S`, HKDF-Expand with an `"AirPlay-Setup"` info
   string, SHA-Interleave (the SRP-6a/SHA-1 historic style adapted), or
   a transcript hash that includes prior request bodies.
2. **Augmented M1 transcript.** Apple may include extra inputs in the M1 hash —
   e.g. the deviceID, the AirPlay session number, the `/info` response,
   or constants like `"AirPlay-Pair-Setup"`.
3. **Different password representation.** Maybe the 8-digit code is not used
   as the UTF-8 string `"940-70-414"` but as 8 BCD bytes, or PBKDF2-stretched,
   or HKDF-derived first.
4. **MFi co-signing.** Apple HAP allows an MFi-signed accessory to use a
   different SRP key derivation. Genuine Apple sources may demand MFi at the
   M5/M6 boundary that we never reach.

What we have **ruled out**: padding of `N` or `g` in `H(N)/H(g)` (tried all 4
combos), case of password, presence of dashes, `I="Pair-Setup"` vs other
common strings, and basic transcript layout.

---

## What to do next (recommended order)

### Step 1 — Get a reference capture from UxPlay

[UxPlay](https://github.com/FDH2/UxPlay) is the most complete open-source
AirPlay 2 mirror receiver and is known to pair successfully with iOS 17+.

1. Install WSL2 + Ubuntu 22.04: `wsl --install -d Ubuntu-22.04`
2. Build UxPlay from source so its SRP can be patched:
   ```bash
   sudo apt update && sudo apt install -y build-essential cmake git \
       libssl-dev libavahi-compat-libdnssd-dev libplist-dev \
       gstreamer1.0-libav gstreamer1.0-plugins-bad gstreamer1.0-plugins-good
   git clone https://github.com/FDH2/UxPlay.git
   cd UxPlay && mkdir build && cd build && cmake .. && make -j
   ```
3. Patch `lib/pairings.c` (or wherever UxPlay does SRP M1) to dump
   `I, salt, B, A, S, K, M1` in the same format we use (`[HAP-SRP-DUMP] field(N)=hex`).
4. Run UxPlay, pair the iPhone, save the log.
5. Run [scripts/srp_check.ps1](scripts/srp_check.ps1) on the UxPlay log.
   The harness's "matches client" line will reveal which variant Apple actually uses.
6. Port that variant back to [src/airplay/hap_srp.cpp](src/airplay/hap_srp.cpp).

### Step 2 — If UxPlay also computes a different M1 than the spec

Then read UxPlay's `K` derivation directly. The harness already verifies our
`K = SHA512(PAD(S))` matches Python's reference — if UxPlay uses a different
formula, that's the bug.

### Step 3 — Add a regression test from the captured vector

Once we know what works, embed the (salt, A, B, password) → M1 vector from
the UxPlay capture into `hap_srp_self_test()` so it never regresses.

### Step 4 — MDM-managed iPad

Even if we fix SRP for the iPhone, MDM-managed iPads ignore the HK feature
bits in mDNS and demand the broken legacy `/pair-pin-start` SRP-6a flow. The
prior author already proved this path can't be fixed by trying SRP variants
(see commit `fe2e0f4`'s "4320 variants" note). Options:

- Document MDM iPad as unsupported.
- Investigate whether iPadOS 18+ has changed this behaviour.
- Investigate MDM AirPlay policies (some MDMs can disable the legacy enforcement).

---

## Key files

- [src/airplay/airplay_server.cpp](src/airplay/airplay_server.cpp)
  - `handle_info()` — `/info` bplist with HK feature mask and `statusFlags=68`
  - `handle_pair_setup()` / `handle_pair_verify()` — dual-path (legacy + HAP)
    routing
  - POST routing 404s `/pair-pin-start` and `/pair-setup-pin` (the legacy SRP-6a
    flow), forcing iOS to attempt HAP instead
- [src/airplay/hap_srp.cpp](src/airplay/hap_srp.cpp) — SRP-3072-SHA512 with
  `[HAP-SRP-DUMP]` instrumentation
- [src/airplay/hap_pairing.cpp](src/airplay/hap_pairing.cpp) — M1–M6 state
  machine + pairing persistence at `%APPDATA%\1PhoneMirror\hap_pairings.txt`
- [src/airplay/hap_verify.cpp](src/airplay/hap_verify.cpp) — `/pair-verify`
  with Curve25519 + Ed25519, returns `Authentication` error when no pairings
  exist so iOS switches to `/pair-setup`
- [src/airplay/mdns_service.cpp](src/airplay/mdns_service.cpp) — mDNS TXT
  records advertising HK feature bits and `flags=0x4`
- [scripts/srp_check.ps1](scripts/srp_check.ps1) — reference SRP replay
  (PowerShell, no Python dependency)
- [scripts/srp_check.py](scripts/srp_check.py) — same logic in Python

## Key commits

- `83c45a8` — switch mDNS feature mask to HK pairing bits
- `50306e1` — try `flags=0x204` / `statusFlags=0x244` (`OneTimePairingRequired`).
  Currently reverted — did not help and broke iPhone pair-setup advertisement.
  Worth re-trying if other changes succeed.
- `af70e4a` — refuse `/pair-verify` M1 when no pairings exist
- `d1c2f44` — SRP fix: `H(g)` uses minimal byte representation
- `fe2e0f4` — historical record: 4320 legacy SRP-6a variants tried, all rejected
  by MDM iPad
- `716d270` — disable legacy SRP path (broken on modern iOS)

## How to resume

1. `git checkout feat/airplay-homekit-pairing`
2. `git pull` if needed
3. Delete any stale pairings: `Remove-Item "$env:APPDATA\1PhoneMirror\hap_pairings.txt"`
4. Build: `& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release`
5. Run the EXE from `C:\Program Files\1PhoneMirror\` (firewall behaves correctly there)
6. Capture a fresh log + run `scripts\srp_check.ps1` to confirm the failure mode hasn't drifted
7. Proceed with the UxPlay diff in **Step 1** above
