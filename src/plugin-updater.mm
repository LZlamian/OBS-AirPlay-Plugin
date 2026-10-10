#import "plugin-updater.h"

#import <CommonCrypto/CommonDigest.h>

NSString *const OBSAirPlayUpdaterErrorDomain = @"com.lzlamian.obs-airplay.updater";

@implementation OBSAirPlayReleaseAsset
@end

namespace {

NSString *const kPluginBundleName = @"obs-airplay.plugin";
// A hidden folder beside the plugin: same volume, so the swap is a rename,
// and not a "*.plugin" entry, so OBS never tries to load it.
NSString *const kStagingFolderName = @".obs-airplay.update";
NSString *const kIncomingFolderName = @"incoming";
NSString *const kPreviousBundleName = @"previous.plugin";
constexpr unsigned long long kMaxUpdateSize = 200ull * 1024 * 1024;

NSError *updaterError(NSString *description)
{
    return [NSError errorWithDomain:OBSAirPlayUpdaterErrorDomain
                               code:1
                           userInfo:@{NSLocalizedDescriptionKey : description}];
}

bool fail(NSError **error, NSString *description)
{
    if (error)
        *error = updaterError(description);
    return false;
}

NSArray<NSNumber *> *parseVersion(NSString *version)
{
    if (![version isKindOfClass:[NSString class]])
        return nil;

    NSString *normalized = [version stringByTrimmingCharactersInSet:
        [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if ([normalized hasPrefix:@"v"] || [normalized hasPrefix:@"V"])
        normalized = [normalized substringFromIndex:1];

    NSArray<NSString *> *parts = [normalized componentsSeparatedByString:@"."];
    if (parts.count != 3)
        return nil;

    NSMutableArray<NSNumber *> *numbers = [NSMutableArray arrayWithCapacity:3];
    NSCharacterSet *nonDigits = [[NSCharacterSet decimalDigitCharacterSet] invertedSet];
    for (NSString *part in parts) {
        if (part.length == 0 || part.length > 6 ||
            [part rangeOfCharacterFromSet:nonDigits].location != NSNotFound)
            return nil;
        [numbers addObject:@(part.integerValue)];
    }
    return numbers;
}

NSString *plainVersion(NSString *version)
{
    NSArray<NSNumber *> *parts = parseVersion(version);
    return parts ? [parts componentsJoinedByString:@"."] : nil;
}

NSString *sha256OfFile(NSURL *url, unsigned long long *size)
{
    NSFileHandle *file = [NSFileHandle fileHandleForReadingFromURL:url error:nil];
    if (!file)
        return nil;

    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    unsigned long long total = 0;
    for (;;) {
        @autoreleasepool {
            NSData *chunk = [file readDataUpToLength:1 << 20 error:nil];
            if (chunk.length == 0)
                break;
            CC_SHA256_Update(&context, chunk.bytes, (CC_LONG)chunk.length);
            total += chunk.length;
        }
    }
    [file closeFile];

    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    NSMutableString *hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (unsigned char byte : digest)
        [hex appendFormat:@"%02x", byte];
    if (size)
        *size = total;
    return hex;
}

bool runTool(NSString *path, NSArray<NSString *> *arguments)
{
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:path];
    task.arguments = arguments;
    task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![task launchAndReturnError:nil])
        return false;
    [task waitUntilExit];
    return task.terminationStatus == 0;
}

NSURL *stagingFolder(NSURL *pluginURL)
{
    return [[pluginURL URLByDeletingLastPathComponent]
        URLByAppendingPathComponent:kStagingFolderName isDirectory:YES];
}

NSURL *stagedBundle(NSURL *pluginURL)
{
    return [stagingFolder(pluginURL) URLByAppendingPathComponent:kPluginBundleName
                                                     isDirectory:YES];
}

NSDictionary *infoDictionary(NSURL *bundleURL)
{
    NSURL *plist = [bundleURL URLByAppendingPathComponent:@"Contents/Info.plist"];
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfURL:plist];
    return [info isKindOfClass:[NSDictionary class]] ? info : nil;
}

NSString *infoString(NSURL *bundleURL, NSString *key)
{
    id value = infoDictionary(bundleURL)[key];
    return [value isKindOfClass:[NSString class]] ? value : nil;
}

// The staged bundle must be this plugin, at the announced version, able to
// run here, and exactly as it was signed (an ad-hoc seal still shows whether
// any file in the bundle was changed, added or removed after packaging).
bool checkStagedBundle(NSURL *staged, OBSAirPlayReleaseAsset *asset, NSURL *pluginURL,
                       NSError **error)
{
    NSString *identifier = infoString(staged, @"CFBundleIdentifier");
    NSString *installedIdentifier = infoString(pluginURL, @"CFBundleIdentifier");
    if (!identifier || !installedIdentifier || ![identifier isEqualToString:installedIdentifier])
        return fail(error, @"The download is not the OBS AirPlay plugin.");

    NSString *version = plainVersion(infoString(staged, @"CFBundleShortVersionString"));
    if (!version || ![version isEqualToString:asset.version])
        return fail(error, @"The download is not the announced version.");

    NSString *minimumSystem = infoString(staged, @"LSMinimumSystemVersion");
    if (minimumSystem) {
        NSArray<NSString *> *parts = [minimumSystem componentsSeparatedByString:@"."];
        NSOperatingSystemVersion required = {
            parts.count > 0 ? parts[0].integerValue : 0,
            parts.count > 1 ? parts[1].integerValue : 0,
            parts.count > 2 ? parts[2].integerValue : 0,
        };
        if (![[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:required])
            return fail(error, [NSString stringWithFormat:
                @"The update needs macOS %@ or later.", minimumSystem]);
    }

#if defined(__arm64__)
    NSNumber *architecture = @(NSBundleExecutableArchitectureARM64);
#else
    NSNumber *architecture = @(NSBundleExecutableArchitectureX86_64);
#endif
    if (![[NSBundle bundleWithURL:staged].executableArchitectures containsObject:architecture])
        return fail(error, @"The update is not built for this Mac.");

    if (!runTool(@"/usr/bin/codesign", @[@"--verify", @"--deep", @"--strict", staged.path]))
        return fail(error, @"The update is damaged.");
    return true;
}

} // namespace

NSString *OBSAirPlayUpdaterArchitecture(void)
{
#if defined(__arm64__)
    return @"arm64";
#else
    return @"x86_64";
#endif
}

BOOL OBSAirPlayIsNewerVersion(NSString *candidate, NSString *current)
{
    NSArray<NSNumber *> *candidateParts = parseVersion(candidate);
    NSArray<NSNumber *> *currentParts = parseVersion(current);
    if (!candidateParts || !currentParts)
        return NO;

    for (NSUInteger index = 0; index < 3; ++index) {
        const NSInteger candidatePart = candidateParts[index].integerValue;
        const NSInteger currentPart = currentParts[index].integerValue;
        if (candidatePart != currentPart)
            return candidatePart > currentPart;
    }
    return NO;
}

OBSAirPlayReleaseAsset *OBSAirPlayInstallableAsset(NSDictionary *release, NSString *architecture)
{
    if (![release isKindOfClass:[NSDictionary class]])
        return nil;
    NSString *version = plainVersion(release[@"tag_name"]);
    NSArray *assets = release[@"assets"];
    if (!version || ![assets isKindOfClass:[NSArray class]])
        return nil;

    NSString *expectedName = [NSString stringWithFormat:
        @"obs-airplay-v%@-macos-%@.zip", version, architecture];
    for (NSDictionary *entry in assets) {
        if (![entry isKindOfClass:[NSDictionary class]] ||
            ![entry[@"name"] isKindOfClass:[NSString class]] ||
            ![entry[@"name"] isEqualToString:expectedName])
            continue;

        NSString *address = entry[@"browser_download_url"];
        NSString *digest = entry[@"digest"];
        NSNumber *size = entry[@"size"];
        if (![address isKindOfClass:[NSString class]] ||
            ![digest isKindOfClass:[NSString class]] ||
            ![size isKindOfClass:[NSNumber class]])
            return nil;

        NSURL *url = [NSURL URLWithString:address];
        if (![url.scheme isEqualToString:@"https"] || ![url.host isEqualToString:@"github.com"])
            return nil;

        NSString *prefix = @"sha256:";
        if (![digest hasPrefix:prefix] || digest.length != prefix.length + 64)
            return nil;
        if (size.unsignedLongLongValue == 0 || size.unsignedLongLongValue > kMaxUpdateSize)
            return nil;

        OBSAirPlayReleaseAsset *asset = [[OBSAirPlayReleaseAsset alloc] init];
        asset.version = version;
        asset.downloadURL = url;
        asset.sha256 = [digest substringFromIndex:prefix.length].lowercaseString;
        asset.size = size.unsignedLongLongValue;
        return asset;
    }
    return nil;
}

NSURL *OBSAirPlayPluginURLForHelper(NSURL *helperBundleURL)
{
    // …/obs-airplay.plugin/Contents/Resources/OBS AirPlay Discovery.app
    NSURL *plugin = [[[helperBundleURL URLByDeletingLastPathComponent]
        URLByDeletingLastPathComponent] URLByDeletingLastPathComponent];
    if (![plugin.lastPathComponent isEqualToString:kPluginBundleName] ||
        !infoString(plugin, @"CFBundleIdentifier"))
        return nil;
    return plugin;
}

BOOL OBSAirPlayCanReplacePlugin(NSURL *pluginURL)
{
    NSFileManager *files = [NSFileManager defaultManager];
    return [files isWritableFileAtPath:pluginURL.path] &&
           [files isWritableFileAtPath:pluginURL.URLByDeletingLastPathComponent.path];
}

NSURL *OBSAirPlayStageUpdate(NSURL *zipURL, OBSAirPlayReleaseAsset *asset, NSURL *pluginURL,
                             NSError **error)
{
    unsigned long long size = 0;
    NSString *digest = sha256OfFile(zipURL, &size);
    if (!digest || size != asset.size || ![digest isEqualToString:asset.sha256]) {
        fail(error, @"The download does not match the published release.");
        return nil;
    }

    NSFileManager *files = [NSFileManager defaultManager];
    NSURL *folder = stagingFolder(pluginURL);
    [files removeItemAtURL:folder error:nil];
    NSError *folderError = nil;
    if (![files createDirectoryAtURL:folder withIntermediateDirectories:NO attributes:nil
                               error:&folderError]) {
        if (error)
            *error = folderError;
        return nil;
    }

    // Unpacked and checked under another name first, so a bundle at the
    // staged path is always complete and checked.
    NSURL *incoming = [folder URLByAppendingPathComponent:kIncomingFolderName isDirectory:YES];
    NSURL *unpacked = [incoming URLByAppendingPathComponent:kPluginBundleName isDirectory:YES];
    NSURL *staged = stagedBundle(pluginURL);
    bool ok = [files createDirectoryAtURL:incoming withIntermediateDirectories:NO
                               attributes:nil error:nil] &&
              runTool(@"/usr/bin/ditto", @[@"-x", @"-k", zipURL.path, incoming.path]);
    if (!ok) {
        fail(error, @"The update could not be unpacked.");
    } else {
        // The zip holds the plugin bundle and nothing else.
        NSArray<NSString *> *contents = [files contentsOfDirectoryAtPath:incoming.path error:nil];
        NSDictionary *attributes = [files attributesOfItemAtPath:unpacked.path error:nil];
        ok = contents.count == 1 && [contents.firstObject isEqualToString:kPluginBundleName] &&
             [attributes.fileType isEqualToString:NSFileTypeDirectory];
        if (!ok)
            fail(error, @"The download is not the OBS AirPlay plugin.");
        else
            ok = checkStagedBundle(unpacked, asset, pluginURL, error);
    }
    if (ok && ![files moveItemAtURL:unpacked toURL:staged error:nil])
        ok = fail(error, @"The update could not be prepared.");

    if (!ok) {
        [files removeItemAtURL:folder error:nil];
        return nil;
    }
    [files removeItemAtURL:incoming error:nil];
    return staged;
}

void OBSAirPlayDownloadAndStageUpdate(OBSAirPlayReleaseAsset *asset, NSURL *pluginURL,
                                      NSString *userAgent, void (^completion)(NSError *error))
{
    NSMutableURLRequest *request = [NSMutableURLRequest
        requestWithURL:asset.downloadURL
           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
       timeoutInterval:30.0];
    [request setValue:userAgent forHTTPHeaderField:@"User-Agent"];

    NSURLSessionDownloadTask *task = [[NSURLSession sharedSession]
        downloadTaskWithRequest:request
              completionHandler:^(NSURL *location, NSURLResponse *response, NSError *error) {
        NSError *failure = error;
        NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)response;
        if (!failure && (![httpResponse isKindOfClass:[NSHTTPURLResponse class]] ||
                         httpResponse.statusCode != 200 || !location))
            failure = updaterError(@"The download failed.");
        // The downloaded file exists only until this handler returns.
        if (!failure)
            OBSAirPlayStageUpdate(location, asset, pluginURL, &failure);
        completion(failure);
    }];
    [task resume];
}

NSString *OBSAirPlayStagedUpdateVersion(NSURL *pluginURL)
{
    return plainVersion(infoString(stagedBundle(pluginURL), @"CFBundleShortVersionString"));
}

void OBSAirPlayDiscardStagedUpdate(NSURL *pluginURL)
{
    [[NSFileManager defaultManager] removeItemAtURL:stagingFolder(pluginURL) error:nil];
}

BOOL OBSAirPlayInstallStagedUpdate(NSURL *pluginURL, NSError **error)
{
    NSFileManager *files = [NSFileManager defaultManager];
    NSURL *staged = stagedBundle(pluginURL);
    if (!OBSAirPlayStagedUpdateVersion(pluginURL))
        return fail(error, @"No update is waiting to be installed.");

    // The seal is checked again: the update may have waited here for days.
    if (!runTool(@"/usr/bin/codesign", @[@"--verify", @"--deep", @"--strict", staged.path])) {
        OBSAirPlayDiscardStagedUpdate(pluginURL);
        return fail(error, @"The update is damaged.");
    }

    NSURL *previous = [stagingFolder(pluginURL) URLByAppendingPathComponent:kPreviousBundleName
                                                                isDirectory:YES];
    [files removeItemAtURL:previous error:nil];
    NSError *moveError = nil;
    if (![files moveItemAtURL:pluginURL toURL:previous error:&moveError]) {
        if (error)
            *error = moveError;
        return NO;
    }
    if (![files moveItemAtURL:staged toURL:pluginURL error:&moveError]) {
        [files moveItemAtURL:previous toURL:pluginURL error:nil];
        if (error)
            *error = moveError;
        return NO;
    }
    OBSAirPlayDiscardStagedUpdate(pluginURL);
    return YES;
}
