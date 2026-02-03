#pragma once

#include <cstdint>
#include <vector>

// RTP header structure (from RFC 3550)
#pragma pack(push, 1)
struct RTPHeader {
    uint8_t vpxcc;      // Version (2 bits), Padding (1 bit), Extension (1 bit), CSRC count (4 bits)
    uint8_t mpt;        // Marker (1 bit), Payload type (7 bits)
    uint16_t seq;       // Sequence number
    uint32_t timestamp; // Timestamp
    uint32_t ssrc;      // Synchronization source identifier
};
#pragma pack(pop)

class RTPReceiver {
public:
    RTPReceiver();
    ~RTPReceiver();
    
    // Start receiving on a UDP port
    bool start(int port);
    
    // Stop receiving
    void stop();
    
    // Process a received RTP packet
    // Returns the payload data (e.g., H.264 NAL unit)
    std::vector<uint8_t> processPacket(const uint8_t* data, size_t len);
    
    // Get last sequence number (for detecting packet loss)
    uint16_t getLastSequence() const { return m_last_seq; }
    
    // Get last timestamp
    uint32_t getLastTimestamp() const { return m_last_timestamp; }
    
    // Get socket file descriptor (for direct access)
    int getSocket() const { return m_socket; }
    
private:
    int m_socket;
    uint16_t m_last_seq;
    uint32_t m_last_timestamp;
    bool m_running;
    
    // Parse RTP header
    bool parseHeader(const uint8_t* data, size_t len, RTPHeader& header);
    
    // Handle packet reordering
    void handleReordering(uint16_t seq);
};
