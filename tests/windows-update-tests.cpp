// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows self-updater's rules (src/windows-update.cpp): versions,
// release asset selection and the Ed25519 update signature.
//   obs-airplay-windows-update-tests [SIG_FILE NAME VERSION SIZE SHA256 PUBLIC_KEY_HEX]
// With arguments it only checks that SIG_FILE (made by scripts/sign-release.sh)
// verifies for that asset and key.
#include "windows-update.hpp"

#include <openssl/evp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace windows_update;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #condition);                                             \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

const std::string kBase = "https://github.com/LZlamian/OBS-AirPlay-Plugin/releases/download/v2.7.0/";
const std::string kDigest = "sha256:" + std::string(64, 'A');

std::vector<ReleaseAsset> releaseAssets()
{
    return {
        {"obs-airplay-v2.7.0-macos-arm64.zip", kBase + "obs-airplay-v2.7.0-macos-arm64.zip", 5000, kDigest},
        {"obs-airplay-v2.7.0-windows-x64-setup.exe", kBase + "obs-airplay-v2.7.0-windows-x64-setup.exe",
         6000000, kDigest},
        {"obs-airplay-v2.7.0-windows-x64-setup.exe.sig",
         kBase + "obs-airplay-v2.7.0-windows-x64-setup.exe.sig", 130, ""},
    };
}

void testVersions()
{
    CHECK(plain_version("v2.7.0") == "2.7.0");
    CHECK(plain_version("2.10.3") == "2.10.3");
    CHECK(plain_version("2.7").empty());
    CHECK(plain_version("2.7.0-beta").empty());
    CHECK(plain_version("v2.7.0.1").empty());
    CHECK(is_newer("v2.7.0", "2.6.0"));
    CHECK(is_newer("2.10.0", "2.9.9"));
    CHECK(!is_newer("2.6.0", "2.6.0"));
    CHECK(!is_newer("2.5.9", "2.6.0"));
    CHECK(!is_newer("garbage", "2.6.0"));
}

void testUrls()
{
    CHECK(is_project_download_url(kBase + "x.exe"));
    CHECK(!is_project_download_url("http://github.com/LZlamian/OBS-AirPlay-Plugin/releases/download/v2.7.0/x.exe"));
    CHECK(!is_project_download_url("https://github.com/someone/OBS-AirPlay-Plugin/releases/download/v2.7.0/x.exe"));
    CHECK(!is_project_download_url(kBase + "x.exe?y=1"));
    CHECK(!is_project_download_url(kBase + "../../../other/x.exe"));
    CHECK(!is_project_download_url("https://github.com.evil.example/LZlamian/OBS-AirPlay-Plugin/releases/download/x"));
}

void testInstallable()
{
    auto update = installable("v2.7.0", releaseAssets());
    CHECK(update.has_value());
    if (update) {
        CHECK(update->version == "2.7.0");
        CHECK(update->name == "obs-airplay-v2.7.0-windows-x64-setup.exe");
        CHECK(update->sha256 == std::string(64, 'a'));
        CHECK(update->size == 6000000);
        CHECK(update->signature_url == kBase + "obs-airplay-v2.7.0-windows-x64-setup.exe.sig");
    }

    auto assets = releaseAssets();
    assets.pop_back();  // no signature
    CHECK(!installable("v2.7.0", assets));

    assets = releaseAssets();
    assets[1].digest.clear();  // no published digest
    CHECK(!installable("v2.7.0", assets));

    assets = releaseAssets();
    assets[1].url = "https://example.com/obs-airplay-v2.7.0-windows-x64-setup.exe";
    CHECK(!installable("v2.7.0", assets));

    assets = releaseAssets();
    assets[1].size = kMaxInstallerSize + 1;
    CHECK(!installable("v2.7.0", assets));

    // The tag decides the name: a 2.7.0 installer is not 2.8.0's.
    CHECK(!installable("v2.8.0", releaseAssets()));
    CHECK(!installable("latest", releaseAssets()));
}

std::string base64(const unsigned char* data, size_t size)
{
    std::string out(4 * ((size + 2) / 3) + 1, '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), data,
                                       static_cast<int>(size));
    out.resize(static_cast<size_t>(length));
    return out;
}

void testSignature()
{
    EVP_PKEY_CTX* keygen = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY* key = nullptr;
    CHECK(keygen && EVP_PKEY_keygen_init(keygen) == 1 && EVP_PKEY_keygen(keygen, &key) == 1);
    EVP_PKEY_CTX_free(keygen);
    if (!key) return;
    std::array<uint8_t, 32> public_key{};
    size_t public_size = public_key.size();
    CHECK(EVP_PKEY_get_raw_public_key(key, public_key.data(), &public_size) == 1);
    const std::vector<std::array<uint8_t, 32>> keys = {public_key};

    Update update = *installable("v2.7.0", releaseAssets());
    const std::string message = signed_message(update);
    CHECK(message ==
          "obs-airplay-update-v1\nname=obs-airplay-v2.7.0-windows-x64-setup.exe\n"
          "version=2.7.0\nsize=6000000\nsha256=" + std::string(64, 'a') + "\n");

    unsigned char signature[64];
    size_t signature_size = sizeof(signature);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    CHECK(EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1);
    CHECK(EVP_DigestSign(context, signature, &signature_size,
                         reinterpret_cast<const unsigned char*>(message.data()),
                         message.size()) == 1);
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);

    const std::string file = "obs-airplay-update-signature-v1\nsig: " +
                             base64(signature, signature_size) + "\n";
    CHECK(verify_signature(update, file, &keys));
    CHECK(verify_signature(update, "obs-airplay-update-signature-v1\r\nsig: " +
                                       base64(signature, signature_size) + "\r\n", &keys));
    // Not by a trusted key (the built-in one).
    CHECK(!verify_signature(update, file));

    Update other = update;
    other.size += 1;
    CHECK(!verify_signature(other, file, &keys));
    other = update;
    other.sha256[0] = 'b';
    CHECK(!verify_signature(other, file, &keys));
    other = update;
    other.version = "2.7.1";
    CHECK(!verify_signature(other, file, &keys));

    CHECK(!verify_signature(update, "", &keys));
    CHECK(!verify_signature(update, "sig: " + base64(signature, signature_size), &keys));
    CHECK(!verify_signature(update, "obs-airplay-update-signature-v2\nsig: " +
                                        base64(signature, signature_size), &keys));
    std::string damaged = file;
    damaged[45] = damaged[45] == 'A' ? 'B' : 'A';
    CHECK(!verify_signature(update, damaged, &keys));
    CHECK(!verify_signature(update, "obs-airplay-update-signature-v1\nsig: AAAA\n", &keys));
}

void testSha256()
{
    Sha256 empty;
    CHECK(empty.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Sha256 abc;
    abc.update("a", 1);
    abc.update("bc", 2);
    CHECK(abc.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// SIG_FILE NAME VERSION SIZE SHA256 PUBLIC_KEY_HEX: a signature made by
// scripts/sign-release.sh verifies here.
int checkExternal(char** argv)
{
    FILE* file = std::fopen(argv[1], "rb");
    if (!file) return 2;
    std::string signature(4096, '\0');
    signature.resize(std::fread(&signature[0], 1, signature.size(), file));
    std::fclose(file);
    Update update;
    update.name = argv[2];
    update.version = argv[3];
    update.size = std::stoull(argv[4]);
    update.sha256 = argv[5];
    std::array<uint8_t, 32> key{};
    const std::string hex = argv[6];
    for (size_t i = 0; i < 32 && hex.size() == 64; ++i) {
        key[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }
    const std::vector<std::array<uint8_t, 32>> keys = {key};
    const bool ok = verify_signature(update, signature, &keys);
    std::printf("external signature: %s\n", ok ? "valid" : "INVALID");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 7) return checkExternal(argv);
    testVersions();
    testUrls();
    testInstallable();
    testSignature();
    testSha256();
    std::printf("windows-update-tests: %d check(s) failed\n", g_failures);
    return g_failures ? 1 : 0;
}
