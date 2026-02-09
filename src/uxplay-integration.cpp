#include "uxplay-integration.hpp"
#include <obs-module.h>
#include <array>
#include <cctype>
#include <cstring>

// UxPlay headers (extern "C" to avoid C++ name mangling issues)
extern "C" {
#include "../uxplay/lib/raop.h"
#include "../uxplay/lib/netutils.h"
#include "../uxplay/lib/logger.h"
#include "../uxplay/lib/stream.h"
#include "../uxplay/lib/dnssd.h"
}

UxPlayIntegration::UxPlayIntegration()
    : m_running(false)
    , m_worker_thread(nullptr)
    , m_should_stop(false)
    , m_raop(nullptr)
    , m_dnssd(nullptr)
    , m_actual_port(0)
{
}

UxPlayIntegration::~UxPlayIntegration()
{
    stop();
}

bool UxPlayIntegration::start(const std::string& device_id_str, int port)
{
    blog(LOG_INFO, "=== Starting UxPlay Integration ===");
    blog(LOG_INFO, "Requested port: %d", port);
    
    if (m_running) {
        blog(LOG_WARNING, "UxPlay integration already running");
        return true;
    }
    
    // Initialize network first
    blog(LOG_INFO, "Initializing network...");
    int net_result = netutils_init();
    if (net_result != 0) {
        blog(LOG_ERROR, "Failed to initialize network, error code: %d", net_result);
        return false;
    }
    blog(LOG_INFO, "Network initialized successfully");
    
    try {
        auto parse_device_id = [](const std::string& value, std::array<char, 6>& out_hw_addr) -> bool {
            std::string compact;
            compact.reserve(12);
            for (char ch : value) {
                if (ch == ':') {
                    continue;
                }
                compact.push_back(ch);
            }

            if (compact.size() != 12) {
                return false;
            }

            for (size_t i = 0; i < compact.size(); ++i) {
                if (!std::isxdigit(static_cast<unsigned char>(compact[i]))) {
                    return false;
                }
            }

            for (size_t i = 0; i < out_hw_addr.size(); ++i) {
                const std::string byte_str = compact.substr(i * 2, 2);
                out_hw_addr[i] = static_cast<char>(std::strtoul(byte_str.c_str(), nullptr, 16));
            }
            return true;
        };

        auto hex_no_colon = [](const std::string& value) -> std::string {
            std::string compact;
            compact.reserve(12);
            for (char ch : value) {
                if (ch != ':') {
                    compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
                }
            }
            return compact;
        };

        // Set up RAOP callbacks
        raop_callbacks_t callbacks = {};
        callbacks.cls = this;
        
        // Set video and audio processing callbacks
        callbacks.video_process = [](void* cls, raop_ntp_t* ntp, video_decode_struct* data) {
            if (cls && data) {
                auto* self = static_cast<UxPlayIntegration*>(cls);
                self->processVideoData(data);
            }
        };
        
        callbacks.audio_process = [](void* cls, raop_ntp_t* ntp, audio_decode_struct* data) {
            if (cls && data) {
                auto* self = static_cast<UxPlayIntegration*>(cls);
                self->processAudioData(data);
            }
        };

        // Required by UxPlay runtime; provide safe defaults for callbacks
        // that UxPlay may call without null checks.
        callbacks.conn_feedback = [](void* cls) {
            UNUSED_PARAMETER(cls);
        };
        callbacks.conn_reset = [](void* cls, int reason) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(reason);
        };
        callbacks.video_flush = [](void* cls) {
            UNUSED_PARAMETER(cls);
        };
        callbacks.audio_flush = [](void* cls) {
            UNUSED_PARAMETER(cls);
        };
        callbacks.audio_set_volume = [](void* cls, float volume) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(volume);
        };
        callbacks.audio_set_metadata = [](void* cls, const void* buffer, int buflen) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(buffer);
            UNUSED_PARAMETER(buflen);
        };
        callbacks.audio_set_coverart = [](void* cls, const void* buffer, int buflen) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(buffer);
            UNUSED_PARAMETER(buflen);
        };
        callbacks.audio_set_progress = [](void* cls, uint32_t* start, uint32_t* curr, uint32_t* end) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(start);
            UNUSED_PARAMETER(curr);
            UNUSED_PARAMETER(end);
        };
        callbacks.audio_set_client_volume = [](void* cls) -> double {
            UNUSED_PARAMETER(cls);
            return -15.0;
        };
        callbacks.on_video_play = [](void* cls, const char* location, const float start_position) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(location);
            UNUSED_PARAMETER(start_position);
        };
        callbacks.on_video_scrub = [](void* cls, const float position) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(position);
        };
        callbacks.on_video_rate = [](void* cls, const float rate) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(rate);
        };
        callbacks.on_video_stop = [](void* cls) {
            UNUSED_PARAMETER(cls);
        };
        callbacks.on_video_acquire_playback_info = [](void* cls, playback_info_t* playback_video) {
            UNUSED_PARAMETER(cls);
            if (!playback_video) {
                return;
            }
            playback_video->stallcount = 0;
            playback_video->duration = 0.0;
            playback_video->position = 0.0;
            playback_video->seek_start = 0.0;
            playback_video->seek_duration = 0.0;
            playback_video->rate = 0.0f;
            playback_video->ready_to_play = false;
            playback_video->playback_buffer_empty = true;
            playback_video->playback_buffer_full = false;
            playback_video->playback_likely_to_keep_up = false;
            playback_video->num_loaded_time_ranges = 0;
            playback_video->num_seekable_time_ranges = 0;
            playback_video->loadedTimeRanges = nullptr;
            playback_video->seekableTimeRanges = nullptr;
        };
        callbacks.on_video_playlist_remove = [](void* cls) -> float {
            UNUSED_PARAMETER(cls);
            return 0.0f;
        };

        // Required by UxPlay mirror thread.
        callbacks.video_pause = [](void* cls) {
            UNUSED_PARAMETER(cls);
        };
        callbacks.video_resume = [](void* cls) {
            UNUSED_PARAMETER(cls);
        };
        callbacks.video_reset = [](void* cls, reset_type_t reset_type) {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(reset_type);
        };
        callbacks.video_set_codec = [](void* cls, video_codec_t codec) -> int {
            UNUSED_PARAMETER(cls);
            UNUSED_PARAMETER(codec);
            return 0;
        };
        
        // Initialize RAOP
        blog(LOG_INFO, "Initializing RAOP...");
        m_raop = raop_init(&callbacks);
        if (!m_raop) {
            blog(LOG_ERROR, "Failed to initialize RAOP");
            return false;
        }
        
        // Set log callback to redirect UxPlay logs to OBS
        raop_set_log_callback(m_raop, [](void* cls, int level, const char* msg) {
            int obs_level;
            switch (level) {
                case 0: // LOGGER_EMERG
                case 1: // LOGGER_ALERT
                case 2: // LOGGER_CRIT
                case 3: // LOGGER_ERR
                    obs_level = LOG_ERROR;
                    break;
                case 4: // LOGGER_WARNING
                    obs_level = LOG_WARNING;
                    break;
                case 5: // LOGGER_NOTICE
                case 6: // LOGGER_INFO
                    obs_level = LOG_INFO;
                    break;
                case 7: // LOGGER_DEBUG
                case 8: // LOGGER_DEBUG_DATA
                default:
                    obs_level = LOG_DEBUG;
                    break;
            }
            // Strip trailing newline if present as blog() adds one
            std::string log_msg = msg;
            if (!log_msg.empty() && log_msg.back() == '\n') {
                log_msg.pop_back();
            }
            blog(obs_level, "[UxPlay] %s", log_msg.c_str());
        }, this);
        
        blog(LOG_INFO, "RAOP initialized successfully");

        // Initialize DNS-SD context needed by UxPlay's RAOP handlers (/info and TXT payloads)
        std::array<char, 6> hw_addr = {};
        std::string normalized_device_id = device_id_str;
        if (!parse_device_id(device_id_str, hw_addr)) {
            normalized_device_id = "AA:BB:CC:DD:EE:FF";
            parse_device_id(normalized_device_id, hw_addr);
            blog(LOG_WARNING, "Invalid device_id '%s'; using fallback '%s'",
                 device_id_str.c_str(), normalized_device_id.c_str());
        }

        int dnssd_error = 0;
        const char* service_name = "OBS AirPlay";
        m_dnssd = dnssd_init(service_name,
                             static_cast<int>(std::strlen(service_name)),
                             hw_addr.data(),
                             static_cast<int>(hw_addr.size()),
                             &dnssd_error,
                             0);
        if (!m_dnssd || dnssd_error != DNSSD_ERROR_NOERROR) {
            blog(LOG_ERROR, "Failed to initialize UxPlay dnssd context (error=%d)", dnssd_error);
            raop_destroy(m_raop);
            m_raop = nullptr;
            m_dnssd = nullptr;
            return false;
        }

        // Set port
        blog(LOG_INFO, "Setting RAOP port to %d", port);
        raop_set_port(m_raop, port);
        
        // Initialize RAOP with provided device ID and empty keyfile
        blog(LOG_INFO, "Calling raop_init2 with Device ID: %s", device_id_str.c_str());
        if (raop_init2(m_raop, 1, device_id_str.c_str(), "") != 0) {
            blog(LOG_ERROR, "Failed to initialize RAOP with device ID");
            raop_destroy(m_raop);
            dnssd_destroy(m_dnssd);
            m_raop = nullptr;
            m_dnssd = nullptr;
            return false;
        }
        blog(LOG_INFO, "raop_init2 completed successfully");
        raop_set_dnssd(m_raop, m_dnssd);
        
        // Start HTTP server
        blog(LOG_INFO, "Starting HTTP server...");
        unsigned short actual_port = static_cast<unsigned short>(port);
        int result = raop_start_httpd(m_raop, &actual_port);
        if (result < 0) {
            blog(LOG_ERROR, "Failed to start RAOP HTTP server, error code: %d", result);
            blog(LOG_ERROR, "Requested port: %d, actual_port: %d", port, actual_port);
            
            // Get detailed error information
            int error_code = errno;
            blog(LOG_ERROR, "System errno: %d (%s)", error_code, strerror(error_code));
            
            // Try a different port if 8000 is busy
            if (port == 7000) {
                actual_port = 7001;
                blog(LOG_INFO, "Trying alternative port %d...", actual_port);
                result = raop_start_httpd(m_raop, &actual_port);
                if (result < 0) {
                    blog(LOG_ERROR, "Failed to start RAOP HTTP server on port %d, error code: %d",
                         actual_port, result);
                    blog(LOG_ERROR, "System errno: %d (%s)", errno, strerror(errno));
                    raop_destroy(m_raop);
                    dnssd_destroy(m_dnssd);
                    m_raop = nullptr;
                    m_dnssd = nullptr;
                    return false;
                }
                blog(LOG_INFO, "UxPlay integration started successfully on port %d", actual_port);
            } else {
                raop_destroy(m_raop);
                dnssd_destroy(m_dnssd);
                m_raop = nullptr;
                m_dnssd = nullptr;
                return false;
            }
        } else {
            blog(LOG_INFO, "UxPlay integration started successfully on port %d", actual_port);
        }
        
        // Store the actual port for mDNS advertising
        m_actual_port = actual_port;
        raop_set_port(m_raop, m_actual_port);
        blog(LOG_INFO, "UxPlay integration running on actual port: %d", m_actual_port);

        // Register DNS-SD services so UxPlay can build correct TXT payloads for /info.
        // We still advertise with OBS' custom publisher for discovery consistency.
        int raop_register_error = dnssd_register_raop(m_dnssd, m_actual_port);
        if (raop_register_error != DNSSD_ERROR_NOERROR) {
            blog(LOG_WARNING, "dnssd_register_raop returned %d", raop_register_error);
        }
        int airplay_register_error = dnssd_register_airplay(m_dnssd, m_actual_port);
        if (airplay_register_error != DNSSD_ERROR_NOERROR) {
            blog(LOG_WARNING, "dnssd_register_airplay returned %d", airplay_register_error);
        }
        blog(LOG_INFO, "UxPlay device ID (RAOP): %s", hex_no_colon(normalized_device_id).c_str());
        
        m_running = true;
        return true;
        
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "Exception in UxPlayIntegration::start: %s", e.what());
        if (m_raop) {
            raop_destroy(m_raop);
            m_raop = nullptr;
        }
        if (m_dnssd) {
            dnssd_destroy(m_dnssd);
            m_dnssd = nullptr;
        }
        return false;
    }
}

void UxPlayIntegration::disableInternalMDNS()
{
    // No-op: kept for compatibility with existing call sites.
}

std::string UxPlayIntegration::getPK() const
{
    if (m_raop) {
        return raop_get_pk_str(m_raop);
    }
    return "";
}

void UxPlayIntegration::stop()
{
    if (!m_running) {
        return;
    }
    
    blog(LOG_INFO, "Stopping UxPlay integration");
    
    m_running = false;
    
    // Stop RAOP HTTP server
    if (m_raop) {
        raop_stop_httpd(m_raop);
        raop_destroy(m_raop);
        m_raop = nullptr;
    }

    if (m_dnssd) {
        dnssd_unregister_raop(m_dnssd);
        dnssd_unregister_airplay(m_dnssd);
        dnssd_destroy(m_dnssd);
        m_dnssd = nullptr;
    }
    
    blog(LOG_INFO, "UxPlay integration stopped");
}

void UxPlayIntegration::setVideoCallback(VideoFrameCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_video_callback = callback;
}

void UxPlayIntegration::setAudioCallback(AudioDataCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_audio_callback = callback;
}

void UxPlayIntegration::processVideoData(video_decode_struct* data)
{
    if (!data || !data->data) {
        return;
    }

    VideoFrameCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_video_callback;
    }

    if (!callback) {
        return;
    }

    blog(LOG_DEBUG, "Processing video data: %d bytes, H265: %s",
         data->data_len, data->is_h265 ? "yes" : "no");
    callback(data->data,
             static_cast<size_t>(data->data_len),
             data->ntp_time_local,
             data->is_h265);
}

void UxPlayIntegration::processAudioData(audio_decode_struct* data)
{
    if (!data || !data->data) {
        return;
    }

    AudioDataCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_audio_callback;
    }

    if (!callback) {
        return;
    }

    blog(LOG_DEBUG, "Processing audio data: %d bytes, ct: %d",
         data->data_len, data->ct);

    callback(data->data,
             static_cast<size_t>(data->data_len),
             data->ct,
             data->ntp_time_local);
}

void UxPlayIntegration::workerThread()
{
    blog(LOG_INFO, "UxPlay worker thread started");
    
    while (!m_should_stop) {
        // In a real implementation, this would handle UxPlay's main loop
        // For now, we'll just sleep and check for stop condition
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return m_should_stop.load(); });
    }
    
    blog(LOG_INFO, "UxPlay worker thread stopped");
}
