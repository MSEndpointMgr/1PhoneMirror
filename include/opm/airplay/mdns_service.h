#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace opm::airplay {

// Advertises an AirPlay service via mDNS multicast.
// Implements a lightweight mDNS responder (RFC 6762) that listens
// on 224.0.0.251:5353 and answers queries for the registered services.
// No Bonjour SDK or external dependencies required.

class MdnsService {
public:
    MdnsService();
    ~MdnsService();

    // Register the AirPlay + RAOP services
    // server_name: display name shown on iOS devices
    // port: TCP port the AirPlay server is listening on
    // hw_addr: 6-byte MAC address (used as the legacy AirPlay "deviceid" TXT;
    //          NOT used as the HAP pairing identifier)
    // pi: HAP pairing identifier from HapDevice (e.g. "4a:9c:2f:11:88:b0"),
    //     must be stable across restarts
    // pk_hex: 64-char lowercase hex of the device's Ed25519 long-term public key
    bool register_airplay(const std::string& server_name, uint16_t port,
                          const uint8_t hw_addr[6],
                          const std::string& pi,
                          const std::string& pk_hex);

    void unregister();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace opm::airplay
