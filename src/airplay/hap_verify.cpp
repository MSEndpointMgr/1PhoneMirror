#include <opm/airplay/hap_verify.h>

#include <opm/airplay/hap_crypto.h>
#include <opm/airplay/hap_pairing.h>
#include <opm/airplay/tlv8.h>

#include <openssl/evp.h>

#include <cstring>
#include <iostream>
#include <mutex>

namespace opm::airplay {

namespace {

std::vector<uint8_t> err_tlv(uint8_t state, TlvError e) {
    TlvWriter w;
    w.add_u8(TlvType::State, state);
    w.add_u8(TlvType::Error, static_cast<uint8_t>(e));
    return w.take();
}

// Generate a fresh X25519 keypair. priv/pub each 32B. Returns false on error.
bool x25519_keygen(std::vector<uint8_t>& priv, std::vector<uint8_t>& pub) {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) return false;
    bool ok = EVP_PKEY_keygen_init(ctx) == 1 &&
              EVP_PKEY_keygen(ctx, &pkey) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !pkey) { if (pkey) EVP_PKEY_free(pkey); return false; }
    priv.resize(32); pub.resize(32);
    size_t plen = 32, ulen = 32;
    ok = EVP_PKEY_get_raw_private_key(pkey, priv.data(), &plen) == 1 &&
         EVP_PKEY_get_raw_public_key(pkey,  pub.data(),  &ulen) == 1 &&
         plen == 32 && ulen == 32;
    EVP_PKEY_free(pkey);
    return ok;
}

// shared = X25519(my_priv, peer_pub). 32B on success, empty on failure.
std::vector<uint8_t> x25519_derive(const std::vector<uint8_t>& my_priv,
                                   const std::vector<uint8_t>& peer_pub) {
    if (my_priv.size() != 32 || peer_pub.size() != 32) return {};
    EVP_PKEY* p = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                               my_priv.data(), 32);
    EVP_PKEY* q = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                              peer_pub.data(), 32);
    if (!p || !q) { EVP_PKEY_free(p); EVP_PKEY_free(q); return {}; }
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(p, nullptr);
    std::vector<uint8_t> out(32);
    size_t outlen = out.size();
    bool ok = ctx &&
              EVP_PKEY_derive_init(ctx) == 1 &&
              EVP_PKEY_derive_set_peer(ctx, q) == 1 &&
              EVP_PKEY_derive(ctx, out.data(), &outlen) == 1 &&
              outlen == 32;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(p);
    EVP_PKEY_free(q);
    return ok ? out : std::vector<uint8_t>{};
}

bool ed25519_verify(const std::vector<uint8_t>& pub,
                    const uint8_t* msg, size_t mlen,
                    const uint8_t* sig, size_t slen) {
    if (pub.size() != 32 || slen != 64) return false;
    EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                              pub.data(), 32);
    if (!k) return false;
    EVP_MD_CTX* mc = EVP_MD_CTX_new();
    bool ok = mc &&
              EVP_DigestVerifyInit(mc, nullptr, nullptr, nullptr, k) == 1 &&
              EVP_DigestVerify(mc, sig, slen, msg, mlen) == 1;
    EVP_MD_CTX_free(mc);
    EVP_PKEY_free(k);
    return ok;
}

} // namespace

struct HapPairVerify::Impl {
    HapDevice& device;
    std::mutex mu;
    enum class S { Idle, AwaitingM3, Done, Failed } state = S::Idle;

    // Per-session ephemeral X25519.
    std::vector<uint8_t> acc_priv;     // 32
    std::vector<uint8_t> acc_pub;      // 32
    std::vector<uint8_t> ctrl_pub;     // 32

    // Derived after M1.
    std::vector<uint8_t> shared;       // 32 — ECDH
    std::vector<uint8_t> session_key;  // 32 — Pair-Verify-Encrypt-{Salt,Info}

    // Set on M3 success.
    std::vector<uint8_t> ctrl_id;
    std::vector<uint8_t> read_key;     // server→client
    std::vector<uint8_t> write_key;    // client→server

    explicit Impl(HapDevice& d) : device(d) {}

    void clear() {
        state = S::Idle;
        acc_priv.clear(); acc_pub.clear(); ctrl_pub.clear();
        shared.clear(); session_key.clear();
        ctrl_id.clear(); read_key.clear(); write_key.clear();
    }
};

HapPairVerify::HapPairVerify(HapDevice& device)
    : impl_(std::make_unique<Impl>(device)) {}
HapPairVerify::~HapPairVerify() = default;

bool HapPairVerify::is_complete() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->state == Impl::S::Done;
}

void HapPairVerify::reset() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->clear();
}

const std::vector<uint8_t>& HapPairVerify::shared_secret() const { return impl_->shared; }
const std::vector<uint8_t>& HapPairVerify::read_key()      const { return impl_->read_key; }
const std::vector<uint8_t>& HapPairVerify::write_key()     const { return impl_->write_key; }
const std::vector<uint8_t>& HapPairVerify::controller_id() const { return impl_->ctrl_id; }

std::vector<uint8_t> HapPairVerify::handle(const std::vector<uint8_t>& in_tlv) {
    std::lock_guard<std::mutex> lk(impl_->mu);

    TlvReader rd;
    if (!rd.parse(in_tlv)) {
        std::cerr << "[HAP] pair-verify: TLV parse failed\n";
        impl_->clear();
        return err_tlv(0x02, TlvError::Unknown);
    }
    auto st = rd.get_u8(TlvType::State);
    if (!st) {
        std::cerr << "[HAP] pair-verify: missing State\n";
        impl_->clear();
        return err_tlv(0x02, TlvError::Unknown);
    }

    if (*st == 0x01) {
        // ---- M1 → M2 -------------------------------------------------------

        // If no controllers are paired yet, we MUST refuse pair-verify so
        // the iPad falls through to /pair-setup. Replying M2 successfully
        // would lead iPad to look up our pairing ID in its keychain, fail,
        // and give up (it then falls back to legacy /pair-pin-start, which
        // we don't implement). Returning Authentication here causes iPad
        // to retry on /pair-setup immediately.
        if (HapPairingStore::instance().list_ids().empty()) {
            std::cerr << "[HAP] pair-verify M1: no paired controllers; "
                         "returning Authentication so client switches to /pair-setup\n";
            impl_->clear();
            return err_tlv(0x02, TlvError::Authentication);
        }

        auto cpub = rd.get(TlvType::PublicKey);
        if (!cpub || cpub->size() != 32) {
            std::cerr << "[HAP] pair-verify M1: bad PublicKey\n";
            impl_->clear();
            return err_tlv(0x02, TlvError::Unknown);
        }

        impl_->clear();
        if (!x25519_keygen(impl_->acc_priv, impl_->acc_pub)) {
            std::cerr << "[HAP] pair-verify M1: X25519 keygen failed\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x02, TlvError::Unknown);
        }
        impl_->ctrl_pub = *cpub;
        impl_->shared = x25519_derive(impl_->acc_priv, impl_->ctrl_pub);
        if (impl_->shared.size() != 32) {
            std::cerr << "[HAP] pair-verify M1: ECDH failed\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x02, TlvError::Unknown);
        }

        impl_->session_key = hkdf_sha512(
            "Pair-Verify-Encrypt-Salt", impl_->shared,
            "Pair-Verify-Encrypt-Info", 32);
        if (impl_->session_key.size() != 32) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x02, TlvError::Unknown);
        }

        // AccessoryInfo = acc_pub || acc_pi || ctrl_pub  →  Ed25519 sign.
        const std::string& pi = impl_->device.pi();
        std::vector<uint8_t> acc_info;
        acc_info.reserve(32 + pi.size() + 32);
        acc_info.insert(acc_info.end(), impl_->acc_pub.begin(), impl_->acc_pub.end());
        acc_info.insert(acc_info.end(), pi.begin(), pi.end());
        acc_info.insert(acc_info.end(), impl_->ctrl_pub.begin(), impl_->ctrl_pub.end());

        auto acc_sig = impl_->device.sign(acc_info.data(), acc_info.size());
        if (acc_sig.size() != 64) {
            std::cerr << "[HAP] pair-verify M1: accessory sign failed\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x02, TlvError::Unknown);
        }

        TlvWriter sw;
        sw.add_string(TlvType::Identifier, pi);
        sw.add(TlvType::Signature, acc_sig);
        auto sub = sw.take();

        uint8_t nonce[12]; hap_nonce_from_tag("PV-Msg02", nonce);
        auto enc = chacha20_poly1305_encrypt(
            impl_->session_key.data(), nonce, nullptr, 0,
            sub.data(), sub.size());
        if (enc.empty()) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x02, TlvError::Unknown);
        }

        TlvWriter ow;
        ow.add_u8(TlvType::State, 0x02);
        ow.add(TlvType::PublicKey, impl_->acc_pub);
        ow.add(TlvType::EncryptedData, enc);
        impl_->state = Impl::S::AwaitingM3;
        std::cout << "[HAP] pair-verify M1 → M2 (ECDH OK, awaiting M3)\n";
        return ow.take();
    }

    if (*st == 0x03) {
        // ---- M3 → M4 -------------------------------------------------------
        if (impl_->state != Impl::S::AwaitingM3) {
            std::cerr << "[HAP] pair-verify M3: out-of-order (state="
                      << (int)impl_->state << ")\n";
            impl_->clear();
            return err_tlv(0x04, TlvError::Unknown);
        }
        auto enc = rd.get(TlvType::EncryptedData);
        if (!enc || enc->size() < 16) {
            std::cerr << "[HAP] pair-verify M3: missing EncryptedData\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Authentication);
        }
        uint8_t nonce[12]; hap_nonce_from_tag("PV-Msg03", nonce);
        auto plain = chacha20_poly1305_decrypt(
            impl_->session_key.data(), nonce, nullptr, 0,
            enc->data(), enc->size());
        if (plain.empty()) {
            std::cerr << "[HAP] pair-verify M3: AEAD auth failed\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Authentication);
        }

        TlvReader sub;
        if (!sub.parse(plain)) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Authentication);
        }
        auto cid = sub.get(TlvType::Identifier);
        auto csig = sub.get(TlvType::Signature);
        if (!cid || !csig || csig->size() != 64) {
            std::cerr << "[HAP] pair-verify M3: missing sub-TLV fields\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Authentication);
        }

        auto ctrl_ltpk = HapPairingStore::instance().find_ltpk(*cid);
        if (ctrl_ltpk.size() != 32) {
            std::cerr << "[HAP] pair-verify M3: unknown controller id ("
                      << cid->size() << "B)\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Authentication);
        }

        // ControllerInfo = ctrl_pub || ctrl_id || acc_pub
        std::vector<uint8_t> ctrl_info;
        ctrl_info.reserve(32 + cid->size() + 32);
        ctrl_info.insert(ctrl_info.end(), impl_->ctrl_pub.begin(), impl_->ctrl_pub.end());
        ctrl_info.insert(ctrl_info.end(), cid->begin(), cid->end());
        ctrl_info.insert(ctrl_info.end(), impl_->acc_pub.begin(), impl_->acc_pub.end());

        if (!ed25519_verify(ctrl_ltpk,
                            ctrl_info.data(), ctrl_info.size(),
                            csig->data(), csig->size())) {
            std::cerr << "[HAP] pair-verify M3: Ed25519 signature invalid\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Authentication);
        }

        // Derive RTSP/event-channel keys for HAP M6 encrypted framing.
        impl_->read_key = hkdf_sha512(
            "Control-Salt", impl_->shared, "Control-Read-Encryption-Key", 32);
        impl_->write_key = hkdf_sha512(
            "Control-Salt", impl_->shared, "Control-Write-Encryption-Key", 32);
        if (impl_->read_key.size() != 32 || impl_->write_key.size() != 32) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Unknown);
        }
        impl_->ctrl_id = *cid;
        impl_->state = Impl::S::Done;

        TlvWriter ow;
        ow.add_u8(TlvType::State, 0x04);
        std::cout << "[HAP] pair-verify M3 → M4 (verified controller, "
                  << "session keys derived)\n";
        return ow.take();
    }

    std::cerr << "[HAP] pair-verify: unexpected State 0x"
              << std::hex << (int)*st << std::dec << "\n";
    impl_->clear();
    return err_tlv(0x02, TlvError::Unknown);
}

// ----------------------------------------------------------------------------
// Self-test: M1↔M4 round-trip against a synthetic paired controller.
// ----------------------------------------------------------------------------

namespace {

bool ed25519_keygen_seed(std::vector<uint8_t>& seed, std::vector<uint8_t>& pub) {
    EVP_PKEY* p = nullptr;
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!c) return false;
    bool ok = EVP_PKEY_keygen_init(c) == 1 && EVP_PKEY_keygen(c, &p) == 1;
    EVP_PKEY_CTX_free(c);
    if (!ok) { if (p) EVP_PKEY_free(p); return false; }
    seed.resize(32); pub.resize(32);
    size_t sl = 32, pl = 32;
    ok = EVP_PKEY_get_raw_private_key(p, seed.data(), &sl) == 1 &&
         EVP_PKEY_get_raw_public_key(p, pub.data(), &pl) == 1;
    EVP_PKEY_free(p);
    return ok;
}

std::vector<uint8_t> ed25519_sign_seed(const std::vector<uint8_t>& seed,
                                       const uint8_t* msg, size_t len) {
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               seed.data(), 32);
    if (!k) return {};
    EVP_MD_CTX* mc = EVP_MD_CTX_new();
    std::vector<uint8_t> sig(64); size_t sl = 64;
    bool ok = mc &&
              EVP_DigestSignInit(mc, nullptr, nullptr, nullptr, k) == 1 &&
              EVP_DigestSign(mc, sig.data(), &sl, msg, len) == 1 && sl == 64;
    EVP_MD_CTX_free(mc);
    EVP_PKEY_free(k);
    return ok ? sig : std::vector<uint8_t>{};
}

} // namespace

bool hap_verify_self_test() {
    HapDevice dev;
    if (!dev.load_or_create()) {
        std::cerr << "[HAP-verify-test] HapDevice load_or_create failed\n";
        return false;
    }

    // Synthetic controller identity, pre-registered in the pairing store.
    std::vector<uint8_t> ctrl_seed, ctrl_pub_ltpk;
    if (!ed25519_keygen_seed(ctrl_seed, ctrl_pub_ltpk)) return false;
    std::vector<uint8_t> ctrl_id = {'V','E','R','I','F','Y','-','S','E','L','F'};

    auto& store = HapPairingStore::instance();
    if (!store.add(ctrl_id, ctrl_pub_ltpk)) {
        std::cerr << "[HAP-verify-test] store.add failed\n";
        return false;
    }

    HapPairVerify pv(dev);

    // -- Client M1: { State=1, PublicKey=client_curve25519_pub }
    std::vector<uint8_t> cli_priv, cli_pub;
    if (!x25519_keygen(cli_priv, cli_pub)) {
        std::cerr << "[HAP-verify-test] client X25519 keygen failed\n";
        store.remove(ctrl_id);
        return false;
    }
    TlvWriter m1;
    m1.add_u8(TlvType::State, 0x01);
    m1.add(TlvType::PublicKey, cli_pub);
    auto m2 = pv.handle(m1.take());

    TlvReader rd2;
    if (!rd2.parse(m2)) { store.remove(ctrl_id); return false; }
    auto st2 = rd2.get_u8(TlvType::State);
    auto acc_pub = rd2.get(TlvType::PublicKey);
    auto enc2 = rd2.get(TlvType::EncryptedData);
    if (!st2 || *st2 != 0x02 || !acc_pub || acc_pub->size() != 32 ||
        !enc2 || enc2->size() < 16) {
        std::cerr << "[HAP-verify-test] M2 malformed\n";
        store.remove(ctrl_id);
        return false;
    }

    // Client derives shared + session key, decrypts M2 sub-TLV, validates sig.
    auto shared = x25519_derive(cli_priv, *acc_pub);
    if (shared.size() != 32) { store.remove(ctrl_id); return false; }
    auto sess = hkdf_sha512("Pair-Verify-Encrypt-Salt", shared,
                            "Pair-Verify-Encrypt-Info", 32);
    if (sess.size() != 32) { store.remove(ctrl_id); return false; }

    uint8_t n2[12]; hap_nonce_from_tag("PV-Msg02", n2);
    auto sub2 = chacha20_poly1305_decrypt(sess.data(), n2, nullptr, 0,
                                          enc2->data(), enc2->size());
    if (sub2.empty()) {
        std::cerr << "[HAP-verify-test] M2 AEAD decrypt failed\n";
        store.remove(ctrl_id);
        return false;
    }
    TlvReader sr2;
    if (!sr2.parse(sub2)) { store.remove(ctrl_id); return false; }
    auto acc_id = sr2.get(TlvType::Identifier);
    auto acc_sig = sr2.get(TlvType::Signature);
    if (!acc_id || !acc_sig || acc_sig->size() != 64) {
        store.remove(ctrl_id);
        return false;
    }

    // -- Client M3: sign ctrl_pub || ctrl_id || acc_pub with our LTSK seed,
    //               encrypt sub-TLV, send.
    std::vector<uint8_t> ctrl_info;
    ctrl_info.insert(ctrl_info.end(), cli_pub.begin(), cli_pub.end());
    ctrl_info.insert(ctrl_info.end(), ctrl_id.begin(), ctrl_id.end());
    ctrl_info.insert(ctrl_info.end(), acc_pub->begin(), acc_pub->end());
    auto ctrl_sig = ed25519_sign_seed(ctrl_seed, ctrl_info.data(), ctrl_info.size());
    if (ctrl_sig.size() != 64) { store.remove(ctrl_id); return false; }

    TlvWriter sub3;
    sub3.add(TlvType::Identifier, ctrl_id);
    sub3.add(TlvType::Signature, ctrl_sig);
    auto sub3_b = sub3.take();
    uint8_t n3[12]; hap_nonce_from_tag("PV-Msg03", n3);
    auto enc3 = chacha20_poly1305_encrypt(sess.data(), n3, nullptr, 0,
                                          sub3_b.data(), sub3_b.size());
    if (enc3.empty()) { store.remove(ctrl_id); return false; }

    TlvWriter m3;
    m3.add_u8(TlvType::State, 0x03);
    m3.add(TlvType::EncryptedData, enc3);
    auto m4 = pv.handle(m3.take());

    TlvReader rd4;
    if (!rd4.parse(m4)) { store.remove(ctrl_id); return false; }
    auto st4 = rd4.get_u8(TlvType::State);
    if (!st4 || *st4 != 0x04 || rd4.has(TlvType::Error)) {
        std::cerr << "[HAP-verify-test] M4 not clean (state=" << (st4 ? (int)*st4 : -1)
                  << " hasErr=" << rd4.has(TlvType::Error) << ")\n";
        store.remove(ctrl_id);
        return false;
    }

    if (!pv.is_complete() ||
        pv.read_key().size()  != 32 ||
        pv.write_key().size() != 32 ||
        pv.shared_secret()    != shared ||
        pv.controller_id()    != ctrl_id) {
        std::cerr << "[HAP-verify-test] post-condition mismatch\n";
        store.remove(ctrl_id);
        return false;
    }

    store.remove(ctrl_id);
    std::cout << "[HAP-verify-test] OK — M1→M4 round-trip + key derivation.\n";
    return true;
}

} // namespace opm::airplay
