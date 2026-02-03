#pragma once

#include <stdint.h>
#include <sys/types.h>

// RTP Header structure
struct RTPHeader {
    uint8_t flags;          // V(2), P(1), X(1), CC(4)
    uint8_t m_pt;           // M(1), PT(7)
    uint16_t seq;           // Sequence number
    uint32_t timestamp;     // Timestamp
    uint32_t ssrc;          // Synchronization source
};

// RTP Packet info
struct RTPPacket {
    RTPHeader header;
    uint8_t* payload;
    size_t payload_size;
    uint64_t pts;           // Presentation timestamp
};

class RTPHandler {
public:
    RTPHandler();
    ~RTPHandler();
    
    // Parse RTP packet from buffer
    bool parsePacket(const uint8_t* data, size_t len, RTPPacket& packet);
    
    // Get payload type
    uint8_t getPayloadType(const RTPPacket& packet);
    
    // Check if marker bit is set (end of frame)
    bool isMarkerSet(const RTPPacket& packet);
    
    // Get sequence number
    uint16_t getSequence(const RTPPacket& packet);
    
private:
    uint16_t m_last_seq;
    uint32_t m_last_timestamp;
};
