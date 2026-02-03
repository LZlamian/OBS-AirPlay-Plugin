#include "rtp-handler.hpp"
#include <obs-module.h>
#include <string.h>
#include <arpa/inet.h>

RTPHandler::RTPHandler()
    : m_last_seq(0)
    , m_last_timestamp(0)
{
}

RTPHandler::~RTPHandler()
{
}

bool RTPHandler::parsePacket(const uint8_t* data, size_t len, RTPPacket& packet)
{
    if (len < 12) { // Minimum RTP header size
        return false;
    }
    
    // Parse fixed header
    packet.header.flags = data[0];
    packet.header.m_pt = data[1];
    packet.header.seq = ntohs(*(uint16_t*)(data + 2));
    packet.header.timestamp = ntohl(*(uint32_t*)(data + 4));
    packet.header.ssrc = ntohl(*(uint32_t*)(data + 8));
    
    // Check RTP version (should be 2)
    uint8_t version = (packet.header.flags >> 6) & 0x03;
    if (version != 2) {
        blog(LOG_WARNING, "RTP: Invalid version %d", version);
        return false;
    }
    
    // Calculate header length
    size_t header_len = 12;
    
    // Handle CSRC (Contributing Source) identifiers
    uint8_t csrc_count = packet.header.flags & 0x0F;
    header_len += csrc_count * 4;
    
    // Handle extension header if present
    if (packet.header.flags & 0x10) { // Extension bit
        if (len < header_len + 4) {
            return false;
        }
        uint16_t ext_len = ntohs(*(uint16_t*)(data + header_len + 2));
        header_len += 4 + (ext_len * 4);
    }
    
    // Handle padding if present
    size_t payload_len = len - header_len;
    if (packet.header.flags & 0x20) { // Padding bit
        if (payload_len > 0) {
            uint8_t padding_len = data[len - 1];
            if (padding_len <= payload_len) {
                payload_len -= padding_len;
            }
        }
    }
    
    // Set payload pointer and size
    if (payload_len > 0) {
        packet.payload = (uint8_t*)(data + header_len);
        packet.payload_size = payload_len;
    } else {
        packet.payload = nullptr;
        packet.payload_size = 0;
    }
    
    // Calculate PTS from RTP timestamp (90kHz clock for video)
    packet.pts = packet.header.timestamp;
    
    // Track sequence for loss detection
    if (m_last_seq != 0 && packet.header.seq != (uint16_t)(m_last_seq + 1)) {
        blog(LOG_DEBUG, "RTP: Sequence gap %d -> %d", m_last_seq, packet.header.seq);
    }
    m_last_seq = packet.header.seq;
    m_last_timestamp = packet.header.timestamp;
    
    return true;
}

uint8_t RTPHandler::getPayloadType(const RTPPacket& packet)
{
    return packet.header.m_pt & 0x7F;
}

bool RTPHandler::isMarkerSet(const RTPPacket& packet)
{
    return (packet.header.m_pt & 0x80) != 0;
}

uint16_t RTPHandler::getSequence(const RTPPacket& packet)
{
    return packet.header.seq;
}
