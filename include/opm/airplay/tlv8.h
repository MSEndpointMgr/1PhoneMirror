#pragma once

// HomeKit Accessory Protocol (HAP) TLV8 codec.
//
// Wire format: a sequence of (tag:u8, length:u8, value:length bytes) items.
// Values longer than 255 bytes are split into consecutive items sharing the
// same tag, each carrying up to 255 bytes. The reader rejoins those
// fragments transparently.
//
// Used by AirPlay HomeKit pair-setup (/pair-setup) and pair-verify
// (/pair-verify) — both POST the binary TLV directly as the HTTP body with
// Content-Type: application/pairing+tlv8.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace opm::airplay {

// HAP TLV item types (HomeKit Accessory Protocol R2, table 5-6).
enum class TlvType : uint8_t {
    Method         = 0x00,  // u8 — 0=pair-setup, 1=pair-setup-with-auth, 2=pair-verify, 3=add-pairing, 4=remove-pairing, 5=list-pairings
    Identifier     = 0x01,  // utf-8
    Salt           = 0x02,  // 16 bytes
    PublicKey      = 0x03,  // SRP/Curve25519 public key
    Proof          = 0x04,  // SRP M1/M2
    EncryptedData  = 0x05,  // ChaCha20-Poly1305 ciphertext || 16-byte tag
    State          = 0x06,  // u8 — M1..M6
    Error          = 0x07,  // u8 — see TlvError
    RetryDelay     = 0x08,  // u16 seconds
    Certificate    = 0x09,
    Signature      = 0x0A,  // Ed25519 64 bytes
    Permissions    = 0x0B,  // u8 — 0=user, 1=admin
    FragmentData   = 0x0C,
    FragmentLast   = 0x0D,
    Flags          = 0x13,
    Separator      = 0xFF,  // zero-length, used between sub-items
};

enum class TlvError : uint8_t {
    Unknown        = 0x01,
    Authentication = 0x02,  // wrong PIN / bad signature
    Backoff        = 0x03,  // too many attempts
    MaxPeers       = 0x04,
    MaxTries       = 0x05,
    Unavailable    = 0x06,
    Busy           = 0x07,
};

// Streaming writer. add() may be called multiple times for the same tag;
// each call emits its own item(s) so callers can intersperse Separator
// items if needed. Values > 255 bytes are auto-fragmented.
class TlvWriter {
public:
    void add(TlvType tag, const uint8_t* data, size_t len);
    void add(TlvType tag, const std::vector<uint8_t>& v) { add(tag, v.data(), v.size()); }
    void add_u8(TlvType tag, uint8_t v) { add(tag, &v, 1); }
    void add_string(TlvType tag, const std::string& s) {
        add(tag, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    void add_separator() { add(TlvType::Separator, nullptr, 0); }

    const std::vector<uint8_t>& bytes() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }

private:
    std::vector<uint8_t> buf_;
};

// Parser. Concatenates consecutive items with the same tag (HAP
// fragmentation rule). Items separated by a Separator tag are kept
// distinct via get_list().
class TlvReader {
public:
    // Returns false on truncated input.
    bool parse(const uint8_t* data, size_t len);
    bool parse(const std::vector<uint8_t>& v) { return parse(v.data(), v.size()); }

    bool has(TlvType tag) const { return items_.find((uint8_t)tag) != items_.end(); }

    // Returns the (possibly defragmented) value for tag, or nullopt.
    std::optional<std::vector<uint8_t>> get(TlvType tag) const;

    // Convenience: 1-byte values (Method/State/Error/Permissions).
    std::optional<uint8_t> get_u8(TlvType tag) const;

    // For tags repeated across Separator boundaries (e.g. list-pairings).
    // Each vector is one item; same tag in different "groups" stays split.
    std::vector<std::vector<uint8_t>> get_list(TlvType tag) const;

private:
    // Defragmented map for simple get(). For get_list() we replay items_raw_.
    std::map<uint8_t, std::vector<uint8_t>> items_;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> items_raw_;
};

// Self-test: encode/decode round-trip including a fragmented (>255B) value
// and a Separator-split repeated tag. Returns true on success, prints
// diagnostics to stderr on failure. Wired into --tlv8-self-test CLI flag.
bool tlv8_self_test();

} // namespace opm::airplay
