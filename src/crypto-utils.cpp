#include "crypto-utils.hpp"
#include <obs-module.h>
#include <cstring>
#include <cstdio>
#include <cinttypes>

// AESDecryptor Implementation

AESDecryptor::AESDecryptor()
    : m_cipher_ctx(nullptr)
    , m_block_offset(0)
    , m_overflow_count(0)
    , m_initialized(false)
{
    memset(m_key, 0, sizeof(m_key));
    memset(m_iv, 0, sizeof(m_iv));
    memset(m_audio_key, 0, sizeof(m_audio_key));
    memset(m_overflow_buffer, 0, sizeof(m_overflow_buffer));
}

AESDecryptor::~AESDecryptor()
{
    if (m_cipher_ctx) {
        EVP_CIPHER_CTX_free(m_cipher_ctx);
        m_cipher_ctx = nullptr;
    }
}

bool AESDecryptor::init(const uint8_t* audio_key, uint16_t audio_key_len, uint64_t stream_connection_id)
{
    if (!audio_key) {
        blog(LOG_ERROR, "Audio key is null");
        return false;
    }

    if (audio_key_len > sizeof(m_audio_key)) {
        blog(LOG_ERROR, "Audio key length %d exceeds maximum %zu", audio_key_len, sizeof(m_audio_key));
        return false;
    }

    if (m_cipher_ctx) {
        EVP_CIPHER_CTX_free(m_cipher_ctx);
        m_cipher_ctx = nullptr;
    }
    
    // Store audio key
    memset(m_audio_key, 0, sizeof(m_audio_key));
    memcpy(m_audio_key, audio_key, audio_key_len);
    
    // Derive video key and IV from stream connection ID and audio key
    deriveVideoKey(stream_connection_id);
    
    // Initialize OpenSSL cipher context
    m_cipher_ctx = EVP_CIPHER_CTX_new();
    if (!m_cipher_ctx) {
        blog(LOG_ERROR, "Failed to create EVP cipher context");
        return false;
    }
    
    // Initialize AES-128-CTR
    if (EVP_EncryptInit_ex(m_cipher_ctx, EVP_aes_128_ctr(), nullptr, m_key, m_iv) != 1) {
        blog(LOG_ERROR, "Failed to initialize AES-128-CTR");
        EVP_CIPHER_CTX_free(m_cipher_ctx);
        m_cipher_ctx = nullptr;
        return false;
    }
    
    // Disable padding (CTR mode doesn't need it)
    EVP_CIPHER_CTX_set_padding(m_cipher_ctx, 0);
    
    m_initialized = true;
    m_block_offset = 0;
    m_overflow_count = 0;
    
    blog(LOG_INFO, "AES decryptor initialized successfully");
    return true;
}

void AESDecryptor::deriveVideoKey(uint64_t stream_connection_id)
{
    // AirPlay derives video key/IV from:
    // - A string template ("AirPlayStreamKey" + connection ID)
    // - The audio AES key (from RAOP setup)
    // - SHA-512 hash of the above
    
    char key_template[64];
    char iv_template[64];
    
    snprintf(key_template, sizeof(key_template), "AirPlayStreamKey%" PRIu64, stream_connection_id);
    snprintf(iv_template, sizeof(iv_template), "AirPlayStreamIV%" PRIu64, stream_connection_id);
    
    // Hash key template + audio key -> video key (using modern EVP API)
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        blog(LOG_ERROR, "Failed to create EVP_MD_CTX");
        return;
    }
    
    // Derive key
    EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(md_ctx, key_template, strlen(key_template));
    EVP_DigestUpdate(md_ctx, m_audio_key, sizeof(m_audio_key));
    
    uint8_t key_hash[EVP_MAX_MD_SIZE];
    unsigned int key_hash_len = 0;
    EVP_DigestFinal_ex(md_ctx, key_hash, &key_hash_len);
    memcpy(m_key, key_hash, 16); // Take first 16 bytes
    
    // Derive IV
    EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(md_ctx, iv_template, strlen(iv_template));
    EVP_DigestUpdate(md_ctx, m_audio_key, sizeof(m_audio_key));
    
    uint8_t iv_hash[EVP_MAX_MD_SIZE];
    unsigned int iv_hash_len = 0;
    EVP_DigestFinal_ex(md_ctx, iv_hash, &iv_hash_len);
    memcpy(m_iv, iv_hash, 16); // Take first 16 bytes
    
    EVP_MD_CTX_free(md_ctx);
    
    blog(LOG_DEBUG, "Derived video key for stream connection ID: %" PRIu64, stream_connection_id);
}

void AESDecryptor::decrypt(uint8_t* data, size_t length)
{
    if (!m_initialized || !m_cipher_ctx) {
        blog(LOG_ERROR, "AES decryptor not initialized");
        return;
    }
    
    // Handle overflow from previous packet
    if (m_overflow_count > 0) {
        size_t bytes_to_process = std::min((size_t)m_overflow_count, length);
        for (size_t i = 0; i < bytes_to_process; i++) {
            data[i] ^= m_overflow_buffer[(16 - m_overflow_count) + i];
        }
        m_overflow_count -= bytes_to_process;
        
        if (bytes_to_process >= length) {
            return; // Entire packet handled by overflow
        }
        
        data += bytes_to_process;
        length -= bytes_to_process;
    }
    
    // Process complete 16-byte blocks
    size_t complete_blocks = length / 16;
    size_t complete_bytes = complete_blocks * 16;
    
    if (complete_bytes > 0) {
        // Start at a fresh block boundary
        startFreshBlock();
        
        // Decrypt complete blocks
        int out_len = 0;
        EVP_EncryptUpdate(m_cipher_ctx, data, &out_len, data, complete_bytes);
        
        m_block_offset = (m_block_offset + complete_bytes) % 16;
    }
    
    // Handle remaining bytes
    size_t remaining = length % 16;
    if (remaining > 0) {
        uint8_t* remaining_start = data + complete_bytes;
        
        // Encrypt a full block to get the keystream
        memset(m_overflow_buffer, 0, 16);
        memcpy(m_overflow_buffer, remaining_start, remaining);
        
        int out_len = 0;
        EVP_EncryptUpdate(m_cipher_ctx, m_overflow_buffer, &out_len, m_overflow_buffer, 16);
        
        // XOR the remaining bytes
        for (size_t i = 0; i < remaining; i++) {
            remaining_start[i] = m_overflow_buffer[i];
        }
        
        m_overflow_count = 16 - remaining;
    }
}

void AESDecryptor::startFreshBlock()
{
    if (m_block_offset == 0) {
        return; // Already at block boundary
    }
    
    // Advance to next block boundary by encrypting waste bytes
    uint8_t waste[16];
    int bytes_to_waste = 16 - m_block_offset;
    int out_len = 0;
    
    EVP_EncryptUpdate(m_cipher_ctx, waste, &out_len, waste, bytes_to_waste);
    m_block_offset = 0;
}

// NALProcessor Implementation

void NALProcessor::replaceNALSizeWithStartCode(uint8_t* data, size_t offset)
{
    // Replace 4-byte big-endian size with H.264 start code
    data[offset + 0] = 0x00;
    data[offset + 1] = 0x00;
    data[offset + 2] = 0x00;
    data[offset + 3] = 0x01;
}

uint8_t NALProcessor::getNALType(uint8_t header_byte)
{
    // NAL unit type is in bits 0-4 of the header byte
    return header_byte & 0x1F;
}

bool NALProcessor::isSPS(uint8_t nal_type)
{
    return nal_type == 7; // Sequence Parameter Set
}

bool NALProcessor::isPPS(uint8_t nal_type)
{
    return nal_type == 8; // Picture Parameter Set
}

bool NALProcessor::isIDR(uint8_t nal_type)
{
    return nal_type == 5; // IDR (Instantaneous Decoder Refresh)
}

bool NALProcessor::isSEI(uint8_t nal_type)
{
    return nal_type == 6; // Supplemental Enhancement Information
}

// SHA-512 helpers

std::vector<uint8_t> sha512(const std::vector<uint8_t>& data)
{
    return sha512(data.data(), data.size());
}

std::vector<uint8_t> sha512(const uint8_t* data, size_t length)
{
    std::vector<uint8_t> hash(EVP_MAX_MD_SIZE);
    unsigned int hash_len = 0;
    
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (md_ctx) {
        EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
        EVP_DigestUpdate(md_ctx, data, length);
        EVP_DigestFinal_ex(md_ctx, hash.data(), &hash_len);
        EVP_MD_CTX_free(md_ctx);
    }
    
    hash.resize(hash_len);
    return hash;
}
