#pragma once

#include <cstdint>
#include <vector>
#include <openssl/evp.h>

// AES-128-CTR decryption for AirPlay streams
// Based on UxPlay implementation

class AESDecryptor {
public:
    AESDecryptor();
    ~AESDecryptor();
    
    // Initialize AES context with audio key and stream connection ID
    bool init(const uint8_t* audio_key, uint16_t audio_key_len, uint64_t stream_connection_id);
    
    // Decrypt data in-place
    void decrypt(uint8_t* data, size_t length);
    
    // Reset cipher to fresh block boundary
    void startFreshBlock();
    
private:
    EVP_CIPHER_CTX* m_cipher_ctx;
    uint8_t m_key[16];
    uint8_t m_iv[16];
    uint8_t m_audio_key[16];
    int m_block_offset;
    uint8_t m_overflow_buffer[16];
    int m_overflow_count;
    bool m_initialized;
    
    void deriveVideoKey(uint64_t stream_connection_id);
};

// Helper functions for NAL unit processing
class NALProcessor {
public:
    // Replace NAL size prefix with start code (0x00 0x00 0x00 0x01)
    static void replaceNALSizeWithStartCode(uint8_t* data, size_t offset);
    
    // Get NAL unit type from header byte
    static uint8_t getNALType(uint8_t header_byte);
    
    // Check if NAL is SPS (Sequence Parameter Set)
    static bool isSPS(uint8_t nal_type);
    
    // Check if NAL is PPS (Picture Parameter Set)
    static bool isPPS(uint8_t nal_type);
    
    // Check if NAL is IDR (Instantaneous Decoder Refresh)
    static bool isIDR(uint8_t nal_type);
    
    // Check if NAL is SEI (Supplemental Enhancement Information)
    static bool isSEI(uint8_t nal_type);
};

// SHA-512 helper
std::vector<uint8_t> sha512(const std::vector<uint8_t>& data);
std::vector<uint8_t> sha512(const uint8_t* data, size_t length);
