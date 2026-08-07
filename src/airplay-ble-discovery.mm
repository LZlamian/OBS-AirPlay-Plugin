#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <unistd.h>

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

        __strong AirPlayBLEDelegate *delegate = [[AirPlayBLEDelegate alloc] init];
        (void)delegate;

        [NSTimer scheduledTimerWithTimeInterval:1.0
                                         repeats:YES
                                           block:^(__unused NSTimer *timer) {
            if (kill(parentPID, 0) != 0 && errno == ESRCH)
                exit(0);
        }];

        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}
