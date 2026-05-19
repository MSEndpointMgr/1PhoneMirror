// cast_proto.h — Minimal protobuf encoder/decoder for Google Cast v2 CastMessage
// No protobuf library dependency — manual varint/length-delimited encoding.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace opm::cast {

struct CastMessage {
    uint32_t protocol_version = 0; // CASTV2_1_0
    std::string source_id;
    std::string destination_id;
    std::string ns;                // namespace
    uint32_t payload_type = 0;     // 0=STRING, 1=BINARY
    std::string payload_utf8;
    std::vector<uint8_t> payload_binary;

    std::vector<uint8_t> encode() const {
        std::vector<uint8_t> out;
        enc_varint(out, (1 << 3) | 0); enc_varint(out, protocol_version);
        enc_str(out, 2, source_id);
        enc_str(out, 3, destination_id);
        enc_str(out, 4, ns);
        enc_varint(out, (5 << 3) | 0); enc_varint(out, payload_type);
        if (payload_type == 0 && !payload_utf8.empty())
            enc_str(out, 6, payload_utf8);
        if (payload_type == 1 && !payload_binary.empty())
            enc_bytes(out, 7, payload_binary);
        return out;
    }

    static bool decode(const uint8_t* data, size_t len, CastMessage& msg) {
        size_t pos = 0;
        while (pos < len) {
            uint32_t tag = dec_varint(data, len, pos);
            uint32_t fn = tag >> 3, wt = tag & 7;
            if (wt == 0) {
                uint32_t v = dec_varint(data, len, pos);
                if (fn == 1) msg.protocol_version = v;
                else if (fn == 5) msg.payload_type = v;
            } else if (wt == 2) {
                uint32_t slen = dec_varint(data, len, pos);
                if (pos + slen > len) return false;
                std::string s((const char*)data + pos, slen);
                if (fn == 2) msg.source_id = s;
                else if (fn == 3) msg.destination_id = s;
                else if (fn == 4) msg.ns = s;
                else if (fn == 6) msg.payload_utf8 = s;
                else if (fn == 7) msg.payload_binary.assign(data + pos, data + pos + slen);
                pos += slen;
            } else {
                return false;
            }
        }
        return true;
    }

private:
    static void enc_varint(std::vector<uint8_t>& o, uint32_t v) {
        while (v > 0x7f) { o.push_back((v & 0x7f) | 0x80); v >>= 7; }
        o.push_back(v & 0x7f);
    }
    static void enc_str(std::vector<uint8_t>& o, int f, const std::string& s) {
        enc_varint(o, (f << 3) | 2); enc_varint(o, (uint32_t)s.size());
        o.insert(o.end(), s.begin(), s.end());
    }
    static void enc_bytes(std::vector<uint8_t>& o, int f, const std::vector<uint8_t>& b) {
        enc_varint(o, (f << 3) | 2); enc_varint(o, (uint32_t)b.size());
        o.insert(o.end(), b.begin(), b.end());
    }
    static uint32_t dec_varint(const uint8_t* d, size_t len, size_t& pos) {
        uint32_t v = 0; int shift = 0;
        while (pos < len) {
            uint8_t b = d[pos++];
            v |= (uint32_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift >= 35) break;
        }
        return v;
    }
};

// Minimal protobuf for DeviceAuth messages
struct AuthChallengeMsg {
    // signature_algorithm (field 1, varint) — 0=RSASSA_PKCS1v15
    uint32_t sig_algo = 0;
    // sender_nonce (field 2, bytes)
    std::vector<uint8_t> sender_nonce;
    // hash_algorithm (field 3, varint) — 0=SHA1, 1=SHA256
    uint32_t hash_algo = 0;

    static bool decode(const uint8_t* data, size_t len, AuthChallengeMsg& msg) {
        size_t pos = 0;
        while (pos < len) {
            // inline varint parsing for tag
            uint32_t raw = 0; int shift = 0;
            while (pos < len) {
                uint8_t b = data[pos++];
                raw |= (uint32_t)(b & 0x7f) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }
            uint32_t fn = raw >> 3, wt = raw & 7;
            if (wt == 0) {
                uint32_t v = 0; shift = 0;
                while (pos < len) {
                    uint8_t b = data[pos++];
                    v |= (uint32_t)(b & 0x7f) << shift;
                    if (!(b & 0x80)) break;
                    shift += 7;
                }
                if (fn == 1) msg.sig_algo = v;
                else if (fn == 3) msg.hash_algo = v;
            } else if (wt == 2) {
                uint32_t slen = 0; shift = 0;
                while (pos < len) {
                    uint8_t b = data[pos++];
                    slen |= (uint32_t)(b & 0x7f) << shift;
                    if (!(b & 0x80)) break;
                    shift += 7;
                }
                if (pos + slen > len) return false;
                if (fn == 2) msg.sender_nonce.assign(data + pos, data + pos + slen);
                pos += slen;
            } else {
                return false;
            }
        }
        return true;
    }
};

struct AuthResponseMsg {
    std::vector<uint8_t> signature;        // field 1
    std::vector<uint8_t> client_auth_cert; // field 2
    std::vector<std::vector<uint8_t>> intermediate_certs; // field 3 (repeated)
    uint32_t signature_algorithm = 0;      // field 4 (0=UNSPECIFIED, 1=RSASSA_PKCS1v15, 2=RSASSA_PSS)
    std::vector<uint8_t> sender_nonce;     // field 5 (echo back)
    uint32_t hash_algorithm = 0;           // field 6 (0=SHA1, 1=SHA256)

    std::vector<uint8_t> encode() const {
        std::vector<uint8_t> out;
        auto enc_varint = [](std::vector<uint8_t>& o, uint32_t v) {
            while (v > 0x7f) { o.push_back((v & 0x7f) | 0x80); v >>= 7; }
            o.push_back(v & 0x7f);
        };
        auto enc_bytes = [&enc_varint](std::vector<uint8_t>& o, int f, const std::vector<uint8_t>& b) {
            enc_varint(o, (f << 3) | 2); enc_varint(o, (uint32_t)b.size());
            o.insert(o.end(), b.begin(), b.end());
        };
        enc_bytes(out, 1, signature);
        enc_bytes(out, 2, client_auth_cert);
        for (const auto& cert : intermediate_certs) {
            enc_bytes(out, 3, cert);
        }
        if (signature_algorithm != 0) {
            enc_varint(out, (4 << 3) | 0);
            enc_varint(out, signature_algorithm);
        }
        if (!sender_nonce.empty()) {
            enc_bytes(out, 5, sender_nonce);
        }
        if (hash_algorithm != 0) {
            enc_varint(out, (6 << 3) | 0);
            enc_varint(out, hash_algorithm);
        }
        return out;
    }
};

// DeviceAuthMessage wrapper — the outer protobuf in urn:x-cast:com.google.cast.tp.deviceauth
// Fields: 1=AuthChallenge, 2=AuthResponse, 3=AuthError
struct DeviceAuthMsg {
    std::vector<uint8_t> inner; // raw bytes of whichever field was present

    // Extract the inner sub-message from field_num (1=challenge, 2=response)
    static bool unwrap(const uint8_t* data, size_t len, int field_num,
                       std::vector<uint8_t>& out) {
        size_t pos = 0;
        while (pos < len) {
            uint32_t raw = 0; int shift = 0;
            while (pos < len) {
                uint8_t b = data[pos++];
                raw |= (uint32_t)(b & 0x7f) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }
            uint32_t fn = raw >> 3, wt = raw & 7;
            if (wt == 2) {
                uint32_t slen = 0; shift = 0;
                while (pos < len) {
                    uint8_t b = data[pos++];
                    slen |= (uint32_t)(b & 0x7f) << shift;
                    if (!(b & 0x80)) break;
                    shift += 7;
                }
                if (pos + slen > len) return false;
                if ((int)fn == field_num) {
                    out.assign(data + pos, data + pos + slen);
                    return true;
                }
                pos += slen;
            } else if (wt == 0) {
                while (pos < len && (data[pos] & 0x80)) pos++;
                if (pos < len) pos++;
            } else {
                return false;
            }
        }
        return false;
    }

    // Wrap encoded response bytes in DeviceAuthMessage field 2
    static std::vector<uint8_t> wrap_response(const std::vector<uint8_t>& response) {
        std::vector<uint8_t> out;
        auto enc_varint = [](std::vector<uint8_t>& o, uint32_t v) {
            while (v > 0x7f) { o.push_back((v & 0x7f) | 0x80); v >>= 7; }
            o.push_back(v & 0x7f);
        };
        enc_varint(out, (2 << 3) | 2); // field 2, wire type 2
        enc_varint(out, (uint32_t)response.size());
        out.insert(out.end(), response.begin(), response.end());
        return out;
    }
};

} // namespace opm::cast
