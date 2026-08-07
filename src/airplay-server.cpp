#include "airplay-server.hpp"
#include "airplay-source.hpp"
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

std::atomic<bool> g_latency_telemetry_enabled{false};

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
    
    // Start listener threads
    blog(LOG_INFO, "Step 3: Starting listener threads...");
    m_running = true;
    m_airplay_listener_thread = std::thread(&AirPlayServer::airplayListenerLoop, this);
    m_raop_listener_thread = std::thread(&AirPlayServer::raopListenerLoop, this);
    
    blog(LOG_INFO, "========================================");
    blog(LOG_INFO, "✓ AirPlay server '%s' STARTED SUCCESSFULLY", m_server_name.c_str());
    blog(LOG_INFO, "✓ AirPlay listening on: 0.0.0.0:%d", m_airplay_port);
    blog(LOG_INFO, "✓ RAOP listening on: 0.0.0.0:%d", m_raop_port);
    blog(LOG_INFO, "✓ Ready to accept iPad connections!");
    blog(LOG_INFO, "========================================");
    
    return true;
}

void AirPlayServer::resetDecoders()
{
    std::lock_guard<std::mutex> lock(m_decoder_mutex);
    if (m_h264_decoder) m_h264_decoder->flush();
    if (m_h265_decoder) m_h265_decoder->flush();
    if (m_audio_decoder) m_audio_decoder->flush();
    m_video_frame_counter = 0; // re-enable first-frame timing log on reconnect
    m_first_decoded_frame_ns = 0;
    blog(LOG_INFO, "AirPlay decoders flushed for reconnect");
}

void AirPlayServer::stop()
{
    if (!m_running) {
        return;
    }
    
    m_running = false;
    
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
    
    // Release all weak source refs
    {
        std::lock_guard<std::mutex> lock(m_sources_mutex);
        for (obs_weak_source_t* weak : m_registered_sources) {
            obs_weak_source_release(weak);
        }
        m_registered_sources.clear();
    }

    blog(LOG_INFO, "AirPlay server stopped");
}

void AirPlayServer::airplayListenerLoop()
{
    blog(LOG_INFO, "AirPlay listener thread started - waiting for connections on port %d", m_airplay_port);
    blog(LOG_INFO, "Socket FD: %d, Bound to: 0.0.0.0:%d", m_airplay_socket, m_airplay_port);

    // Set accept timeout once, outside the loop
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(m_airplay_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (m_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
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
        
        char client_ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_buf, sizeof(client_ip_buf));
        std::string client_ip(client_ip_buf);
        uint16_t client_port = ntohs(client_addr.sin_port);
        blog(LOG_INFO, "*** NEW AIRPLAY CONNECTION *** from %s:%d (socket fd=%d)", 
             client_ip.c_str(), client_port, client_socket);
        
        // Insert into map before starting thread so closeConnection() can always find the entry
        auto conn = std::make_unique<AirPlayConnection>();
        conn->socket_fd = client_socket;
        conn->client_address = client_ip;
        conn->active = true;
        {
            std::lock_guard<std::mutex> lock(m_connections_mutex);
            m_connections[client_socket] = std::move(conn);
            m_connections[client_socket]->handler_thread =
                std::thread(&AirPlayServer::handleAirPlayConnection, this, client_socket, client_ip);
        }
    }
    
    blog(LOG_INFO, "AirPlay listener thread stopped");
}

void AirPlayServer::raopListenerLoop()
{
    blog(LOG_INFO, "RAOP listener thread started - waiting for connections on port %d", m_raop_port);
    blog(LOG_INFO, "Socket FD: %d, Bound to: 0.0.0.0:%d", m_raop_socket, m_raop_port);

    // Set accept timeout once, outside the loop
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(m_raop_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (m_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
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
        
        char client_ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_buf, sizeof(client_ip_buf));
        std::string client_ip(client_ip_buf);
        uint16_t client_port = ntohs(client_addr.sin_port);
        blog(LOG_INFO, "*** NEW RAOP CONNECTION *** from %s:%d (socket fd=%d)", 
             client_ip.c_str(), client_port, client_socket);
        
        // Insert into map before starting thread so closeConnection() can always find the entry
        auto conn = std::make_unique<AirPlayConnection>();
        conn->socket_fd = client_socket;
        conn->client_address = client_ip;
        conn->active = true;
        {
            std::lock_guard<std::mutex> lock(m_connections_mutex);
            m_connections[client_socket] = std::move(conn);
            m_connections[client_socket]->handler_thread =
                std::thread(&AirPlayServer::handleRAOPConnection, this, client_socket, client_ip);
        }
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
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char buf[100];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
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
    m_registered_sources.push_back(obs_source_get_weak_source(source));
    blog(LOG_INFO, "Source registered with AirPlay server");
}

void AirPlayServer::unregisterSource(obs_source_t* source)
{
    std::lock_guard<std::mutex> lock(m_sources_mutex);
    auto it = std::find_if(m_registered_sources.begin(), m_registered_sources.end(),
        [source](obs_weak_source_t* weak) {
            obs_source_t* s = obs_weak_source_get_source(weak);
            bool match = (s == source);
            if (s) obs_source_release(s);
            return match;
        });
    if (it != m_registered_sources.end()) {
        obs_weak_source_release(*it);
        m_registered_sources.erase(it);
        blog(LOG_INFO, "Source unregistered from AirPlay server");
    }
}

void AirPlayServer::ingestVideoBitstream(const uint8_t* data, size_t size, uint64_t pts, bool is_h265)
{
    UNUSED_PARAMETER(pts);

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

    // DecodedVideoFrame references buffers owned by the decoder. Keep this lock
    // until OBS has copied the frame so another packet or reconnect reset cannot
    // invalidate those buffers underneath obs_source_output_video().
    std::unique_lock<std::mutex> dec_lock(m_decoder_mutex);
    DecodedVideoFrame decoded;
    const bool tele = g_latency_telemetry_enabled.load(std::memory_order_relaxed);
    const bool first_frame = m_video_frame_counter == 0;
    const uint64_t t_decode_start = (tele || first_frame) ? os_gettime_ns() : 0;
    if (!decoder->decodeToI420(data, size, decoded)) {
        return;
    }
    const uint64_t t_decode_end = (tele || first_frame) ? os_gettime_ns() : 0;

    obs_source_frame frame = {};
    frame.data[0] = decoded.data[0];
    frame.data[1] = decoded.data[1];
    frame.data[2] = decoded.data[2];
    frame.linesize[0] = decoded.linesize[0];
    frame.linesize[1] = decoded.linesize[1];
    frame.linesize[2] = decoded.linesize[2];
    frame.width = static_cast<uint32_t>(decoded.width);
    frame.height = static_cast<uint32_t>(decoded.height);
    frame.format = VIDEO_FORMAT_I420;
    frame.full_range = false;
    frame.trc = VIDEO_TRC_DEFAULT;
    video_format_get_parameters_for_format(VIDEO_CS_709,
                                           VIDEO_RANGE_PARTIAL,
                                           frame.format,
                                           frame.color_matrix,
                                           frame.color_range_min,
                                           frame.color_range_max);
    frame.timestamp = os_gettime_ns();

    std::lock_guard<std::mutex> lock(m_sources_mutex);
    const uint64_t t_output_start = tele ? os_gettime_ns() : 0;

    // Log the very first frame output to OBS for connection timing diagnostics.
    if (first_frame) {
        m_first_decoded_frame_ns = t_decode_end;
        blog(LOG_INFO, "[CONNECT] first decoded frame queued to OBS (%dx%d, decode %.2fms)",
             decoded.width, decoded.height, (t_decode_end - t_decode_start) / 1e6);
    }

    const uint64_t frame_number = m_video_frame_counter + 1;
    if (frame_number == 1 || frame_number == 30 || frame_number == 60 ||
        frame_number == 120 || frame_number == 180 || frame_number == 240) {
        uint64_t luma_sum = 0;
        uint64_t luma_hash = UINT64_C(1469598103934665603);
        uint32_t samples = 0;
        uint8_t luma_min = 255;
        uint8_t luma_max = 0;
        for (int y = 0; y < decoded.height; y += 24) {
            const uint8_t* row = decoded.data[0] + y * decoded.linesize[0];
            for (int x = 0; x < decoded.width; x += 24) {
                const uint8_t value = row[x];
                luma_sum += value;
                luma_min = std::min(luma_min, value);
                luma_max = std::max(luma_max, value);
                luma_hash ^= value;
                luma_hash *= UINT64_C(1099511628211);
                ++samples;
            }
        }
        const uint64_t sampled_ns = os_gettime_ns();
        const double elapsed_ms = m_first_decoded_frame_ns && sampled_ns >= m_first_decoded_frame_ns
            ? (sampled_ns - m_first_decoded_frame_ns) / 1e6 : 0.0;
        blog(LOG_INFO,
             "[DISPLAY] decoded frame #%llu +%.1fms luma avg=%.1f range=%u-%u hash=%016llx",
             static_cast<unsigned long long>(frame_number), elapsed_ms,
             samples ? static_cast<double>(luma_sum) / samples : 0.0,
             static_cast<unsigned>(luma_min), static_cast<unsigned>(luma_max),
             static_cast<unsigned long long>(luma_hash));
    }

    for (obs_weak_source_t* weak : m_registered_sources) {
        obs_source_t* source = obs_weak_source_get_source(weak);
        if (source) {
            obs_source_output_video(source, &frame);
            obs_source_release(source);
        }
    }
    if (first_frame) {
        airplay_source_notify_frame_queued(os_gettime_ns());
    }
    const uint64_t t_output_end = tele ? os_gettime_ns() : 0;

    ++m_video_frame_counter;

    if (tele) {
        const uint64_t decode_ns = t_decode_end - t_decode_start;
        const uint64_t output_ns = t_output_end - t_output_start;
        m_v_decode_ns_sum += decode_ns;
        if (decode_ns > m_v_decode_ns_max) m_v_decode_ns_max = decode_ns;
        m_v_output_ns_sum += output_ns;
        if (output_ns > m_v_output_ns_max) m_v_output_ns_max = output_ns;
        if (m_v_last_output_ns != 0) {
            const uint64_t interval = t_output_end - m_v_last_output_ns;
            m_v_interval_ns_sum += interval;
            if (interval > m_v_interval_ns_max) m_v_interval_ns_max = interval;
        }
        m_v_last_output_ns = t_output_end;
        ++m_v_window_count;
        if (m_v_window_count >= 120) {
            const double n = static_cast<double>(m_v_window_count);
            const double intervals_n = n > 1.0 ? n - 1.0 : 1.0;
            blog(LOG_INFO,
                 "[latency] video N=%u  decode avg=%.2fms max=%.2fms  "
                 "output avg=%.2fms max=%.2fms  interval avg=%.2fms max=%.2fms (~%.1ffps)",
                 m_v_window_count,
                 (m_v_decode_ns_sum / n) / 1e6,
                 m_v_decode_ns_max / 1e6,
                 (m_v_output_ns_sum / n) / 1e6,
                 m_v_output_ns_max / 1e6,
                 (m_v_interval_ns_sum / intervals_n) / 1e6,
                 m_v_interval_ns_max / 1e6,
                 1e9 * intervals_n / static_cast<double>(m_v_interval_ns_sum ? m_v_interval_ns_sum : 1));
            m_v_decode_ns_sum = m_v_decode_ns_max = 0;
            m_v_output_ns_sum = m_v_output_ns_max = 0;
            m_v_interval_ns_sum = m_v_interval_ns_max = 0;
            m_v_window_count = 0;
        }
    } else if (m_v_window_count != 0) {
        m_v_decode_ns_sum = m_v_decode_ns_max = 0;
        m_v_output_ns_sum = m_v_output_ns_max = 0;
        m_v_interval_ns_sum = m_v_interval_ns_max = 0;
        m_v_window_count = 0;
        m_v_last_output_ns = 0;
    }

    if ((m_video_frame_counter % 120) == 0) {
        blog(LOG_INFO, "Output video frame #%llu (%s, %ux%u)",
             static_cast<unsigned long long>(m_video_frame_counter),
             is_h265 ? "HEVC" : "H264",
             frame.width,
             frame.height);
    }
}

void AirPlayServer::ingestAudioBitstream(const uint8_t* data, size_t size, uint8_t codec_type, uint64_t pts)
{
    UNUSED_PARAMETER(pts);

    if (!data || size == 0 || !m_audio_decoder) {
        return;
    }

    std::vector<float> left;
    std::vector<float> right;
    int sample_rate = 0;
    const bool tele = g_latency_telemetry_enabled.load(std::memory_order_relaxed);
    const uint64_t t_decode_start = tele ? os_gettime_ns() : 0;
    {
        std::lock_guard<std::mutex> dec_lock(m_decoder_mutex);
        if (!m_audio_decoder->decode(data, size, codec_type, left, right, sample_rate)) {
            return;
        }
    }
    const uint64_t t_decode_end = tele ? os_gettime_ns() : 0;

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
    audio.timestamp = os_gettime_ns();

    std::lock_guard<std::mutex> lock(m_sources_mutex);
    const uint64_t t_output_start = tele ? os_gettime_ns() : 0;
    for (obs_weak_source_t* weak : m_registered_sources) {
        obs_source_t* source = obs_weak_source_get_source(weak);
        if (source) {
            obs_source_output_audio(source, &audio);
            obs_source_release(source);
        }
    }
    const uint64_t t_output_end = tele ? os_gettime_ns() : 0;

    ++m_audio_frame_counter;

    if (tele) {
        const uint64_t decode_ns = t_decode_end - t_decode_start;
        const uint64_t output_ns = t_output_end - t_output_start;
        m_a_decode_ns_sum += decode_ns;
        if (decode_ns > m_a_decode_ns_max) m_a_decode_ns_max = decode_ns;
        m_a_output_ns_sum += output_ns;
        if (output_ns > m_a_output_ns_max) m_a_output_ns_max = output_ns;
        ++m_a_window_count;
        if (m_a_window_count >= 240) {
            const double n = static_cast<double>(m_a_window_count);
            blog(LOG_INFO,
                 "[latency] audio N=%u  decode avg=%.2fms max=%.2fms  "
                 "output avg=%.2fms max=%.2fms",
                 m_a_window_count,
                 (m_a_decode_ns_sum / n) / 1e6,
                 m_a_decode_ns_max / 1e6,
                 (m_a_output_ns_sum / n) / 1e6,
                 m_a_output_ns_max / 1e6);
            m_a_decode_ns_sum = m_a_decode_ns_max = 0;
            m_a_output_ns_sum = m_a_output_ns_max = 0;
            m_a_window_count = 0;
        }
    } else if (m_a_window_count != 0) {
        m_a_decode_ns_sum = m_a_decode_ns_max = 0;
        m_a_output_ns_sum = m_a_output_ns_max = 0;
        m_a_window_count = 0;
    }

    if ((m_audio_frame_counter % 240) == 0) {
        blog(LOG_INFO, "Output audio frame #%llu (codec=%u, frames=%u, rate=%u)",
             static_cast<unsigned long long>(m_audio_frame_counter),
             static_cast<unsigned int>(codec_type),
             audio.frames,
             audio.samples_per_sec);
    }
}
