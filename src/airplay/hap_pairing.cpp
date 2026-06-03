#include <opm/airplay/hap_pairing.h>

#include <opm/airplay/hap_crypto.h>
#include <opm/airplay/tlv8.h>

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/bn.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace opm::airplay {

namespace {

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string out(n * 2, '\0');
    for (size_t i = 0; i < n; ++i) {
        out[2*i]   = h[p[i] >> 4];
        out[2*i+1] = h[p[i] & 0xf];
    }
    return out;
}

bool from_hex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.resize(s.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = nyb(s[2*i]), lo = nyb(s[2*i+1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string appdata_dir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path) != S_OK)
        return {};
    char buf[MAX_PATH * 2] = {};
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, buf, sizeof(buf), nullptr, nullptr);
    if (n <= 0) return {};
    return std::string(buf) + "\\1PhoneMirror";
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/1PhoneMirror";
#endif
}

// Helper: build error TLV.
std::vector<uint8_t> err_tlv(uint8_t state, TlvError e) {
    TlvWriter w;
    w.add_u8(TlvType::State, state);
    w.add_u8(TlvType::Error, static_cast<uint8_t>(e));
    return w.take();
}

// Ed25519 sign with raw 32-byte seed.
std::vector<uint8_t> ed25519_sign(const std::vector<uint8_t>& seed,
                                   const uint8_t* msg, size_t len) {
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               seed.data(), seed.size());
    if (!k) return {};
    EVP_MD_CTX* mc = EVP_MD_CTX_new();
    std::vector<uint8_t> sig(64);
    size_t slen = sig.size();
    bool ok = mc &&
              EVP_DigestSignInit(mc, nullptr, nullptr, nullptr, k) == 1 &&
              EVP_DigestSign(mc, sig.data(), &slen, msg, len) == 1 &&
              slen == 64;
    EVP_MD_CTX_free(mc);
    EVP_PKEY_free(k);
    return ok ? sig : std::vector<uint8_t>{};
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

// ----------------------------------------------------------------------------
// HapPairingStore — persistence at %APPDATA%\1PhoneMirror\hap_pairings.txt
// ----------------------------------------------------------------------------

HapPairingStore& HapPairingStore::instance() {
    static HapPairingStore s;
    return s;
}

std::string HapPairingStore::file_path() {
    auto d = appdata_dir();
    return d.empty() ? std::string() : d + "/hap_pairings.txt";
}

void HapPairingStore::reload() {
    std::lock_guard<std::mutex> lk(mu_);
    rows_.clear();
    auto path = file_path();
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::vector<uint8_t> id, pk;
        if (from_hex(line.substr(0, eq), id) &&
            from_hex(line.substr(eq + 1), pk) &&
            pk.size() == 32 && !id.empty()) {
            rows_.emplace_back(std::move(id), std::move(pk));
        }
    }
}

bool HapPairingStore::save_locked() const {
    auto path = file_path();
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# 1PhoneMirror HomeKit pairings. Delete to forget all controllers.\n";
    for (const auto& [id, pk] : rows_) {
        f << to_hex(id.data(), id.size()) << "="
          << to_hex(pk.data(), pk.size()) << "\n";
    }
    return static_cast<bool>(f);
}

bool HapPairingStore::add(const std::vector<uint8_t>& id,
                           const std::vector<uint8_t>& pk) {
    if (id.empty() || pk.size() != 32) return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(rows_.begin(), rows_.end(),
                           [&](auto& r) { return r.first == id; });
    if (it == rows_.end()) rows_.emplace_back(id, pk);
    else                   it->second = pk;
    return save_locked();
}

std::vector<uint8_t> HapPairingStore::find_ltpk(
    const std::vector<uint8_t>& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(rows_.begin(), rows_.end(),
                           [&](auto& r) { return r.first == id; });
    return it == rows_.end() ? std::vector<uint8_t>{} : it->second;
}

bool HapPairingStore::remove(const std::vector<uint8_t>& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(rows_.begin(), rows_.end(),
                           [&](auto& r) { return r.first == id; });
    if (it == rows_.end()) return false;
    rows_.erase(it);
    return save_locked();
}

std::vector<std::vector<uint8_t>> HapPairingStore::list_ids() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::vector<uint8_t>> out;
    out.reserve(rows_.size());
    for (auto& r : rows_) out.push_back(r.first);
    return out;
}

bool HapPairingStore::clear_all() {
    std::lock_guard<std::mutex> lk(mu_);
    rows_.clear();
    return save_locked();
}

// ----------------------------------------------------------------------------
// HapPairSetup state machine
// ----------------------------------------------------------------------------

struct HapPairSetup::Impl {
    HapDevice& device;
    std::mutex mu;
    enum class S { Idle, AwaitingM3, AwaitingM5, Done, Failed } state = S::Idle;
    HapSrpServer srp;
    std::vector<uint8_t> session_K; // 64-byte SHA-512(S)
    explicit Impl(HapDevice& d) : device(d) {}
};

HapPairSetup::HapPairSetup(HapDevice& device)
    : impl_(std::make_unique<Impl>(device)) {}

HapPairSetup::~HapPairSetup() = default;

bool HapPairSetup::is_complete() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->state == Impl::S::Done;
}

void HapPairSetup::reset() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->state = Impl::S::Idle;
    impl_->session_K.clear();
    // srp is reinitialized by start() on next M1
}

std::vector<uint8_t> HapPairSetup::handle(const std::vector<uint8_t>& in_tlv,
                                           const std::string& setup_code) {
    std::lock_guard<std::mutex> lk(impl_->mu);

    TlvReader r;
    if (!r.parse(in_tlv)) {
        std::cerr << "[HAP] pair-setup: malformed TLV (" << in_tlv.size() << " B)\n";
        impl_->state = Impl::S::Failed;
        return err_tlv(0x02, TlvError::Unknown);
    }
    auto state_opt = r.get_u8(TlvType::State);
    if (!state_opt) {
        impl_->state = Impl::S::Failed;
        return err_tlv(0x02, TlvError::Unknown);
    }
    uint8_t client_state = *state_opt;

    // ---- M1 ----------------------------------------------------------------
    if (client_state == 0x01) {
        if (setup_code.empty()) {
            std::cerr << "[HAP] pair-setup M1: no setup code available\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x02, TlvError::Authentication);
        }
        std::vector<uint8_t> salt, B;
        if (!impl_->srp.start(setup_code, salt, B)) {
            std::cerr << "[HAP] pair-setup M1: SRP start failed\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x02, TlvError::Unknown);
        }
        impl_->state = Impl::S::AwaitingM3;
        std::cout << "[HAP] pair-setup M1 → M2 (salt+B), code='" << setup_code << "'\n";
        TlvWriter w;
        w.add_u8(TlvType::State, 0x02);
        w.add(TlvType::PublicKey, B);
        w.add(TlvType::Salt, salt);
        return w.take();
    }

    // ---- M3 ----------------------------------------------------------------
    if (client_state == 0x03) {
        if (impl_->state != Impl::S::AwaitingM3) {
            std::cerr << "[HAP] pair-setup M3 out of order (state="
                      << static_cast<int>(impl_->state) << ")\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Unknown);
        }
        auto A_opt = r.get(TlvType::PublicKey);
        auto M_opt = r.get(TlvType::Proof);
        if (!A_opt || !M_opt) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Authentication);
        }
        auto M2 = impl_->srp.verify_client_proof(*A_opt, *M_opt);
        if (M2.empty()) {
            std::cerr << "[HAP] pair-setup M3: client proof rejected (wrong setup code?)\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x04, TlvError::Authentication);
        }
        impl_->session_K = impl_->srp.session_key();
        impl_->state = Impl::S::AwaitingM5;
        std::cout << "[HAP] pair-setup M3 → M4 (proof verified, K derived)\n";
        TlvWriter w;
        w.add_u8(TlvType::State, 0x04);
        w.add(TlvType::Proof, M2);
        return w.take();
    }

    // ---- M5 ----------------------------------------------------------------
    if (client_state == 0x05) {
        if (impl_->state != Impl::S::AwaitingM5 || impl_->session_K.size() != 64) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Unknown);
        }
        auto enc_opt = r.get(TlvType::EncryptedData);
        if (!enc_opt || enc_opt->size() < 16) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Authentication);
        }

        // Derive ChaCha20-Poly1305 key from SRP session key.
        auto session_key = hkdf_sha512(
            "Pair-Setup-Encrypt-Salt", impl_->session_K,
            "Pair-Setup-Encrypt-Info", 32);
        if (session_key.size() != 32) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Unknown);
        }
        uint8_t nonce[12]; hap_nonce_from_tag("PS-Msg05", nonce);
        auto sub_plain = chacha20_poly1305_decrypt(
            session_key.data(), nonce, nullptr, 0,
            enc_opt->data(), enc_opt->size());
        if (sub_plain.empty()) {
            std::cerr << "[HAP] pair-setup M5: ChaCha20-Poly1305 auth failed\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Authentication);
        }

        TlvReader sub;
        if (!sub.parse(sub_plain)) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Authentication);
        }
        auto id_opt  = sub.get(TlvType::Identifier);
        auto pub_opt = sub.get(TlvType::PublicKey);
        auto sig_opt = sub.get(TlvType::Signature);
        if (!id_opt || !pub_opt || !sig_opt ||
            pub_opt->size() != 32 || sig_opt->size() != 64) {
            std::cerr << "[HAP] pair-setup M5: sub-TLV missing fields\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Authentication);
        }

        // Verify iOS device signature.
        //   iOSDeviceX = HKDF-SHA512(salt="Pair-Setup-Controller-Sign-Salt",
        //                            ikm=K, info="Pair-Setup-Controller-Sign-Info", L=32)
        //   message    = iOSDeviceX || iOSDevicePairingID || iOSDeviceLTPK
        auto iOSDeviceX = hkdf_sha512(
            "Pair-Setup-Controller-Sign-Salt", impl_->session_K,
            "Pair-Setup-Controller-Sign-Info", 32);
        std::vector<uint8_t> msg;
        msg.reserve(32 + id_opt->size() + 32);
        msg.insert(msg.end(), iOSDeviceX.begin(), iOSDeviceX.end());
        msg.insert(msg.end(), id_opt->begin(),    id_opt->end());
        msg.insert(msg.end(), pub_opt->begin(),   pub_opt->end());

        if (!ed25519_verify(*pub_opt, msg.data(), msg.size(),
                            sig_opt->data(), sig_opt->size())) {
            std::cerr << "[HAP] pair-setup M5: iOS device signature INVALID\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Authentication);
        }

        // Persist (id → ltpk). On failure we still complete the handshake;
        // the controller will just need to re-pair later.
        if (!HapPairingStore::instance().add(*id_opt, *pub_opt)) {
            std::cerr << "[HAP] pair-setup M5: failed to persist pairing record\n";
        } else {
            std::cout << "[HAP] pair-setup: controller paired (id_len="
                      << id_opt->size() << ", ltpk="
                      << to_hex(pub_opt->data(), 8) << "...)\n";
        }

        // ---- Build M6 -----------------------------------------------------
        // AccessoryX = HKDF-SHA512(salt="Pair-Setup-Accessory-Sign-Salt",
        //                          ikm=K, info="Pair-Setup-Accessory-Sign-Info", L=32)
        // message    = AccessoryX || AccessoryPairingID || AccessoryLTPK
        // signature  = Ed25519-Sign(AccessoryLTSK, message)
        auto AccessoryX = hkdf_sha512(
            "Pair-Setup-Accessory-Sign-Salt", impl_->session_K,
            "Pair-Setup-Accessory-Sign-Info", 32);

        // AccessoryPairingID = ASCII bytes of our pi (e.g. "62:7f:f7:78:9b:96").
        const std::string& pi = impl_->device.pi();
        std::vector<uint8_t> pi_bytes(pi.begin(), pi.end());
        const auto& ltpk = impl_->device.ltpk();

        std::vector<uint8_t> acc_msg;
        acc_msg.reserve(32 + pi_bytes.size() + 32);
        acc_msg.insert(acc_msg.end(), AccessoryX.begin(), AccessoryX.end());
        acc_msg.insert(acc_msg.end(), pi_bytes.begin(),   pi_bytes.end());
        acc_msg.insert(acc_msg.end(), ltpk.begin(),       ltpk.end());

        // Sign — we use HapDevice::sign which uses our persisted LTSK seed.
        auto acc_sig = impl_->device.sign(acc_msg.data(), acc_msg.size());
        if (acc_sig.size() != 64) {
            std::cerr << "[HAP] pair-setup M5: accessory sign failed\n";
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Unknown);
        }

        TlvWriter sw;
        sw.add(TlvType::Identifier, pi_bytes);
        sw.add(TlvType::PublicKey,  ltpk);
        sw.add(TlvType::Signature,  acc_sig);
        auto sub_resp = sw.take();

        uint8_t nonce6[12]; hap_nonce_from_tag("PS-Msg06", nonce6);
        auto enc_resp = chacha20_poly1305_encrypt(
            session_key.data(), nonce6, nullptr, 0,
            sub_resp.data(), sub_resp.size());
        if (enc_resp.empty()) {
            impl_->state = Impl::S::Failed;
            return err_tlv(0x06, TlvError::Unknown);
        }

        TlvWriter ow;
        ow.add_u8(TlvType::State, 0x06);
        ow.add(TlvType::EncryptedData, enc_resp);
        impl_->state = Impl::S::Done;
        std::cout << "[HAP] pair-setup M5 → M6 (accessory paired, "
                  << HapPairingStore::instance().list_ids().size()
                  << " total controllers)\n";
        return ow.take();
    }

    std::cerr << "[HAP] pair-setup: unexpected state byte 0x"
              << std::hex << static_cast<int>(client_state) << std::dec << "\n";
    impl_->state = Impl::S::Failed;
    return err_tlv(0x02, TlvError::Unknown);
}

// ----------------------------------------------------------------------------
// Self-test: full M1↔M6 round-trip, then a wrong-code attempt, then verify
// the pairing record survived on disk.
// ----------------------------------------------------------------------------

namespace {

// Build the iOS-side request TLVs by running through the protocol with
// a freshly-generated controller Ed25519 LTKP + a UUID-style identifier.
struct TestController {
    std::vector<uint8_t> seed;
    std::vector<uint8_t> pub;
    std::vector<uint8_t> id; // "iOS-TEST-CONTROLLER" raw bytes

    bool init() {
        EVP_PKEY* k = nullptr;
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        bool ok = c && EVP_PKEY_keygen_init(c) == 1 && EVP_PKEY_keygen(c, &k) == 1;
        if (c) EVP_PKEY_CTX_free(c);
        if (!ok || !k) { if (k) EVP_PKEY_free(k); return false; }
        size_t n = 32;
        seed.assign(32, 0); pub.assign(32, 0);
        ok = (EVP_PKEY_get_raw_private_key(k, seed.data(), &n) == 1 && n == 32);
        n = 32;
        ok = ok && (EVP_PKEY_get_raw_public_key(k, pub.data(), &n) == 1 && n == 32);
        EVP_PKEY_free(k);
        const char* s = "iOS-TEST-CONTROLLER";
        id.assign(reinterpret_cast<const uint8_t*>(s),
                  reinterpret_cast<const uint8_t*>(s) + std::strlen(s));
        return ok;
    }
};

} // namespace

bool hap_pair_self_test() {
    using std::cerr;

    // We exercise the state machine in-process. Real pairings would clutter
    // the user's persistent store, so wipe it at start and end.
    HapPairingStore::instance().clear_all();

    HapDevice dev;
    if (!dev.load_or_create()) { cerr << "[HAP-PAIR] device load failed\n"; return false; }

    HapPairSetup srv(dev);
    const std::string code = "031-45-154";

    // ---- M1 ----------------------------------------------------------------
    {
        TlvWriter w;
        w.add_u8(TlvType::State, 0x01);
        w.add_u8(TlvType::Method, 0x00); // PairSetup
        auto m2 = srv.handle(w.take(), code);
        TlvReader r; if (!r.parse(m2)) { cerr << "[HAP-PAIR] M2 parse failed\n"; return false; }
        auto st = r.get_u8(TlvType::State);
        if (!st || *st != 0x02) { cerr << "[HAP-PAIR] M2 wrong state\n"; return false; }
        if (r.has(TlvType::Error)) { cerr << "[HAP-PAIR] M2 carried error\n"; return false; }
    }
    // We can't drive M3/M5 without a real SRP client and the server's B/salt,
    // so we re-issue M1 to capture them, then walk the rest. This is a
    // limitation of testing through the public API — a deeper test would
    // require exposing internals. Instead we run a second handler instance.

    HapPairSetup srv2(dev);
    std::vector<uint8_t> B, salt;
    std::vector<uint8_t> M2bytes;
    {
        TlvWriter w;
        w.add_u8(TlvType::State, 0x01);
        w.add_u8(TlvType::Method, 0x00);
        M2bytes = srv2.handle(w.take(), code);
        TlvReader r; r.parse(M2bytes);
        auto Bo = r.get(TlvType::PublicKey);
        auto so = r.get(TlvType::Salt);
        if (!Bo || !so || Bo->size() != 384 || so->size() != 16) {
            cerr << "[HAP-PAIR] M2 missing B/salt (B="
                 << (Bo ? Bo->size() : 0) << " salt=" << (so ? so->size() : 0) << ")\n";
            return false;
        }
        B = *Bo; salt = *so;
    }

    // ---- Client side: compute A, M1_proof using the same SRP math --------
    // We piggyback on the SRP self-test plumbing by reproducing the
    // computations here. This mirrors hap_srp_self_test()'s client side.
    BIGNUM* N = nullptr; BN_hex2bn(&N,
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
        "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF");
    BIGNUM* g = BN_new(); BN_set_word(g, 5);
    BN_CTX* ctx = BN_CTX_new();

    // x = H(salt | H(I:p))
    SHA512_CTX h1; SHA512_Init(&h1);
    const std::string I = "Pair-Setup";
    std::string ip = I + ":" + code;
    SHA512_Update(&h1, ip.data(), ip.size());
    uint8_t inner[64]; SHA512_Final(inner, &h1);
    SHA512_CTX h2; SHA512_Init(&h2);
    SHA512_Update(&h2, salt.data(), salt.size());
    SHA512_Update(&h2, inner, 64);
    uint8_t xbuf[64]; SHA512_Final(xbuf, &h2);
    BIGNUM* x = BN_bin2bn(xbuf, 64, nullptr);

    // a, A = g^a mod N
    BIGNUM* a = BN_new(); BN_rand(a, 256, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY);
    BIGNUM* A = BN_new(); BN_mod_exp(A, g, a, N, ctx);
    std::vector<uint8_t> Abytes(384, 0);
    BN_bn2bin(A, Abytes.data() + (384 - BN_num_bytes(A)));

    // k = H(N | PAD(g)); kgx, base
    std::vector<uint8_t> Npad(384, 0), gpad(384, 0);
    BN_bn2bin(N, Npad.data() + (384 - BN_num_bytes(N)));
    BN_bn2bin(g, gpad.data() + (384 - BN_num_bytes(g)));
    SHA512_CTX hk; SHA512_Init(&hk);
    SHA512_Update(&hk, Npad.data(), 384);
    SHA512_Update(&hk, gpad.data(), 384);
    uint8_t kbuf[64]; SHA512_Final(kbuf, &hk);
    BIGNUM* k = BN_bin2bn(kbuf, 64, nullptr);

    // u = H(PAD(A) | PAD(B))
    SHA512_CTX hu; SHA512_Init(&hu);
    SHA512_Update(&hu, Abytes.data(), 384);
    SHA512_Update(&hu, B.data(), 384);
    uint8_t ubuf[64]; SHA512_Final(ubuf, &hu);
    BIGNUM* u = BN_bin2bn(ubuf, 64, nullptr);

    // S = (B - k*g^x) ^ (a + u*x) mod N
    BIGNUM* gx = BN_new(); BN_mod_exp(gx, g, x, N, ctx);
    BIGNUM* kgx = BN_new(); BN_mod_mul(kgx, k, gx, N, ctx);
    BIGNUM* Bn = BN_bin2bn(B.data(), 384, nullptr);
    BIGNUM* base = BN_new(); BN_mod_sub(base, Bn, kgx, N, ctx);
    BIGNUM* ux = BN_new(); BN_mul(ux, u, x, ctx);
    BIGNUM* exp = BN_new(); BN_add(exp, a, ux);
    BIGNUM* S = BN_new(); BN_mod_exp(S, base, exp, N, ctx);
    std::vector<uint8_t> Sbytes(384, 0);
    BN_bn2bin(S, Sbytes.data() + (384 - BN_num_bytes(S)));
    uint8_t Kbuf[64]; SHA512(Sbytes.data(), 384, Kbuf);
    std::vector<uint8_t> K(Kbuf, Kbuf + 64);

    // M1 = H( H(N) XOR H(g) | H(I) | salt | A | B | K )
    uint8_t hN[64], hg[64], hI[64];
    SHA512(Npad.data(), 384, hN);
    SHA512(gpad.data(), 384, hg);
    SHA512(reinterpret_cast<const uint8_t*>(I.data()), I.size(), hI);
    uint8_t xorbuf[64];
    for (int i = 0; i < 64; ++i) xorbuf[i] = hN[i] ^ hg[i];
    SHA512_CTX hm; SHA512_Init(&hm);
    SHA512_Update(&hm, xorbuf, 64);
    SHA512_Update(&hm, hI, 64);
    SHA512_Update(&hm, salt.data(), salt.size());
    SHA512_Update(&hm, Abytes.data(), 384);
    SHA512_Update(&hm, B.data(), 384);
    SHA512_Update(&hm, K.data(), 64);
    uint8_t M1[64]; SHA512_Final(M1, &hm);

    // Free BN scratch.
    BN_free(N); BN_free(g); BN_free(x); BN_free(a); BN_free(A); BN_free(k);
    BN_free(u); BN_free(gx); BN_free(kgx); BN_free(Bn); BN_free(base);
    BN_free(ux); BN_free(exp); BN_free(S);
    BN_CTX_free(ctx);

    // ---- M3 → M4 ----------------------------------------------------------
    {
        TlvWriter w;
        w.add_u8(TlvType::State, 0x03);
        w.add(TlvType::PublicKey, Abytes);
        w.add(TlvType::Proof, std::vector<uint8_t>(M1, M1 + 64));
        auto m4 = srv2.handle(w.take(), code);
        TlvReader r; if (!r.parse(m4)) { cerr << "[HAP-PAIR] M4 parse failed\n"; return false; }
        auto st = r.get_u8(TlvType::State);
        if (!st || *st != 0x04) {
            if (r.has(TlvType::Error)) {
                auto e = r.get_u8(TlvType::Error);
                cerr << "[HAP-PAIR] M4 error=" << (e ? int(*e) : -1) << "\n";
            } else {
                cerr << "[HAP-PAIR] M4 wrong state\n";
            }
            return false;
        }
    }

    // ---- M5 → M6 ----------------------------------------------------------
    TestController ctl;
    if (!ctl.init()) { cerr << "[HAP-PAIR] controller keygen failed\n"; return false; }

    auto session_key = hkdf_sha512(
        "Pair-Setup-Encrypt-Salt", K,
        "Pair-Setup-Encrypt-Info", 32);
    auto iOSDeviceX = hkdf_sha512(
        "Pair-Setup-Controller-Sign-Salt", K,
        "Pair-Setup-Controller-Sign-Info", 32);

    // Sign iOSDeviceX || id || pub  with controller LTSK.
    std::vector<uint8_t> sigmsg;
    sigmsg.insert(sigmsg.end(), iOSDeviceX.begin(), iOSDeviceX.end());
    sigmsg.insert(sigmsg.end(), ctl.id.begin(),     ctl.id.end());
    sigmsg.insert(sigmsg.end(), ctl.pub.begin(),    ctl.pub.end());
    auto ctl_sig = ed25519_sign(ctl.seed, sigmsg.data(), sigmsg.size());
    if (ctl_sig.size() != 64) { cerr << "[HAP-PAIR] controller sign failed\n"; return false; }

    TlvWriter sub;
    sub.add(TlvType::Identifier, ctl.id);
    sub.add(TlvType::PublicKey,  ctl.pub);
    sub.add(TlvType::Signature,  ctl_sig);
    auto sub_plain = sub.take();

    uint8_t n5[12]; hap_nonce_from_tag("PS-Msg05", n5);
    auto sub_enc = chacha20_poly1305_encrypt(
        session_key.data(), n5, nullptr, 0,
        sub_plain.data(), sub_plain.size());

    TlvWriter w5;
    w5.add_u8(TlvType::State, 0x05);
    w5.add(TlvType::EncryptedData, sub_enc);
    auto m6 = srv2.handle(w5.take(), code);

    TlvReader r6; if (!r6.parse(m6)) { cerr << "[HAP-PAIR] M6 parse failed\n"; return false; }
    auto st6 = r6.get_u8(TlvType::State);
    if (!st6 || *st6 != 0x06) {
        if (r6.has(TlvType::Error)) {
            auto e = r6.get_u8(TlvType::Error);
            cerr << "[HAP-PAIR] M6 error=" << (e ? int(*e) : -1) << "\n";
        } else {
            cerr << "[HAP-PAIR] M6 wrong state\n";
        }
        return false;
    }
    auto enc6 = r6.get(TlvType::EncryptedData);
    if (!enc6) { cerr << "[HAP-PAIR] M6 missing EncryptedData\n"; return false; }

    // Decrypt M6 sub-TLV and verify accessory signature.
    uint8_t n6[12]; hap_nonce_from_tag("PS-Msg06", n6);
    auto m6_plain = chacha20_poly1305_decrypt(
        session_key.data(), n6, nullptr, 0,
        enc6->data(), enc6->size());
    if (m6_plain.empty()) { cerr << "[HAP-PAIR] M6 decrypt failed\n"; return false; }
    TlvReader sub6; sub6.parse(m6_plain);
    auto acc_id  = sub6.get(TlvType::Identifier);
    auto acc_pub = sub6.get(TlvType::PublicKey);
    auto acc_sig = sub6.get(TlvType::Signature);
    if (!acc_id || !acc_pub || !acc_sig ||
        acc_pub->size() != 32 || acc_sig->size() != 64) {
        cerr << "[HAP-PAIR] M6 sub-TLV malformed\n"; return false;
    }
    auto AccessoryX = hkdf_sha512(
        "Pair-Setup-Accessory-Sign-Salt", K,
        "Pair-Setup-Accessory-Sign-Info", 32);
    std::vector<uint8_t> accmsg;
    accmsg.insert(accmsg.end(), AccessoryX.begin(), AccessoryX.end());
    accmsg.insert(accmsg.end(), acc_id->begin(),    acc_id->end());
    accmsg.insert(accmsg.end(), acc_pub->begin(),   acc_pub->end());
    if (!ed25519_verify(*acc_pub, accmsg.data(), accmsg.size(),
                        acc_sig->data(), acc_sig->size())) {
        cerr << "[HAP-PAIR] accessory signature did NOT verify\n"; return false;
    }
    if (*acc_pub != dev.ltpk()) {
        cerr << "[HAP-PAIR] accessory LTPK in M6 doesn't match HapDevice\n"; return false;
    }

    // Persisted?
    auto found = HapPairingStore::instance().find_ltpk(ctl.id);
    if (found != ctl.pub) {
        cerr << "[HAP-PAIR] pairing not persisted (or LTPK mismatch)\n"; return false;
    }
    if (!srv2.is_complete()) {
        cerr << "[HAP-PAIR] state machine not in Done\n"; return false;
    }

    // Wrong code attempt on a fresh server must reject at M3.
    {
        HapPairSetup bad(dev);
        TlvWriter w1; w1.add_u8(TlvType::State, 0x01); w1.add_u8(TlvType::Method, 0x00);
        bad.handle(w1.take(), "999-99-999"); // different code
        TlvWriter w3; w3.add_u8(TlvType::State, 0x03);
        w3.add(TlvType::PublicKey, Abytes);
        w3.add(TlvType::Proof, std::vector<uint8_t>(M1, M1 + 64));
        auto resp = bad.handle(w3.take(), "999-99-999");
        TlvReader rr; rr.parse(resp);
        if (!rr.has(TlvType::Error)) {
            cerr << "[HAP-PAIR] wrong-code attempt did NOT produce an error TLV\n";
            return false;
        }
    }

    // Cleanup.
    HapPairingStore::instance().clear_all();

    cerr << "[HAP-PAIR] PASS (M1→M2→M3→M4→M5→M6 + sig verify + persist)\n";
    return true;
}

} // namespace opm::airplay
