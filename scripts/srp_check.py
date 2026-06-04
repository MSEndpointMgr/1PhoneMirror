#!/usr/bin/env python3
"""
SRP-3072-SHA512 capture-replay harness for AirPlay 2 HAP pair-setup.

Usage:
    1. Run 1PhoneMirror.exe Release build (it now dumps [HAP-SRP-DUMP] lines).
    2. Connect an iPhone via AirPlay screen-mirroring; the iPhone will prompt
       for an 8-digit code. Type the code shown on the PC.
    3. The PC will print one M2 block (server-side) and one M3 block (after
       client sends A + M1). Copy the entire console output (including
       [HAP] setup code line) to a text file, say capture.log.
    4. Run:  python scripts/srp_check.py capture.log

This script implements SRP-6a / RFC 5054 with N=RFC3526-Group15 (3072-bit),
g=5, H=SHA-512. It then recomputes M1 from the captured (I, password, salt,
A, B) and compares against the dumped server_M1 and client_M1.

Verdicts:
 - reference_M1 == server_M1 == client_M1  →  pairing succeeds (shouldn't reach here on failure)
 - reference_M1 == server_M1 != client_M1  →  our C++ matches spec; iOS uses a different
                                              I or password format (try I="", I=device-id, etc.)
 - reference_M1 != server_M1               →  our C++ SRP is buggy
"""

import hashlib
import re
import sys
from pathlib import Path

# RFC 3526 group 15 (3072-bit MODP), g = 5.
N_HEX = (
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
    "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
    "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
    "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
    "DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64"
    "ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6B"
    "F12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB31"
    "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF"
)
N = int(N_HEX, 16)
G = 5
PRIME_LEN = 384  # 3072 / 8


def H(*chunks: bytes) -> bytes:
    h = hashlib.sha512()
    for c in chunks:
        h.update(c)
    return h.digest()


def pad(i: int) -> bytes:
    return i.to_bytes(PRIME_LEN, "big")


def minimal(i: int) -> bytes:
    n = max(1, (i.bit_length() + 7) // 8)
    return i.to_bytes(n, "big")


def srp_M1(I: str, password: str, salt: bytes, A: int, B: int, S: int):
    """Returns (M1, K, x, k) for the given inputs."""
    K = H(pad(S))
    # x = H(salt | H(I:p))
    inner = H((I + ":" + password).encode("utf-8"))
    x = int.from_bytes(H(salt, inner), "big")
    # k = H(N | PAD(g))
    k = int.from_bytes(H(minimal(N), pad(G)), "big")
    # M1 = H( H(N) XOR H(g) | H(I) | salt | A | B | K )
    # Per HAP / RFC 5054: H(N) uses minimal N, H(g) uses minimal g.
    # Our C++ uses padded N here. Try both — we'll report both.
    hN_min = H(minimal(N))
    hN_pad = H(pad(N))
    hg_min = H(minimal(G))
    hg_pad = H(pad(G))
    hI = H(I.encode("utf-8"))

    variants = {}
    for nlabel, hN in (("Nmin", hN_min), ("Npad", hN_pad)):
        for glabel, hg in (("gmin", hg_min), ("gpad", hg_pad)):
            xor = bytes(a ^ b for a, b in zip(hN, hg))
            M1 = H(xor, hI, salt, pad(A), pad(B), K)
            variants[f"{nlabel}+{glabel}"] = M1
    return variants, K, x, k


def parse_capture(text: str) -> dict:
    """Extract dumped values from log text."""
    fields = {}
    # I="Pair-Setup"
    m = re.search(r'\[HAP-SRP-DUMP\]\s+I="([^"]*)"', text)
    if m:
        fields["I"] = m.group(1)
    m = re.search(r'\[HAP-SRP-DUMP\]\s+setup_code="([^"]*)"', text)
    if m:
        fields["setup_code"] = m.group(1)
    # salt(16)=...
    for key in ("salt", "B", "v", "A", "S", "K", "M1_server", "M1_client"):
        m = re.search(rf'\[HAP-SRP-DUMP\]\s+{key}\(\d+\)=([0-9a-fA-F]+)', text)
        if m:
            fields[key] = bytes.fromhex(m.group(1))
    return fields


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    f = parse_capture(text)

    required = ("I", "setup_code", "salt", "B", "A", "S", "K", "M1_server", "M1_client")
    missing = [r for r in required if r not in f]
    if missing:
        print(f"ERROR: capture is missing fields: {missing}")
        print("Make sure you captured BOTH the M2 dump (after first /pair-setup)")
        print("AND the M3 dump (after second /pair-setup).")
        sys.exit(1)

    I = f["I"]
    pw = f["setup_code"]
    salt = f["salt"]
    A = int.from_bytes(f["A"], "big")
    B = int.from_bytes(f["B"], "big")
    S = int.from_bytes(f["S"], "big")

    print(f"I            = {I!r}")
    print(f"setup_code   = {pw!r}")
    print(f"salt         ({len(salt)}B) = {salt.hex()}")
    print(f"|A|={len(f['A'])}B  |B|={len(f['B'])}B  |S|={len(f['S'])}B  |K|={len(f['K'])}B")
    print()

    # 1. Verify our K matches Python's K (i.e. that our S is correct).
    K_ref = H(pad(S))
    K_ok = K_ref == f["K"]
    print(f"K (ours) == K (python H(pad(S))) ?  {K_ok}")
    if not K_ok:
        print(f"  ours:   {f['K'].hex()}")
        print(f"  python: {K_ref.hex()}")
    print()

    # 2. Compute M1 in all 4 H(N)/H(g) padding variants.
    variants, _K, x, k = srp_M1(I, pw, salt, A, B, S)
    print(f"x = {hex(x)[:80]}...")
    print(f"k = {hex(k)[:80]}...")
    print()
    print("M1 variants (first 16 bytes shown):")
    for name, m1 in variants.items():
        marker_s = " <- matches server" if m1 == f["M1_server"] else ""
        marker_c = " <- matches client" if m1 == f["M1_client"] else ""
        print(f"  {name:12} = {m1[:16].hex()}{marker_s}{marker_c}")
    print()
    print(f"server M1[0:16] = {f['M1_server'][:16].hex()}")
    print(f"client M1[0:16] = {f['M1_client'][:16].hex()}")
    print()

    # 3. Try alternate I/password formats against client M1.
    print("=== fallback: try alternate I / password formats vs client M1 ===")
    candidates = [
        ("Pair-Setup", pw),
        ("", pw),
        ("Pair-Setup", pw.replace("-", "")),
        ("", pw.replace("-", "")),
        ("admin", pw),
        ("Pair-Setup", pw.lower()),
    ]
    for cand_I, cand_pw in candidates:
        v, _, _, _ = srp_M1(cand_I, cand_pw, salt, A, B, S)
        for vname, m1 in v.items():
            if m1 == f["M1_client"]:
                print(f"  MATCH! I={cand_I!r}  pw={cand_pw!r}  variant={vname}")
                return
    print("  no match found — iPhone is sending an M1 we cannot reproduce with")
    print("  these (I, password, variant) combinations. Likely iPhone is")
    print("  computing on a different S (e.g. different N or g), or hashing")
    print("  K differently, or this is a different SRP variant entirely.")


if __name__ == "__main__":
    main()
