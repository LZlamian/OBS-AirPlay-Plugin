#include "mdns-publisher.hpp"
#include <obs-module.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>
#include <cctype>

#ifdef __APPLE__
#include <dns_sd.h>
#include <arpa/inet.h>
#endif

MDNSPublisher::MDNSPublisher()
    : m_active(false)
#ifdef __APPLE__
    , m_airplay_service(nullptr)
    , m_raop_service(nullptr)
#endif
{
}

MDNSPublisher::~MDNSPublisher()
{
    stop();
}

static std::string make_raop_service_id(const std::string& mac_address)
{
    std::string normalized;
    normalized.reserve(12);

    for (char ch : mac_address) {
        if (ch == ':') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }

    return normalized;
}

std::string MDNSPublisher::generateDeviceID()
{
    if (!m_mac_address.empty()) {
        return m_mac_address;
    }
    
    // Generate a random MAC address
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    std::stringstream ss;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ss << ":";
        ss << std::hex << std::setw(2) << std::setfill('0') << dis(gen);
    }
    
    m_mac_address = ss.str();
    return m_mac_address;
}

std::string MDNSPublisher::generateFeatures()
{
    // AirPlay 2 feature flags for video mirroring
    // 0x527FFFF7,0x1E = Full AirPlay with video, audio, photos
    // This shows up in Screen Mirroring list
    return "0x527FFFF7,0x1E";
}

std::vector<std::string> MDNSPublisher::createAirPlayTxtRecord()
{
    std::vector<std::string> txt_records;
    
    std::string device_id = generateDeviceID();
    std::string features = generateFeatures();
    
    // AirPlay TXT records that appear in Screen Mirroring
    txt_records.push_back("deviceid=" + device_id);
    txt_records.push_back("features=" + features);
    txt_records.push_back("srcvers=377.28.01");
    txt_records.push_back("model=AppleTV6,2");
    txt_records.push_back("protovers=1.1");
    txt_records.push_back("acl=0");
    txt_records.push_back("flags=0x4");
    txt_records.push_back("vv=2");
    txt_records.push_back("pw=false");
    txt_records.push_back("psi=00000000-0000-0000-0000-000000000000");
    txt_records.push_back("gid=00000000-0000-0000-0000-000000000000");
    
    // Add additional critical AirPlay fields
    std::string pk_val = m_pk.empty() ? device_id : m_pk;
    txt_records.push_back("pk=" + pk_val);     // Public key identifier
    txt_records.push_back("sf=0x4");           // Source features
    txt_records.push_back("rhd=1.0");          // Remote HD version
    txt_records.push_back("pi=" + device_id); // Pairing identifier
    txt_records.push_back("c#features=" + features); // Client features
    
    return txt_records;
}

std::vector<std::string> MDNSPublisher::createRAOPTxtRecord()
{
    std::vector<std::string> txt_records;
    
    std::string device_id = generateDeviceID();
    
    // Required RAOP (Remote Audio Output Protocol) TXT records
    txt_records.push_back("txtvers=1");
    txt_records.push_back("ch=2");           // 2 audio channels (stereo)
    txt_records.push_back("cn=0,1,2,3");     // Codec types supported
    txt_records.push_back("da=true");        // Digital audio
    txt_records.push_back("et=0,3,5");       // Encryption types
    txt_records.push_back("md=0,1,2");       // Metadata types
    txt_records.push_back("pw=false");       // No password
    txt_records.push_back("sr=44100");       // Sample rate
    txt_records.push_back("ss=16");          // Sample size
    txt_records.push_back("tp=UDP");         // Transport protocol
    txt_records.push_back("vn=65537");       // Version number
    txt_records.push_back("vs=377.28.01");   // Version string
    txt_records.push_back("am=OBSAirPlay1,1"); // Model
    txt_records.push_back("sf=0x4");         // Supported features
    
    if (!m_pk.empty()) {
        txt_records.push_back("pk=" + m_pk);
    }
    
    return txt_records;
}

#ifdef __APPLE__
void DNSSD_API MDNSPublisher::register_callback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    DNSServiceErrorType errorCode,
    const char *name,
    const char *regtype,
    const char *domain,
    void *context)
{
    if (errorCode == kDNSServiceErr_NoError) {
        blog(LOG_INFO, "Successfully registered service: %s.%s%s", name, regtype, domain);
    } else {
        blog(LOG_ERROR, "Failed to register service: error %d", errorCode);
    }
}
#endif

bool MDNSPublisher::start(const std::string& server_name, uint16_t airplay_port, uint16_t raop_port, const std::string& mac_address, const std::string& pk)
{
    if (m_active) {
        blog(LOG_WARNING, "mDNS publisher already active");
        return false;
    }
    
    m_server_name = server_name;
    m_mac_address = mac_address;
    m_pk = pk;
    
#ifdef __APPLE__
    DNSServiceErrorType error;
    
    // Prepare AirPlay TXT record
    std::vector<std::string> airplay_txt = createAirPlayTxtRecord();
    TXTRecordRef airplay_txt_record;
    TXTRecordCreate(&airplay_txt_record, 0, NULL);
    
    for (const auto& record : airplay_txt) {
        size_t pos = record.find('=');
        if (pos != std::string::npos) {
            std::string key = record.substr(0, pos);
            std::string value = record.substr(pos + 1);
            TXTRecordSetValue(&airplay_txt_record, key.c_str(), value.length(), value.c_str());
        }
    }
    
    // Register _airplay._tcp service
    error = DNSServiceRegister(
        &m_airplay_service,
        0,                                    // flags
        0,                                    // interface (0 = all)
        m_server_name.c_str(),                // name
        "_airplay._tcp",                      // service type
        NULL,                                 // domain (NULL = default)
        NULL,                                 // host (NULL = default)
        htons(airplay_port),                  // port
        TXTRecordGetLength(&airplay_txt_record),
        TXTRecordGetBytesPtr(&airplay_txt_record),
        register_callback,                    // callback
        this                                  // context
    );
    
    TXTRecordDeallocate(&airplay_txt_record);
    
    if (error != kDNSServiceErr_NoError) {
        blog(LOG_ERROR, "Failed to register _airplay._tcp service: error %d", error);
        return false;
    }
    
    // Prepare RAOP TXT record
    std::vector<std::string> raop_txt = createRAOPTxtRecord();
    TXTRecordRef raop_txt_record;
    TXTRecordCreate(&raop_txt_record, 0, NULL);
    
    for (const auto& record : raop_txt) {
        size_t pos = record.find('=');
        if (pos != std::string::npos) {
            std::string key = record.substr(0, pos);
            std::string value = record.substr(pos + 1);
            TXTRecordSetValue(&raop_txt_record, key.c_str(), value.length(), value.c_str());
        }
    }
    
    // Create RAOP service name (MAC@Name format)
    std::string device_id = generateDeviceID();
    std::string raop_name = make_raop_service_id(device_id) + "@" + m_server_name;
    
    // Register _raop._tcp service
    error = DNSServiceRegister(
        &m_raop_service,
        0,                                    // flags
        0,                                    // interface
        raop_name.c_str(),                    // name
        "_raop._tcp",                         // service type
        NULL,                                 // domain
        NULL,                                 // host
        htons(raop_port),                     // port
        TXTRecordGetLength(&raop_txt_record),
        TXTRecordGetBytesPtr(&raop_txt_record),
        register_callback,                    // callback
        this                                  // context
    );
    
    TXTRecordDeallocate(&raop_txt_record);
    
    if (error != kDNSServiceErr_NoError) {
        blog(LOG_ERROR, "Failed to register _raop._tcp service: error %d", error);
        DNSServiceRefDeallocate(m_airplay_service);
        m_airplay_service = nullptr;
        return false;
    }
    
    m_active = true;
    blog(LOG_INFO, "mDNS advertising started for '%s' on ports %d (AirPlay) and %d (RAOP)", 
         m_server_name.c_str(), airplay_port, raop_port);
    
    return true;
#else
    blog(LOG_ERROR, "mDNS publishing not supported on this platform");
    return false;
#endif
}

void MDNSPublisher::stop()
{
    if (!m_active) {
        return;
    }
    
#ifdef __APPLE__
    if (m_airplay_service) {
        DNSServiceRefDeallocate(m_airplay_service);
        m_airplay_service = nullptr;
    }
    
    if (m_raop_service) {
        DNSServiceRefDeallocate(m_raop_service);
        m_raop_service = nullptr;
    }
#endif
    
    m_active = false;
    blog(LOG_INFO, "mDNS advertising stopped");
}
