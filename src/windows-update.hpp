// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Windows self-updater's rules, without I/O (src/windows-helper.cpp does
// the requests, the prompt and the launch). They follow the macOS updater
// (src/plugin-updater.mm): the update is the release's installer
// "obs-airplay-v<x.y.z>-windows-x64-setup.exe", installable only with a
// "<installer>.sig" Ed25519 signature by a key in src/update-trusted-keys.h
// over its name, version, size and SHA-256, a SHA-256 digest published by
// GitHub, and download addresses under this project's releases.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace windows_update {

constexpr uint64_t kMaxInstallerSize = 200ull * 1024 * 1024;
constexpr uint64_t kMaxSignatureSize = 4096;

// One entry of a GitHub release's "assets".
struct ReleaseAsset {
    std::string name;
    std::string url;     // browser_download_url
    uint64_t size = 0;
    std::string digest;  // "sha256:<64 hex>" (GitHub's published digest)
};

struct Update {
    std::string version;  // "2.7.0", no leading "v"
    std::string name;     // "obs-airplay-v2.7.0-windows-x64-setup.exe"
    std::string url;
    std::string signature_url;
    std::string sha256;   // lowercase hex
    uint64_t size = 0;
};

// "2.7.0" from "v2.7.0" or "2.7.0"; empty unless it is x.y.z.
std::string plain_version(const std::string& version);
// True when `candidate` is a later x.y.z than `current`.
bool is_newer(const std::string& candidate, const std::string& current);
// https://github.com/LZlamian/OBS-AirPlay-Plugin/releases/download/..., no
// query, no user info.
bool is_project_download_url(const std::string& url);
// The installer of a release (its tag and assets), or nothing when it has no
// installer for this version with a published SHA-256, a signature asset,
// and project download addresses.
std::optional<Update> installable(const std::string& tag, const std::vector<ReleaseAsset>& assets);

// The text an update signature signs (same as the macOS updater's).
std::string signed_message(const Update& update);
// Whether `signature_file` (the downloaded .sig asset) is a valid signature
// of `update` by a trusted key (or by `keys` when given: tests).
bool verify_signature(const Update& update, const std::string& signature_file,
                      const std::vector<std::array<uint8_t, 32>>* keys = nullptr);

// Incremental SHA-256 (libcrypto), for a download checked as it is written.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t size);
    std::string hex();  // finishes; call once
private:
    void* m_context;
};

} // namespace windows_update
