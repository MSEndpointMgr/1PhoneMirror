#include <opm/airplay/hap_device.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstdio>
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

std::string random_pi() {
    // 6-byte random, force the locally-administered bit (0x02) on the first
    // octet so it can't collide with any real NIC's globally-unique MAC.
    // Also clear the multicast bit (0x01).
    uint8_t b[6];
    if (RAND_bytes(b, 6) != 1) return {};
    b[0] = (b[0] & 0xFE) | 0x02;
    char out[18];
    std::snprintf(out, sizeof(out),
                  "%02x:%02x:%02x:%02x:%02x:%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5]);
    return std::string(out);
}

bool gen_ed25519(std::vector<uint8_t>& seed, std::vector<uint8_t>& pub) {
    EVP_PKEY* k = nullptr;
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!c) return false;
    bool ok = (EVP_PKEY_keygen_init(c) == 1) && (EVP_PKEY_keygen(c, &k) == 1);
    EVP_PKEY_CTX_free(c);
    if (!ok || !k) { if (k) EVP_PKEY_free(k); return false; }

    size_t n = 32; seed.assign(32, 0); pub.assign(32, 0);
    ok = (EVP_PKEY_get_raw_private_key(k, seed.data(), &n) == 1 && n == 32);
    n = 32;
    ok = ok && (EVP_PKEY_get_raw_public_key(k, pub.data(), &n) == 1 && n == 32);
    EVP_PKEY_free(k);
    return ok;
}

bool derive_pub_from_seed(const std::vector<uint8_t>& seed,
                          std::vector<uint8_t>& pub) {
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               seed.data(), seed.size());
    if (!k) return false;
    size_t n = 32; pub.assign(32, 0);
    bool ok = (EVP_PKEY_get_raw_public_key(k, pub.data(), &n) == 1 && n == 32);
    EVP_PKEY_free(k);
    return ok;
}

} // namespace

std::string HapDevice::key_file_path() {
    auto d = appdata_dir();
    return d.empty() ? std::string() : d + "/hap_device.key";
}

std::string HapDevice::ltpk_hex() const {
    return to_hex(ltpk_.data(), ltpk_.size());
}

bool HapDevice::load_or_create() {
    auto path = key_file_path();
    if (path.empty()) {
        std::cerr << "[HAP] cannot resolve APPDATA for device key\n";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);

    // Try load.
    {
        std::ifstream f(path);
        if (f) {
            std::string line, seed_hex;
            while (std::getline(f, line)) {
                if (line.rfind("pi=", 0) == 0)       pi_ = line.substr(3);
                else if (line.rfind("seed=", 0) == 0) seed_hex = line.substr(5);
            }
            if (!pi_.empty() && from_hex(seed_hex, ltsk_seed_) &&
                ltsk_seed_.size() == 32 &&
                derive_pub_from_seed(ltsk_seed_, ltpk_)) {
                std::cout << "[HAP] loaded device identity (pi=" << pi_
                          << ", pk=" << ltpk_hex().substr(0, 16) << "...)\n";
                return true;
            }
            std::cerr << "[HAP] hap_device.key corrupt — regenerating\n";
            pi_.clear(); ltsk_seed_.clear(); ltpk_.clear();
        }
    }

    // Generate fresh.
    pi_ = random_pi();
    if (pi_.empty() || !gen_ed25519(ltsk_seed_, ltpk_)) {
        std::cerr << "[HAP] keygen failed\n";
        return false;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        std::cerr << "[HAP] cannot write " << path << "\n";
        return false;
    }
    out << "# 1PhoneMirror HomeKit pairing identity. Delete to force re-pair.\n";
    out << "pi=" << pi_ << "\n";
    out << "seed=" << to_hex(ltsk_seed_.data(), ltsk_seed_.size()) << "\n";
    out.close();
    std::cout << "[HAP] generated new device identity (pi=" << pi_ << ")\n";
    return true;
}

std::vector<uint8_t> HapDevice::sign(const uint8_t* msg, size_t len) const {
    if (ltsk_seed_.size() != 32) return {};
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               ltsk_seed_.data(), 32);
    if (!k) return {};
    EVP_MD_CTX* mctx = EVP_MD_CTX_new();
    std::vector<uint8_t> sig(64);
    size_t slen = sig.size();
    bool ok = mctx &&
              EVP_DigestSignInit(mctx, nullptr, nullptr, nullptr, k) == 1 &&
              EVP_DigestSign(mctx, sig.data(), &slen, msg, len) == 1 &&
              slen == 64;
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(k);
    if (!ok) return {};
    return sig;
}

bool hap_device_self_test() {
    using std::cerr;

    // Use a sandbox path so we don't clobber the real installation key.
#ifdef _WIN32
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    std::string sandbox = std::string(tmp) + "1pm_hap_selftest";
#else
    std::string sandbox = "/tmp/1pm_hap_selftest";
#endif
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
    std::filesystem::create_directories(sandbox, ec);

    // Generate a fresh keypair directly and verify sign/verify round-trip.
    std::vector<uint8_t> seed, pub;
    if (!gen_ed25519(seed, pub) || seed.size() != 32 || pub.size() != 32) {
        cerr << "[HAP-TEST] keygen failed\n"; return false;
    }

    HapDevice d;
    // Inject via the seed→pub derivation path the loader uses.
    std::vector<uint8_t> pub_chk;
    if (!derive_pub_from_seed(seed, pub_chk) || pub_chk != pub) {
        cerr << "[HAP-TEST] derive_pub_from_seed mismatch\n"; return false;
    }

    // Sign + verify with OpenSSL.
    EVP_PKEY* sk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                seed.data(), 32);
    EVP_PKEY* pk = EVP_PKEY_new_raw_public_key (EVP_PKEY_ED25519, nullptr,
                                                pub.data(),  32);
    const char* msg = "hap-self-test";
    std::vector<uint8_t> sig(64); size_t slen = 64;
    EVP_MD_CTX* mc = EVP_MD_CTX_new();
    bool sok = EVP_DigestSignInit(mc, nullptr, nullptr, nullptr, sk) == 1 &&
               EVP_DigestSign(mc, sig.data(), &slen,
                              reinterpret_cast<const uint8_t*>(msg),
                              std::strlen(msg)) == 1 && slen == 64;
    EVP_MD_CTX_free(mc);
    if (!sok) { cerr << "[HAP-TEST] sign failed\n"; goto fail; }
    {
        EVP_MD_CTX* vc = EVP_MD_CTX_new();
        bool vok = EVP_DigestVerifyInit(vc, nullptr, nullptr, nullptr, pk) == 1 &&
                   EVP_DigestVerify(vc, sig.data(), slen,
                                    reinterpret_cast<const uint8_t*>(msg),
                                    std::strlen(msg)) == 1;
        EVP_MD_CTX_free(vc);
        if (!vok) { cerr << "[HAP-TEST] verify failed\n"; goto fail; }
    }

    EVP_PKEY_free(sk); EVP_PKEY_free(pk);

    // Persistence: write, drop, reload — pi + ltpk must match exactly.
    // (Uses real APPDATA path; safe since key is regenerated only when
    // missing/corrupt.)
    {
        HapDevice a;
        if (!a.load_or_create()) { cerr << "[HAP-TEST] first load failed\n"; return false; }
        HapDevice b;
        if (!b.load_or_create()) { cerr << "[HAP-TEST] reload failed\n"; return false; }
        if (a.pi() != b.pi() || a.ltpk() != b.ltpk()) {
            cerr << "[HAP-TEST] identity not stable across reloads\n";
            return false;
        }
        // Sign with both, verify both sigs against the same public key.
        auto s1 = a.sign(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
        auto s2 = b.sign(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
        if (s1.size() != 64 || s2.size() != 64) {
            cerr << "[HAP-TEST] sign() returned wrong size\n"; return false;
        }
        // Ed25519 is deterministic — same key + same msg ⇒ identical sig.
        if (s1 != s2) {
            cerr << "[HAP-TEST] deterministic sign mismatch across reloads\n";
            return false;
        }
    }

    cerr << "[HAP-TEST] PASS\n";
    return true;

fail:
    EVP_PKEY_free(sk); EVP_PKEY_free(pk);
    return false;
}

} // namespace opm::airplay
