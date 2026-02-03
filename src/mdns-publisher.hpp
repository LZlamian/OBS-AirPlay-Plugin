#pragma once

#include <string>
#include <vector>
#include <memory>

#ifdef __APPLE__
#include <dns_sd.h>
#endif

class MDNSPublisher {
public:
    MDNSPublisher();
    ~MDNSPublisher();
    
    // Start advertising AirPlay services
    bool start(const std::string& server_name, uint16_t airplay_port, uint16_t raop_port, const std::string& mac_address);
    
    // Stop advertising
    void stop();
    
    // Check if currently advertising
    bool isActive() const { return m_active; }
    
    // Get the current server name
    std::string getServerName() const { return m_server_name; }
    
private:
    bool m_active;
    std::string m_server_name;
    std::string m_mac_address;
    
#ifdef __APPLE__
    DNSServiceRef m_airplay_service;
    DNSServiceRef m_raop_service;
    
    static void DNSSD_API register_callback(
        DNSServiceRef sdRef,
        DNSServiceFlags flags,
        DNSServiceErrorType errorCode,
        const char *name,
        const char *regtype,
        const char *domain,
        void *context);
#endif
    
    std::string generateDeviceID();
    std::string generateFeatures();
    std::vector<std::string> createAirPlayTxtRecord();
    std::vector<std::string> createRAOPTxtRecord();
};
