// Update download checks, staging and the swap, on scratch copies of a built
// plugin bundle. Nothing outside SCRATCH_DIR is touched and no network is used.
//   obs-airplay-updater-smoke BUILT_PLUGIN_BUNDLE SCRATCH_DIR
// With --live first, the latest GitHub release is downloaded and installed
// over a scratch copy instead.

#import "plugin-updater.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int failures = 0;

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

// Zips everything in `folder`; the asset describes the zip as GitHub would.
OBSAirPlayReleaseAsset *makeZip(NSString *folder, NSString *zip, NSString *version)
{
    shell([NSString stringWithFormat:@"cd %@ && /usr/bin/zip -q -r -y %@ .",
           quoted(folder), quoted(zip)]);
    OBSAirPlayReleaseAsset *asset = [[OBSAirPlayReleaseAsset alloc] init];
    asset.version = version;
    asset.downloadURL = [NSURL URLWithString:@"https://github.com/example"];
    asset.sha256 = output([NSString stringWithFormat:
        @"shasum -a 256 %@ | cut -d' ' -f1", quoted(zip)]);
    asset.size = [[[NSFileManager defaultManager] attributesOfItemAtPath:zip error:nil] fileSize];
    return asset;
}

NSDictionary *releaseJSON(NSString *name, NSString *address, id digest)
{
    NSMutableDictionary *asset = [@{
        @"name" : name, @"browser_download_url" : address, @"size" : @1234567,
    } mutableCopy];
    if (digest)
        asset[@"digest"] = digest;
    return @{@"tag_name" : @"v9.9.9", @"assets" : @[asset]};
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
        if (argc == 4 && std::strcmp(argv[1], "--live") == 0) {
            return live(@(argv[2]),
                        [@(argv[3]) stringByAppendingPathComponent:@"updater-smoke-live"]);
        }
        if (argc != 3) {
            std::fprintf(stderr, "usage: %s [--live] BUILT_PLUGIN_BUNDLE SCRATCH_DIR\n", argv[0]);
            return 64;
        }
        NSString *built = @(argv[1]);
        NSString *scratch = [@(argv[2]) stringByAppendingPathComponent:@"updater-smoke"];
        NSFileManager *files = [NSFileManager defaultManager];
        [files removeItemAtPath:scratch error:nil];

        // Versions and release parsing.
        expect(OBSAirPlayIsNewerVersion(@"v2.4.0", @"2.3.9") &&
               OBSAirPlayIsNewerVersion(@"2.10.0", @"2.9.9") &&
               !OBSAirPlayIsNewerVersion(@"2.3.0", @"2.3.0") &&
               !OBSAirPlayIsNewerVersion(@"2.4", @"2.3.0") &&
               !OBSAirPlayIsNewerVersion(nil, @"2.3.0"), "version comparison");

        NSString *digest = [@"sha256:" stringByPaddingToLength:71 withString:@"ab" startingAtIndex:0];
        NSString *name = @"obs-airplay-v9.9.9-macos-arm64.zip";
        NSString *address = @"https://github.com/LZlamian/OBS-AirPlay-Plugin/releases/download/v9.9.9/x.zip";
        OBSAirPlayReleaseAsset *parsed =
            OBSAirPlayInstallableAsset(releaseJSON(name, address, digest), @"arm64");
        expect(parsed && [parsed.version isEqualToString:@"9.9.9"] && parsed.size == 1234567 &&
               parsed.sha256.length == 64, "release with zip and digest is installable");
        expect(!OBSAirPlayInstallableAsset(releaseJSON(name, address, nil), @"arm64"),
               "release without a digest is not");
        expect(!OBSAirPlayInstallableAsset(releaseJSON(name, address, digest), @"x86_64"),
               "zip for another architecture is not");
        expect(!OBSAirPlayInstallableAsset(
                   releaseJSON(name, @"https://example.com/x.zip", digest), @"arm64") &&
               !OBSAirPlayInstallableAsset(
                   releaseJSON(name, @"http://github.com/x.zip", digest), @"arm64"),
               "download outside https://github.com is not");

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

        NSString *good = [scratch stringByAppendingPathComponent:@"good"];
        makeBundle(built, good, @"CFBundleShortVersionString", @"9.9.9");
        NSString *goodZip = [scratch stringByAppendingPathComponent:@"good.zip"];
        OBSAirPlayReleaseAsset *asset = makeZip(good, goodZip, @"9.9.9");
        NSURL *goodZipURL = [NSURL fileURLWithPath:goodZip];
        NSError *error = nil;

        // Downloads that must be refused. Each leaves the plugin untouched.
        OBSAirPlayReleaseAsset *wrongDigest = makeZip(good, goodZip, @"9.9.9");
        wrongDigest.sha256 = [@"" stringByPaddingToLength:64 withString:@"0" startingAtIndex:0];
        expect(!OBSAirPlayStageUpdate(goodZipURL, wrongDigest, pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "zip with another digest is refused");

        OBSAirPlayReleaseAsset *wrongVersion = makeZip(good, goodZip, @"9.9.8");
        expect(!OBSAirPlayStageUpdate(goodZipURL, wrongVersion, pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "bundle of another version is refused");

        NSString *tampered = [scratch stringByAppendingPathComponent:@"tampered"];
        NSString *tamperedBundle = makeBundle(built, tampered, @"CFBundleShortVersionString", @"9.9.9");
        shell([NSString stringWithFormat:@"echo changed >> %@/Contents/Resources/locale/en-US.ini",
               quoted(tamperedBundle)]);
        NSString *tamperedZip = [scratch stringByAppendingPathComponent:@"tampered.zip"];
        OBSAirPlayReleaseAsset *tamperedAsset = makeZip(tampered, tamperedZip, @"9.9.9");
        expect(!OBSAirPlayStageUpdate([NSURL fileURLWithPath:tamperedZip], tamperedAsset,
                                      pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "bundle changed after signing is refused");

        NSString *other = [scratch stringByAppendingPathComponent:@"other"];
        makeBundle(built, other, @"CFBundleIdentifier", @"com.example.other");
        NSString *otherZip = [scratch stringByAppendingPathComponent:@"other.zip"];
        OBSAirPlayReleaseAsset *otherAsset = makeZip(other, otherZip, installedVersion);
        expect(!OBSAirPlayStageUpdate([NSURL fileURLWithPath:otherZip], otherAsset,
                                      pluginURL, &error) &&
               ![files fileExistsAtPath:staging], "another bundle identifier is refused");

        NSString *extra = [scratch stringByAppendingPathComponent:@"extra"];
        makeBundle(built, extra, @"CFBundleShortVersionString", @"9.9.9");
        shell([NSString stringWithFormat:@"echo x > %@/extra.txt", quoted(extra)]);
        NSString *extraZip = [scratch stringByAppendingPathComponent:@"extra.zip"];
        OBSAirPlayReleaseAsset *extraAsset = makeZip(extra, extraZip, @"9.9.9");
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

        staged = OBSAirPlayStageUpdate(goodZipURL, asset, pluginURL, &error);
        const bool installedOK = staged && OBSAirPlayInstallStagedUpdate(pluginURL, &error);
        expect(installedOK && [bundleVersion(installed) isEqualToString:@"9.9.9"] &&
               ![files fileExistsAtPath:staging] &&
               shell([NSString stringWithFormat:
                   @"codesign --verify --deep --strict %@ 2>/dev/null", quoted(installed)]),
               "staged update replaces the plugin");

        [files removeItemAtPath:scratch error:nil];
        std::printf("updater-smoke -> %s\n", failures == 0 ? "ok" : "FAILED");
        return failures == 0 ? 0 : 1;
    }
}
