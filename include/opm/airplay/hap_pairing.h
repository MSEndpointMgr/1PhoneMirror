#pragma once

#include <opm/airplay/hap_device.h>
#include <opm/airplay/hap_srp.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace opm::airplay {

// Server-side HomeKit pair-setup state machine.
//
// Handles the M1↔M6 exchange over the /pair-setup endpoint:
//   M1 (client→server): { State=1, Method=PairSetup|PairSetupWithAuth }
//   M2 (server→client): { State=2, PublicKey=B, Salt=s }                  ← SRP-3072
//   M3 (client→server): { State=3, PublicKey=A, Proof=M1_proof }
//   M4 (server→client): { State=4, Proof=M2_proof }
//   M5 (client→server): { State=5, EncryptedData=ChaCha20(sub-TLV{ID,LTPK,Sig}) }
//   M6 (server→client): { State=6, EncryptedData=ChaCha20(sub-TLV{ID,LTPK,Sig}) }
//
// One instance per accessory (only one pair-setup may be in flight at a time
// per HAP spec — a second concurrent attempt is answered with kTLVError_Busy).
//
// On success, persists the (iOSDevicePairingID → iOSDeviceLTPK) record to
// %APPDATA%\1PhoneMirror\hap_pairings.txt; subsequent pair-verify exchanges
// (a separate handler) look the controller up there to validate signatures.

class HapPairSetup {
public:
    // device: the accessory's persistent Ed25519 identity (used to sign M6).
    explicit HapPairSetup(HapDevice& device);
    ~HapPairSetup();

    // Process one client TLV request. Returns the response TLV ready to send
    // back over HTTP. On terminal error the returned TLV carries kTLVType_Error
    // and the session resets so the controller can retry from M1.
    // get_setup_code is called once, when transitioning out of Idle on M1, to
    // ask the host for the 8-digit code shown to the user (format "031-45-154"
    // or any UTF-8 password). May return empty to force authentication failure.
    std::vector<uint8_t> handle(const std::vector<uint8_t>& in_tlv,
                                 const std::string& setup_code);

    // True once M6 has been sent and persistence has succeeded.
    bool is_complete() const;

    // Drop session state (e.g. on RTSP teardown).
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// On-disk pairing record store. Thread-safe. One row per controller:
//   <iOSDevicePairingID hex>=<iOSDeviceLTPK hex>
class HapPairingStore {
public:
    static HapPairingStore& instance();

    // Add or replace (idempotent on duplicate IDs).
    bool add(const std::vector<uint8_t>& pairing_id,
             const std::vector<uint8_t>& ltpk);

    // Lookup. Returns empty vector if not found.
    std::vector<uint8_t> find_ltpk(const std::vector<uint8_t>& pairing_id) const;

    // Forget a controller.
    bool remove(const std::vector<uint8_t>& pairing_id);

    // For settings UI: snapshot of all paired controller IDs.
    std::vector<std::vector<uint8_t>> list_ids() const;

    // Drop all in-memory + on-disk pairings. Used by --hap-pair-self-test.
    bool clear_all();

    static std::string file_path();

private:
    HapPairingStore() { reload(); }
    void reload();
    bool save_locked() const;

    mutable std::mutex mu_;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> rows_;
};

bool hap_pair_self_test();

} // namespace opm::airplay
