#pragma once

#include <cstdint>

// RAOP (Remote Audio Output Protocol) server
// This will be used for audio-only AirPlay

class RAOPServer {
public:
    RAOPServer();
    ~RAOPServer();
    
    bool start(uint16_t port);
    void stop();
    
private:
    bool m_running;
};
