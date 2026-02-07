#pragma once

#include "mdns-publisher.hpp"
#include "uxplay-integration.hpp"
#include "h264-decoder.hpp"
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <map>
#include <cstddef>

// Forward declaration
struct obs_source;
typedef struct obs_source obs_source_t;

struct AirPlayConnection {
    int socket_fd;
    std::string client_address;
    std::thread handler_thread;
    std::atomic<bool> active;
};

class AirPlayServer {
public:
    AirPlayServer();
    ~AirPlayServer();
    
    // Start the AirPlay server
    bool start(const std::string& server_name = "OBS AirPlay", 
               uint16_t airplay_port = 7000, 
               uint16_t raop_port = 5000);
    
    // Start only mDNS advertising (for UxPlay integration)
    bool startMDNS(const std::string& server_name = "OBS AirPlay", 
                   uint16_t airplay_port = 8000,
                   uint16_t raop_port = 8000,
                   const std::string& pk = "");
    
    // Stop the server
    void stop();
    
    // Check if server is running
    bool isRunning() const { return m_running; }
    
    // Register a source to receive AirPlay data
    void registerSource(obs_source_t* source);
    void unregisterSource(obs_source_t* source);
    
    // Get server info
    std::string getServerName() const { return m_server_name; }
    uint16_t getAirPlayPort() const { return m_airplay_port; }
    uint16_t getRAOPPort() const { return m_raop_port; }
    std::string getMACAddress() const { return m_mac_address; }

    // Feed encoded video from UxPlay and output decoded frames to registered OBS sources.
    void ingestVideoBitstream(const uint8_t* data, size_t size, uint64_t pts, bool is_h265);
    
private:
    std::atomic<bool> m_running;
    std::string m_server_name;
    uint16_t m_airplay_port;
    uint16_t m_raop_port;
    
    // Server sockets
    int m_airplay_socket;
    int m_raop_socket;
    
    // mDNS publisher
    std::unique_ptr<MDNSPublisher> m_mdns_publisher;
    
    // Listener threads
    std::thread m_airplay_listener_thread;
    std::thread m_raop_listener_thread;
    
    // Active connections
    std::mutex m_connections_mutex;
    std::map<int, std::unique_ptr<AirPlayConnection>> m_connections;
    
    // Registered sources
    std::mutex m_sources_mutex;
    std::vector<obs_source_t*> m_registered_sources;
    
    // Server methods
    bool createServerSocket(int& socket_fd, uint16_t port);
    void airplayListenerLoop();
    void raopListenerLoop();
    void handleAirPlayConnection(int client_socket, const std::string& client_addr);
    void handleRAOPConnection(int client_socket, const std::string& client_addr);
    void closeConnection(int socket_fd);
    
    // HTTP request handling
    std::string handleHTTPRequest(const std::string& request);
    std::string handleServerInfo(const std::string& cseq = "0");
    std::string handleOK(const std::string& cseq = "0");
    std::string handleRTSPOK(const std::string& cseq = "0");
    std::string handleOptions(const std::string& cseq = "0");
    std::string handlePairSetup(const std::string& cseq = "0");
    std::string handlePairVerify(const std::string& cseq = "0");
    std::string handleFairPlaySetup(const std::string& cseq = "0");
    std::string handlePlay(const std::string& cseq = "0");
    std::string handleStop(const std::string& cseq = "0");
    std::string handleRate(const std::string& cseq = "0");
    std::string handlePlaybackInfo(const std::string& cseq = "0");
    std::string handleStreamSetup(const std::string& request, const std::string& cseq = "0");
    
    // Helper functions
    std::string getCurrentDate();

    // UxPlay callback handlers
    void handleVideoFrame(uint8_t** data, int* linesize, int width, int height, uint64_t pts);
    void handleAudioData(uint8_t* data, int samples, int channels, int sample_rate, uint64_t pts);

    // Generate MAC address
    std::string generateMACAddress();
    std::string m_mac_address;

    std::unique_ptr<H264Decoder> m_h264_decoder;
    std::unique_ptr<H264Decoder> m_h265_decoder;
    uint64_t m_video_frame_counter = 0;
};
