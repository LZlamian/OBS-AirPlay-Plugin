// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows-update.hpp"
#include "update-trusted-keys.h"

#include <openssl/evp.h>

#include <cctype>
#include <cstdio>
#include <cstring>

namespace windows_update {
namespace {

constexpr const char* kDownloadPrefix =
    "https://github.com/LZlamian/OBS-AirPlay-Plugin/releases/download/";

bool parse_version(const std::string& text, int parts[3])
{
    std::string version = text;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) version.erase(0, 1);
    size_t start = 0;
    for (int index = 0; index < 3; ++index) {
        const size_t end = index < 2 ? version.find('.', start) : version.size();
        if (end == std::string::npos) return false;
        const std::string part = version.substr(start, end - start);
        if (part.empty() || part.size() > 6) return false;
        for (const char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        parts[index] = std::stoi(part);
        start = end + 1;
    }
    return true;
}

bool from_hex(const std::string& hex, std::vector<uint8_t>& out)
{
    if (hex.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned value = 0;
        if (!std::isxdigit(static_cast<unsigned char>(hex[i])) ||
            !std::isxdigit(static_cast<unsigned char>(hex[i + 1])) ||
            std::sscanf(hex.c_str() + i, "%2x", &value) != 1) {
            return false;
        }
        out.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

std::vector<std::array<uint8_t, 32>> trusted_keys()
{
    std::vector<std::array<uint8_t, 32>> keys;
    for (const char* hex : kTrustedUpdateKeys) {
        std::vector<uint8_t> bytes;
        if (from_hex(hex, bytes) && bytes.size() == 32) {
            std::array<uint8_t, 32> key{};
            std::memcpy(key.data(), bytes.data(), 32);
            keys.push_back(key);
        }
    }
    return keys;
}

bool ed25519_verify(const std::array<uint8_t, 32>& key, const std::string& message,
                    const std::vector<uint8_t>& signature)
{
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size());
    if (!pkey) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const bool valid = context &&
        EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestVerify(context, signature.data(), signature.size(),
                         reinterpret_cast<const unsigned char*>(message.data()),
                         message.size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(pkey);
    return valid;
}

} // namespace

std::string plain_version(const std::string& version)
{
    int parts[3];
    if (!parse_version(version, parts)) return {};
    return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
           std::to_string(parts[2]);
}

bool is_newer(const std::string& candidate, const std::string& current)
{
    int a[3];
    int b[3];
    if (!parse_version(candidate, a) || !parse_version(current, b)) return false;
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

bool is_project_download_url(const std::string& url)
{
    if (url.compare(0, std::strlen(kDownloadPrefix), kDownloadPrefix) != 0) return false;
    // No query, fragment or path tricks; '@' would only matter before the host.
    return url.find_first_of("?#\\") == std::string::npos &&
           url.find("/../") == std::string::npos && url.find("/./") == std::string::npos;
}

std::optional<Update> installable(const std::string& tag, const std::vector<ReleaseAsset>& assets)
{
    const std::string version = plain_version(tag);
    if (version.empty()) return std::nullopt;
    const std::string name = "obs-airplay-v" + version + "-windows-x64-setup.exe";
    const std::string signature_name = name + ".sig";

    std::string signature_url;
    for (const ReleaseAsset& asset : assets) {
        if (asset.name == signature_name && is_project_download_url(asset.url) &&
            asset.size > 0 && asset.size <= kMaxSignatureSize) {
            signature_url = asset.url;
        }
    }
    if (signature_url.empty()) return std::nullopt;  // unsigned: not installed

    for (const ReleaseAsset& asset : assets) {
        if (asset.name != name) continue;
        const std::string prefix = "sha256:";
        if (!is_project_download_url(asset.url) || asset.size == 0 ||
            asset.size > kMaxInstallerSize || asset.digest.compare(0, prefix.size(), prefix) != 0 ||
            asset.digest.size() != prefix.size() + 64) {
            return std::nullopt;
        }
        std::string sha256 = asset.digest.substr(prefix.size());
        for (char& c : sha256) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        Update update;
        update.version = version;
        update.name = name;
        update.url = asset.url;
        update.signature_url = signature_url;
        update.sha256 = sha256;
        update.size = asset.size;
        return update;
    }
    return std::nullopt;
}

std::string signed_message(const Update& update)
{
    return "obs-airplay-update-v1\nname=" + update.name + "\nversion=" + update.version +
           "\nsize=" + std::to_string(update.size) + "\nsha256=" + update.sha256 + "\n";
}

// The .sig asset is two lines:
//   obs-airplay-update-signature-v1
//   sig: <base64 Ed25519 signature of signed_message>
bool verify_signature(const Update& update, const std::string& signature_file,
                      const std::vector<std::array<uint8_t, 32>>* keys)
{
    if (signature_file.empty() || signature_file.size() > kMaxSignatureSize ||
        update.name.empty() || update.version.empty() || update.sha256.size() != 64) {
        return false;
    }
    const std::string header = "obs-airplay-update-signature-v1";
    size_t first_end = signature_file.find_first_of("\r\n");
    if (first_end == std::string::npos || signature_file.compare(0, first_end, header) != 0 ||
        first_end != header.size()) {
        return false;
    }
    size_t second = signature_file.find_first_not_of("\r\n", first_end);
    if (second == std::string::npos || signature_file.compare(second, 5, "sig: ") != 0) return false;
    size_t second_end = signature_file.find_first_of("\r\n", second);
    const std::string encoded = signature_file.substr(
        second + 5, second_end == std::string::npos ? std::string::npos : second_end - second - 5);
    if (encoded.size() != 88) return false;  // 64 bytes in base64, with "==" padding

    std::vector<uint8_t> decoded(66);
    const int length = EVP_DecodeBlock(decoded.data(),
                                       reinterpret_cast<const unsigned char*>(encoded.data()),
                                       static_cast<int>(encoded.size()));
    // EVP_DecodeBlock counts the padding as decoded bytes.
    if (length != 66 || encoded.compare(86, 2, "==") != 0) return false;
    decoded.resize(64);

    const std::string message = signed_message(update);
    const std::vector<std::array<uint8_t, 32>> trusted = keys ? *keys : trusted_keys();
    for (const auto& key : trusted) {
        if (ed25519_verify(key, message, decoded)) return true;
    }
    return false;
}

Sha256::Sha256() : m_context(EVP_MD_CTX_new())
{
    if (m_context) EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(m_context), EVP_sha256(), nullptr);
}

Sha256::~Sha256()
{
    EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(m_context));
}

void Sha256::update(const void* data, size_t size)
{
    if (m_context) EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(m_context), data, size);
}

std::string Sha256::hex()
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!m_context ||
        EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(m_context), digest, &length) != 1) {
        return {};
    }
    std::string hex;
    char byte[3];
    for (unsigned int i = 0; i < length; ++i) {
        std::snprintf(byte, sizeof(byte), "%02x", digest[i]);
        hex += byte;
    }
    return hex;
}

} // namespace windows_update
