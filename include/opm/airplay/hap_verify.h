#pragma once

#include <opm/airplay/hap_device.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opm::airplay {

// Server-side HomeKit pair-verify state machine.
//
// /pair-verify TLV8 exchange:
//   M1 (client→server): { State=1, PublicKey=client_curve25519_pub (32B) }
//   M2 (server→client): { State=2, PublicKey=accessory_curve25519_pub (32B),
//                         EncryptedData=ChaCha20(sub-TLV{Identifier=accessory_pi,
//                                                         Signature=Ed25519(
//                                                            accessory_pub
//                                                          || accessory_pi
//                                                          || client_pub)}) }
//   M3 (client→server): { State=3, EncryptedData=ChaCha20(sub-TLV{
//                            Identifier=controller_pi,
//                            Signature=Ed25519(
//                               client_pub || controller_pi || accessory_pub)
//                         }) }   — signed by controller_ltpk looked up in
//                                  HapPairingStore.
//   M4 (server→client): { State=4 }
//
// On success, derives the two ChaCha20-Poly1305 session keys used by the
// subsequent encrypted RTSP framing (HAP M6 / RFC 7539):
//   accessory→controller (server→client write key)
//   controller→accessory (server read key)
// session_shared_secret(), read_key() and write_key() are valid only once
// is_complete() returns true.

class HapPairVerify {
public:
    explicit HapPairVerify(HapDevice& device);
    ~HapPairVerify();

    // Process one client TLV. Returns the response TLV or an empty vector on
    // hard error (transport should respond 500 in that case). On TLV-level
    // failures the response carries kTLVType_Error and the state machine
    // resets so the controller can retry from M1.
    std::vector<uint8_t> handle(const std::vector<uint8_t>& in_tlv);

    bool is_complete() const;
    void reset();

    // Post-handshake outputs. Empty until is_complete().
    const std::vector<uint8_t>& shared_secret() const;  // 32B X25519 result
    const std::vector<uint8_t>& read_key()      const;  // 32B ChaCha20 key
    const std::vector<uint8_t>& write_key()     const;  // 32B ChaCha20 key
    const std::vector<uint8_t>& controller_id() const;  // peer's pairing ID

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool hap_verify_self_test();

} // namespace opm::airplay
