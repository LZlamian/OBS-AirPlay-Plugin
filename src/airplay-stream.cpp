#include "airplay-stream.hpp"
#include "crypto-utils.hpp"
#include <obs-module.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

// Helper to read big-endian uint32
static inline uint32_t read_be32(const uint8_t* data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

AirPlayStream::AirPlayStream(obs_source_t* source)
    : m_source(source)
    , m_streaming(false)
    , m_pending_sps_pps(false)
{
    // Create decryptors
    m_video_decryptor = std::make_unique<AESDecryptor>();
    m_audio_decryptor = std::make_unique<AESDecryptor>();
    
    blog(LOG_INFO, "AirPlayStream created with decryption support");
}

AirPlayStream::~AirPlayStream()
{
    stop();
}

bool AirPlayStream::parseConfig(const std::string& plist_xml)
{
    // Parse the SETUP plist to get stream configuration
    plist_t root = nullptr;
    plist_from_xml(plist_xml.c_str(), plist_xml.length(), &root);
    
    if (!root) {
        blog(LOG_ERROR, "Failed to parse stream configuration plist");
        return false;
    }
    
    // Extract stream connection ID (needed for key derivation)
    plist_t stream_id_node = plist_dict_get_item(root, "streamConnectionID");
    if (stream_id_node && plist_get_node_type(stream_id_node) == PLIST_UINT) {
        plist_get_uint_val(stream_id_node, &m_config.stream_connection_id);
        blog(LOG_INFO, "Stream connection ID: %llu", m_config.stream_connection_id);
    } else {
        // Default if not present
        m_config.stream_connection_id = 1;
        blog(LOG_WARNING, "No stream connection ID in config, using default");
    }
    
    // Extract video configuration
    plist_t streams_node = plist_dict_get_item(root, "streams");
    if (streams_node) {
        // Parse stream details if available
        // For now, use defaults
    }
    
    // Set default ports based on AirPlay spec
    m_config.video_data_port = 6000;
    m_config.video_control_port = 6001;
    m_config.audio_data_port = 7000;
    m_config.audio_control_port = 7001;
    
    // Default video config
    m_config.video_width = 1920;
    m_config.video_height = 1080;
    m_config.video_fps = 60;
    m_config.video_codec = "H264";
    
    // Default audio config
    m_config.audio_sample_rate = 44100;
    m_config.audio_channels = 2;
    m_config.audio_codec = "AAC";
    
    // AirPlay always encrypts streams
    m_config.encrypted = true;
    
    plist_free(root);
    
    blog(LOG_INFO, "Stream config: %dx%d @ %d fps (encrypted: %s)", 
         m_config.video_width, m_config.video_height, m_config.video_fps,
         m_config.encrypted ? "yes" : "no");
    
    return true;
}

bool AirPlayStream::start()
{
    if (m_streaming) {
        return false;
    }
    
    blog(LOG_INFO, "Starting AirPlay stream...");

    if (!m_video_decryptor) {
        m_video_decryptor = std::make_unique<AESDecryptor>();
    }
    if (!m_audio_decryptor) {
        m_audio_decryptor = std::make_unique<AESDecryptor>();
    }
    
    // Initialize video decryption if stream is encrypted
    // Note: aes_key should be set from RAOP handshake before calling start()
    if (m_config.encrypted && !m_config.aes_key.empty()) {
        if (!m_video_decryptor->init(m_config.aes_key.data(), 
                                      m_config.aes_key.size(),
                                      m_config.stream_connection_id)) {
            blog(LOG_ERROR, "Failed to initialize video decryption");
            return false;
        }
        blog(LOG_INFO, "✓ Video decryption initialized (stream ID: %llu)", 
             m_config.stream_connection_id);
    } else {
        blog(LOG_WARNING, "No encryption key available, streams may be unencrypted");
    }
    
    // Create RTP receivers
    m_video_receiver = std::make_unique<RTPReceiver>();
    m_audio_receiver = std::make_unique<RTPReceiver>();
    
    // Create decoders
    m_video_decoder = std::make_unique<H264Decoder>();
    m_audio_decoder = std::make_unique<AudioDecoder>();
    
    m_streaming = true;
    
    // Start receiver threads
    m_video_thread = std::thread(&AirPlayStream::videoReceiverThread, this);
    m_audio_thread = std::thread(&AirPlayStream::audioReceiverThread, this);
    
    blog(LOG_INFO, "✓ AirPlay stream started on ports %d (video), %d (audio)",
         m_config.video_data_port, m_config.audio_data_port);
    
    return true;
}

void AirPlayStream::stop()
{
    if (!m_streaming) {
        return;
    }
    
    blog(LOG_INFO, "Stopping AirPlay stream...");
    
    m_streaming = false;
    
    // Wait for threads to finish
    if (m_video_thread.joinable()) {
        m_video_thread.join();
    }
    if (m_audio_thread.joinable()) {
        m_audio_thread.join();
    }
    
    // Clean up
    m_video_receiver.reset();
    m_audio_receiver.reset();
    m_video_decoder.reset();
    m_audio_decoder.reset();
    
    // Clear buffers
    m_sps_pps_buffer.clear();
    m_pending_sps_pps = false;
    
    blog(LOG_INFO, "AirPlay stream stopped");
}

void AirPlayStream::videoReceiverThread()
{
    blog(LOG_INFO, "Video receiver thread started");
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        blog(LOG_ERROR, "Failed to create video socket");
        return;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_config.video_data_port);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        blog(LOG_ERROR, "Failed to bind video socket to port %d: %s", 
             m_config.video_data_port, strerror(errno));
        close(sockfd);
        return;
    }
    
    blog(LOG_INFO, "Video socket bound to port %d", m_config.video_data_port);
    
    // Set non-blocking with timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    uint8_t buffer[65536]; // Large buffer for video packets

    while (m_streaming) {
        ssize_t len = recvfrom(sockfd, buffer, sizeof(buffer), 0, nullptr, nullptr);

        if (len > 0) {
            std::vector<uint8_t> payload = m_video_receiver->processPacket(buffer, len);
            if (!payload.empty()) {
                processVideoPacket(payload);
            }
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            blog(LOG_ERROR, "Video receive error: %s", strerror(errno));
        }
    }

    close(sockfd);
    blog(LOG_INFO, "Video receiver thread stopped");
}

void AirPlayStream::audioReceiverThread()
{
    blog(LOG_INFO, "Audio receiver thread started");
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        blog(LOG_ERROR, "Failed to create audio socket");
        return;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_config.audio_data_port);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        blog(LOG_ERROR, "Failed to bind audio socket to port %d: %s",
             m_config.audio_data_port, strerror(errno));
        close(sockfd);
        return;
    }
    
    blog(LOG_INFO, "Audio socket bound to port %d", m_config.audio_data_port);
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    uint8_t buffer[2048];
    
    while (m_streaming) {
        ssize_t len = recvfrom(sockfd, buffer, sizeof(buffer), 0, nullptr, nullptr);
        
        if (len > 0) {
            std::vector<uint8_t> payload = m_audio_receiver->processPacket(buffer, len);
            if (!payload.empty()) {
                processAudioPacket(payload);
            }
        }
    }
    
    close(sockfd);
    blog(LOG_INFO, "Audio receiver thread stopped");
}

void AirPlayStream::processVideoPacket(const std::vector<uint8_t>& rtp_data)
{
    // RTP header is typically 12 bytes, but check we have enough data
    if (rtp_data.size() < 14) {
        return;
    }

    const uint8_t* payload = rtp_data.data() + 12;
    size_t payload_size = rtp_data.size() - 12;

    // Check packet type markers (first 2 bytes after RTP header)
    uint16_t marker = (payload[0] << 8) | payload[1];

    if (marker == 0x0100) {
        // Unencrypted SPS/PPS packet
        blog(LOG_INFO, "Received SPS/PPS packet (%zu bytes)", payload_size);
        handleUnencryptedSPSPPS((uint8_t*)payload, payload_size);
        return;
    }

    if (marker == 0x0000 || marker == 0x0010) {
        // Encrypted video data
        // 0x0000 = Non-IDR frame (P-frame)
        // 0x0010 = IDR keyframe (I-frame)
        // Skip 4-byte header (marker + 2 reserved bytes)
        if (payload_size < 4) {
            return;
        }

        uint8_t* video_data = (uint8_t*)payload + 4;
        size_t video_size = payload_size - 4;

        // Decrypt in-place
        if (m_config.encrypted && m_video_decryptor) {
            if (!decryptVideoData(video_data, video_size)) {
                blog(LOG_ERROR, "Video decryption failed");
                return;
            }
        }

        // Allocate output buffer (may need space for prepended SPS/PPS)
        size_t output_size = video_size;
        if (m_pending_sps_pps) {
            output_size += m_sps_pps_buffer.size();
        }

        std::vector<uint8_t> h264_data(output_size);

        // Prepend SPS/PPS if pending
        size_t offset = 0;
        if (m_pending_sps_pps) {
            memcpy(h264_data.data(), m_sps_pps_buffer.data(),
                   m_sps_pps_buffer.size());
            offset = m_sps_pps_buffer.size();
            m_pending_sps_pps = false;
            m_sps_pps_buffer.clear();
        }

        // Copy decrypted video data
        memcpy(h264_data.data() + offset, video_data, video_size);

        // Process NAL units (replace sizes with start codes)
        processNALUnits(h264_data.data() + offset, video_size);

        // Send to H.264 decoder
        if (m_video_decoder) {
            m_video_decoder->decode(h264_data.data(), h264_data.size(), {});
        }
    } else {
        static uint64_t logged_unknown = 0;
        if ((logged_unknown++ % 240) == 0) {
            blog(LOG_WARNING, "Unknown video packet marker: 0x%04X", marker);
        }
    }
}

void AirPlayStream::handleUnencryptedSPSPPS(uint8_t* data, size_t length)
{
    blog(LOG_INFO, "Processing unencrypted SPS/PPS packet (%zu bytes)", length);
    
    // AirPlay SPS/PPS packets have a 128-byte header
    if (length < 128) {
        blog(LOG_ERROR, "SPS/PPS packet too small: %zu bytes", length);
        return;
    }
    
    // Skip 128-byte header
    data += 128;
    length -= 128;
    
    if (length == 0) {
        blog(LOG_ERROR, "No SPS/PPS data after header");
        return;
    }
    
    // Allocate buffer for SPS+PPS
    m_sps_pps_buffer.resize(length);
    memcpy(m_sps_pps_buffer.data(), data, length);
    
    // Process NAL units in buffer (replace sizes with start codes)
    processNALUnits(m_sps_pps_buffer.data(), m_sps_pps_buffer.size());
    
    // Mark as pending
    m_pending_sps_pps = true;
    
    blog(LOG_INFO, "✓ Buffered SPS/PPS (%zu bytes), will prepend to next frame", 
         m_sps_pps_buffer.size());
}

void AirPlayStream::processNALUnits(uint8_t* data, size_t length)
{
    size_t offset = 0;

    while (offset + 4 < length) {
        // Read 4-byte big-endian NAL size
        uint32_t nal_size = read_be32(data + offset);

        if (offset + 4 + nal_size > length) {
            blog(LOG_ERROR, "Invalid NAL size %u at offset %zu (total length %zu)",
                 nal_size, offset, length);
            break;
        }

        // Replace size with H.264 start code (0x00 0x00 0x00 0x01)
        NALProcessor::replaceNALSizeWithStartCode(data, offset);

        // Move to next NAL
        offset += 4 + nal_size;
    }
}

bool AirPlayStream::decryptVideoData(uint8_t* data, size_t length)
{
    if (!m_video_decryptor) {
        blog(LOG_ERROR, "Video decryptor not initialized");
        return false;
    }
    
    m_video_decryptor->decrypt(data, length);
    return true;
}

bool AirPlayStream::decryptAudioData(uint8_t* data, size_t length)
{
    if (!m_audio_decryptor) {
        blog(LOG_ERROR, "Audio decryptor not initialized");
        return false;
    }
    
    m_audio_decryptor->decrypt(data, length);
    return true;
}

void AirPlayStream::processAudioPacket(const std::vector<uint8_t>& data)
{
    std::vector<float> left;
    std::vector<float> right;
    int sample_rate = 0;
    const uint8_t codec_type = (m_config.audio_codec == "ALAC") ? 2 : 8;

    // Audio packets may also be encrypted
    if (m_config.encrypted && data.size() > 0) {
        std::vector<uint8_t> decrypted_data = data;
        if (!decryptAudioData(decrypted_data.data(), decrypted_data.size())) {
            blog(LOG_ERROR, "Audio decryption failed");
            return;
        }
        
        // Feed audio data to decoder
        if (m_audio_decoder && !decrypted_data.empty()) {
            m_audio_decoder->decode(decrypted_data.data(),
                                    decrypted_data.size(),
                                    codec_type,
                                    left,
                                    right,
                                    sample_rate);
        }
    } else {
        // Unencrypted audio
        if (m_audio_decoder && !data.empty()) {
            m_audio_decoder->decode(data.data(),
                                    data.size(),
                                    codec_type,
                                    left,
                                    right,
                                    sample_rate);
        }
    }
}

void AirPlayStream::outputVideoFrame(uint8_t** data, int* linesize, int width, int height, uint64_t pts)
{
    if (!m_source) {
        return;
    }
    
    struct obs_source_frame frame = {};
    frame.data[0] = data[0];
    frame.data[1] = data[1];
    frame.data[2] = data[2];
    frame.linesize[0] = linesize[0];
    frame.linesize[1] = linesize[1];
    frame.linesize[2] = linesize[2];
    frame.width = width;
    frame.height = height;
    frame.format = VIDEO_FORMAT_I420;
    frame.timestamp = pts;
    
    obs_source_output_video(m_source, &frame);
}

void AirPlayStream::outputAudioData(uint8_t* data, int samples, int channels, int sample_rate, uint64_t pts)
{
    if (!m_source) {
        return;
    }
    
    struct obs_source_audio audio = {};
    audio.data[0] = data;
    audio.frames = samples;
    audio.speakers = (channels == 2) ? SPEAKERS_STEREO : SPEAKERS_MONO;
    audio.samples_per_sec = sample_rate;
    audio.format = AUDIO_FORMAT_FLOAT;
    audio.timestamp = pts;
    
    obs_source_output_audio(m_source, &audio);
}
