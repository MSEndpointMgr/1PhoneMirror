// mdns_service.cpp — AirPlay mDNS service advertisement
//
// Two-tier approach for maximum compatibility:
//   1) Try to dynamically load dnssd.dll (Bonjour) at runtime — if the
//      Bonjour service is installed (via iTunes, AirServer, etc.), use it.
//   2) Fall back to a built-in mDNS multicast responder (RFC 6762) that
//      works on any machine without additional software.
//
// No compile-time SDK dependency in either case.

#include <openmirror/airplay/mdns_service.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using mdns_socket_t = SOCKET;
constexpr mdns_socket_t INVALID_MDNS_SOCK = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <dlfcn.h>
using mdns_socket_t = int;
constexpr mdns_socket_t INVALID_MDNS_SOCK = -1;
#endif

static constexpr const char* MDNS_ADDR = "224.0.0.251";
static constexpr uint16_t MDNS_PORT = 5353;

// DNS record types (for built-in responder)
static constexpr uint16_t DNS_TYPE_A     = 1;
static constexpr uint16_t DNS_TYPE_PTR   = 12;
static constexpr uint16_t DNS_TYPE_TXT   = 16;
static constexpr uint16_t DNS_TYPE_SRV   = 33;
static constexpr uint16_t DNS_TYPE_ANY   = 255;
static constexpr uint16_t DNS_CLASS_IN   = 1;
static constexpr uint16_t DNS_CLASS_FLUSH = 0x8001;

// ============================================================
// Bonjour dns_sd API — types and function pointers for dynamic loading
// ============================================================
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

namespace openmirror::airplay {

// ---- Shared helpers ----

static std::string mac_to_string(const uint8_t addr[6]) {
    std::ostringstream oss;
    for (int i = 0; i < 6; i++) {
        if (i > 0) oss << ':';
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(addr[i]);
    }
    return oss.str();
}

static std::string mac_to_id(const uint8_t addr[6]) {
    std::ostringstream oss;
    for (int i = 0; i < 6; i++) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(addr[i]);
    }
    return oss.str();
}

static std::vector<uint8_t> build_txt_payload(
    const std::map<std::string, std::string>& entries) {
    std::vector<uint8_t> txt;
    for (const auto& [key, val] : entries) {
        std::string entry = key + "=" + val;
        if (entry.size() > 255) continue;
        txt.push_back(static_cast<uint8_t>(entry.size()));
        txt.insert(txt.end(), entry.begin(), entry.end());
    }
    return txt;
}

static std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
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
            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            if (sa->sin_family == AF_INET) {
                uint32_t ip = ntohl(sa->sin_addr.s_addr);
                // Skip link-local (169.254.x.x)
                if ((ip >> 16) == 0xA9FE) continue;
                // Skip loopback range
                if ((ip >> 24) == 127) continue;
                // Prefer interfaces with a default gateway
                if (a->FirstGatewayAddress != nullptr) return ip;
                if (fallback == 0) fallback = ip;
            }
        }
    }
    return fallback;
#else
    struct ifaddrs* ifa_list = nullptr;
    if (getifaddrs(&ifa_list) != 0) return 0;
    uint32_t ip = 0;
    for (auto* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        uint32_t addr = ntohl(sa->sin_addr.s_addr);
        if ((addr >> 24) == 127) continue;
        ip = addr;
        break;
    }
    freeifaddrs(ifa_list);
    return ip;
#endif
}

// ============================================================
// DNS packet helpers (for built-in responder)
// ============================================================

static void put_u16(std::vector<uint8_t>& pkt, uint16_t val) {
    pkt.push_back(static_cast<uint8_t>(val >> 8));
    pkt.push_back(static_cast<uint8_t>(val & 0xFF));
}

static void put_u32(std::vector<uint8_t>& pkt, uint32_t val) {
    pkt.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(val & 0xFF));
}

static void put_name(std::vector<uint8_t>& pkt, const std::string& name) {
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

static std::vector<uint8_t> encode_name(const std::string& name) {
    std::vector<uint8_t> out;
    put_name(out, name);
    return out;
}

static std::string parse_name(const uint8_t* pkt, size_t pkt_len, size_t& offset) {
    std::string name;
    size_t saved_offset = 0;
    bool jumped = false;
    int safety = 0;
    while (offset < pkt_len && safety++ < 128) {
        uint8_t len = pkt[offset];
        if (len == 0) { offset++; break; }
        if ((len & 0xC0) == 0xC0) {
            if (offset + 1 >= pkt_len) break;
            if (!jumped) saved_offset = offset + 2;
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
    if (jumped) offset = saved_offset;
    return name;
}

// ============================================================
// Impl — holds state for whichever backend is active
// ============================================================

struct MdnsService::Impl {
    enum class Backend { None, Bonjour, Builtin };
    Backend backend = Backend::None;

    // --- Bonjour backend ---
#ifdef _WIN32
    HMODULE bonjour_dll = nullptr;
#else
    void* bonjour_dll = nullptr;
#endif
    FnDNSServiceRegister fn_register = nullptr;
    FnDNSServiceRefDeallocate fn_dealloc = nullptr;
    DNSServiceRef airplay_ref = nullptr;
    DNSServiceRef raop_ref = nullptr;

    // --- Built-in mDNS responder ---
    mdns_socket_t sock = INVALID_MDNS_SOCK;
    std::thread listener;
    std::atomic<bool> running{false};

    std::string server_name;
    uint16_t port = 0;
    std::string hostname;
    std::string airplay_service;
    std::string raop_service;
    std::string airplay_type;
    std::string raop_type;
    std::vector<uint8_t> airplay_txt;
    std::vector<uint8_t> raop_txt;
    uint32_t local_ip = 0;

    // Bonjour loading
    bool load_bonjour();
    void unload_bonjour();

    // Built-in responder
    void listen_loop();
    void handle_query(const uint8_t* pkt, size_t len);
    std::vector<uint8_t> build_response(const std::string& qname, uint16_t qtype);
    void add_service_records(std::vector<uint8_t>& pkt, uint16_t& count,
                             const std::string& svc, const std::string& type,
                             const std::vector<uint8_t>& txt);
    void send_announcement();
    void send_mdns(const std::vector<uint8_t>& pkt);
};

// ============================================================
// Bonjour dynamic loading
// ============================================================

bool MdnsService::Impl::load_bonjour() {
#ifdef _WIN32
    bonjour_dll = LoadLibraryA("dnssd.dll");
    if (!bonjour_dll) return false;
    fn_register = reinterpret_cast<FnDNSServiceRegister>(
        GetProcAddress(bonjour_dll, "DNSServiceRegister"));
    fn_dealloc = reinterpret_cast<FnDNSServiceRefDeallocate>(
        GetProcAddress(bonjour_dll, "DNSServiceRefDeallocate"));
#else
    bonjour_dll = dlopen("libdns_sd.so", RTLD_LAZY);
    if (!bonjour_dll) bonjour_dll = dlopen("libdns_sd.so.1", RTLD_LAZY);
    if (!bonjour_dll) return false;
    fn_register = reinterpret_cast<FnDNSServiceRegister>(
        dlsym(bonjour_dll, "DNSServiceRegister"));
    fn_dealloc = reinterpret_cast<FnDNSServiceRefDeallocate>(
        dlsym(bonjour_dll, "DNSServiceRefDeallocate"));
#endif
    return fn_register && fn_dealloc;
}

void MdnsService::Impl::unload_bonjour() {
    if (airplay_ref && fn_dealloc) fn_dealloc(airplay_ref);
    if (raop_ref && fn_dealloc) fn_dealloc(raop_ref);
    airplay_ref = nullptr;
    raop_ref = nullptr;
#ifdef _WIN32
    if (bonjour_dll) { FreeLibrary(bonjour_dll); bonjour_dll = nullptr; }
#else
    if (bonjour_dll) { dlclose(bonjour_dll); bonjour_dll = nullptr; }
#endif
}

// ============================================================
// Built-in mDNS responder
// ============================================================

void MdnsService::Impl::send_mdns(const std::vector<uint8_t>& pkt) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_ADDR, &dest.sin_addr);
    sendto(sock, reinterpret_cast<const char*>(pkt.data()),
           static_cast<int>(pkt.size()), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

void MdnsService::Impl::add_service_records(
    std::vector<uint8_t>& pkt, uint16_t& count,
    const std::string& svc, const std::string& type,
    const std::vector<uint8_t>& txt) {

    uint32_t ttl = 4500;

    put_name(pkt, type);
    put_u16(pkt, DNS_TYPE_PTR); put_u16(pkt, DNS_CLASS_IN);
    put_u32(pkt, ttl);
    auto ptr_rdata = encode_name(svc);
    put_u16(pkt, static_cast<uint16_t>(ptr_rdata.size()));
    pkt.insert(pkt.end(), ptr_rdata.begin(), ptr_rdata.end());
    count++;

    put_name(pkt, svc);
    put_u16(pkt, DNS_TYPE_SRV); put_u16(pkt, DNS_CLASS_FLUSH);
    put_u32(pkt, ttl);
    auto host_enc = encode_name(hostname);
    put_u16(pkt, static_cast<uint16_t>(6 + host_enc.size()));
    put_u16(pkt, 0); put_u16(pkt, 0);
    put_u16(pkt, port);
    pkt.insert(pkt.end(), host_enc.begin(), host_enc.end());
    count++;

    put_name(pkt, svc);
    put_u16(pkt, DNS_TYPE_TXT); put_u16(pkt, DNS_CLASS_FLUSH);
    put_u32(pkt, ttl);
    put_u16(pkt, static_cast<uint16_t>(txt.size()));
    pkt.insert(pkt.end(), txt.begin(), txt.end());
    count++;

    if (local_ip != 0) {
        put_name(pkt, hostname);
        put_u16(pkt, DNS_TYPE_A); put_u16(pkt, DNS_CLASS_FLUSH);
        put_u32(pkt, ttl); put_u16(pkt, 4);
        put_u32(pkt, local_ip);
        count++;
    }
}

std::vector<uint8_t> MdnsService::Impl::build_response(
    const std::string& qname, uint16_t qtype) {

    std::string q = to_lower(qname);
    bool match_airplay_type = (q == to_lower(airplay_type));
    bool match_raop_type    = (q == to_lower(raop_type));
    bool match_airplay_svc  = (q == to_lower(airplay_service));
    bool match_raop_svc     = (q == to_lower(raop_service));
    bool match_host         = (q == to_lower(hostname));
    bool match_services     = (q == "_services._dns-sd._udp.local");

    bool want_ptr = (qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY);
    bool want_svc = (qtype == DNS_TYPE_SRV || qtype == DNS_TYPE_TXT || qtype == DNS_TYPE_ANY);
    bool want_a   = (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY);

    std::vector<uint8_t> pkt;
    put_u16(pkt, 0); put_u16(pkt, 0x8400);
    put_u16(pkt, 0);
    size_t anpos = pkt.size();
    put_u16(pkt, 0); put_u16(pkt, 0); put_u16(pkt, 0);
    uint16_t count = 0;

    if (match_services && want_ptr) {
        put_name(pkt, "_services._dns-sd._udp.local");
        put_u16(pkt, DNS_TYPE_PTR); put_u16(pkt, DNS_CLASS_IN);
        put_u32(pkt, 4500);
        auto r = encode_name(airplay_type);
        put_u16(pkt, static_cast<uint16_t>(r.size()));
        pkt.insert(pkt.end(), r.begin(), r.end());
        count++;

        put_name(pkt, "_services._dns-sd._udp.local");
        put_u16(pkt, DNS_TYPE_PTR); put_u16(pkt, DNS_CLASS_IN);
        put_u32(pkt, 4500);
        auto r2 = encode_name(raop_type);
        put_u16(pkt, static_cast<uint16_t>(r2.size()));
        pkt.insert(pkt.end(), r2.begin(), r2.end());
        count++;
    }

    if ((match_airplay_type || match_airplay_svc) && (want_ptr || want_svc))
        add_service_records(pkt, count, airplay_service, airplay_type, airplay_txt);
    if ((match_raop_type || match_raop_svc) && (want_ptr || want_svc))
        add_service_records(pkt, count, raop_service, raop_type, raop_txt);
    if (match_host && want_a && local_ip != 0) {
        put_name(pkt, hostname);
        put_u16(pkt, DNS_TYPE_A); put_u16(pkt, DNS_CLASS_FLUSH);
        put_u32(pkt, 4500); put_u16(pkt, 4);
        put_u32(pkt, local_ip);
        count++;
    }

    if (count == 0) return {};
    pkt[anpos] = static_cast<uint8_t>(count >> 8);
    pkt[anpos + 1] = static_cast<uint8_t>(count & 0xFF);
    return pkt;
}

void MdnsService::Impl::handle_query(const uint8_t* pkt, size_t len) {
    if (len < 12) return;
    uint16_t flags = (pkt[2] << 8) | pkt[3];
    if (flags & 0x8000) return;
    uint16_t qdcount = (pkt[4] << 8) | pkt[5];
    size_t offset = 12;
    for (uint16_t i = 0; i < qdcount && offset < len; i++) {
        std::string qname = parse_name(pkt, len, offset);
        if (offset + 4 > len) break;
        uint16_t qtype = (pkt[offset] << 8) | pkt[offset + 1];
        offset += 4;
        auto resp = build_response(qname, qtype);
        if (!resp.empty()) send_mdns(resp);
    }
}

void MdnsService::Impl::send_announcement() {
    std::vector<uint8_t> pkt;
    put_u16(pkt, 0); put_u16(pkt, 0x8400);
    put_u16(pkt, 0);
    size_t anpos = pkt.size();
    put_u16(pkt, 0); put_u16(pkt, 0); put_u16(pkt, 0);
    uint16_t count = 0;
    add_service_records(pkt, count, airplay_service, airplay_type, airplay_txt);
    add_service_records(pkt, count, raop_service, raop_type, raop_txt);
    pkt[anpos] = static_cast<uint8_t>(count >> 8);
    pkt[anpos + 1] = static_cast<uint8_t>(count & 0xFF);
    send_mdns(pkt);
    send_mdns(pkt);
}

void MdnsService::Impl::listen_loop() {
    uint8_t buf[4096];
    while (running.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        timeval tv{1, 0};
        int ret = select(static_cast<int>(sock) + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;
        sockaddr_in from{};
        int fromlen = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n > 0) handle_query(buf, static_cast<size_t>(n));
    }
}

// ============================================================
// Public API
// ============================================================

MdnsService::MdnsService() : impl_(new Impl) {}
MdnsService::~MdnsService() { unregister(); delete impl_; }

bool MdnsService::register_airplay(const std::string& server_name, uint16_t port,
                                     const uint8_t hw_addr[6],
                                     bool require_pin) {
    std::string device_id = mac_to_string(hw_addr);
    std::string mac_id = mac_to_id(hw_addr);

    // Print local IP for diagnostics
    uint32_t lip = get_local_ipv4();
    if (lip != 0) {
        std::cout << "[mDNS] Local IP: "
                  << ((lip >> 24) & 0xFF) << "." << ((lip >> 16) & 0xFF) << "."
                  << ((lip >> 8) & 0xFF)  << "." << (lip & 0xFF) << "\n";
    }

    // ---- Try Bonjour first ----
    if (impl_->load_bonjour()) {
        std::cout << "[mDNS] Bonjour service found, using dnssd.dll\n";

        auto airplay_txt = build_txt_payload({
            {"deviceid", device_id},
            {"features", "0x5A7FFEE6"},
            // PIN-required pairing: flags bit 0x8 = "PIN required" (no plaintext
            // password). When require_pin is false we just advertise as AirPlay
            // capable. We deliberately keep pw=false in PIN mode so iOS shows the
            // "Enter the onscreen code" prompt instead of the password prompt.
            {"flags", require_pin ? "0x8" : "0x4"},
            {"model", "AppleTV3,2"},
            {"pi", "2e388006-13ba-4041-9a67-25dd4a43d536"},
            {"pk", "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7"},
            {"srcvers", "220.68"},
            {"vv", "2"},
        });

        auto err = impl_->fn_register(
            &impl_->airplay_ref, 0, 0,
            server_name.c_str(), "_airplay._tcp",
            nullptr, nullptr, htons(port),
            static_cast<uint16_t>(airplay_txt.size()), airplay_txt.data(),
            nullptr, nullptr);

        if (err != 0) {
            std::cerr << "[mDNS] Bonjour _airplay._tcp register failed: " << err << "\n";
            impl_->unload_bonjour();
            // Fall through to built-in responder
        } else {
            std::string raop_name = mac_id + "@" + server_name;
            auto raop_txt = build_txt_payload({
                {"am", "AppleTV3,2"}, {"ch", "2"}, {"cn", "0,1,2,3"}, {"da", "true"},
                {"et", "0,3,5"}, {"ft", "0x5A7FFEE6"}, {"md", "0,1,2"},
                {"pk", "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7"},
                {"pw", "false"}, {"rhd", "5.6.0.0"},
                {"sf", require_pin ? "0x8" : "0x4"},
                {"sr", "44100"}, {"ss", "16"}, {"sv", "false"}, {"tp", "UDP"},
                {"txtvers", "1"}, {"vn", "65537"}, {"vs", "220.68"}, {"vv", "2"},
            });

            err = impl_->fn_register(
                &impl_->raop_ref, 0, 0,
                raop_name.c_str(), "_raop._tcp",
                nullptr, nullptr, htons(port),
                static_cast<uint16_t>(raop_txt.size()), raop_txt.data(),
                nullptr, nullptr);

            if (err != 0) {
                std::cerr << "[mDNS] Bonjour _raop._tcp register failed: " << err
                          << ", continuing with _airplay._tcp only\n";
            }

            impl_->backend = Impl::Backend::Bonjour;
            std::cout << "[mDNS] AirPlay advertised as '" << server_name
                      << "' on port " << port << " (Bonjour)\n";
            return true;
        }
    }

    // ---- Fall back to built-in mDNS multicast responder ----
    std::cout << "[mDNS] Bonjour not available, using built-in mDNS responder\n";

    impl_->server_name = server_name;
    impl_->port = port;
    impl_->local_ip = lip;

    if (impl_->local_ip == 0) {
        std::cerr << "[mDNS] Could not determine local IP address\n";
        return false;
    }

    char hname[256] = {};
    gethostname(hname, sizeof(hname));
    impl_->hostname = std::string(hname) + ".local";

    impl_->airplay_type    = "_airplay._tcp.local";
    impl_->raop_type       = "_raop._tcp.local";
    impl_->airplay_service = server_name + "._airplay._tcp.local";
    impl_->raop_service    = mac_id + "@" + server_name + "._raop._tcp.local";

    impl_->airplay_txt = build_txt_payload({
        {"deviceid", device_id},
        {"features", "0x5A7FFEE6"},
        {"flags", require_pin ? "0x8" : "0x4"},
        {"model", "AppleTV3,2"},
        {"pi", "2e388006-13ba-4041-9a67-25dd4a43d536"},
        {"pk", "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7"},
        {"srcvers", "220.68"},
        {"vv", "2"},
    });

    impl_->raop_txt = build_txt_payload({
        {"am", "AppleTV3,2"}, {"ch", "2"}, {"cn", "0,1,2,3"}, {"da", "true"},
        {"et", "0,3,5"}, {"ft", "0x5A7FFEE6"}, {"md", "0,1,2"},
        {"pk", "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7"},
        {"pw", "false"}, {"rhd", "5.6.0.0"},
        {"sf", require_pin ? "0x8" : "0x4"},
        {"sr", "44100"}, {"ss", "16"}, {"sv", "false"}, {"tp", "UDP"},
        {"txtvers", "1"}, {"vn", "65537"}, {"vs", "220.68"}, {"vv", "2"},
    });

    impl_->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->sock == INVALID_MDNS_SOCK) {
        std::cerr << "[mDNS] Failed to create socket\n";
        return false;
    }

    int reuse = 1;
    setsockopt(impl_->sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(MDNS_PORT);

    if (bind(impl_->sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
#ifdef _WIN32
        std::cerr << "[mDNS] Bind to port 5353 failed (error "
                  << WSAGetLastError() << ")\n";
#else
        std::cerr << "[mDNS] Bind to port 5353 failed\n";
#endif
        return false;
    }

    ip_mreq mreq{};
    inet_pton(AF_INET, MDNS_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(impl_->local_ip);
    if (setsockopt(impl_->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
        std::cerr << "[mDNS] Failed to join multicast group\n";
        return false;
    }

    int ttl = 255;
    setsockopt(impl_->sock, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    int loop = 1;
    setsockopt(impl_->sock, IPPROTO_IP, IP_MULTICAST_LOOP,
               reinterpret_cast<const char*>(&loop), sizeof(loop));
    // Send multicast on the correct interface
    in_addr mc_if{};
    mc_if.s_addr = htonl(impl_->local_ip);
    setsockopt(impl_->sock, IPPROTO_IP, IP_MULTICAST_IF,
               reinterpret_cast<const char*>(&mc_if), sizeof(mc_if));

    impl_->running.store(true);
    impl_->listener = std::thread(&Impl::listen_loop, impl_);
    impl_->send_announcement();

    impl_->backend = Impl::Backend::Builtin;
    std::cout << "[mDNS] AirPlay advertised as '" << server_name
              << "' on port " << port << " (built-in mDNS)\n";
    return true;
}

void MdnsService::unregister() {
    if (!impl_) return;

    if (impl_->backend == Impl::Backend::Bonjour) {
        impl_->unload_bonjour();
    }

    if (impl_->backend == Impl::Backend::Builtin) {
        impl_->running.store(false);
        if (impl_->sock != INVALID_MDNS_SOCK) {
            ip_mreq mreq{};
            inet_pton(AF_INET, MDNS_ADDR, &mreq.imr_multiaddr);
            mreq.imr_interface.s_addr = INADDR_ANY;
            setsockopt(impl_->sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                       reinterpret_cast<const char*>(&mreq), sizeof(mreq));
#ifdef _WIN32
            closesocket(impl_->sock);
#else
            close(impl_->sock);
#endif
            impl_->sock = INVALID_MDNS_SOCK;
        }
        if (impl_->listener.joinable()) impl_->listener.join();
    }

    impl_->backend = Impl::Backend::None;
}

} // namespace openmirror::airplay
