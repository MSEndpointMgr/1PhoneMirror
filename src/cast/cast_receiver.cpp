// cast_receiver.cpp — Google Cast v2 receiver implementation
//
// Implements:
//   - mDNS advertisement (_googlecast._tcp) via Bonjour or multicast
//   - TLS server on port 8009 with self-signed certificate
//   - CastChannel protobuf message framing (4-byte length prefix)
//   - Cast session management (connect, heartbeat, receiver status, auth)
//   - Mirroring app launch and WebRTC offer/answer negotiation
//   - DTLS-SRTP receiver for H.264 RTP screen mirroring streams

#ifdef ENABLE_CAST

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using sock_t = SOCKET;
constexpr sock_t BAD_SOCK = INVALID_SOCKET;
#define SOCK_CLOSE closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using sock_t = int;
constexpr sock_t BAD_SOCK = -1;
#define SOCK_CLOSE close
#endif

#include <openmirror/cast/cast_receiver.h>
#include "cast_proto.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/rand.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <vector>
#include <thread>
#include <algorithm>
#include <random>
#include <cstring>

// ---- Bonjour dynamic loading (same approach as AirPlay mDNS) ----
typedef void* DNSServiceRef;
typedef int32_t DNSServiceErrorType;
typedef uint32_t DNSServiceFlags;

typedef void (*DNSServiceRegisterReply)(
    DNSServiceRef, DNSServiceFlags, DNSServiceErrorType,
    const char*, const char*, const char*, void*);

using FnDNSServiceRegister = DNSServiceErrorType(*)(
    DNSServiceRef*, DNSServiceFlags, uint32_t,
    const char*, const char*, const char*, const char*,
    uint16_t, uint16_t, const void*,
    DNSServiceRegisterReply, void*);

using FnDNSServiceRefDeallocate = void(*)(DNSServiceRef);

// ---- Cast protocol constants ----
static constexpr const char* NS_CONNECTION = "urn:x-cast:com.google.cast.tp.connection";
static constexpr const char* NS_HEARTBEAT  = "urn:x-cast:com.google.cast.tp.heartbeat";
static constexpr const char* NS_RECEIVER   = "urn:x-cast:com.google.cast.receiver";
static constexpr const char* NS_DEVICEAUTH = "urn:x-cast:com.google.cast.tp.deviceauth";
static constexpr const char* NS_WEBRTC     = "urn:x-cast:com.google.cast.webrtc";
static constexpr const char* NS_MIRROR     = "urn:x-cast:com.google.cast.mirror";
static constexpr const char* NS_MEDIA      = "urn:x-cast:com.google.cast.media";

static constexpr const char* MIRROR_APP_ID = "0F5096E8";

namespace openmirror::cast {

// ---- Helpers ----

static std::string random_hex(int bytes) {
    std::vector<uint8_t> buf(bytes);
    RAND_bytes(buf.data(), bytes);
    std::ostringstream oss;
    for (auto b : buf) oss << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    return oss.str();
}

// Extract a JSON string value (basic, no nesting needed for Cast messages)
static std::string json_get(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    }
    // numeric or other
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) end = json.size();
    std::string val = json.substr(pos, end - pos);
    // trim whitespace
    while (!val.empty() && val.back() == ' ') val.pop_back();
    return val;
}

static int json_get_int(const std::string& json, const std::string& key, int def = 0) {
    std::string v = json_get(json, key);
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

static std::vector<uint8_t> build_txt_payload(const std::map<std::string, std::string>& entries) {
    std::vector<uint8_t> txt;
    for (const auto& [key, val] : entries) {
        std::string entry = key + "=" + val;
        if (entry.size() > 255) continue;
        txt.push_back(static_cast<uint8_t>(entry.size()));
        txt.insert(txt.end(), entry.begin(), entry.end());
    }
    return txt;
}

static uint32_t get_local_ipv4() {
#ifdef _WIN32
    ULONG buf_len = 15000;
    std::vector<uint8_t> buf(buf_len);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG ret = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
        nullptr, addrs, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buf_len);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
            nullptr, addrs, &buf_len);
    }
    if (ret != NO_ERROR) return 0;

    uint32_t fallback = 0;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                uint32_t ip = ntohl(sin->sin_addr.s_addr);
                // Skip link-local (169.254.x.x)
                if ((ip >> 16) == 0xA9FE) continue;
                // Skip loopback range
                if ((ip >> 24) == 127) continue;
                // Prefer DHCP/manual routable IPs: 192.168.x.x, 10.x.x.x, 172.16-31.x.x
                uint8_t hi = ip >> 24;
                if (hi == 192 || hi == 10 || (hi == 172 && ((ip >> 16) & 0xFF) >= 16 && ((ip >> 16) & 0xFF) <= 31)) {
                    // Prefer interfaces with a default gateway (real connectivity)
                    if (a->FirstGatewayAddress != nullptr) return ip;
                    if (fallback == 0) fallback = ip;
                }
                if (fallback == 0) fallback = ip;
            }
        }
    }
    return fallback;
#endif
    return 0;
}

static std::string ip_to_string(uint32_t ip) {
    return std::to_string((ip >> 24) & 0xff) + "." +
           std::to_string((ip >> 16) & 0xff) + "." +
           std::to_string((ip >> 8)  & 0xff) + "." +
           std::to_string(ip & 0xff);
}

// ---- SSL certificate generation ----

struct SslCert {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    std::vector<uint8_t> cert_der;
    std::string fingerprint_sha256;

    ~SslCert() {
        if (cert) X509_free(cert);
        if (key) EVP_PKEY_free(key);
    }

    bool generate() {
        // Generate RSA-2048 key (Cast auth protocol requires RSA signatures)
        key = EVP_PKEY_new();
        if (!key) return false;
        BIGNUM* bn = BN_new();
        RSA* rsa = RSA_new();
        BN_set_word(bn, RSA_F4);
        RSA_generate_key_ex(rsa, 2048, bn, nullptr);
        EVP_PKEY_assign_RSA(key, rsa);
        BN_free(bn);

        // Create self-signed X509 certificate with proper extensions
        cert = X509_new();
        if (!cert) return false;
        X509_set_version(cert, 2); // v3

        // Random serial number
        BIGNUM* serial_bn = BN_new();
        BN_rand(serial_bn, 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY);
        BN_to_ASN1_INTEGER(serial_bn, X509_get_serialNumber(cert));
        BN_free(serial_bn);

        X509_gmtime_adj(X509_get_notBefore(cert), 0);
        X509_gmtime_adj(X509_get_notAfter(cert), 10 * 365 * 24 * 3600);
        X509_set_pubkey(cert, key);

        auto* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            (const unsigned char*)"1PhoneMirror Cast", -1, -1, 0);
        X509_set_issuer_name(cert, name);

        // Add Subject Alternative Name (required by modern TLS stacks)
        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        X509V3_set_ctx(&v3ctx, cert, cert, nullptr, nullptr, 0);

        auto* san = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_subject_alt_name,
                                         (char*)"DNS:1PhoneMirror");
        if (san) { X509_add_ext(cert, san, -1); X509_EXTENSION_free(san); }

        // Key Usage: digitalSignature, keyEncipherment
        auto* ku = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_key_usage,
                                        (char*)"critical,digitalSignature,keyEncipherment");
        if (ku) { X509_add_ext(cert, ku, -1); X509_EXTENSION_free(ku); }

        // Extended Key Usage: serverAuth
        auto* eku = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_ext_key_usage,
                                         (char*)"serverAuth");
        if (eku) { X509_add_ext(cert, eku, -1); X509_EXTENSION_free(eku); }

        // Basic Constraints: CA=FALSE
        auto* bc = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_basic_constraints,
                                        (char*)"critical,CA:FALSE");
        if (bc) { X509_add_ext(cert, bc, -1); X509_EXTENSION_free(bc); }

        X509_sign(cert, key, EVP_sha256());

        // DER encoding
        uint8_t* der = nullptr;
        int der_len = i2d_X509(cert, &der);
        if (der_len > 0) {
            cert_der.assign(der, der + der_len);
            OPENSSL_free(der);
        }

        // SHA-256 fingerprint
        uint8_t md[32];
        unsigned int md_len = 32;
        X509_digest(cert, EVP_sha256(), md, &md_len);
        std::ostringstream fp;
        for (unsigned i = 0; i < md_len; i++) {
            if (i > 0) fp << ":";
            fp << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)md[i];
        }
        fingerprint_sha256 = fp.str();
        return true;
    }
};

// ---- CastReceiver Implementation ----

struct CastReceiver::Impl {
    Config config;
    SslCert ssl_cert;
    SSL_CTX* ssl_ctx = nullptr;
    sock_t listen_sock = BAD_SOCK;
    std::string device_id;
    std::string local_ip;

    // mDNS
    DNSServiceRef mdns_ref = nullptr;
    FnDNSServiceRegister fn_register = nullptr;
    FnDNSServiceRefDeallocate fn_dealloc = nullptr;
    void* bonjour_lib = nullptr;

    // Built-in mDNS responder (fallback when Bonjour not installed)
    enum class MdnsBackend { None, Bonjour, Builtin };
    MdnsBackend mdns_backend = MdnsBackend::None;
    sock_t mdns_sock = BAD_SOCK;
    std::thread mdns_thread;
    std::atomic<bool> mdns_running{false};
    std::string mdns_hostname;
    std::string mdns_service_name;  // e.g. "1PhoneMirror._googlecast._tcp.local"
    std::string mdns_service_type;  // "_googlecast._tcp.local"
    std::vector<uint8_t> mdns_txt;
    uint32_t mdns_local_ip = 0;
    uint16_t mdns_port = 0;

    // Threads
    std::thread accept_thread;
    std::thread client_thread;
    SSL* client_ssl = nullptr;
    sock_t client_sock = BAD_SOCK;
    std::mutex client_mutex;

    // Session state
    std::string session_id;
    std::string transport_id;
    bool mirroring_active = false;

    // WebRTC state
    std::thread webrtc_thread;
    sock_t udp_sock = BAD_SOCK;
    uint16_t udp_port = 0;
    SSL_CTX* dtls_ctx = nullptr;
    std::string ice_ufrag;
    std::string ice_pwd;

    // Decoder for WebRTC H.264 stream
    media::Decoder decoder;

    static void ssl_info_cb(const SSL* ssl, int where, int ret) {
        if (where & SSL_CB_HANDSHAKE_START) {
            std::cout << "[Cast] TLS handshake started\n";
        }
        if (where & SSL_CB_HANDSHAKE_DONE) {
            std::cout << "[Cast] TLS handshake done: " << SSL_get_version(ssl)
                      << " " << SSL_get_cipher_name(ssl) << "\n";
        }
        if (where & SSL_CB_ALERT) {
            const char* atype = (where & SSL_CB_READ) ? "read" : "write";
            std::cout << "[Cast] TLS alert " << atype << ": "
                      << SSL_alert_type_string_long(ret) << " / "
                      << SSL_alert_desc_string_long(ret) << "\n";
        }
    }

    // --- Initialization ---

    bool init_ssl() {
        ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (!ssl_ctx) return false;

        // Cast v2 uses TLS 1.2, but allow TLS 1.3 for Android phone compatibility
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
        // Lower security level for older Android Cast clients (OnePlus, etc.)
        SSL_CTX_set_security_level(ssl_ctx, 0);
        // Enable all compatibility workarounds
        SSL_CTX_set_options(ssl_ctx, SSL_OP_ALL | SSL_OP_NO_COMPRESSION);
        // Don't verify client certificates — Cast handles auth at the application layer
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);
        // Set cipher suites compatible with Cast clients (RSA preferred, ECDHE for PFS)
        SSL_CTX_set_cipher_list(ssl_ctx,
            "ALL:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!SRP:!CAMELLIA");
        SSL_CTX_set_info_callback(ssl_ctx, ssl_info_cb);
        if (SSL_CTX_use_certificate(ssl_ctx, ssl_cert.cert) != 1) return false;
        if (SSL_CTX_use_PrivateKey(ssl_ctx, ssl_cert.key) != 1) return false;
        return true;
    }

    bool init_dtls_ctx() {
        dtls_ctx = SSL_CTX_new(DTLS_server_method());
        if (!dtls_ctx) return false;
        SSL_CTX_set_min_proto_version(dtls_ctx, DTLS1_2_VERSION);
        if (SSL_CTX_use_certificate(dtls_ctx, ssl_cert.cert) != 1) return false;
        if (SSL_CTX_use_PrivateKey(dtls_ctx, ssl_cert.key) != 1) return false;

        // Set SRTP profiles
        SSL_CTX_set_tlsext_use_srtp(dtls_ctx, "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32");
        return true;
    }

    bool init_listen(uint16_t port) {
        listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock == BAD_SOCK) return false;

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) return false;
        if (listen(listen_sock, 2) < 0) return false;
        return true;
    }

    // --- mDNS ---

    // Built-in mDNS helpers (same pattern as AirPlay module)
    static constexpr const char* MDNS_MCAST_ADDR = "224.0.0.251";
    static constexpr uint16_t MDNS_MCAST_PORT = 5353;
    static constexpr uint16_t DNS_TYPE_A     = 1;
    static constexpr uint16_t DNS_TYPE_PTR   = 12;
    static constexpr uint16_t DNS_TYPE_TXT   = 16;
    static constexpr uint16_t DNS_TYPE_SRV   = 33;
    static constexpr uint16_t DNS_TYPE_ANY   = 255;
    static constexpr uint16_t DNS_CLASS_IN   = 1;
    static constexpr uint16_t DNS_CLASS_FLUSH = 0x8001;

    static void mdns_put_u16(std::vector<uint8_t>& pkt, uint16_t val) {
        pkt.push_back(static_cast<uint8_t>(val >> 8));
        pkt.push_back(static_cast<uint8_t>(val & 0xFF));
    }
    static void mdns_put_u32(std::vector<uint8_t>& pkt, uint32_t val) {
        pkt.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        pkt.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        pkt.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>(val & 0xFF));
    }
    static void mdns_put_name(std::vector<uint8_t>& pkt, const std::string& name) {
        size_t pos = 0;
        while (pos < name.size()) {
            size_t dot = name.find('.', pos);
            if (dot == std::string::npos) dot = name.size();
            size_t len = dot - pos;
            if (len > 63) len = 63;
            pkt.push_back(static_cast<uint8_t>(len));
            pkt.insert(pkt.end(), name.begin() + pos, name.begin() + pos + len);
            pos = dot + 1;
        }
        pkt.push_back(0);
    }
    static std::vector<uint8_t> mdns_encode_name(const std::string& name) {
        std::vector<uint8_t> out;
        mdns_put_name(out, name);
        return out;
    }

    static std::string mdns_parse_name(const uint8_t* pkt, size_t pkt_len, size_t& offset) {
        std::string name;
        size_t saved = 0;
        bool jumped = false;
        int safety = 0;
        while (offset < pkt_len && safety++ < 128) {
            uint8_t len = pkt[offset];
            if (len == 0) { offset++; break; }
            if ((len & 0xC0) == 0xC0) {
                if (offset + 1 >= pkt_len) break;
                if (!jumped) saved = offset + 2;
                offset = ((len & 0x3F) << 8) | pkt[offset + 1];
                jumped = true;
                continue;
            }
            offset++;
            if (offset + len > pkt_len) break;
            if (!name.empty()) name += '.';
            name.append(reinterpret_cast<const char*>(pkt + offset), len);
            offset += len;
        }
        if (jumped) offset = saved;
        return name;
    }

    static std::string str_to_lower(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(), ::tolower);
        return out;
    }

    void mdns_send(const std::vector<uint8_t>& pkt) {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(MDNS_MCAST_PORT);
        inet_pton(AF_INET, MDNS_MCAST_ADDR, &dest.sin_addr);
        sendto(mdns_sock, reinterpret_cast<const char*>(pkt.data()),
               static_cast<int>(pkt.size()), 0,
               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    }

    void mdns_add_service_records(std::vector<uint8_t>& pkt, uint16_t& count) {
        uint32_t ttl = 4500;
        // PTR: _googlecast._tcp.local → instance
        mdns_put_name(pkt, mdns_service_type);
        mdns_put_u16(pkt, DNS_TYPE_PTR); mdns_put_u16(pkt, DNS_CLASS_IN);
        mdns_put_u32(pkt, ttl);
        auto ptr_rdata = mdns_encode_name(mdns_service_name);
        mdns_put_u16(pkt, static_cast<uint16_t>(ptr_rdata.size()));
        pkt.insert(pkt.end(), ptr_rdata.begin(), ptr_rdata.end());
        count++;

        // SRV: instance → hostname:port
        mdns_put_name(pkt, mdns_service_name);
        mdns_put_u16(pkt, DNS_TYPE_SRV); mdns_put_u16(pkt, DNS_CLASS_FLUSH);
        mdns_put_u32(pkt, ttl);
        auto host_enc = mdns_encode_name(mdns_hostname);
        mdns_put_u16(pkt, static_cast<uint16_t>(6 + host_enc.size()));
        mdns_put_u16(pkt, 0); mdns_put_u16(pkt, 0);
        mdns_put_u16(pkt, mdns_port);
        pkt.insert(pkt.end(), host_enc.begin(), host_enc.end());
        count++;

        // TXT
        mdns_put_name(pkt, mdns_service_name);
        mdns_put_u16(pkt, DNS_TYPE_TXT); mdns_put_u16(pkt, DNS_CLASS_FLUSH);
        mdns_put_u32(pkt, ttl);
        mdns_put_u16(pkt, static_cast<uint16_t>(mdns_txt.size()));
        pkt.insert(pkt.end(), mdns_txt.begin(), mdns_txt.end());
        count++;

        // A: hostname → IP
        if (mdns_local_ip != 0) {
            mdns_put_name(pkt, mdns_hostname);
            mdns_put_u16(pkt, DNS_TYPE_A); mdns_put_u16(pkt, DNS_CLASS_FLUSH);
            mdns_put_u32(pkt, ttl); mdns_put_u16(pkt, 4);
            mdns_put_u32(pkt, mdns_local_ip);
            count++;
        }
    }

    void mdns_send_announcement() {
        std::vector<uint8_t> pkt;
        mdns_put_u16(pkt, 0); mdns_put_u16(pkt, 0x8400); // response, authoritative
        mdns_put_u16(pkt, 0); // questions
        size_t anpos = pkt.size();
        mdns_put_u16(pkt, 0); // answers (placeholder)
        mdns_put_u16(pkt, 0); mdns_put_u16(pkt, 0);
        uint16_t count = 0;
        mdns_add_service_records(pkt, count);
        pkt[anpos] = static_cast<uint8_t>(count >> 8);
        pkt[anpos + 1] = static_cast<uint8_t>(count & 0xFF);
        mdns_send(pkt);
        mdns_send(pkt); // send twice for reliability
    }

    std::vector<uint8_t> mdns_build_response(const std::string& qname, uint16_t qtype) {
        std::string q = str_to_lower(qname);
        bool match_type = (q == str_to_lower(mdns_service_type));
        bool match_svc  = (q == str_to_lower(mdns_service_name));
        bool match_host = (q == str_to_lower(mdns_hostname));
        bool match_services = (q == "_services._dns-sd._udp.local");

        bool want_ptr = (qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY);
        bool want_svc = (qtype == DNS_TYPE_SRV || qtype == DNS_TYPE_TXT || qtype == DNS_TYPE_ANY);
        bool want_a   = (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY);

        std::vector<uint8_t> pkt;
        mdns_put_u16(pkt, 0); mdns_put_u16(pkt, 0x8400);
        mdns_put_u16(pkt, 0);
        size_t anpos = pkt.size();
        mdns_put_u16(pkt, 0); mdns_put_u16(pkt, 0); mdns_put_u16(pkt, 0);
        uint16_t count = 0;

        if (match_services && want_ptr) {
            mdns_put_name(pkt, "_services._dns-sd._udp.local");
            mdns_put_u16(pkt, DNS_TYPE_PTR); mdns_put_u16(pkt, DNS_CLASS_IN);
            mdns_put_u32(pkt, 4500);
            auto r = mdns_encode_name(mdns_service_type);
            mdns_put_u16(pkt, static_cast<uint16_t>(r.size()));
            pkt.insert(pkt.end(), r.begin(), r.end());
            count++;
        }

        if ((match_type || match_svc) && (want_ptr || want_svc)) {
            mdns_add_service_records(pkt, count);
        }

        if (match_host && want_a && mdns_local_ip != 0) {
            mdns_put_name(pkt, mdns_hostname);
            mdns_put_u16(pkt, DNS_TYPE_A); mdns_put_u16(pkt, DNS_CLASS_FLUSH);
            mdns_put_u32(pkt, 4500); mdns_put_u16(pkt, 4);
            mdns_put_u32(pkt, mdns_local_ip);
            count++;
        }

        if (count == 0) return {};
        pkt[anpos] = static_cast<uint8_t>(count >> 8);
        pkt[anpos + 1] = static_cast<uint8_t>(count & 0xFF);
        return pkt;
    }

    void mdns_handle_query(const uint8_t* pkt, size_t len) {
        if (len < 12) return;
        uint16_t flags = (pkt[2] << 8) | pkt[3];
        if (flags & 0x8000) return; // ignore responses
        uint16_t qdcount = (pkt[4] << 8) | pkt[5];
        size_t offset = 12;
        for (uint16_t i = 0; i < qdcount && offset < len; i++) {
            std::string qname = mdns_parse_name(pkt, len, offset);
            if (offset + 4 > len) break;
            uint16_t qtype = (pkt[offset] << 8) | pkt[offset + 1];
            offset += 4;
            auto resp = mdns_build_response(qname, qtype);
            if (!resp.empty()) mdns_send(resp);
        }
    }

    void mdns_listen_loop() {
        uint8_t buf[4096];
        while (mdns_running.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(mdns_sock, &fds);
            timeval tv{1, 0};
            int ret = select(0, &fds, nullptr, nullptr, &tv);
            if (ret <= 0) continue;
            sockaddr_in from{};
            int fromlen = sizeof(from);
            int n = recvfrom(mdns_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromlen);
            if (n > 0) mdns_handle_query(buf, static_cast<size_t>(n));
        }
    }

    bool init_mdns_builtin(const std::string& name, uint16_t port) {
        mdns_local_ip = get_local_ipv4();
        if (mdns_local_ip == 0) {
            std::cerr << "[Cast] Could not determine local IP for mDNS\n";
            return false;
        }
        mdns_port = port;

        char hname[256] = {};
        gethostname(hname, sizeof(hname));
        mdns_hostname = std::string(hname) + ".local";
        mdns_service_type = "_googlecast._tcp.local";
        mdns_service_name = name + "._googlecast._tcp.local";

        mdns_txt = build_txt_payload({
            {"id", device_id},
            {"cd", "A4F99FBA49A3C21A"},
            {"rm", ""},
            {"ve", "05"},
            {"md", "1PhoneMirror"},
            {"ic", "/setup/icon.png"},
            {"fn", name},
            {"ca", "199172"},
            {"st", "0"},
            {"bs", "FA8FCA4B5E28"},
            {"nf", "1"},
            {"rs", ""},
        });

        mdns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (mdns_sock == BAD_SOCK) {
            std::cerr << "[Cast] Failed to create mDNS socket\n";
            return false;
        }

        int reuse = 1;
        setsockopt(mdns_sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = htons(MDNS_MCAST_PORT);

        if (bind(mdns_sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            std::cerr << "[Cast] mDNS bind to port 5353 failed (error "
                      << WSAGetLastError() << ")\n";
            SOCK_CLOSE(mdns_sock);
            mdns_sock = BAD_SOCK;
            return false;
        }

        ip_mreq mreq{};
        inet_pton(AF_INET, MDNS_MCAST_ADDR, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(mdns_local_ip);
        if (setsockopt(mdns_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
            std::cerr << "[Cast] Failed to join mDNS multicast group\n";
            SOCK_CLOSE(mdns_sock);
            mdns_sock = BAD_SOCK;
            return false;
        }

        int ttl = 255;
        setsockopt(mdns_sock, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl), sizeof(ttl));
        int loop = 1;
        setsockopt(mdns_sock, IPPROTO_IP, IP_MULTICAST_LOOP,
                   reinterpret_cast<const char*>(&loop), sizeof(loop));
        // Send multicast on the correct interface
        in_addr mc_if{};
        mc_if.s_addr = htonl(mdns_local_ip);
        setsockopt(mdns_sock, IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char*>(&mc_if), sizeof(mc_if));

        mdns_running.store(true);
        mdns_thread = std::thread(&Impl::mdns_listen_loop, this);
        mdns_send_announcement();

        mdns_backend = MdnsBackend::Builtin;
        std::cout << "[Cast] Advertised as '" << name << "' on port " << port
                  << " (_googlecast._tcp, built-in mDNS, IP "
                  << ip_to_string(mdns_local_ip) << ")\n";
        return true;
    }

    bool init_mdns(const std::string& name, uint16_t port) {
        // Try Bonjour first
#ifdef _WIN32
        bonjour_lib = LoadLibraryA("dnssd.dll");
        if (bonjour_lib) {
            fn_register = (FnDNSServiceRegister)GetProcAddress((HMODULE)bonjour_lib, "DNSServiceRegister");
            fn_dealloc = (FnDNSServiceRefDeallocate)GetProcAddress((HMODULE)bonjour_lib, "DNSServiceRefDeallocate");
        }
        if (fn_register && fn_dealloc) {
            std::map<std::string, std::string> txt;
            txt["id"] = device_id;
            txt["cd"] = "A4F99FBA49A3C21A";
            txt["rm"] = "";
            txt["ve"] = "05";
            txt["md"] = "1PhoneMirror";
            txt["ic"] = "/setup/icon.png";
            txt["fn"] = name;
            txt["ca"] = "199172";
            txt["st"] = "0";
            txt["bs"] = "FA8FCA4B5E28";
            txt["nf"] = "1";
            txt["rs"] = "";

            auto txt_data = build_txt_payload(txt);

            DNSServiceErrorType err = fn_register(
                &mdns_ref, 0, 0,
                name.c_str(), "_googlecast._tcp", nullptr, nullptr,
                htons(port), (uint16_t)txt_data.size(), txt_data.data(),
                nullptr, nullptr);

            if (err == 0) {
                mdns_backend = MdnsBackend::Bonjour;
                std::cout << "[Cast] Advertised as '" << name << "' on port " << port
                          << " (_googlecast._tcp, Bonjour)\n";
                return true;
            }
            std::cerr << "[Cast] Bonjour registration failed (err=" << err
                      << "), falling back to built-in mDNS\n";
        } else {
            std::cout << "[Cast] Bonjour not available, using built-in mDNS\n";
            if (bonjour_lib) { FreeLibrary((HMODULE)bonjour_lib); bonjour_lib = nullptr; }
        }
#endif
        // Fall back to built-in mDNS responder
        return init_mdns_builtin(name, port);
    }

    // --- Message I/O ---

    bool send_cast_msg(SSL* ssl, const CastMessage& msg) {
        auto data = msg.encode();
        uint32_t len = htonl((uint32_t)data.size());
        if (SSL_write(ssl, &len, 4) != 4) return false;
        int written = SSL_write(ssl, data.data(), (int)data.size());
        return written == (int)data.size();
    }

    bool recv_cast_msg(SSL* ssl, CastMessage& msg) {
        uint8_t hdr[4];
        int n = SSL_read(ssl, hdr, 4);
        if (n != 4) {
            int ssl_err = SSL_get_error(ssl, n);
            unsigned long err = ERR_get_error();
            char errbuf[256];
            ERR_error_string_n(err, errbuf, sizeof(errbuf));
            std::cerr << "[Cast] recv header failed: SSL_read=" << n
                      << " SSL_error=" << ssl_err << " " << errbuf << "\n";
            return false;
        }
        uint32_t len = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
        if (len > 1 * 1024 * 1024) return false; // 1MB limit

        std::vector<uint8_t> buf(len);
        size_t got = 0;
        while (got < len) {
            n = SSL_read(ssl, buf.data() + got, (int)(len - got));
            if (n <= 0) return false;
            got += n;
        }
        return CastMessage::decode(buf.data(), buf.size(), msg);
    }

    void send_json(SSL* ssl, const std::string& src, const std::string& dst,
                   const std::string& ns, const std::string& json) {
        CastMessage m;
        m.source_id = src;
        m.destination_id = dst;
        m.ns = ns;
        m.payload_type = 0;
        m.payload_utf8 = json;
        send_cast_msg(ssl, m);
    }

    void send_binary(SSL* ssl, const std::string& src, const std::string& dst,
                     const std::string& ns, const std::vector<uint8_t>& data) {
        CastMessage m;
        m.source_id = src;
        m.destination_id = dst;
        m.ns = ns;
        m.payload_type = 1;
        m.payload_binary = data;
        send_cast_msg(ssl, m);
    }

    // --- Cast message handlers ---

    void handle_connection(SSL* ssl, const CastMessage& msg) {
        std::string type = json_get(msg.payload_utf8, "type");
        if (type == "CONNECT") {
            std::cout << "[Cast] Client connected: " << msg.source_id << "\n";
            // No explicit response needed for CONNECT
        } else if (type == "CLOSE") {
            std::cout << "[Cast] Client disconnected: " << msg.source_id << "\n";
        }
    }

    void handle_heartbeat(SSL* ssl, const CastMessage& msg) {
        std::string type = json_get(msg.payload_utf8, "type");
        if (type == "PING") {
            send_json(ssl, "receiver-0", msg.source_id, NS_HEARTBEAT,
                      R"({"type":"PONG"})");
        }
    }

    void handle_receiver(SSL* ssl, const CastMessage& msg) {
        std::string type = json_get(msg.payload_utf8, "type");
        int req_id = json_get_int(msg.payload_utf8, "requestId");

        if (type == "GET_STATUS") {
            send_receiver_status(ssl, msg.source_id, req_id);
        } else if (type == "LAUNCH") {
            std::string app_id = json_get(msg.payload_utf8, "appId");
            std::cout << "[Cast] LAUNCH app: " << app_id << "\n";

            // Generate session and transport IDs
            session_id = random_hex(16);
            transport_id = "web-" + std::to_string(std::random_device{}() % 10000);
            mirroring_active = true;

            send_receiver_status(ssl, msg.source_id, req_id);
        } else if (type == "STOP") {
            mirroring_active = false;
            session_id.clear();
            send_receiver_status(ssl, msg.source_id, req_id);
        }
    }

    void send_receiver_status(SSL* ssl, const std::string& dst, int req_id) {
        std::ostringstream json;
        json << R"({"type":"RECEIVER_STATUS","requestId":)" << req_id
             << R"(,"status":{"volume":{"level":1.0,"muted":false,"controlType":"attenuation","stepInterval":0.05},"applications":[)";

        if (mirroring_active) {
            json << R"({"appId":")" << MIRROR_APP_ID
                 << R"(","displayName":"Backdrop","isIdleScreen":false,"launchedFromCloud":false,"namespaces":[)"
                 << R"({"name":")" << NS_WEBRTC << R"("},)"
                 << R"({"name":")" << NS_MIRROR << R"("},)"
                 << R"({"name":")" << NS_MEDIA << R"("})"
                 << R"(],"sessionId":")" << session_id
                 << R"(","statusText":"Mirroring","transportId":")" << transport_id
                 << R"("})";
        }

        json << "]}}";
        send_json(ssl, "receiver-0", dst, NS_RECEIVER, json.str());
    }

    void handle_auth(SSL* ssl, const CastMessage& msg) {
        std::cout << "[Cast] Auth challenge received (" << msg.payload_binary.size() << " bytes)\n";

        // Unwrap DeviceAuthMessage to get inner AuthChallenge bytes (field 1)
        AuthChallengeMsg challenge;
        if (!msg.payload_binary.empty()) {
            std::vector<uint8_t> challenge_bytes;
            if (DeviceAuthMsg::unwrap(msg.payload_binary.data(),
                                       msg.payload_binary.size(), 1, challenge_bytes)) {
                AuthChallengeMsg::decode(challenge_bytes.data(),
                                         challenge_bytes.size(), challenge);
                std::cout << "[Cast]   Nonce=" << challenge.sender_nonce.size()
                          << "B sig_algo=" << challenge.sig_algo
                          << " hash_algo=" << challenge.hash_algo << "\n";
            } else {
                // Fallback: try parsing directly (unlikely)
                AuthChallengeMsg::decode(msg.payload_binary.data(),
                                         msg.payload_binary.size(), challenge);
            }
        }

        // Sign sender_nonce + OUR TLS cert DER (Cast v2 auth protocol)
        // The sender verifies using our client_auth_cert's public key and
        // checks it matches the TLS server cert they received during handshake.
        std::vector<uint8_t> sig;
        if (ssl_cert.key) {
            const EVP_MD* md = (challenge.hash_algo == 1) ? EVP_sha256() : EVP_sha1();
            EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
            EVP_DigestSignInit(md_ctx, nullptr, md, nullptr, ssl_cert.key);
            if (!challenge.sender_nonce.empty()) {
                EVP_DigestSignUpdate(md_ctx, challenge.sender_nonce.data(),
                                      challenge.sender_nonce.size());
            }
            // Append OUR cert DER (the TLS cert the sender sees)
            EVP_DigestSignUpdate(md_ctx, ssl_cert.cert_der.data(),
                                  ssl_cert.cert_der.size());
            size_t sig_len = 0;
            EVP_DigestSignFinal(md_ctx, nullptr, &sig_len);
            sig.resize(sig_len);
            EVP_DigestSignFinal(md_ctx, sig.data(), &sig_len);
            sig.resize(sig_len);
            EVP_MD_CTX_free(md_ctx);
        }

        // Build AuthResponse
        AuthResponseMsg resp;
        resp.signature = sig;
        resp.client_auth_cert = ssl_cert.cert_der;
        resp.intermediate_certs.push_back(ssl_cert.cert_der); // self-signed acts as its own CA
        resp.signature_algorithm = 1; // RSASSA_PKCS1v15
        resp.sender_nonce = challenge.sender_nonce; // echo back
        resp.hash_algorithm = (challenge.hash_algo == 1) ? 1 : 0;

        // Wrap in DeviceAuthMessage (field 2 = response)
        CastMessage reply;
        reply.source_id = "receiver-0";
        reply.destination_id = msg.source_id;
        reply.ns = NS_DEVICEAUTH;
        reply.payload_type = 1;
        reply.payload_binary = DeviceAuthMsg::wrap_response(resp.encode());
        send_cast_msg(ssl, reply);

        std::cout << "[Cast] Auth response sent (cert=" << ssl_cert.cert_der.size()
                  << "B, sig=" << sig.size() << "B, nonce=" << challenge.sender_nonce.size() << "B)\n";
    }

    void handle_webrtc(SSL* ssl, const CastMessage& msg) {
        std::string type = json_get(msg.payload_utf8, "type");
        std::cout << "[Cast] WebRTC message: type=" << type << "\n";

        if (type == "OFFER") {
            handle_webrtc_offer(ssl, msg);
        } else {
            std::cout << "[Cast] WebRTC payload: " << msg.payload_utf8.substr(0, 500) << "\n";
        }
    }

    void handle_webrtc_offer(SSL* ssl, const CastMessage& msg) {
        // Extract SDP offer from the message
        // The offer is in msg.payload_utf8 as JSON with an "offer" or "sdp" field
        std::cout << "[Cast] WebRTC OFFER received\n";
        std::cout << "[Cast] Payload: " << msg.payload_utf8.substr(0, 1000) << "\n";

        // Parse remote ICE credentials from the SDP
        std::string sdp = json_get(msg.payload_utf8, "sdp");
        if (sdp.empty()) {
            // Try extracting from "offer" sub-object
            auto offer_pos = msg.payload_utf8.find("\"offer\"");
            if (offer_pos != std::string::npos) {
                auto sdp_pos = msg.payload_utf8.find("\"sdp\"", offer_pos);
                if (sdp_pos != std::string::npos) {
                    sdp = json_get(msg.payload_utf8.substr(sdp_pos - 1), "sdp");
                }
            }
        }

        if (sdp.empty()) {
            std::cout << "[Cast] Could not extract SDP from offer\n";
            return;
        }

        // Unescape \\r\\n in JSON string
        std::string sdp_clean;
        for (size_t i = 0; i < sdp.size(); i++) {
            if (sdp[i] == '\\' && i + 1 < sdp.size()) {
                if (sdp[i+1] == 'r') { sdp_clean += '\r'; i++; }
                else if (sdp[i+1] == 'n') { sdp_clean += '\n'; i++; }
                else sdp_clean += sdp[i];
            } else {
                sdp_clean += sdp[i];
            }
        }

        std::cout << "[Cast] SDP offer:\n" << sdp_clean << "\n";

        // Extract ICE credentials and media info from SDP
        std::string remote_ufrag, remote_pwd;
        std::string remote_fingerprint;
        std::string remote_candidate;
        int video_pt = 96;

        std::istringstream ss(sdp_clean);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.substr(0, 12) == "a=ice-ufrag:") remote_ufrag = line.substr(12);
            else if (line.substr(0, 10) == "a=ice-pwd:") remote_pwd = line.substr(10);
            else if (line.substr(0, 14) == "a=fingerprint:") remote_fingerprint = line.substr(14);
            else if (line.substr(0, 12) == "a=candidate:") remote_candidate = line.substr(12);
            else if (line.substr(0, 9) == "a=rtpmap:" && line.find("H264") != std::string::npos) {
                video_pt = std::stoi(line.substr(9));
            }
        }

        std::cout << "[Cast] Remote ICE: ufrag=" << remote_ufrag
                  << " pwd=" << remote_pwd << "\n";
        std::cout << "[Cast] Remote fingerprint: " << remote_fingerprint << "\n";
        if (!remote_candidate.empty())
            std::cout << "[Cast] Remote candidate: " << remote_candidate << "\n";

        // Set up our UDP socket for WebRTC
        if (udp_sock == BAD_SOCK) {
            udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in uaddr{};
            uaddr.sin_family = AF_INET;
            uaddr.sin_addr.s_addr = INADDR_ANY;
            uaddr.sin_port = 0; // let OS pick
            bind(udp_sock, (sockaddr*)&uaddr, sizeof(uaddr));

            sockaddr_in bound{};
            int blen = sizeof(bound);
            getsockname(udp_sock, (sockaddr*)&bound, &blen);
            udp_port = ntohs(bound.sin_port);
        }

        // Generate our ICE credentials
        ice_ufrag = random_hex(4);
        ice_pwd = random_hex(12);

        // Build SDP answer
        std::ostringstream answer;
        answer << "v=0\\r\\n"
               << "o=- " << std::random_device{}() << " 1 IN IP4 " << local_ip << "\\r\\n"
               << "s=-\\r\\n"
               << "t=0 0\\r\\n"
               << "a=group:BUNDLE 0\\r\\n"
               << "m=video " << udp_port << " UDP/TLS/RTP/SAVPF " << video_pt << "\\r\\n"
               << "c=IN IP4 " << local_ip << "\\r\\n"
               << "a=rtcp-mux\\r\\n"
               << "a=rtpmap:" << video_pt << " H264/90000\\r\\n"
               << "a=recvonly\\r\\n"
               << "a=ice-ufrag:" << ice_ufrag << "\\r\\n"
               << "a=ice-pwd:" << ice_pwd << "\\r\\n"
               << "a=fingerprint:sha-256 " << ssl_cert.fingerprint_sha256 << "\\r\\n"
               << "a=setup:active\\r\\n"
               << "a=mid:0\\r\\n"
               << "a=candidate:1 1 UDP 2113937151 " << local_ip << " " << udp_port << " typ host\\r\\n";

        // Send ANSWER
        std::ostringstream resp;
        int seq_num = json_get_int(msg.payload_utf8, "seqNum");
        resp << R"({"type":"ANSWER","seqNum":)" << seq_num
             << R"(,"answer":{"sdp":")" << answer.str() << R"(","type":"answer"},"result":"ok"})";

        send_json(ssl, transport_id, msg.source_id, NS_WEBRTC, resp.str());
        std::cout << "[Cast] SDP answer sent on UDP port " << udp_port << "\n";

        // Start WebRTC receiver thread
        if (webrtc_thread.joinable()) {
            // Already running
        } else {
            webrtc_thread = std::thread([this, remote_ufrag, remote_pwd, video_pt]() {
                run_webrtc_receiver(remote_ufrag, remote_pwd, video_pt);
            });
        }
    }

    // --- WebRTC receiver (STUN + DTLS + SRTP + RTP) ---

    void run_webrtc_receiver(const std::string& remote_ufrag,
                             const std::string& remote_pwd, int video_pt) {
        std::cout << "[Cast/WebRTC] Receiver started on UDP:" << udp_port << "\n";

        // Set socket timeout for polling
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

        uint8_t buf[65536];
        sockaddr_in remote_addr{};
        int remote_addr_len = sizeof(remote_addr);
        bool dtls_done = false;

        // DTLS setup
        SSL* dtls_ssl = nullptr;
        BIO* rbio = nullptr;
        BIO* wbio = nullptr;

        auto init_dtls = [&]() {
            dtls_ssl = SSL_new(dtls_ctx);
            rbio = BIO_new(BIO_s_mem());
            wbio = BIO_new(BIO_s_mem());
            SSL_set_bio(dtls_ssl, rbio, wbio);
            SSL_set_accept_state(dtls_ssl);
        };

        // SRTP keys (derived after DTLS)
        std::vector<uint8_t> srtp_key;
        std::vector<uint8_t> srtp_salt;
        bool srtp_ready = false;

        // H.264 NAL reassembly buffer
        std::vector<uint8_t> nal_buf;
        uint16_t last_seq = 0;

        while (true) {
            // Check if we should stop
            // (this is checked via the CastReceiver's running_ flag through impl pointer)

            int n = recvfrom(udp_sock, (char*)buf, sizeof(buf), 0,
                             (sockaddr*)&remote_addr, &remote_addr_len);
            if (n <= 0) continue;

            // Classify packet
            uint8_t first = buf[0];

            if (first == 0x00 || first == 0x01) {
                // STUN binding request
                handle_stun(buf, n, remote_addr, remote_ufrag);
            } else if (first >= 20 && first <= 63) {
                // DTLS record
                if (!dtls_ssl) init_dtls();

                // Feed to DTLS
                BIO_write(rbio, buf, n);
                if (!dtls_done) {
                    int ret = SSL_do_handshake(dtls_ssl);
                    if (ret == 1) {
                        dtls_done = true;
                        std::cout << "[Cast/WebRTC] DTLS handshake complete\n";
                        // Extract SRTP keys
                        extract_srtp_keys(dtls_ssl, srtp_key, srtp_salt);
                        srtp_ready = true;
                        std::cout << "[Cast/WebRTC] SRTP keys derived\n";
                    }
                    // Flush any DTLS output
                    flush_dtls_output(wbio, remote_addr);
                } else {
                    // Post-handshake DTLS data (shouldn't happen for SRTP)
                    uint8_t dbuf[4096];
                    SSL_read(dtls_ssl, dbuf, sizeof(dbuf));
                }
            } else if (first >= 128 && first <= 191 && srtp_ready) {
                // RTP/SRTP packet
                // For now, log that we're receiving data
                uint8_t pt = buf[1] & 0x7f;
                uint16_t seq = (buf[2] << 8) | buf[3];
                if (pt == video_pt) {
                    // Decrypt SRTP and extract H.264 NALUs
                    // TODO: Full SRTP decryption
                    // For now log receipt
                    if (seq % 100 == 0) {
                        std::cout << "[Cast/WebRTC] RTP video seq=" << seq
                                  << " size=" << n << "\n";
                    }
                }
            } else if ((first & 0xC0) == 0x80 && (buf[1] >= 200 && buf[1] <= 209)) {
                // RTCP packet — ignore for now
            }
        }

        if (dtls_ssl) SSL_free(dtls_ssl);
    }

    void handle_stun(const uint8_t* data, int len,
                     const sockaddr_in& remote, const std::string& remote_ufrag) {
        // STUN binding request: type 0x0001
        if (len < 20) return;
        uint16_t msg_type = (data[0] << 8) | data[1];
        if (msg_type != 0x0001) return;

        // Transaction ID (bytes 8-19)
        uint8_t tid[12];
        memcpy(tid, data + 8, 12);

        std::cout << "[Cast/WebRTC] STUN binding request received\n";

        // Build STUN binding success response
        std::vector<uint8_t> resp;
        resp.resize(32, 0);

        // Header: type=0x0101 (success), length=12, magic cookie, tid
        resp[0] = 0x01; resp[1] = 0x01;
        resp[2] = 0x00; resp[3] = 0x0C; // body length
        resp[4] = 0x21; resp[5] = 0x12; resp[6] = 0xA4; resp[7] = 0x42; // magic cookie
        memcpy(resp.data() + 8, tid, 12);

        // XOR-MAPPED-ADDRESS attribute (0x0020)
        resp[20] = 0x00; resp[21] = 0x20; // type
        resp[22] = 0x00; resp[23] = 0x08; // length
        resp[24] = 0x00; resp[25] = 0x01; // family=IPv4
        uint16_t port = ntohs(remote.sin_port) ^ 0x2112;
        resp[26] = port >> 8; resp[27] = port & 0xff;
        uint32_t ip = ntohl(remote.sin_addr.s_addr) ^ 0x2112A442;
        resp[28] = (ip >> 24) & 0xff;
        resp[29] = (ip >> 16) & 0xff;
        resp[30] = (ip >> 8)  & 0xff;
        resp[31] = ip & 0xff;

        sendto(udp_sock, (const char*)resp.data(), (int)resp.size(), 0,
               (const sockaddr*)&remote, sizeof(remote));
    }

    void flush_dtls_output(BIO* wbio, const sockaddr_in& remote) {
        uint8_t buf[4096];
        int pending;
        while ((pending = BIO_ctrl_pending(wbio)) > 0) {
            int n = BIO_read(wbio, buf, std::min(pending, (int)sizeof(buf)));
            if (n > 0) {
                sendto(udp_sock, (const char*)buf, n, 0,
                       (const sockaddr*)&remote, sizeof(remote));
            }
        }
    }

    void extract_srtp_keys(SSL* ssl, std::vector<uint8_t>& key, std::vector<uint8_t>& salt) {
        // Export keying material for SRTP
        // SRTP_AES128_CM_SHA1_80: 16-byte key + 14-byte salt per direction
        uint8_t material[60]; // 2 * (16 + 14)
        if (SSL_export_keying_material(ssl, material, sizeof(material),
                "EXTRACTOR-dtls_srtp", 19, nullptr, 0, 0) != 1) {
            std::cerr << "[Cast/WebRTC] Failed to export SRTP keying material\n";
            return;
        }
        // Layout: client_key(16) + server_key(16) + client_salt(14) + server_salt(14)
        // We're the server, sender is client
        key.assign(material, material + 16);         // client master key
        salt.assign(material + 32, material + 46);   // client master salt
    }

    // --- Client handler ---

    void handle_client(sock_t sock) {
        // Peek at the first bytes to diagnose TLS
        uint8_t peek[300] = {};
        int peeked = recv(sock, reinterpret_cast<char*>(peek), sizeof(peek), MSG_PEEK);
        if (peeked >= 5) {
            if (peek[0] != 0x16) {
                std::cerr << "[Cast] Non-TLS data received: first byte = 0x"
                          << std::hex << (int)peek[0] << std::dec
                          << " (" << peeked << " bytes peeked)\n";
                SOCK_CLOSE(sock);
                return;
            }
            uint16_t rec_len = (peek[3] << 8) | peek[4];
            std::cout << "[Cast] TLS ClientHello: version=" << (int)peek[1] << "." << (int)peek[2]
                      << " record_len=" << rec_len;
            if (peeked >= 11) {
                std::cout << " hs_type=" << (int)peek[5]
                          << " hs_version=" << (int)peek[9] << "." << (int)peek[10];
            }
            std::cout << "\n";

            // For small ClientHellos (likely phones), dump cipher suites
            if (rec_len < 400 && peeked >= 44) {
                // offset 5: handshake type(1) + length(3) + version(2) + random(32) = 38
                size_t off = 5 + 1 + 3 + 2 + 32; // = 43
                if (off < (size_t)peeked) {
                    uint8_t sess_len = peek[off]; off += 1 + sess_len;
                    if (off + 2 <= (size_t)peeked) {
                        uint16_t cs_len = (peek[off] << 8) | peek[off + 1]; off += 2;
                        std::cout << "[Cast]   Cipher suites (" << (cs_len / 2) << "):";
                        for (uint16_t i = 0; i + 1 < cs_len && off + 1 < (size_t)peeked; i += 2) {
                            std::cout << " 0x" << std::hex
                                      << std::setfill('0') << std::setw(2) << (int)peek[off]
                                      << std::setw(2) << (int)peek[off + 1] << std::dec;
                            off += 2;
                        }
                        std::cout << "\n";
                    }
                }
            }
        } else if (peeked <= 0) {
            std::cerr << "[Cast] Client sent no data (peek=" << peeked << ")\n";
            SOCK_CLOSE(sock);
            return;
        }

        SSL* ssl = SSL_new(ssl_ctx);
        if (!ssl) { SOCK_CLOSE(sock); return; }

        SSL_set_fd(ssl, (int)sock);

        if (SSL_accept(ssl) <= 0) {
            int ssl_err = SSL_get_error(ssl, -1);
            unsigned long err = ERR_get_error();
            char errbuf[256];
            ERR_error_string_n(err, errbuf, sizeof(errbuf));
            std::cerr << "[Cast] TLS handshake failed: " << errbuf
                      << " (SSL_error=" << ssl_err << ")\n";
            while ((err = ERR_get_error()) != 0) {
                ERR_error_string_n(err, errbuf, sizeof(errbuf));
                std::cerr << "[Cast]   chained: " << errbuf << "\n";
            }
            // If it was a small ClientHello, dump the raw hex
            if (peeked > 0 && peeked <= 300) {
                std::cerr << "[Cast]   Raw ClientHello (" << peeked << " bytes): ";
                for (int i = 0; i < peeked && i < 300; i++) {
                    char h[4]; snprintf(h, sizeof(h), "%02X", peek[i]);
                    std::cerr << h;
                    if (i % 32 == 31) std::cerr << "\n[Cast]     ";
                }
                std::cerr << "\n";
            }
            SSL_free(ssl);
            SOCK_CLOSE(sock);
            return;
        }

        std::cout << "[Cast] TLS client connected\n";

        {
            std::lock_guard<std::mutex> lock(client_mutex);
            client_ssl = ssl;
            client_sock = sock;
        }

        // Message loop
        while (true) {
            CastMessage msg;
            if (!recv_cast_msg(ssl, msg)) break;

            std::cout << "[Cast] " << msg.ns << " from " << msg.source_id
                      << " → " << msg.destination_id;
            if (msg.payload_type == 0 && !msg.payload_utf8.empty()) {
                std::string type = json_get(msg.payload_utf8, "type");
                std::cout << " type=" << type;
            }
            std::cout << "\n";

            if (msg.ns == NS_CONNECTION)       handle_connection(ssl, msg);
            else if (msg.ns == NS_HEARTBEAT)   handle_heartbeat(ssl, msg);
            else if (msg.ns == NS_RECEIVER)    handle_receiver(ssl, msg);
            else if (msg.ns == NS_DEVICEAUTH)  handle_auth(ssl, msg);
            else if (msg.ns == NS_WEBRTC)      handle_webrtc(ssl, msg);
            else if (msg.ns == NS_MIRROR)      handle_webrtc(ssl, msg);
            else {
                std::cout << "[Cast] Unknown namespace: " << msg.ns << "\n";
                if (msg.payload_type == 0)
                    std::cout << "[Cast]   payload: " << msg.payload_utf8.substr(0, 300) << "\n";
            }
        }

        std::cout << "[Cast] Client disconnected\n";
        mirroring_active = false;

        {
            std::lock_guard<std::mutex> lock(client_mutex);
            client_ssl = nullptr;
            client_sock = BAD_SOCK;
        }

        SSL_shutdown(ssl);
        SSL_free(ssl);
        SOCK_CLOSE(sock);
    }

    // --- Cleanup ---

    void cleanup() {
        // Clean up mDNS (Bonjour or built-in)
        if (mdns_backend == MdnsBackend::Bonjour) {
            if (mdns_ref && fn_dealloc) {
                fn_dealloc(mdns_ref);
                mdns_ref = nullptr;
            }
        }
        if (mdns_backend == MdnsBackend::Builtin) {
            mdns_running.store(false);
            if (mdns_sock != BAD_SOCK) {
                ip_mreq mreq{};
                inet_pton(AF_INET, MDNS_MCAST_ADDR, &mreq.imr_multiaddr);
                mreq.imr_interface.s_addr = INADDR_ANY;
                setsockopt(mdns_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                           reinterpret_cast<const char*>(&mreq), sizeof(mreq));
                SOCK_CLOSE(mdns_sock);
                mdns_sock = BAD_SOCK;
            }
            if (mdns_thread.joinable()) mdns_thread.join();
        }
        mdns_backend = MdnsBackend::None;

        if (listen_sock != BAD_SOCK) { SOCK_CLOSE(listen_sock); listen_sock = BAD_SOCK; }
        if (udp_sock != BAD_SOCK) { SOCK_CLOSE(udp_sock); udp_sock = BAD_SOCK; }

        {
            std::lock_guard<std::mutex> lock(client_mutex);
            if (client_ssl) { SSL_shutdown(client_ssl); }
            if (client_sock != BAD_SOCK) { SOCK_CLOSE(client_sock); client_sock = BAD_SOCK; }
        }

        if (ssl_ctx) { SSL_CTX_free(ssl_ctx); ssl_ctx = nullptr; }
        if (dtls_ctx) { SSL_CTX_free(dtls_ctx); dtls_ctx = nullptr; }

#ifdef _WIN32
        if (bonjour_lib) { FreeLibrary((HMODULE)bonjour_lib); bonjour_lib = nullptr; }
#endif
    }
};

// ---- Public API ----

CastReceiver::CastReceiver() : impl_(new Impl) {}

CastReceiver::~CastReceiver() {
    stop();
    delete impl_;
}

bool CastReceiver::start(const Config& config) {
    impl_->config = config;
    impl_->device_id = random_hex(16);

    uint32_t ip = get_local_ipv4();
    impl_->local_ip = ip_to_string(ip);

    // Generate TLS certificate
    if (!impl_->ssl_cert.generate()) {
        std::cerr << "[Cast] Failed to generate TLS certificate\n";
        return false;
    }
    std::cout << "[Cast] Certificate fingerprint: " << impl_->ssl_cert.fingerprint_sha256 << "\n";

    // Initialize SSL contexts
    if (!impl_->init_ssl()) {
        std::cerr << "[Cast] Failed to initialize TLS\n";
        return false;
    }
    if (!impl_->init_dtls_ctx()) {
        std::cerr << "[Cast] Failed to initialize DTLS\n";
        return false;
    }

    // Start TCP listener
    if (!impl_->init_listen(config.port)) {
        std::cerr << "[Cast] Failed to listen on port " << config.port << "\n";
        return false;
    }

    // Register mDNS
    if (!impl_->init_mdns(config.device_name, config.port)) {
        std::cerr << "[Cast] Warning: mDNS registration failed, Cast discovery won't work\n";
    }

    running_.store(true);

    // Accept thread
    impl_->accept_thread = std::thread([this]() {
        while (running_.load()) {
            sockaddr_in client_addr{};
            int addr_len = sizeof(client_addr);
            sock_t client = accept(impl_->listen_sock, (sockaddr*)&client_addr, &addr_len);
            if (client == BAD_SOCK) continue;

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
            std::cout << "[Cast] TCP connection from " << ip << ":"
                      << ntohs(client_addr.sin_port) << "\n";

            // Handle one client at a time (Cast protocol is single-sender)
            if (impl_->client_thread.joinable()) {
                impl_->client_thread.join();
            }
            impl_->client_thread = std::thread([this, client]() {
                impl_->handle_client(client);
                if (on_disconnect_) on_disconnect_();
            });
        }
    });

    std::cout << "[Cast] Receiver started as '" << config.device_name
              << "' on port " << config.port << "\n";
    std::cout << "[Cast] Android devices can now cast to this PC\n";
    return true;
}

void CastReceiver::stop() {
    running_.store(false);

    impl_->cleanup();

    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    if (impl_->client_thread.joinable()) impl_->client_thread.join();
    if (impl_->webrtc_thread.joinable()) impl_->webrtc_thread.join();

    std::cout << "[Cast] Receiver stopped\n";
}

} // namespace openmirror::cast

#endif // ENABLE_CAST
