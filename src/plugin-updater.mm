// SPDX-License-Identifier: GPL-3.0-or-later
#import "plugin-updater.h"

#import <CommonCrypto/CommonDigest.h>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// update-signature.swift (CryptoKit)
extern "C" int obs_airplay_ed25519_verify(const uint8_t *public_key, const uint8_t *message,
                                          long message_length, const uint8_t *signature);

NSString *const OBSAirPlayUpdaterErrorDomain = @"com.lzlamian.obs-airplay.updater";

@implementation OBSAirPlayReleaseAsset
@end

// A one-request session that cancels a response growing past `limit` bytes:
// in memory (data), or to a file (download, handed to `fileHandler` before
// NSURLSession deletes it).
@interface OBSAirPlayLimitedFetch : NSObject <NSURLSessionDataDelegate, NSURLSessionDownloadDelegate>
@property (nonatomic) unsigned long long limit;
@property (nonatomic, strong) NSMutableData *data;
@property (nonatomic, copy) void (^dataCompletion)(NSData *, NSHTTPURLResponse *, NSError *);
@property (nonatomic, copy) NSError * (^fileHandler)(NSURL *location, NSHTTPURLResponse *response);
@property (nonatomic, copy) void (^fileCompletion)(NSError *);
@property (nonatomic, strong) NSError *fileError;
@property (nonatomic) BOOL tooLarge;
@end

@implementation OBSAirPlayLimitedFetch

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveData:(NSData *)data
{
    (void)session;
    if (self.data.length + data.length > self.limit) {
        self.tooLarge = YES;
        [dataTask cancel];
        return;
    }
    [self.data appendData:data];
}

- (void)URLSession:(NSURLSession *)session
                 downloadTask:(NSURLSessionDownloadTask *)downloadTask
                 didWriteData:(int64_t)bytesWritten
            totalBytesWritten:(int64_t)totalBytesWritten
    totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite
{
    (void)session;
    (void)bytesWritten;
    (void)totalBytesExpectedToWrite;
    if ((unsigned long long)totalBytesWritten > self.limit) {
        self.tooLarge = YES;
        [downloadTask cancel];
    }
}

- (void)URLSession:(NSURLSession *)session
                 downloadTask:(NSURLSessionDownloadTask *)downloadTask
    didFinishDownloadingToURL:(NSURL *)location
{
    (void)session;
    // The file exists only until this returns.
    self.fileError = self.fileHandler(location, (NSHTTPURLResponse *)downloadTask.response);
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error
{
    NSError *failure = self.tooLarge
        ? [NSError errorWithDomain:OBSAirPlayUpdaterErrorDomain
                              code:1
                          userInfo:@{NSLocalizedDescriptionKey : @"The response is larger than expected."}]
        : error;
    NSHTTPURLResponse *response = (NSHTTPURLResponse *)task.response;
    if (![response isKindOfClass:[NSHTTPURLResponse class]])
        response = nil;
    if (self.dataCompletion)
        self.dataCompletion(failure ? nil : self.data, response, failure);
    if (self.fileCompletion)
        self.fileCompletion(failure ?: self.fileError);
    self.dataCompletion = nil;
    self.fileCompletion = nil;
    self.fileHandler = nil;
    [session finishTasksAndInvalidate];
}

@end

void OBSAirPlayFetchLimited(NSURLRequest *request, NSUInteger limit,
                            void (^completion)(NSData *, NSHTTPURLResponse *, NSError *))
{
    OBSAirPlayLimitedFetch *fetch = [[OBSAirPlayLimitedFetch alloc] init];
    fetch.limit = limit;
    fetch.data = [NSMutableData data];
    fetch.dataCompletion = completion;
    NSURLSession *session =
        [NSURLSession sessionWithConfiguration:[NSURLSessionConfiguration defaultSessionConfiguration]
                                      delegate:fetch
                                 delegateQueue:nil];
    [[session dataTaskWithRequest:request] resume];
}

namespace {

NSString *const kPluginBundleName = @"obs-airplay.plugin";
// A hidden folder beside the plugin: same volume, so the swap is a rename,
// and not a "*.plugin" entry, so OBS never tries to load it.
NSString *const kStagingFolderName = @".obs-airplay.update";
NSString *const kIncomingFolderName = @"incoming";
// The version an update replaced, kept until a newer plugin has started.
NSString *const kPreviousFolderName = @".obs-airplay.previous";
NSString *const kLockName = @".obs-airplay.update.lock";
NSString *const kDownloadPathPrefix = @"/LZlamian/OBS-AirPlay-Plugin/releases/download/";
constexpr unsigned long long kMaxUpdateSize = 200ull * 1024 * 1024;
constexpr unsigned long long kMaxSignatureSize = 4096;

// Ed25519 public keys whose signatures make a release installable. The
// private key is held by the maintainer outside the repository
// (scripts/sign-release.sh). To rotate, ship a release signed by the old
// key that lists both, then drop the old one a release later.
const char *const kTrustedUpdateKeys[] = {
    "ca4d3512515ca43033f9994d36fe58c0dac5da39ede4f645a375f740310372af",
};
NSArray<NSData *> *gTrustedKeysForTesting = nil;
NSString *gUpdateRootForTesting = nil;

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

NSURL *previousBundle(NSURL *pluginURL)
{
    return [[[pluginURL URLByDeletingLastPathComponent]
        URLByAppendingPathComponent:kPreviousFolderName isDirectory:YES]
        URLByAppendingPathComponent:kPluginBundleName isDirectory:YES];
}

NSData *dataFromHex(NSString *hex)
{
    if (hex.length % 2)
        return nil;
    NSMutableData *data = [NSMutableData dataWithCapacity:hex.length / 2];
    for (NSUInteger i = 0; i < hex.length; i += 2) {
        unsigned int byte = 0;
        NSScanner *scanner = [NSScanner scannerWithString:[hex substringWithRange:NSMakeRange(i, 2)]];
        if (![scanner scanHexInt:&byte])
            return nil;
        const unsigned char value = (unsigned char)byte;
        [data appendBytes:&value length:1];
    }
    return data;
}

NSArray<NSData *> *trustedKeys()
{
    if (gTrustedKeysForTesting)
        return gTrustedKeysForTesting;
    NSMutableArray<NSData *> *keys = [NSMutableArray array];
    for (const char *hex : kTrustedUpdateKeys) {
        NSData *key = dataFromHex(@(hex));
        if (key.length == 32)
            [keys addObject:key];
    }
    return keys;
}

bool isProjectDownloadURL(NSURL *url)
{
    return [url.scheme isEqualToString:@"https"] && [url.host isEqualToString:@"github.com"] &&
           [url.path hasPrefix:kDownloadPathPrefix] && !url.query && !url.user;
}

// A folder only this user can read or change.
bool isPrivateFolder(NSURL *folder)
{
    struct stat info = {};
    return lstat(folder.fileSystemRepresentation, &info) == 0 && S_ISDIR(info.st_mode) &&
           info.st_uid == getuid() && (info.st_mode & 077) == 0;
}

bool ownedByUser(NSURL *url)
{
    struct stat info = {};
    return lstat(url.fileSystemRepresentation, &info) == 0 && info.st_uid == getuid();
}

// O_EXCL lock file beside the plugin, held while staging or swapping, so two
// helpers (two OBS instances, or a crash and relaunch) never interleave. A
// lock whose process has gone is taken over.
class UpdateLock {
public:
    // An flock() on the lock file: the kernel drops it when the holder
    // exits, so a lock left by a crashed process never blocks, and nothing
    // has to guess from a stored PID whether its owner is alive. The holder
    // removes the file before letting go; a waiter that then locks the
    // removed file finds it is no longer the one at the path and starts over.
    explicit UpdateLock(NSURL *pluginURL)
        : m_path([[pluginURL URLByDeletingLastPathComponent]
              URLByAppendingPathComponent:kLockName].path)
    {
        const char *path = m_path.fileSystemRepresentation;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const int fd = open(path, O_CREAT | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0)
                return;
            if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
                close(fd);
                return; // another installer holds it
            }
            struct stat opened = {};
            struct stat current = {};
            if (fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
                opened.st_uid == getuid() && lstat(path, &current) == 0 &&
                opened.st_dev == current.st_dev && opened.st_ino == current.st_ino) {
                m_fd = fd;
                // For people looking at the folder; not used for locking.
                const char *pid = [NSString stringWithFormat:@"%d\n", getpid()].UTF8String;
                if (ftruncate(fd, 0) == 0)
                    (void)write(fd, pid, strlen(pid));
                return;
            }
            close(fd);
            if (S_ISREG(opened.st_mode) && opened.st_uid != getuid())
                return; // someone else's file: never ours to take
        }
    }
    ~UpdateLock()
    {
        if (m_fd >= 0) {
            unlink(m_path.fileSystemRepresentation);
            close(m_fd);
        }
    }
    UpdateLock(const UpdateLock &) = delete;
    UpdateLock &operator=(const UpdateLock &) = delete;
    bool held() const { return m_fd >= 0; }

private:
    NSString *m_path;
    int m_fd = -1;
};

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

// The staged bundle must be this plugin, at the expected version, able to
// run here, and exactly as it was signed (an ad-hoc seal still shows whether
// any file in the bundle was changed, added or removed after packaging).
bool checkStagedBundle(NSURL *staged, NSString *expectedVersion, NSURL *pluginURL,
                       NSError **error)
{
    NSString *identifier = infoString(staged, @"CFBundleIdentifier");
    NSString *installedIdentifier = infoString(pluginURL, @"CFBundleIdentifier");
    if (!identifier || !installedIdentifier || ![identifier isEqualToString:installedIdentifier])
        return fail(error, @"The download is not the OBS AirPlay plugin.");

    NSString *version = plainVersion(infoString(staged, @"CFBundleShortVersionString"));
    if (!version || ![version isEqualToString:expectedVersion])
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

NSString *OBSAirPlayUpdateSignedMessage(OBSAirPlayReleaseAsset *asset)
{
    return [NSString stringWithFormat:
        @"obs-airplay-update-v1\nname=%@\nversion=%@\nsize=%llu\nsha256=%@\n",
        asset.name, asset.version, asset.size, asset.sha256];
}

// The .sig asset is two lines:
//   obs-airplay-update-signature-v1
//   sig: <base64 Ed25519 signature of OBSAirPlayUpdateSignedMessage>
BOOL OBSAirPlayVerifyUpdateSignature(OBSAirPlayReleaseAsset *asset)
{
    if (!asset.signature || asset.signature.length > kMaxSignatureSize || !asset.name ||
        !asset.version || !asset.sha256)
        return NO;
    NSString *text = [[NSString alloc] initWithData:asset.signature encoding:NSUTF8StringEncoding];
    NSArray<NSString *> *lines = [text componentsSeparatedByCharactersInSet:
        [NSCharacterSet newlineCharacterSet]];
    if (lines.count < 2 || ![lines[0] isEqualToString:@"obs-airplay-update-signature-v1"] ||
        ![lines[1] hasPrefix:@"sig: "])
        return NO;
    NSData *signature = [[NSData alloc] initWithBase64EncodedString:[lines[1] substringFromIndex:5]
                                                            options:0];
    if (signature.length != 64)
        return NO;
    NSData *message = [OBSAirPlayUpdateSignedMessage(asset) dataUsingEncoding:NSUTF8StringEncoding];
    for (NSData *key in trustedKeys()) {
        if (obs_airplay_ed25519_verify(static_cast<const uint8_t *>(key.bytes),
                                       static_cast<const uint8_t *>(message.bytes),
                                       (long)message.length,
                                       static_cast<const uint8_t *>(signature.bytes)))
            return YES;
    }
    return NO;
}

void OBSAirPlaySetTrustedUpdateKeysForTesting(NSArray<NSData *> *keys)
{
    gTrustedKeysForTesting = [keys copy];
}

void OBSAirPlaySetUpdateRootForTesting(NSString *root)
{
    gUpdateRootForTesting = [root copy];
}

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
    NSString *signatureName = [expectedName stringByAppendingString:@".sig"];
    NSURL *signatureURL = nil;
    for (NSDictionary *entry in assets) {
        if (![entry isKindOfClass:[NSDictionary class]] ||
            ![entry[@"name"] isKindOfClass:[NSString class]] ||
            ![entry[@"name"] isEqualToString:signatureName] ||
            ![entry[@"browser_download_url"] isKindOfClass:[NSString class]] ||
            ![entry[@"size"] isKindOfClass:[NSNumber class]])
            continue;
        NSURL *url = [NSURL URLWithString:entry[@"browser_download_url"]];
        if (isProjectDownloadURL(url) && [entry[@"size"] unsignedLongLongValue] > 0 &&
            [entry[@"size"] unsignedLongLongValue] <= kMaxSignatureSize)
            signatureURL = url;
    }
    // Releases from 2.6.0 on are signed; an unsigned one is not installed.
    if (!signatureURL)
        return nil;
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
        if (!isProjectDownloadURL(url))
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
        asset.name = expectedName;
        asset.signatureURL = signatureURL;
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
    // Only a plugin in this user's own folder: one in a shared location such
    // as /Library may be writable by others, who could plant the staged
    // update another user's helper installs.
    NSString *root = gUpdateRootForTesting ?: NSHomeDirectory();
    NSString *resolvedRoot = [root stringByResolvingSymlinksInPath];
    NSString *resolvedPlugin = pluginURL.URLByResolvingSymlinksInPath.path;
    if (![resolvedPlugin hasPrefix:[resolvedRoot stringByAppendingString:@"/"]])
        return NO;
    NSURL *plugins = pluginURL.URLByDeletingLastPathComponent;
    NSFileManager *files = [NSFileManager defaultManager];
    return ownedByUser(pluginURL) && ownedByUser(plugins) &&
           [files isWritableFileAtPath:pluginURL.path] &&
           [files isWritableFileAtPath:plugins.path];
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
    if (!OBSAirPlayVerifyUpdateSignature(asset)) {
        fail(error, @"The download is not signed by the OBS AirPlay project.");
        return nil;
    }

    UpdateLock lock(pluginURL);
    if (!lock.held()) {
        fail(error, @"Another update is being prepared or installed.");
        return nil;
    }
    NSFileManager *files = [NSFileManager defaultManager];
    NSURL *folder = stagingFolder(pluginURL);
    [files removeItemAtURL:folder error:nil];
    NSError *folderError = nil;
    if (![files createDirectoryAtURL:folder withIntermediateDirectories:NO
                          attributes:@{NSFilePosixPermissions : @0700}
                               error:&folderError]) {
        if (error)
            *error = folderError;
        return nil;
    }
    if (!isPrivateFolder(folder)) {
        [files removeItemAtURL:folder error:nil];
        fail(error, @"The update folder could not be made private.");
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
            ok = checkStagedBundle(unpacked, asset.version, pluginURL, error);
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

namespace {

void downloadAndStage(OBSAirPlayReleaseAsset *asset, NSURL *pluginURL, NSString *userAgent,
                      void (^completion)(NSError *error))
{
    NSMutableURLRequest *request = [NSMutableURLRequest
        requestWithURL:asset.downloadURL
           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
       timeoutInterval:30.0];
    [request setValue:userAgent forHTTPHeaderField:@"User-Agent"];

    // Cancelled as soon as it is larger than the size the release lists.
    OBSAirPlayLimitedFetch *fetch = [[OBSAirPlayLimitedFetch alloc] init];
    fetch.limit = asset.size;
    fetch.fileHandler = ^NSError *(NSURL *location, NSHTTPURLResponse *response) {
        if (![response isKindOfClass:[NSHTTPURLResponse class]] || response.statusCode != 200 ||
            !location)
            return updaterError(@"The download failed.");
        NSError *failure = nil;
        OBSAirPlayStageUpdate(location, asset, pluginURL, &failure);
        return failure;
    };
    fetch.fileCompletion = ^(NSError *error) {
        completion(error ?: nil);
    };
    NSURLSession *session =
        [NSURLSession sessionWithConfiguration:[NSURLSessionConfiguration defaultSessionConfiguration]
                                      delegate:fetch
                                 delegateQueue:nil];
    [[session downloadTaskWithRequest:request] resume];
}

} // namespace

void OBSAirPlayDownloadAndStageUpdate(OBSAirPlayReleaseAsset *asset, NSURL *pluginURL,
                                      NSString *userAgent, void (^completion)(NSError *error))
{
    if (!asset.signatureURL) {
        completion(updaterError(@"The release is not signed."));
        return;
    }
    // The signature first: it is small, and without it the zip is useless.
    NSMutableURLRequest *request = [NSMutableURLRequest
        requestWithURL:asset.signatureURL
           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
       timeoutInterval:30.0];
    [request setValue:userAgent forHTTPHeaderField:@"User-Agent"];
    OBSAirPlayFetchLimited(request, kMaxSignatureSize,
                           ^(NSData *data, NSHTTPURLResponse *httpResponse, NSError *error) {
        if (error || httpResponse.statusCode != 200 || !data) {
            completion(error ?: updaterError(@"The release signature could not be downloaded."));
            return;
        }
        asset.signature = data;
        if (!OBSAirPlayVerifyUpdateSignature(asset)) {
            completion(updaterError(@"The release is not signed by the OBS AirPlay project."));
            return;
        }
        downloadAndStage(asset, pluginURL, userAgent, completion);
    });
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
    UpdateLock lock(pluginURL);
    if (!lock.held())
        return fail(error, @"Another update is being installed.");

    NSFileManager *files = [NSFileManager defaultManager];
    NSURL *staged = stagedBundle(pluginURL);
    NSString *stagedVersion = OBSAirPlayStagedUpdateVersion(pluginURL);
    if (!stagedVersion)
        return fail(error, @"No update is waiting to be installed.");

    // Everything is checked again: the update may have waited here for days,
    // and only a folder this user alone controls is trusted.
    NSString *installedVersion = infoString(pluginURL, @"CFBundleShortVersionString");
    if (!isPrivateFolder(stagingFolder(pluginURL)) || !OBSAirPlayCanReplacePlugin(pluginURL) ||
        !OBSAirPlayIsNewerVersion(stagedVersion, installedVersion) ||
        !checkStagedBundle(staged, stagedVersion, pluginURL, error)) {
        OBSAirPlayDiscardStagedUpdate(pluginURL);
        if (error && !*error)
            *error = updaterError(@"The waiting update cannot be installed.");
        return NO;
    }

    // The replaced version stays beside the plugin until a newer one has
    // started (OBSAirPlayForgetPreviousVersion).
    NSURL *previous = previousBundle(pluginURL);
    NSURL *previousFolder = previous.URLByDeletingLastPathComponent;
    [files removeItemAtURL:previousFolder error:nil];
    NSError *moveError = nil;
    if (![files createDirectoryAtURL:previousFolder withIntermediateDirectories:NO
                          attributes:@{NSFilePosixPermissions : @0700}
                               error:&moveError] ||
        ![files moveItemAtURL:pluginURL toURL:previous error:&moveError]) {
        [files removeItemAtURL:previousFolder error:nil];
        if (error)
            *error = moveError;
        return NO;
    }
    if (![files moveItemAtURL:staged toURL:pluginURL error:&moveError]) {
        [files moveItemAtURL:previous toURL:pluginURL error:nil];
        [files removeItemAtURL:previousFolder error:nil];
        if (error)
            *error = moveError;
        return NO;
    }
    OBSAirPlayDiscardStagedUpdate(pluginURL);
    return YES;
}

NSString *OBSAirPlayPreviousVersion(NSURL *pluginURL)
{
    return plainVersion(infoString(previousBundle(pluginURL), @"CFBundleShortVersionString"));
}

void OBSAirPlayForgetPreviousVersion(NSURL *pluginURL)
{
    NSString *previous = OBSAirPlayPreviousVersion(pluginURL);
    NSString *current = infoString(pluginURL, @"CFBundleShortVersionString");
    // Only once the plugin that replaced it is the one running.
    if (previous && OBSAirPlayIsNewerVersion(current, previous))
        [[NSFileManager defaultManager]
            removeItemAtURL:previousBundle(pluginURL).URLByDeletingLastPathComponent
                      error:nil];
}
