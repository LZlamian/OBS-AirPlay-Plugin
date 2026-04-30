#include "airplay-server.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <ctime>
#include <sstream>
#include <random>
#include <iomanip>
#include <vector>

AirPlayServer::AirPlayServer()
    : m_running(false)
    , m_airplay_port(7000)
    , m_raop_port(5000)
    , m_airplay_socket(-1)
    , m_raop_socket(-1)
{
    m_mac_address = generateMACAddress();
    m_h264_decoder = std::make_unique<H264Decoder>();
    m_h265_decoder = std::make_unique<H264Decoder>(AV_CODEC_ID_HEVC);
    m_audio_decoder = std::make_unique<AudioDecoder>();
    
    // Note: UxPlay integration is now handled in plugin-main.cpp
    // This server only handles basic AirPlay connections and mDNS
}

AirPlayServer::~AirPlayServer()
{
    stop();
}

std::string AirPlayServer::generateMACAddress()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    std::stringstream ss;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ss << ":";
        ss << std::hex << std::setw(2) << std::setfill('0') << std::uppercase << dis(gen);
    }
    
    return ss.str();
}

bool AirPlayServer::createServerSocket(int& socket_fd, uint16_t port)
{
    blog(LOG_INFO, "Creating server socket on port %d...", port);
    
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        blog(LOG_ERROR, "Failed to create socket: %s", strerror(errno));
        return false;
    }
    blog(LOG_INFO, "Socket created: fd=%d", socket_fd);
    
    // Set socket options
    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        blog(LOG_WARNING, "Failed to set SO_REUSEADDR: %s", strerror(errno));
    } else {
        blog(LOG_INFO, "SO_REUSEADDR set successfully");
    }
    
    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    blog(LOG_INFO, "Attempting to bind socket %d to 0.0.0.0:%d...", socket_fd, port);
    if (bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        blog(LOG_ERROR, "Failed to bind socket to port %d: %s (errno=%d)", 
             port, strerror(errno), errno);
        close(socket_fd);
        socket_fd = -1;
        return false;
    }
    blog(LOG_INFO, "Socket %d bound successfully to 0.0.0.0:%d", socket_fd, port);
    
    // Listen
    blog(LOG_INFO, "Calling listen() on socket %d...", socket_fd);
    if (listen(socket_fd, 5) < 0) {
        blog(LOG_ERROR, "Failed to listen on socket: %s (errno=%d)", strerror(errno), errno);
        close(socket_fd);
        socket_fd = -1;
        return false;
    }
    
    blog(LOG_INFO, "✓ Server socket %d is LISTENING on 0.0.0.0:%d", socket_fd, port);
    return true;
}

bool AirPlayServer::start(const std::string& server_name, uint16_t airplay_port, uint16_t raop_port)
{
    blog(LOG_INFO, "========================================");
    blog(LOG_INFO, "Starting AirPlay Server");
    blog(LOG_INFO, "  Name: %s", server_name.c_str());
    blog(LOG_INFO, "  AirPlay Port: %d", airplay_port);
    blog(LOG_INFO, "  RAOP Port: %d", raop_port);
    blog(LOG_INFO, "  MAC Address: %s", m_mac_address.c_str());
    blog(LOG_INFO, "========================================");
    
    if (m_running) {
        blog(LOG_WARNING, "AirPlay server already running");
        return false;
    }
    
    m_server_name = server_name;
    m_airplay_port = airplay_port;
    m_raop_port = raop_port;
    
    // Create server sockets
    blog(LOG_INFO, "Step 1: Creating AirPlay socket...");
    if (!createServerSocket(m_airplay_socket, m_airplay_port)) {
        blog(LOG_ERROR, "Failed to create AirPlay socket");
        return false;
    }
    
    blog(LOG_INFO, "Step 2: Creating RAOP socket...");
    if (!createServerSocket(m_raop_socket, m_raop_port)) {
        blog(LOG_ERROR, "Failed to create RAOP socket");
        close(m_airplay_socket);
        m_airplay_socket = -1;
        return false;
    }
    
    // Start mDNS advertising
    blog(LOG_INFO, "Step 3: Starting mDNS advertising...");
    m_mdns_publisher = std::make_unique<MDNSPublisher>();
    if (!m_mdns_publisher->start(m_server_name, m_airplay_port, m_raop_port, m_mac_address)) {
        blog(LOG_ERROR, "Failed to start mDNS advertising");
        close(m_airplay_socket);
        close(m_raop_socket);
        m_airplay_socket = -1;
        m_raop_socket = -1;
        return false;
    }
    
    // Start listener threads
    blog(LOG_INFO, "Step 4: Starting listener threads...");
    m_running = true;
    m_airplay_listener_thread = std::thread(&AirPlayServer::airplayListenerLoop, this);
    m_raop_listener_thread = std::thread(&AirPlayServer::raopListenerLoop, this);
    
    blog(LOG_INFO, "========================================");
    blog(LOG_INFO, "✓ AirPlay server '%s' STARTED SUCCESSFULLY", m_server_name.c_str());
    blog(LOG_INFO, "✓ AirPlay listening on: 0.0.0.0:%d", m_airplay_port);
    blog(LOG_INFO, "✓ RAOP listening on: 0.0.0.0:%d", m_raop_port);
    blog(LOG_INFO, "✓ mDNS advertising active");
    blog(LOG_INFO, "✓ Ready to accept iPad connections!");
    blog(LOG_INFO, "========================================");
    
    return true;
}

bool AirPlayServer::startMDNS(const std::string& server_name, uint16_t airplay_port, uint16_t raop_port, const std::string& pk)
{
    blog(LOG_INFO, "Starting mDNS advertising for UxPlay...");
    blog(LOG_INFO, "  Name: %s", server_name.c_str());
    blog(LOG_INFO, "  AirPlay Port: %d", airplay_port);
    blog(LOG_INFO, "  RAOP Port: %d", raop_port);
    blog(LOG_INFO, "  MAC Address: %s", m_mac_address.c_str());
    blog(LOG_INFO, "  Public Key: %s", pk.empty() ? "DEFAULT" : pk.c_str());
    
    m_server_name = server_name;
    m_airplay_port = airplay_port;
    m_raop_port = raop_port;
    m_running = true;  // Mark as running for mDNS purposes
    
    // Start mDNS advertising only
    m_mdns_publisher = std::make_unique<MDNSPublisher>();
    if (!m_mdns_publisher->start(m_server_name, m_airplay_port, m_raop_port, m_mac_address, pk)) {
        blog(LOG_ERROR, "Failed to start mDNS advertising");
        m_running = false;
        return false;
    }
    
    blog(LOG_INFO, "mDNS advertising started successfully for UxPlay");
    return true;
}

void AirPlayServer::stop()
{
    if (!m_running) {
        return;
    }
    
    m_running = false;
    
    // Stop mDNS advertising
    if (m_mdns_publisher) {
        m_mdns_publisher->stop();
        m_mdns_publisher.reset();
    }
    
    // Close server sockets
    if (m_airplay_socket >= 0) {
        close(m_airplay_socket);
        m_airplay_socket = -1;
    }
    
    if (m_raop_socket >= 0) {
        close(m_raop_socket);
        m_raop_socket = -1;
    }
    
    // Close all connections. Move threads out of the map before joining to
    // avoid deadlocking with closeConnection(), which also takes this mutex.
    std::vector<std::thread> connection_threads;
    {
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        connection_threads.reserve(m_connections.size());
        for (auto& [fd, conn] : m_connections) {
            conn->active = false;
            close(fd);
            if (conn->handler_thread.joinable()) {
                connection_threads.emplace_back(std::move(conn->handler_thread));
            }
        }
        m_connections.clear();
    }
    for (auto& t : connection_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    // Wait for listener threads
    if (m_airplay_listener_thread.joinable()) {
        m_airplay_listener_thread.join();
    }
    
    if (m_raop_listener_thread.joinable()) {
        m_raop_listener_thread.join();
    }
    
    blog(LOG_INFO, "AirPlay server stopped");
}

void AirPlayServer::airplayListenerLoop()
{
    blog(LOG_INFO, "AirPlay listener thread started - waiting for connections on port %d", m_airplay_port);
    blog(LOG_INFO, "Socket FD: %d, Bound to: 0.0.0.0:%d", m_airplay_socket, m_airplay_port);
    
    while (m_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // Set socket to non-blocking for accept
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(m_airplay_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        blog(LOG_DEBUG, "AirPlay: Calling accept() on socket %d...", m_airplay_socket);
        int client_socket = accept(m_airplay_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Timeout - this is normal, just loop again
                continue;
            }
            if (m_running) {
                blog(LOG_ERROR, "Accept failed: %s (errno=%d)", strerror(errno), errno);
            }
            continue;
        }
        
        std::string client_ip = inet_ntoa(client_addr.sin_addr);
        uint16_t client_port = ntohs(client_addr.sin_port);
        blog(LOG_INFO, "*** NEW AIRPLAY CONNECTION *** from %s:%d (socket fd=%d)", 
             client_ip.c_str(), client_port, client_socket);
        
        // Handle connection in a new thread
        auto conn = std::make_unique<AirPlayConnection>();
        conn->socket_fd = client_socket;
        conn->client_address = client_ip;
        conn->active = true;
        conn->handler_thread = std::thread(&AirPlayServer::handleAirPlayConnection, this, client_socket, client_ip);
        
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        m_connections[client_socket] = std::move(conn);
    }
    
    blog(LOG_INFO, "AirPlay listener thread stopped");
}

void AirPlayServer::raopListenerLoop()
{
    blog(LOG_INFO, "RAOP listener thread started - waiting for connections on port %d", m_raop_port);
    blog(LOG_INFO, "Socket FD: %d, Bound to: 0.0.0.0:%d", m_raop_socket, m_raop_port);
    
    while (m_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(m_raop_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        blog(LOG_DEBUG, "RAOP: Calling accept() on socket %d...", m_raop_socket);
        int client_socket = accept(m_raop_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                continue;
            }
            if (m_running) {
                blog(LOG_ERROR, "RAOP accept failed: %s (errno=%d)", strerror(errno), errno);
            }
            continue;
        }
        
        std::string client_ip = inet_ntoa(client_addr.sin_addr);
        uint16_t client_port = ntohs(client_addr.sin_port);
        blog(LOG_INFO, "*** NEW RAOP CONNECTION *** from %s:%d (socket fd=%d)", 
             client_ip.c_str(), client_port, client_socket);
        
        auto conn = std::make_unique<AirPlayConnection>();
        conn->socket_fd = client_socket;
        conn->client_address = client_ip;
        conn->active = true;
        conn->handler_thread = std::thread(&AirPlayServer::handleRAOPConnection, this, client_socket, client_ip);
        
        std::lock_guard<std::mutex> lock(m_connections_mutex);
        m_connections[client_socket] = std::move(conn);
    }
    
    blog(LOG_INFO, "RAOP listener thread stopped");
}

void AirPlayServer::handleAirPlayConnection(int client_socket, const std::string& client_addr)
{
    blog(LOG_INFO, "==> Handling AirPlay connection from %s (socket %d)", client_addr.c_str(), client_socket);
    
    char buffer[4096];
    while (m_running) {
        memset(buffer, 0, sizeof(buffer));
        blog(LOG_DEBUG, "AirPlay handler: Waiting for data from %s...", client_addr.c_str());
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_read <= 0) {
            if (bytes_read < 0) {
                blog(LOG_ERROR, "Receive error from %s: %s (errno=%d)", 
                     client_addr.c_str(), strerror(errno), errno);
            } else {
                blog(LOG_INFO, "Client %s closed connection (bytes_read=0)", client_addr.c_str());
            }
            break;
        }
        
        blog(LOG_INFO, "Received %zd bytes from %s", bytes_read, client_addr.c_str());
        std::string request(buffer, bytes_read);
        blog(LOG_DEBUG, "AirPlay request from %s (%zd bytes)", client_addr.c_str(), bytes_read);
        
        std::string response = handleHTTPRequest(request);
        
        if (!response.empty()) {
            blog(LOG_INFO, "Sending %zu byte response to %s", response.length(), client_addr.c_str());
            ssize_t sent = send(client_socket, response.c_str(), response.length(), 0);
            if (sent < 0) {
                blog(LOG_ERROR, "Failed to send response to %s: %s", 
                     client_addr.c_str(), strerror(errno));
                break;
            }
            blog(LOG_INFO, "Sent %zd bytes to %s", sent, client_addr.c_str());
        } else {
            blog(LOG_WARNING, "Empty response generated for request from %s", client_addr.c_str());
        }
    }
    
    closeConnection(client_socket);
    blog(LOG_INFO, "<== AirPlay connection from %s closed", client_addr.c_str());
}

void AirPlayServer::handleRAOPConnection(int client_socket, const std::string& client_addr)
{
    blog(LOG_INFO, "Handling RAOP connection from %s", client_addr.c_str());
    
    char buffer[4096];
    while (m_running) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_read <= 0) {
            break;
        }
        
        std::string request(buffer, bytes_read);
        
        // Parse request
        std::istringstream iss(request);
        std::string method, uri, version;
        iss >> method >> uri >> version;
        
        // Parse headers
        std::map<std::string, std::string> headers;
        std::string line;
        blog(LOG_INFO, "Starting header parsing for request...");
        
        // Skip the first line (method, uri, version)
        std::getline(iss, line);
        blog(LOG_INFO, "Request line: %s", line.c_str());
        
        // Parse headers until empty line
        while (std::getline(iss, line)) {
            // Remove \r if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            
            blog(LOG_INFO, "Header line: '%s' (length=%zu)", line.c_str(), line.length());
            
            // Empty line marks end of headers
            if (line.empty()) {
                break;
            }
            
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                headers[key] = value;
                blog(LOG_INFO, "Parsed header: '%s' = '%s'", key.c_str(), value.c_str());
            }
        }
        blog(LOG_INFO, "Header parsing complete. Found %zu headers.", headers.size());
        
        std::string cseq = headers.count("CSeq") ? headers["CSeq"] : "0";
        blog(LOG_INFO, "Extracted CSeq: '%s' from headers", cseq.c_str());
        std::string response;
        
        // On RAOP port, ALL requests use RTSP protocol (even POST/GET)
        // The protocol detection should be based on port, not method
        bool use_rtsp = true;  // RAOP port always uses RTSP
        
        if (use_rtsp) {
            // RTSP protocol for all RAOP requests
            blog(LOG_INFO, "RAOP RTSP: %s %s", method.c_str(), uri.c_str());
            
            if (method == "OPTIONS") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
            else if (method == "ANNOUNCE") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
            else if (method == "GET" && uri == "/info") {
                response = handleServerInfo(cseq);
                blog(LOG_INFO, "Sent server info response");
            }
            else if (method == "POST" && uri == "/pair-setup") {
                blog(LOG_DEBUG, "RAOP RTSP: pair-setup request (%zu bytes)", request.size());
                response = handlePairSetup(cseq);
                blog(LOG_INFO, "Sent pair-setup response");
            }
            else if (method == "POST" && uri == "/pair-verify") {
                response = handlePairVerify(cseq);
                blog(LOG_INFO, "Sent pair-verify response");
            }
            else if (method == "POST" && uri == "/fp-setup") {
                response = handleFairPlaySetup(cseq);
                blog(LOG_INFO, "Sent fp-setup response");
            }
            else if (method == "POST" && uri.find("/command") != std::string::npos) {
                response = handleRTSPOK(cseq);
                blog(LOG_INFO, "Sent command response");
            }
            else if (method == "POST" && uri == "/stream") {
                // This is the key request - iOS wants to start streaming!
                blog(LOG_INFO, "RAOP RTSP: Stream setup request received!");
                response = handleStreamSetup(request, cseq);
            }
            else {
                // Default OK for any other RTSP request
                blog(LOG_INFO, "RAOP RTSP: Unhandled %s %s - responding OK", method.c_str(), uri.c_str());
                response = handleRTSPOK(cseq);
            }
        }
        else {
            // RTSP protocol (older AirPlay)
            blog(LOG_INFO, "RAOP RTSP: %s %s", method.c_str(), uri.c_str());
            
            if (method == "OPTIONS") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
            else if (method == "ANNOUNCE") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
            else if (method == "SETUP") {
                std::string session = "DEADBEEF";
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Session: " + session + "\r\n";
                response += "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=6000;control_port=6001;timing_port=6002\r\n";
                response += "Audio-Jack-Status: connected\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
            else if (method == "RECORD") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Audio-Latency: 0\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
            else if (method == "SET_PARAMETER") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
            else if (method == "GET_PARAMETER") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "Content-Type: text/parameters\r\n";
                response += "Content-Length: 0\r\n";
                response += "\r\n";
            }
            else if (method == "FLUSH") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
            else if (method == "TEARDOWN") {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
                send(client_socket, response.c_str(), response.length(), 0);
                break;
            }
            else {
                response = "RTSP/1.0 200 OK\r\n";
                response += "CSeq: " + cseq + "\r\n";
                response += "Server: AirTunes/377.28.01\r\n";
                response += "\r\n";
            }
        }
        
        if (!response.empty()) {
            ssize_t sent = send(client_socket, response.c_str(), response.length(), 0);
            if (sent < 0) {
                blog(LOG_ERROR, "Failed to send response: %s", strerror(errno));
                break;
            }
            blog(LOG_DEBUG, "Sent %zd bytes response", sent);
        }
    }
    
    closeConnection(client_socket);
    blog(LOG_INFO, "RAOP connection from %s closed", client_addr.c_str());
}

std::string AirPlayServer::handleHTTPRequest(const std::string& request)
{
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;
    
    blog(LOG_DEBUG, "HTTP Request: %s %s %s", method.c_str(), path.c_str(), version.c_str());
    
    // Parse headers
    std::map<std::string, std::string> headers;
    std::string line;
    while (std::getline(iss, line) && line != "\r" && !line.empty()) {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            headers[key] = value;
        }
    }
    
    // Extract CSeq for response
    std::string cseq = "0";
    if (headers.find("CSeq") != headers.end()) {
        cseq = headers["CSeq"];
    }
    
    if (path == "/server-info") {
        return handleServerInfo(cseq);
    } else if (path == "/info") {
        return handleServerInfo(cseq);
    } else if (path == "/pair-setup") {
        return handlePairSetup(cseq);
    } else if (path == "/pair-verify") {
        return handlePairVerify(cseq);
    } else if (path == "/fp-setup") {
        return handleFairPlaySetup(cseq);
    } else if (method == "POST" && path == "/play") {
        return handlePlay(cseq);
    } else if (method == "POST" && path == "/stop") {
        return handleStop(cseq);
    } else if (method == "POST" && path == "/rate") {
        return handleRate(cseq);
    } else if (method == "GET" && path == "/playback-info") {
        return handlePlaybackInfo(cseq);
    } else if (method == "POST" && path.find("/feedback") != std::string::npos) {
        return handleOK(cseq);
    } else if (method == "OPTIONS") {
        return handleOptions(cseq);
    }
    
    // Log unhandled request for debugging
    blog(LOG_WARNING, "Unhandled AirPlay request: %s %s", method.c_str(), path.c_str());
    
    // Default OK response for unhandled requests
    return handleOK(cseq);
}

std::string AirPlayServer::handleOK(const std::string& cseq)
{
    std::stringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Server: AirTunes/377.28.01\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";
    return response.str();
}

std::string AirPlayServer::handleRTSPOK(const std::string& cseq)
{
    std::stringstream response;
    response << "RTSP/1.0 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Server: AirTunes/377.28.01\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";
    return response.str();
}

std::string AirPlayServer::handleOptions(const std::string& cseq)
{
    std::stringstream response;
    response << "RTSP/1.0 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET\r\n";
    response << "Server: AirTunes/377.28.01\r\n";
    response << "\r\n";
    return response.str();
}

std::string AirPlayServer::getCurrentDate()
{
    time_t now = time(0);
    struct tm tm = *gmtime(&now);
    char buf[100];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return std::string(buf);
}

std::string AirPlayServer::handlePairSetup(const std::string& cseq)
{
    // iOS is asking for pairing but we advertised as open
    // Respond that pairing succeeded immediately (no challenge needed)
    blog(LOG_INFO, "Pair-setup requested - responding with no-auth success");
    
    // For no-auth pairing, we need to respond with a specific plist structure
    std::stringstream plist;
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    plist << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    plist << "<plist version=\"1.0\">\n";
    plist << "<dict>\n";
    plist << "  <key>pk</key>\n";
    plist << "  <data></data>\n";  // Empty public key for no-auth
    plist << "  <key>pu</key>\n";
    plist << "  <data></data>\n";  // Empty public key for no-auth
    plist << "  <key>sp</key>\n";
    plist << "  <data></data>\n";  // Empty salt for no-auth
    plist << "</dict>\n";
    plist << "</plist>\n";
    
    std::string body = plist.str();
    
    // Use RTSP protocol for RAOP port (even for POST requests)
    std::stringstream response;
    response << "RTSP/1.0 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";  // Echo the CSeq from request
    response << "Server: AirTunes/366.0\r\n";
    response << "Content-Type: application/x-apple-binary-plist\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "\r\n";
    response << body;
    
    blog(LOG_INFO, "Pair-setup response:\n%s", response.str().c_str());
    
    return response.str();
}

std::string AirPlayServer::handlePairVerify(const std::string& cseq)
{
    // Return success without verification since we disabled auth
    blog(LOG_INFO, "Pair-verify requested (skipped - no auth required)");
    
    // Use RTSP protocol for RAOP port (even for POST requests)
    std::stringstream response;
    response << "RTSP/1.0 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";  // Echo the CSeq from request
    response << "Server: AirTunes/366.0\r\n";
    response << "Content-Type: application/octet-stream\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";
    
    return response.str();
}

std::string AirPlayServer::handleFairPlaySetup(const std::string& cseq)
{
    // Return success without FairPlay since we're not doing DRM
    std::stringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";
    blog(LOG_INFO, "FairPlay setup requested (skipped)");
    return response.str();
}

std::string AirPlayServer::handleStreamSetup(const std::string& request, const std::string& cseq)
{
    blog(LOG_INFO, "Stream setup request - iOS wants to start streaming!");
    
    // Parse the request to get the plist body
    size_t body_start = request.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        std::string body = request.substr(body_start + 4);
        blog(LOG_DEBUG, "Stream config body: %s", body.c_str());
        
        // TODO: Parse plist for stream configuration
        // For now, we'll just respond with our ports
    }
    
    // Respond with RTP port configuration
    std::stringstream plist;
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    plist << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    plist << "<plist version=\"1.0\">\n";
    plist << "<dict>\n";
    plist << "  <key>streams</key>\n";
    plist << "  <array>\n";
    plist << "    <dict>\n";
    plist << "      <key>type</key>\n";
    plist << "      <integer>110</integer>\n";  // Video stream type
    plist << "      <key>dataPort</key>\n";
    plist << "      <integer>6000</integer>\n";  // RTP video data port
    plist << "      <key>controlPort</key>\n";
    plist << "      <integer>6001</integer>\n";  // RTP video control port
    plist << "    </dict>\n";
    plist << "    <dict>\n";
    plist << "      <key>type</key>\n";
    plist << "      <integer>96</integer>\n";   // Audio stream type
    plist << "      <key>dataPort</key>\n";
    plist << "      <integer>7000</integer>\n";  // RTP audio data port
    plist << "      <key>controlPort</key>\n";
    plist << "      <integer>7001</integer>\n";  // RTP audio control port
    plist << "    </dict>\n";
    plist << "  </array>\n";
    plist << "  <key>eventPort</key>\n";
    plist << "  <integer>0</integer>\n";
    plist << "</dict>\n";
    plist << "</plist>\n";
    
    std::string body = plist.str();
    
    std::stringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Date: " << getCurrentDate() << "\r\n";
    response << "Content-Type: application/x-apple-binary-plist\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Connection: keep-alive\r\n";
    response << "Server: AirTunes/377.28.01\r\n";
    if (!cseq.empty() && cseq != "0") {
        response << "CSeq: " << cseq << "\r\n";
    }
    response << "\r\n";
    response << body;
    
    blog(LOG_INFO, "Sent stream setup response with RTP ports: video=6000, audio=7000");
    
    return response.str();
}

std::string AirPlayServer::handlePlaybackInfo(const std::string& cseq)
{
    std::stringstream plist;
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    plist << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    plist << "<plist version=\"1.0\">\n";
    plist << "<dict>\n";
    plist << "  <key>readyToPlay</key>\n";
    plist << "  <true/>\n";
    plist << "  <key>playbackBufferEmpty</key>\n";
    plist << "  <true/>\n";
    plist << "  <key>rate</key>\n";
    plist << "  <real>1.0</real>\n";
    plist << "  <key>loadedTimeRanges</key>\n";
    plist << "  <array/>\n";
    plist << "  <key>seekableTimeRanges</key>\n";
    plist << "  <array/>\n";
    plist << "</dict>\n";
    plist << "</plist>\n";
    
    std::string body = plist.str();
    std::stringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Content-Type: text/x-apple-plist+xml\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "\r\n";
    response << body;
    
    return response.str();
}

std::string AirPlayServer::handleServerInfo(const std::string& cseq)
{
    std::stringstream plist;
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    plist << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    plist << "<plist version=\"1.0\">\n";
    plist << "<dict>\n";
    plist << "  <key>deviceid</key>\n";
    plist << "  <string>" << m_mac_address << "</string>\n";
    plist << "  <key>features</key>\n";
    plist << "  <integer>1543503879</integer>\n";
    plist << "  <key>model</key>\n";
    plist << "  <string>AppleTV6,2</string>\n";
    plist << "  <key>protovers</key>\n";
    plist << "  <string>1.1</string>\n";
    plist << "  <key>srcvers</key>\n";
    plist << "  <string>377.28.01</string>\n";
    plist << "  <key>pi</key>\n";
    plist << "  <string>00000000-0000-0000-0000-000000000000</string>\n";
    plist << "  <key>vv</key>\n";
    plist << "  <integer>2</integer>\n";
    plist << "  <key>statusFlags</key>\n";
    plist << "  <integer>68</integer>\n";
    plist << "</dict>\n";
    plist << "</plist>\n";
    
    std::string body = plist.str();
    std::stringstream response;
    response << "RTSP/1.0 200 OK\r\n";  // Use RTSP for RAOP port
    response << "CSeq: " << cseq << "\r\n";
    response << "Content-Type: text/x-apple-plist+xml\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Server: AirTunes/377.28.01\r\n";
    response << "\r\n";
    response << body;
    
    return response.str();
}

std::string AirPlayServer::handlePlay(const std::string& cseq)
{
    blog(LOG_INFO, "Play request received");
    std::stringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Server: AirTunes/377.28.01\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";
    return response.str();
}

std::string AirPlayServer::handleStop(const std::string& cseq)
{
    blog(LOG_INFO, "Stop request received");
    std::stringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Server: AirTunes/377.28.01\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";
    return response.str();
}

std::string AirPlayServer::handleRate(const std::string& cseq)
{
    std::stringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Server: AirTunes/377.28.01\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";
    return response.str();
}

void AirPlayServer::closeConnection(int socket_fd)
{
    std::lock_guard<std::mutex> lock(m_connections_mutex);
    auto it = m_connections.find(socket_fd);
    if (it != m_connections.end()) {
        it->second->active = false;
        close(socket_fd);
        if (it->second->handler_thread.joinable()) {
            it->second->handler_thread.detach();
        }
        m_connections.erase(it);
    }
}

void AirPlayServer::registerSource(obs_source_t* source)
{
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    m_registered_sources.push_back(source);
    blog(LOG_INFO, "Source registered with AirPlay server");
}

void AirPlayServer::unregisterSource(obs_source_t* source)
{
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    auto it = std::find(m_registered_sources.begin(), m_registered_sources.end(), source);
    if (it != m_registered_sources.end()) {
        m_registered_sources.erase(it);
        blog(LOG_INFO, "Source unregistered from AirPlay server");
    }
}

void AirPlayServer::handleVideoFrame(uint8_t** data, int* linesize, int width, int height, uint64_t pts)
{
    // Handle video frame from UxPlay and send to OBS sources
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    
    blog(LOG_DEBUG, "Received video frame: %dx%d, pts=%llu", width, height, pts);
    
    // For now, just notify sources that we have video
    // In a real implementation, we'd convert the frame to OBS format
    for (obs_source_t* source : m_registered_sources) {
        obs_source_output_video(source, nullptr); // Placeholder
    }
}

void AirPlayServer::handleAudioData(uint8_t* data, int samples, int channels, int sample_rate, uint64_t pts)
{
    // Handle audio data from UxPlay and send to OBS sources
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    
    blog(LOG_DEBUG, "Received audio data: %d samples, %d channels, %d Hz, pts=%llu", 
         samples, channels, sample_rate, pts);
    
    // For now, just notify sources that we have audio
    // In a real implementation, we'd convert the audio to OBS format
    for (obs_source_t* source : m_registered_sources) {
        obs_source_output_audio(source, nullptr); // Placeholder
    }
}

void AirPlayServer::ingestVideoBitstream(const uint8_t* data, size_t size, uint64_t pts, bool is_h265)
{
    if (!data || size == 0) {
        return;
    }

    H264Decoder* decoder = is_h265 ? m_h265_decoder.get() : m_h264_decoder.get();
    if (!decoder) {
        static bool logged_decoder_missing = false;
        if (!logged_decoder_missing) {
            blog(LOG_WARNING, "No %s decoder instance available",
                 is_h265 ? "H265/HEVC" : "H264");
            logged_decoder_missing = true;
        }
        return;
    }

    // UxPlay's `ntp_time_local` is a monotonic local timestamp expressed in
    // nanoseconds (see uxplay/lib/raop_ntp.c::raop_ntp_get_local_time and
    // SECOND_IN_NSECS). OBS expects `obs_source_frame::timestamp` in
    // nanoseconds as well, so forward it directly. Fall back to wall-clock
    // only if the source did not provide one.
    const uint64_t out_ts = pts ? pts : os_gettime_ns();
    const bool is_h265_codec = is_h265;

    decoder->decode(data, size, [this, out_ts, is_h265_codec](const DecodedVideoFrame& v) {
        obs_source_frame frame = {};
        frame.data[0] = const_cast<uint8_t*>(v.data[0]);
        frame.data[1] = const_cast<uint8_t*>(v.data[1]);
        frame.data[2] = const_cast<uint8_t*>(v.data[2]);
        frame.linesize[0] = v.linesize[0];
        frame.linesize[1] = v.linesize[1];
        frame.linesize[2] = v.linesize[2];
        frame.width = static_cast<uint32_t>(v.width);
        frame.height = static_cast<uint32_t>(v.height);
        // v.format: 1 = NV12 (decoder native, no conversion), 2 = I420.
        frame.format = (v.format == 1) ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_I420;
        frame.full_range = false;
        frame.trc = VIDEO_TRC_DEFAULT;
        video_format_get_parameters_for_format(VIDEO_CS_709,
                                               VIDEO_RANGE_PARTIAL,
                                               frame.format,
                                               frame.color_matrix,
                                               frame.color_range_min,
                                               frame.color_range_max);
        frame.timestamp = out_ts;

        std::lock_guard<std::mutex> lock(m_sources_mutex);
        for (obs_source_t* source : m_registered_sources) {
            obs_source_output_video(source, &frame);
        }

        ++m_video_frame_counter;
        if ((m_video_frame_counter % 120) == 0) {
            blog(LOG_INFO, "Output video frame #%llu (%s, %ux%u, %s)",
                 static_cast<unsigned long long>(m_video_frame_counter),
                 is_h265_codec ? "HEVC" : "H264",
                 frame.width,
                 frame.height,
                 frame.format == VIDEO_FORMAT_NV12 ? "NV12" : "I420");
        }
    });
}

void AirPlayServer::ingestAudioBitstream(const uint8_t* data, size_t size, uint8_t codec_type, uint64_t pts)
{
    if (!data || size == 0 || !m_audio_decoder) {
        return;
    }

    std::vector<float> left;
    std::vector<float> right;
    int sample_rate = 0;
    if (!m_audio_decoder->decode(data, size, codec_type, left, right, sample_rate)) {
        return;
    }

    if (left.empty() || right.empty() || left.size() != right.size()) {
        return;
    }

    obs_source_audio audio = {};
    audio.data[0] = reinterpret_cast<uint8_t*>(left.data());
    audio.data[1] = reinterpret_cast<uint8_t*>(right.data());
    audio.frames = static_cast<uint32_t>(left.size());
    audio.speakers = SPEAKERS_STEREO;
    audio.samples_per_sec = static_cast<uint32_t>(sample_rate);
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    // Forward the source-provided NTP-local timestamp (nanoseconds) so OBS
    // can keep audio aligned with video without falling back to its default
    // ~200 ms async cache buffer.
    audio.timestamp = pts ? pts : os_gettime_ns();

    std::lock_guard<std::mutex> lock(m_sources_mutex);
    for (obs_source_t* source : m_registered_sources) {
        obs_source_output_audio(source, &audio);
    }

    ++m_audio_frame_counter;
    if ((m_audio_frame_counter % 240) == 0) {
        blog(LOG_INFO, "Output audio frame #%llu (codec=%u, frames=%u, rate=%u)",
             static_cast<unsigned long long>(m_audio_frame_counter),
             static_cast<unsigned int>(codec_type),
             audio.frames,
             audio.samples_per_sec);
    }
}
