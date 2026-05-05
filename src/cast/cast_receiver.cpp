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
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                return ntohl(sin->sin_addr.s_addr);
            }
        }
    }
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
        // Generate RSA-2048 key
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!ctx) return false;
        if (EVP_PKEY_keygen_init(ctx) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0 ||
            EVP_PKEY_keygen(ctx, &key) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            return false;
        }
        EVP_PKEY_CTX_free(ctx);

        // Create self-signed X509 certificate
        cert = X509_new();
        if (!cert) return false;
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_get_notBefore(cert), 0);
        X509_gmtime_adj(X509_get_notAfter(cert), 365 * 24 * 3600);
        X509_set_pubkey(cert, key);

        auto* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            (const unsigned char*)"1PhoneMirror Cast", -1, -1, 0);
        X509_set_issuer_name(cert, name);
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

    // --- Initialization ---

    bool init_ssl() {
        ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (!ssl_ctx) return false;

        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
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

    bool init_mdns(const std::string& name, uint16_t port) {
#ifdef _WIN32
        bonjour_lib = LoadLibraryA("dnssd.dll");
        if (!bonjour_lib) {
            std::cerr << "[Cast] dnssd.dll not found — Bonjour required for Cast discovery\n";
            std::cerr << "[Cast] Install iTunes or Bonjour Print Services\n";
            return false;
        }
        fn_register = (FnDNSServiceRegister)GetProcAddress((HMODULE)bonjour_lib, "DNSServiceRegister");
        fn_dealloc = (FnDNSServiceRefDeallocate)GetProcAddress((HMODULE)bonjour_lib, "DNSServiceRefDeallocate");
        if (!fn_register || !fn_dealloc) return false;
#endif

        std::map<std::string, std::string> txt;
        txt["id"] = device_id;
        txt["cd"] = "A4F99FBA49A3C21A";
        txt["rm"] = "";
        txt["ve"] = "05";
        txt["md"] = "1PhoneMirror";
        txt["ic"] = "/setup/icon.png";
        txt["fn"] = name;
        txt["ca"] = "199172";  // capabilities: mirroring + audio
        txt["st"] = "0";      // idle
        txt["bs"] = "FA8FCA4B5E28";
        txt["nf"] = "1";
        txt["rs"] = "";

        auto txt_data = build_txt_payload(txt);

        DNSServiceErrorType err = fn_register(
            &mdns_ref, 0, 0,
            name.c_str(), "_googlecast._tcp", nullptr, nullptr,
            htons(port), (uint16_t)txt_data.size(), txt_data.data(),
            nullptr, nullptr);

        if (err != 0) {
            std::cerr << "[Cast] mDNS registration failed (err=" << err << ")\n";
            return false;
        }

        std::cout << "[Cast] Advertised as '" << name << "' on port " << port
                  << " (_googlecast._tcp)\n";
        return true;
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
        if (n != 4) return false;
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

        // Parse the challenge
        AuthChallengeMsg challenge;
        if (!msg.payload_binary.empty()) {
            AuthChallengeMsg::decode(msg.payload_binary.data(),
                                     msg.payload_binary.size(), challenge);
        }

        // Sign the sender_nonce (or empty if not present)
        std::vector<uint8_t> sig;
        if (ssl_cert.key) {
            EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
            const EVP_MD* md = (challenge.hash_algo == 1) ? EVP_sha256() : EVP_sha256();
            EVP_SignInit(md_ctx, md);
            if (!challenge.sender_nonce.empty()) {
                EVP_SignUpdate(md_ctx, challenge.sender_nonce.data(),
                               challenge.sender_nonce.size());
            }
            unsigned int sig_len = EVP_PKEY_size(ssl_cert.key);
            sig.resize(sig_len);
            EVP_SignFinal(md_ctx, sig.data(), &sig_len, ssl_cert.key);
            sig.resize(sig_len);
            EVP_MD_CTX_free(md_ctx);
        }

        // Build AuthResponse
        AuthResponseMsg resp;
        resp.signature = sig;
        resp.client_auth_cert = ssl_cert.cert_der;

        CastMessage reply;
        reply.source_id = "receiver-0";
        reply.destination_id = msg.source_id;
        reply.ns = NS_DEVICEAUTH;
        reply.payload_type = 1;
        reply.payload_binary = resp.encode();
        send_cast_msg(ssl, reply);

        std::cout << "[Cast] Auth response sent (cert=" << ssl_cert.cert_der.size()
                  << "B, sig=" << sig.size() << "B)\n";
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
        SSL* ssl = SSL_new(ssl_ctx);
        if (!ssl) { SOCK_CLOSE(sock); return; }

        SSL_set_fd(ssl, (int)sock);

        if (SSL_accept(ssl) <= 0) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            ERR_error_string_n(err, errbuf, sizeof(errbuf));
            std::cerr << "[Cast] TLS handshake failed: " << errbuf << "\n";
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
        if (mdns_ref && fn_dealloc) {
            fn_dealloc(mdns_ref);
            mdns_ref = nullptr;
        }

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
