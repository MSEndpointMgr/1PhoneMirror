#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace openmirror::airplay {

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
    // hw_addr: 6-byte MAC address (used as device ID)
    // require_pin: advertise password-required (pw=true, flags=0x44)
    //              so iOS prompts for an on-screen PIN.
    bool register_airplay(const std::string& server_name, uint16_t port,
                          const uint8_t hw_addr[6],
                          bool require_pin = false);

    void unregister();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace openmirror::airplay
