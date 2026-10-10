#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#import "plugin-updater.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <unistd.h>

namespace {

NSString *const kLastUpdateCheckKey = @"OBS-AirPlay-LastUpdateCheck";
NSString *const kSkippedVersionKey = @"OBS-AirPlay-SkippedVersion";
NSString *const kLatestReleaseAPI =
    @"https://api.github.com/repos/LZlamian/OBS-AirPlay-Plugin/releases/latest";
NSString *const kLatestReleasePage =
    @"https://github.com/LZlamian/OBS-AirPlay-Plugin/releases/latest";
constexpr NSTimeInterval kUpdateCheckInterval = 24.0 * 60.0 * 60.0;

bool parentIsAlive(pid_t parentPID)
{
    return kill(parentPID, 0) == 0 || errno != ESRCH;
}

// The plugin this helper belongs to, when it can be updated in place.
NSURL *gPluginURL = nil;

void showUpdateResult(NSString *message, NSString *detail, bool offerReleasePage)
{
    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleInformational;
    alert.messageText = message;
    alert.informativeText = detail;
    [alert addButtonWithTitle:@"OK"];
    if (offerReleasePage)
        [alert addButtonWithTitle:@"View Release"];

    [NSApp activateIgnoringOtherApps:YES];
    if ([alert runModal] == NSAlertSecondButtonReturn)
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:kLatestReleasePage]];
}

// Downloads the release zip and stages it beside the plugin. The swap itself
// happens when OBS has quit (see installStagedUpdateAfterOBS).
void downloadUpdate(OBSAirPlayReleaseAsset *asset, pid_t parentPID)
{
    NSString *userAgent = [NSString stringWithFormat:
        @"OBS-AirPlay-Update-Checker/%s", PLUGIN_VERSION];
    OBSAirPlayDownloadAndStageUpdate(asset, gPluginURL, userAgent, ^(NSError *failure) {
        if (failure)
            NSLog(@"[OBS AirPlay Update] %@ not staged: %@", asset.version,
                  failure.localizedDescription);
        else
            NSLog(@"[OBS AirPlay Update] %@ staged; installs when OBS quits", asset.version);

        dispatch_async(dispatch_get_main_queue(), ^{
            if (!parentIsAlive(parentPID))
                return;
            if (failure) {
                showUpdateResult(
                    [NSString stringWithFormat:@"OBS AirPlay %@ could not be installed",
                                               asset.version],
                    [NSString stringWithFormat:
                        @"%@ Nothing was changed. You can install it from the release page.",
                        failure.localizedDescription],
                    true);
            } else {
                showUpdateResult(
                    [NSString stringWithFormat:@"OBS AirPlay %@ is ready", asset.version],
                    @"It will be installed when you quit OBS.", false);
            }
        });
    });
}

void showUpdatePrompt(NSString *latestVersion, NSString *currentVersion,
                      OBSAirPlayReleaseAsset *asset, pid_t parentPID)
{
    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleInformational;
    alert.messageText = [NSString stringWithFormat:
        @"OBS AirPlay %@ is available", latestVersion];
    if (asset) {
        alert.informativeText = [NSString stringWithFormat:
            @"You are using %@. The update is downloaded now and installed when you quit OBS.",
            currentVersion];
        [alert addButtonWithTitle:@"Install Update"];
    } else {
        alert.informativeText = [NSString stringWithFormat:
            @"You are using %@. Close OBS before installing the update.", currentVersion];
    }
    [alert addButtonWithTitle:@"View Release"];
    [alert addButtonWithTitle:@"Later"];
    [alert addButtonWithTitle:@"Skip This Version"];

    [NSApp activateIgnoringOtherApps:YES];
    // Buttons are numbered from NSAlertFirstButtonReturn in the order added.
    NSInteger choice = [alert runModal] - NSAlertFirstButtonReturn;
    if (!asset)
        ++choice;
    if (choice == 0) {
        downloadUpdate(asset, parentPID);
    } else if (choice == 1) {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:kLatestReleasePage]];
    } else if (choice == 3) {
        [[NSUserDefaults standardUserDefaults] setObject:latestVersion
                                                  forKey:kSkippedVersionKey];
    }
}

NSString *currentVersion()
{
    return [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
}

// An update staged earlier is kept only while it is newer than this plugin
// (it is not, once installed another way); a download cut short is removed.
void tidyStagedUpdate()
{
    if (gPluginURL &&
        !OBSAirPlayIsNewerVersion(OBSAirPlayStagedUpdateVersion(gPluginURL), currentVersion()))
        OBSAirPlayDiscardStagedUpdate(gPluginURL);
}

// Called once OBS has gone: nothing is using the plugin any more.
void installStagedUpdateAfterOBS()
{
    if (!gPluginURL)
        return;
    NSString *staged = OBSAirPlayStagedUpdateVersion(gPluginURL);
    if (!OBSAirPlayIsNewerVersion(staged, currentVersion()))
        return;

    NSError *error = nil;
    if (OBSAirPlayInstallStagedUpdate(gPluginURL, &error))
        NSLog(@"[OBS AirPlay Update] installed %@", staged);
    else
        NSLog(@"[OBS AirPlay Update] %@ not installed: %@", staged, error.localizedDescription);
}

void checkForUpdates(pid_t parentPID)
{
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    NSDate *lastCheck = [defaults objectForKey:kLastUpdateCheckKey];
    if ([lastCheck isKindOfClass:[NSDate class]] &&
        -lastCheck.timeIntervalSinceNow < kUpdateCheckInterval) {
        return;
    }

    // Record attempts, not only successes, to avoid hammering GitHub during an
    // outage every time OBS starts.
    [defaults setObject:[NSDate date] forKey:kLastUpdateCheckKey];

    NSMutableURLRequest *request = [NSMutableURLRequest
        requestWithURL:[NSURL URLWithString:kLatestReleaseAPI]
           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
       timeoutInterval:10.0];
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    NSString *userAgent = [NSString stringWithFormat:
        @"OBS-AirPlay-Update-Checker/%s", PLUGIN_VERSION];
    [request setValue:userAgent forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"2022-11-28" forHTTPHeaderField:@"X-GitHub-Api-Version"];

    NSURLSessionDataTask *task = [[NSURLSession sharedSession]
        dataTaskWithRequest:request
          completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        if (error) {
            NSLog(@"[OBS AirPlay Update] check failed: %@", error.localizedDescription);
            return;
        }

        NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)response;
        if (![httpResponse isKindOfClass:[NSHTTPURLResponse class]] ||
            httpResponse.statusCode != 200 || !data) {
            NSLog(@"[OBS AirPlay Update] unexpected HTTP status: %ld",
                  (long)httpResponse.statusCode);
            return;
        }

        NSError *jsonError = nil;
        NSDictionary *release = [NSJSONSerialization JSONObjectWithData:data
                                                                  options:0
                                                                    error:&jsonError];
        NSString *latestVersion = [release isKindOfClass:[NSDictionary class]]
            ? release[@"tag_name"] : nil;
        NSString *installedVersion = currentVersion();
        if (jsonError || !OBSAirPlayIsNewerVersion(latestVersion, installedVersion))
            return;

        NSString *skippedVersion = [defaults stringForKey:kSkippedVersionKey];
        if ([skippedVersion isEqualToString:latestVersion])
            return;

        // Without a plugin this user can replace, or a release zip with a
        // published digest, the prompt only offers the release page.
        OBSAirPlayReleaseAsset *asset = nil;
        if (gPluginURL) {
            asset = OBSAirPlayInstallableAsset(release, OBSAirPlayUpdaterArchitecture());
            // Already downloaded and waiting for OBS to quit.
            if (asset && [OBSAirPlayStagedUpdateVersion(gPluginURL) isEqualToString:asset.version])
                return;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            if (parentIsAlive(parentPID))
                showUpdatePrompt(latestVersion, installedVersion, asset, parentPID);
        });
    }];
    [task resume];
}

} // namespace

@interface AirPlayBLEDelegate : NSObject <CBPeripheralManagerDelegate>
@property(nonatomic, strong) CBPeripheralManager *manager;
@property(nonatomic) BOOL advertisingRequested;
@end

@implementation AirPlayBLEDelegate

- (instancetype)init
{
    self = [super init];
    if (self) {
        _manager = [[CBPeripheralManager alloc] initWithDelegate:self
                                                           queue:dispatch_get_main_queue()
                                                         options:nil];
    }
    return self;
}

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager *)peripheral
{
    if (peripheral.state == CBManagerStatePoweredOn && !self.advertisingRequested) {
        self.advertisingRequested = YES;
        // This intentionally mirrors AirServer's observed advertisement: an
        // always-on, nameless peripheral advertisement with no service data.
        [peripheral startAdvertising:@{}];
        NSLog(@"[OBS AirPlay BLE] advertising requested");
    } else if (peripheral.state != CBManagerStatePoweredOn) {
        NSLog(@"[OBS AirPlay BLE] unavailable, state=%ld", (long)peripheral.state);
    }
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
    didStartAdvertising:(NSError *)error
{
    if (error) {
        NSLog(@"[OBS AirPlay BLE] advertising failed: %@", error);
    } else {
        NSLog(@"[OBS AirPlay BLE] advertising started");
    }
}

@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc != 2)
            return 64;

        const pid_t parentPID = (pid_t)strtol(argv[1], nullptr, 10);
        if (parentPID <= 1)
            return 64;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        NSURL *pluginURL = OBSAirPlayPluginURLForHelper([NSBundle mainBundle].bundleURL);
        if (pluginURL && OBSAirPlayCanReplacePlugin(pluginURL))
            gPluginURL = pluginURL;
        tidyStagedUpdate();

        __strong AirPlayBLEDelegate *delegate = [[AirPlayBLEDelegate alloc] init];
        (void)delegate;

        // Common modes: keep watching the parent while the update prompt's
        // modal session is running, so the helper never outlives OBS.
        NSTimer *parentWatch = [NSTimer timerWithTimeInterval:1.0
                                                      repeats:YES
                                                        block:^(__unused NSTimer *timer) {
            if (kill(parentPID, 0) != 0 && errno == ESRCH) {
                installStagedUpdateAfterOBS();
                exit(0);
            }
        }];
        [[NSRunLoop mainRunLoop] addTimer:parentWatch forMode:NSRunLoopCommonModes];

        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            if (parentIsAlive(parentPID))
                checkForUpdates(parentPID);
        });

        // Run AppKit's event loop, not a bare run loop: an NSApplication
        // that never dequeues window-server events is reported by macOS as
        // "Not Responding" (and counted as hung) even though it is idle.
        [NSApp run];
    }
    return 0;
}
