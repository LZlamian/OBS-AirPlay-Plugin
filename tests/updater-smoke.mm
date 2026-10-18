// SPDX-License-Identifier: GPL-3.0-or-later
// Update download checks, staging and the swap, on scratch copies of a built
// plugin bundle. Nothing outside SCRATCH_DIR is touched and no network is used.
//   obs-airplay-updater-smoke BUILT_PLUGIN_BUNDLE SCRATCH_DIR
// With --live first, the latest GitHub release is downloaded and installed
// over a scratch copy instead.

#import "plugin-updater.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int failures = 0;
// OpenSSL 3 for test signing keys (macOS's /usr/bin/openssl is LibreSSL).
NSString *gOpenSSL = @"/opt/homebrew/bin/openssl";

void expect(bool condition, const char *what)
{
    std::printf("%-58s %s\n", what, condition ? "ok" : "FAILED");
    if (!condition)
        ++failures;
}

bool shell(NSString *command)
{
    return std::system(command.UTF8String) == 0;
}

NSString *quoted(NSString *path)
{
    return [NSString stringWithFormat:@"'%@'", path];
}

NSString *output(NSString *command)
{
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
    task.arguments = @[@"-c", command];
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    [task launchAndReturnError:nil];
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return [text stringByTrimmingCharactersInSet:
        [NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

NSString *bundleVersion(NSString *bundle)
{
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:
        [bundle stringByAppendingPathComponent:@"Contents/Info.plist"]];
    return info[@"CFBundleShortVersionString"];
}

// A copy of the built plugin with one Info.plist value changed, sealed again.
NSString *makeBundle(NSString *source, NSString *folder, NSString *key, NSString *value)
{
    NSString *bundle = [folder stringByAppendingPathComponent:@"obs-airplay.plugin"];
    shell([NSString stringWithFormat:@"mkdir -p %@ && cp -R %@ %@",
           quoted(folder), quoted(source), quoted(bundle)]);
    shell([NSString stringWithFormat:
        @"/usr/libexec/PlistBuddy -c 'Set :%@ %@' %@/Contents/Info.plist",
        key, value, quoted(bundle)]);
    shell([NSString stringWithFormat:
        @"codesign --force --deep --sign - %@ 2>/dev/null", quoted(bundle)]);
    return bundle;
}

NSString *makeKey(NSString *path)
{
    shell([NSString stringWithFormat:@"%@ genpkey -algorithm ed25519 -out %@ 2>/dev/null",
           gOpenSSL, quoted(path)]);
    return path;
}

NSData *publicKey(NSString *keyPath)
{
    NSString *hex = output([NSString stringWithFormat:
        @"%@ pkey -in %@ -pubout -outform DER 2>/dev/null | tail -c 32 | xxd -p -c 64",
        gOpenSSL, quoted(keyPath)]);
    NSMutableData *data = [NSMutableData data];
    for (NSUInteger i = 0; i + 1 < hex.length; i += 2) {
        unsigned int byte = 0;
        [[NSScanner scannerWithString:[hex substringWithRange:NSMakeRange(i, 2)]] scanHexInt:&byte];
        const unsigned char value = (unsigned char)byte;
        [data appendBytes:&value length:1];
    }
    return data;
}

// Signs the asset as scripts/sign-release.sh does (the same signed text).
void sign(OBSAirPlayReleaseAsset *asset, NSString *keyPath, NSString *scratch)
{
    NSString *message = [scratch stringByAppendingPathComponent:@"message.txt"];
    [OBSAirPlayUpdateSignedMessage(asset) writeToFile:message atomically:YES
                                             encoding:NSUTF8StringEncoding error:nil];
    NSString *base64 = output([NSString stringWithFormat:
        @"%@ pkeyutl -sign -inkey %@ -rawin -in %@ | base64", gOpenSSL, quoted(keyPath),
        quoted(message)]);
    asset.signature = [[NSString stringWithFormat:
        @"obs-airplay-update-signature-v1\nsig: %@\n", base64]
        dataUsingEncoding:NSUTF8StringEncoding];
}

// Zips everything in `folder`; the asset describes the zip as GitHub would.
// Always a new archive, without the extra timestamp field (it carries access
// times): `zip` on an existing archive updates it, which could change bytes
// an earlier asset's digest describes.
OBSAirPlayReleaseAsset *makeZip(NSString *folder, NSString *zip, NSString *version)
{
    shell([NSString stringWithFormat:@"rm -f %@ && cd %@ && /usr/bin/zip -q -r -y -X %@ .",
           quoted(zip), quoted(folder), quoted(zip)]);
    OBSAirPlayReleaseAsset *asset = [[OBSAirPlayReleaseAsset alloc] init];
    asset.version = version;
    asset.downloadURL = [NSURL URLWithString:@"https://github.com/example"];
    asset.sha256 = output([NSString stringWithFormat:
        @"shasum -a 256 %@ | cut -d' ' -f1", quoted(zip)]);
    asset.size = [[[NSFileManager defaultManager] attributesOfItemAtPath:zip error:nil] fileSize];
    asset.name = [NSString stringWithFormat:@"obs-airplay-v%@-macos-%@.zip", version,
                                            OBSAirPlayUpdaterArchitecture()];
    return asset;
}

NSDictionary *releaseJSON(NSString *name, NSString *address, id digest,
                          NSString *signatureAddress = nil)
{
    NSMutableDictionary *asset = [@{
        @"name" : name, @"browser_download_url" : address, @"size" : @1234567,
    } mutableCopy];
    if (digest)
        asset[@"digest"] = digest;
    NSMutableArray *assets = [NSMutableArray arrayWithObject:asset];
    if (signatureAddress) {
        [assets addObject:@{
            @"name" : [name stringByAppendingString:@".sig"],
            @"browser_download_url" : signatureAddress, @"size" : @120,
        }];
    }
    return @{@"tag_name" : @"v9.9.9", @"assets" : assets};
}

// The real thing against GitHub: the latest release is downloaded, staged
// and installed over a scratch copy of the built plugin marked as 0.0.1.
int live(NSString *built, NSString *scratch)
{
    NSFileManager *files = [NSFileManager defaultManager];
    [files removeItemAtPath:scratch error:nil];
    NSString *plugins = [scratch stringByAppendingPathComponent:@"plugins"];
    NSString *installed = makeBundle(built, plugins, @"CFBundleShortVersionString", @"0.0.1");
    NSURL *pluginURL = [NSURL fileURLWithPath:installed isDirectory:YES];

    NSData *json = [NSData dataWithContentsOfURL:[NSURL URLWithString:
        @"https://api.github.com/repos/LZlamian/OBS-AirPlay-Plugin/releases/latest"]];
    NSDictionary *release = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil]
                                 : nil;
    OBSAirPlayReleaseAsset *asset = release
        ? OBSAirPlayInstallableAsset(release, OBSAirPlayUpdaterArchitecture()) : nil;
    expect(asset && OBSAirPlayIsNewerVersion(asset.version, @"0.0.1"),
           "latest release has an installable zip");
    if (!asset)
        return 1;

    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSError *failure = nil;
    OBSAirPlayDownloadAndStageUpdate(asset, pluginURL, @"OBS-AirPlay-Updater-Smoke",
                                     ^(NSError *error) {
        failure = error;
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (failure)
        std::printf("  download error: %s\n", failure.localizedDescription.UTF8String);
    expect(!failure && [OBSAirPlayStagedUpdateVersion(pluginURL) isEqualToString:asset.version],
           "release zip downloaded, checked and staged");

    NSError *error = nil;
    expect(OBSAirPlayInstallStagedUpdate(pluginURL, &error) &&
           [bundleVersion(installed) isEqualToString:asset.version],
           "release installed over the scratch plugin");
    std::printf("updater-smoke --live %s -> %s\n", asset.version.UTF8String,
                failures == 0 ? "ok" : "FAILED");
    [files removeItemAtPath:scratch error:nil];
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        // Checks a signed release zip against the keys built into the
        // updater, as a client would (run before uploading a release).
        if (argc == 3 && std::strcmp(argv[1], "--verify-release") == 0) {
            NSString *zip = @(argv[2]);
            OBSAirPlayReleaseAsset *asset = [[OBSAirPlayReleaseAsset alloc] init];
            asset.name = zip.lastPathComponent;
            NSRegularExpression *pattern = [NSRegularExpression regularExpressionWithPattern:
                @"^obs-airplay-v([0-9]+\\.[0-9]+\\.[0-9]+)-macos-(arm64|x86_64)\\.zip$"
                                                                                 options:0
                                                                                   error:nil];
            NSTextCheckingResult *match = [pattern firstMatchInString:asset.name options:0
                                                                range:NSMakeRange(0, asset.name.length)];
            if (!match) {
                std::fprintf(stderr, "not a release zip name: %s\n", asset.name.UTF8String);
                return 1;
            }
            asset.version = [asset.name substringWithRange:[match rangeAtIndex:1]];
            asset.size = [[[NSFileManager defaultManager] attributesOfItemAtPath:zip error:nil]
                fileSize];
            asset.sha256 = output([NSString stringWithFormat:
                @"shasum -a 256 %@ | cut -d' ' -f1", quoted(zip)]);
            asset.signature = [NSData dataWithContentsOfFile:[zip stringByAppendingString:@".sig"]];
            const bool ok = OBSAirPlayVerifyUpdateSignature(asset);
            std::printf("%s: signature %s\n", asset.name.UTF8String,
                        ok ? "valid (built-in key)" : "INVALID");
            return ok ? 0 : 1;
        }
        if (argc == 4 && std::strcmp(argv[1], "--live") == 0) {
            return live(@(argv[2]),
                        [@(argv[3]) stringByAppendingPathComponent:@"updater-smoke-live"]);
        }
        if (argc != 3) {
            std::fprintf(stderr, "usage: %s [--live] BUILT_PLUGIN_BUNDLE SCRATCH_DIR | "
                                 "--verify-release ZIP\n", argv[0]);
            return 64;
        }
        NSString *built = @(argv[1]);
        NSString *scratch = [@(argv[2]) stringByAppendingPathComponent:@"updater-smoke"];
        NSFileManager *files = [NSFileManager defaultManager];
        [files removeItemAtPath:scratch error:nil];
        [files createDirectoryAtPath:scratch withIntermediateDirectories:YES attributes:nil
                               error:nil];
        if (const char *openssl = std::getenv("OPENSSL"))
            gOpenSSL = @(openssl);
        // The scratch plugin is not in the home folder; let it update itself.
        OBSAirPlaySetUpdateRootForTesting(scratch);
        NSString *releaseKey = makeKey([scratch stringByAppendingPathComponent:@"release.pem"]);
        NSString *otherKey = makeKey([scratch stringByAppendingPathComponent:@"other.pem"]);
        NSData *releasePublic = publicKey(releaseKey);
        expect(releasePublic.length == 32, "test signing key created");
        OBSAirPlaySetTrustedUpdateKeysForTesting(@[releasePublic]);

        // Versions and release parsing.
        expect(OBSAirPlayIsNewerVersion(@"v2.4.0", @"2.3.9") &&
               OBSAirPlayIsNewerVersion(@"2.10.0", @"2.9.9") &&
               !OBSAirPlayIsNewerVersion(@"2.3.0", @"2.3.0") &&
               !OBSAirPlayIsNewerVersion(@"2.4", @"2.3.0") &&
               !OBSAirPlayIsNewerVersion(nil, @"2.3.0"), "version comparison");

        NSString *digest = [@"sha256:" stringByPaddingToLength:71 withString:@"ab" startingAtIndex:0];
        NSString *name = @"obs-airplay-v9.9.9-macos-arm64.zip";
        NSString *address = @"https://github.com/LZlamian/OBS-AirPlay-Plugin/releases/download/v9.9.9/x.zip";
        NSString *sigAddress = [address stringByAppendingString:@".sig"];
        OBSAirPlayReleaseAsset *parsed =
            OBSAirPlayInstallableAsset(releaseJSON(name, address, digest, sigAddress), @"arm64");
        expect(parsed && [parsed.version isEqualToString:@"9.9.9"] && parsed.size == 1234567 &&
               parsed.sha256.length == 64 && [parsed.name isEqualToString:name] &&
               [parsed.signatureURL.absoluteString isEqualToString:sigAddress],
               "release with zip, digest and signature is installable");
        expect(!OBSAirPlayInstallableAsset(releaseJSON(name, address, nil, sigAddress), @"arm64"),
               "release without a digest is not");
        expect(!OBSAirPlayInstallableAsset(releaseJSON(name, address, digest), @"arm64"),
               "release without a signature asset is not");
        expect(!OBSAirPlayInstallableAsset(releaseJSON(name, address, digest, sigAddress), @"x86_64"),
               "zip for another architecture is not");
        expect(!OBSAirPlayInstallableAsset(
                   releaseJSON(name, @"https://example.com/x.zip", digest, sigAddress), @"arm64") &&
               !OBSAirPlayInstallableAsset(
                   releaseJSON(name, @"http://github.com/x.zip", digest, sigAddress), @"arm64"),
               "download outside https://github.com is not");
        expect(!OBSAirPlayInstallableAsset(
                   releaseJSON(name, @"https://github.com/someone/fork/releases/download/v9.9.9/x.zip",
                               digest, sigAddress), @"arm64") &&
               !OBSAirPlayInstallableAsset(
                   releaseJSON(name, address, digest,
                               @"https://github.com/someone/fork/releases/download/v9.9.9/x.zip.sig"),
                   @"arm64"),
               "download outside this project's releases is not");

        // The "installed" plugin and a newer release of it.
        NSString *plugins = [scratch stringByAppendingPathComponent:@"plugins"];
        NSString *installed = [plugins stringByAppendingPathComponent:@"obs-airplay.plugin"];
        shell([NSString stringWithFormat:@"mkdir -p %@ && cp -R %@ %@",
               quoted(plugins), quoted(built), quoted(installed)]);
        NSURL *pluginURL = [NSURL fileURLWithPath:installed isDirectory:YES];
        NSString *installedVersion = bundleVersion(installed);
        NSString *staging = [plugins stringByAppendingPathComponent:@".obs-airplay.update"];

        NSURL *helper = [pluginURL URLByAppendingPathComponent:
            @"Contents/Resources/OBS AirPlay Discovery.app" isDirectory:YES];
        expect([OBSAirPlayPluginURLForHelper(helper).path isEqualToString:pluginURL.path] &&
               !OBSAirPlayPluginURLForHelper(pluginURL) && OBSAirPlayCanReplacePlugin(pluginURL),
               "helper finds its plugin");
        OBSAirPlaySetUpdateRootForTesting([scratch stringByAppendingPathComponent:@"elsewhere"]);
        expect(!OBSAirPlayCanReplacePlugin(pluginURL),
               "a plugin outside the user's folder is not self-updated");
        OBSAirPlaySetUpdateRootForTesting(scratch);

        NSString *good = [scratch stringByAppendingPathComponent:@"good"];
        makeBundle(built, good, @"CFBundleShortVersionString", @"9.9.9");
        NSString *goodZip = [scratch stringByAppendingPathComponent:@"good.zip"];
        OBSAirPlayReleaseAsset *asset = makeZip(good, goodZip, @"9.9.9");
        sign(asset, releaseKey, scratch);
        NSURL *goodZipURL = [NSURL fileURLWithPath:goodZip];
        NSError *error = nil;
        expect(OBSAirPlayVerifyUpdateSignature(asset), "release key signature verifies");

        // Downloads that must be refused. Each leaves the plugin untouched.
        OBSAirPlayReleaseAsset *unsignedAsset = makeZip(good, goodZip, @"9.9.9");
        expect(!OBSAirPlayStageUpdate(goodZipURL, unsignedAsset, pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "unsigned zip is refused");

        OBSAirPlayReleaseAsset *otherSigned = makeZip(good, goodZip, @"9.9.9");
        sign(otherSigned, otherKey, scratch);
        expect(!OBSAirPlayStageUpdate(goodZipURL, otherSigned, pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "zip signed by another key is refused");

        // A valid signature for a different release does not carry over.
        OBSAirPlayReleaseAsset *replayed = makeZip(good, goodZip, @"9.9.9");
        replayed.signature = asset.signature;
        replayed.name = @"obs-airplay-v9.9.8-macos-arm64.zip";
        expect(!OBSAirPlayStageUpdate(goodZipURL, replayed, pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "signature of another asset is refused");

        OBSAirPlayReleaseAsset *wrongDigest = makeZip(good, goodZip, @"9.9.9");
        wrongDigest.sha256 = [@"" stringByPaddingToLength:64 withString:@"0" startingAtIndex:0];
        expect(!OBSAirPlayStageUpdate(goodZipURL, wrongDigest, pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "zip with another digest is refused");

        OBSAirPlayReleaseAsset *wrongVersion = makeZip(good, goodZip, @"9.9.8");
        sign(wrongVersion, releaseKey, scratch);
        expect(!OBSAirPlayStageUpdate(goodZipURL, wrongVersion, pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "bundle of another version is refused");

        NSString *tampered = [scratch stringByAppendingPathComponent:@"tampered"];
        NSString *tamperedBundle = makeBundle(built, tampered, @"CFBundleShortVersionString", @"9.9.9");
        shell([NSString stringWithFormat:@"echo changed >> %@/Contents/Resources/locale/en-US.ini",
               quoted(tamperedBundle)]);
        NSString *tamperedZip = [scratch stringByAppendingPathComponent:@"tampered.zip"];
        OBSAirPlayReleaseAsset *tamperedAsset = makeZip(tampered, tamperedZip, @"9.9.9");
        sign(tamperedAsset, releaseKey, scratch);
        expect(!OBSAirPlayStageUpdate([NSURL fileURLWithPath:tamperedZip], tamperedAsset,
                                      pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "bundle changed after signing is refused");

        NSString *other = [scratch stringByAppendingPathComponent:@"other"];
        makeBundle(built, other, @"CFBundleIdentifier", @"com.example.other");
        NSString *otherZip = [scratch stringByAppendingPathComponent:@"other.zip"];
        OBSAirPlayReleaseAsset *otherAsset = makeZip(other, otherZip, installedVersion);
        sign(otherAsset, releaseKey, scratch);
        expect(!OBSAirPlayStageUpdate([NSURL fileURLWithPath:otherZip], otherAsset,
                                      pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "another bundle identifier is refused");

        // Info.plist now carries LSMinimumSystemVersion; a release that needs
        // a newer macOS is refused.
        NSString *future = [scratch stringByAppendingPathComponent:@"future"];
        NSString *futureBundle = makeBundle(built, future, @"CFBundleShortVersionString", @"9.9.9");
        shell([NSString stringWithFormat:
            @"(/usr/libexec/PlistBuddy -c 'Delete :LSMinimumSystemVersion' %@/Contents/Info.plist 2>/dev/null;"
            @" /usr/libexec/PlistBuddy -c 'Add :LSMinimumSystemVersion string 99.0' %@/Contents/Info.plist)"
            @" && codesign --force --deep --sign - %@ 2>/dev/null",
            quoted(futureBundle), quoted(futureBundle), quoted(futureBundle)]);
        NSString *futureZip = [scratch stringByAppendingPathComponent:@"future.zip"];
        OBSAirPlayReleaseAsset *futureAsset = makeZip(future, futureZip, @"9.9.9");
        sign(futureAsset, releaseKey, scratch);
        expect(!OBSAirPlayStageUpdate([NSURL fileURLWithPath:futureZip], futureAsset, pluginURL,
                                      &error) &&
               ![files fileExistsAtPath:staging], "release needing a newer macOS is refused");

        NSString *extra = [scratch stringByAppendingPathComponent:@"extra"];
        makeBundle(built, extra, @"CFBundleShortVersionString", @"9.9.9");
        shell([NSString stringWithFormat:@"echo x > %@/extra.txt", quoted(extra)]);
        NSString *extraZip = [scratch stringByAppendingPathComponent:@"extra.zip"];
        OBSAirPlayReleaseAsset *extraAsset = makeZip(extra, extraZip, @"9.9.9");
        sign(extraAsset, releaseKey, scratch);
        expect(!OBSAirPlayStageUpdate([NSURL fileURLWithPath:extraZip], extraAsset,
                                      pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "zip with anything beside the plugin is refused");

        expect([bundleVersion(installed) isEqualToString:installedVersion] &&
               !OBSAirPlayStagedUpdateVersion(pluginURL) &&
               !OBSAirPlayInstallStagedUpdate(pluginURL, &error),
               "refused downloads leave the plugin as it was");

        // The good download: staged, then swapped in.
        error = nil;
        NSURL *staged = OBSAirPlayStageUpdate(goodZipURL, asset, pluginURL, &error);
        if (error)
            std::printf("  stage error: %s\n", error.localizedDescription.UTF8String);
        expect(staged && [OBSAirPlayStagedUpdateVersion(pluginURL) isEqualToString:@"9.9.9"] &&
               [bundleVersion(installed) isEqualToString:installedVersion],
               "good zip is staged; plugin not yet changed");

        // A staged update damaged while it waits is dropped, not installed.
        shell([NSString stringWithFormat:
            @"echo changed >> %@/obs-airplay.plugin/Contents/Resources/locale/en-US.ini",
            quoted(staging)]);
        expect(!OBSAirPlayInstallStagedUpdate(pluginURL, &error) &&
               [bundleVersion(installed) isEqualToString:installedVersion] &&
               ![files fileExistsAtPath:staging], "staged update damaged later is dropped");

        // A bundle planted in the staging folder (not staged by the updater)
        // is checked at install time and refused.
        NSString *planted = [scratch stringByAppendingPathComponent:@"planted"];
        NSString *plantedBundle = makeBundle(built, planted, @"CFBundleIdentifier", @"com.example.other");
        shell([NSString stringWithFormat:
            @"/usr/libexec/PlistBuddy -c 'Set :CFBundleShortVersionString 9.9.9' %@/Contents/Info.plist"
            @" && codesign --force --deep --sign - %@ 2>/dev/null"
            @" && mkdir -m 700 %@ && mv %@ %@/",
            quoted(plantedBundle), quoted(plantedBundle), quoted(staging), quoted(plantedBundle),
            quoted(staging)]);
        expect(!OBSAirPlayInstallStagedUpdate(pluginURL, &error) &&
               [bundleVersion(installed) isEqualToString:installedVersion] &&
               ![files fileExistsAtPath:staging],
               "planted bundle with another identifier is refused at install");

        // A staging folder others can write to is not trusted.
        staged = OBSAirPlayStageUpdate(goodZipURL, asset, pluginURL, &error);
        shell([NSString stringWithFormat:@"chmod 777 %@", quoted(staging)]);
        expect(staged && !OBSAirPlayInstallStagedUpdate(pluginURL, &error) &&
               [bundleVersion(installed) isEqualToString:installedVersion] &&
               ![files fileExistsAtPath:staging], "staging folder others can change is refused");

        // A lock left by a process that has gone does not block the update;
        // installers racing for it swap exactly once, and the version they
        // replace is kept whole. Repeated, with a stale lock each time: the
        // race is in taking over that lock.
        NSString *lockPath = [plugins stringByAppendingPathComponent:@".obs-airplay.update.lock"];
        constexpr int kRounds = 10;
        constexpr int kInstallers = 4;
        int good_rounds = 0;
        for (int round = 0; round < kRounds; ++round) {
            if (round > 0) {
                // Back to the built version, as before the first round.
                OBSAirPlayDiscardStagedUpdate(pluginURL);
                [files removeItemAtPath:[plugins stringByAppendingPathComponent:@".obs-airplay.previous"]
                                  error:nil];
                [files removeItemAtPath:installed error:nil];
                shell([NSString stringWithFormat:@"cp -R %@ %@", quoted(built), quoted(installed)]);
            }
            error = nil;
            staged = OBSAirPlayStageUpdate(goodZipURL, asset, pluginURL, &error);
            if (error)
                std::printf("  round %d stage error: %s\n", round, error.localizedDescription.UTF8String);
            [@"999999\n" writeToFile:lockPath atomically:NO encoding:NSUTF8StringEncoding error:nil];
            std::atomic<int> swaps{0};
            std::vector<std::thread> installers;
            for (int i = 0; i < kInstallers; ++i) {
                installers.emplace_back([&] {
                    if (OBSAirPlayInstallStagedUpdate(pluginURL, nil)) ++swaps;
                });
            }
            for (std::thread &installer : installers)
                installer.join();
            const bool swapped = staged && swaps.load() == 1 &&
                [bundleVersion(installed) isEqualToString:@"9.9.9"] &&
                ![files fileExistsAtPath:staging] && ![files fileExistsAtPath:lockPath] &&
                shell([NSString stringWithFormat:
                    @"codesign --verify --deep --strict %@ 2>/dev/null", quoted(installed)]);
            const bool kept_same = shell([NSString stringWithFormat:
                @"diff -rq %@ %@/.obs-airplay.previous/obs-airplay.plugin >/dev/null",
                quoted(built), quoted(plugins)]);
            NSString *kept_version = OBSAirPlayPreviousVersion(pluginURL);
            if (swapped && kept_same && [kept_version isEqualToString:installedVersion]) {
                ++good_rounds;
            } else {
                std::printf("  round %d: staged=%d swaps=%d installed=%s kept=%s same=%d\n", round,
                            staged ? 1 : 0, swaps.load(), bundleVersion(installed).UTF8String,
                            kept_version ? kept_version.UTF8String : "(none)", kept_same ? 1 : 0);
            }
        }
        expect(good_rounds == kRounds,
               "installers racing for a stale lock: one swap, replaced version kept");
        OBSAirPlayForgetPreviousVersion(pluginURL);
        expect(!OBSAirPlayPreviousVersion(pluginURL), "kept version removed after a newer start");

        [files removeItemAtPath:scratch error:nil];
        std::printf("updater-smoke -> %s\n", failures == 0 ? "ok" : "FAILED");
        return failures == 0 ? 0 : 1;
    }
}
