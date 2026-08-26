#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

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
        if (part.length == 0 || [part rangeOfCharacterFromSet:nonDigits].location != NSNotFound)
            return nil;
        [numbers addObject:@(part.integerValue)];
    }
    return numbers;
}

bool isNewerVersion(NSString *candidate, NSString *current)
{
    NSArray<NSNumber *> *candidateParts = parseVersion(candidate);
    NSArray<NSNumber *> *currentParts = parseVersion(current);
    if (!candidateParts || !currentParts)
        return false;

    for (NSUInteger index = 0; index < 3; ++index) {
        const NSInteger candidatePart = candidateParts[index].integerValue;
        const NSInteger currentPart = currentParts[index].integerValue;
        if (candidatePart != currentPart)
            return candidatePart > currentPart;
    }
    return false;
}

void showUpdatePrompt(NSString *latestVersion, NSString *currentVersion)
{
    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleInformational;
    alert.messageText = [NSString stringWithFormat:
        @"OBS AirPlay %@ is available", latestVersion];
    alert.informativeText = [NSString stringWithFormat:
        @"You are using %@. Close OBS before installing the update.", currentVersion];
    [alert addButtonWithTitle:@"View Release"];
    [alert addButtonWithTitle:@"Later"];
    [alert addButtonWithTitle:@"Skip This Version"];

    [NSApp activateIgnoringOtherApps:YES];
    const NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:kLatestReleasePage]];
    } else if (response == NSAlertThirdButtonReturn) {
        [[NSUserDefaults standardUserDefaults] setObject:latestVersion
                                                  forKey:kSkippedVersionKey];
    }
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
        NSString *currentVersion = [[NSBundle mainBundle]
            objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        if (jsonError || !isNewerVersion(latestVersion, currentVersion))
            return;

        NSString *skippedVersion = [defaults stringForKey:kSkippedVersionKey];
        if ([skippedVersion isEqualToString:latestVersion])
            return;

        dispatch_async(dispatch_get_main_queue(), ^{
            if (parentIsAlive(parentPID))
                showUpdatePrompt(latestVersion, currentVersion);
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

        __strong AirPlayBLEDelegate *delegate = [[AirPlayBLEDelegate alloc] init];
        (void)delegate;

        [NSTimer scheduledTimerWithTimeInterval:1.0
                                         repeats:YES
                                           block:^(__unused NSTimer *timer) {
            if (kill(parentPID, 0) != 0 && errno == ESRCH)
                exit(0);
        }];

        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            if (parentIsAlive(parentPID))
                checkForUpdates(parentPID);
        });

        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}
