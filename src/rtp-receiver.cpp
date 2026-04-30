#include "rtp-receiver.hpp"
#include <obs-module.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

RTPReceiver::RTPReceiver()
    : m_socket(-1)
    , m_last_seq(0)
    , m_last_timestamp(0)
    , m_running(false)
{
}

RTPReceiver::~RTPReceiver()
{
    stop();
}

bool RTPReceiver::start(int port)
{
    if (m_running) {
        return false;
    }
    
    // Create UDP socket
    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        blog(LOG_ERROR, "RTP: Failed to create UDP socket: %s", strerror(errno));
        return false;
    }
    
    // Set socket options
    int optval = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    // Set larger receive buffer for video data
    int rcvbuf = 512 * 1024; // 512KB
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    // Set non-blocking mode
    int flags = fcntl(m_socket, F_GETFL, 0);
    if (flags < 0 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
        blog(LOG_ERROR, "RTP: Failed to set non-blocking mode: %s", strerror(errno));
        close(m_socket);
        m_socket = -1;
        return false;
    }
    
    // Bind to port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(m_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        blog(LOG_ERROR, "RTP: Failed to bind to port %d: %s", port, strerror(errno));
        close(m_socket);
        m_socket = -1;
        return false;
    }
    
    m_running = true;
    blog(LOG_INFO, "RTP: Receiver started on port %d", port);
    return true;
}

void RTPReceiver::stop()
{
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
    m_running = false;
}

bool RTPReceiver::parseHeader(const uint8_t* data, size_t len, RTPHeader& header)
{
    if (len < sizeof(RTPHeader)) {
        return false;
    }
    
    memcpy(&header, data, sizeof(RTPHeader));
    
    // Convert from network byte order
    header.seq = ntohs(header.seq);
    header.timestamp = ntohl(header.timestamp);
    header.ssrc = ntohl(header.ssrc);
    
    // Extract version (should be 2)
    uint8_t version = (header.vpxcc >> 6) & 0x3;
    if (version != 2) {
        blog(LOG_WARNING, "RTP: Invalid version %d (expected 2)", version);
        return false;
    }
    
    return true;
}

void RTPReceiver::handleReordering(uint16_t seq)
{
    // Loss/reorder accounting kept silent on the hot path; per-packet logging
    // here used to dominate CPU at 1080p60 even when DEBUG was filtered out.
    (void)seq;
}

std::vector<uint8_t> RTPReceiver::processPacket(const uint8_t* data, size_t len)
{
    std::vector<uint8_t> payload;
    
    RTPHeader header;
    if (!parseHeader(data, len, header)) {
        return payload;
    }
    
    // Handle sequence number
    handleReordering(header.seq);
    m_last_seq = header.seq;
    m_last_timestamp = header.timestamp;
    
    // Calculate header size (including CSRC and extensions if present)
    size_t header_size = sizeof(RTPHeader);
    
    // CSRC count
    uint8_t csrc_count = header.vpxcc & 0x0F;
    header_size += csrc_count * 4;
    
    // Extension bit
    bool has_extension = (header.vpxcc & 0x10) != 0;
    if (has_extension && len >= header_size + 4) {
        // Extension header: 16-bit profile + 16-bit length
        uint16_t ext_len_net = 0;
        memcpy(&ext_len_net, data + header_size + 2, sizeof(ext_len_net));
        uint16_t ext_len = ntohs(ext_len_net);
        header_size += 4 + (ext_len * 4);
    }
    
    // Extract payload
    if (len > header_size) {
        size_t payload_size = len - header_size;
        payload.resize(payload_size);
        memcpy(payload.data(), data + header_size, payload_size);
    }
    
    return payload;
}
