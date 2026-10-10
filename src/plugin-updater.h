#pragma once

#import <Foundation/Foundation.h>

// Downloading and installing a plugin update from a GitHub release.
//
// The update is the release's zip asset. It is checked against the SHA-256
// digest GitHub publishes for the asset, unpacked next to the installed
// plugin, checked again as a bundle (identifier, version, code seal), and
// swapped in once OBS has quit. Nothing here needs administrator rights: a
// plugin the user cannot write to is simply not updated this way.

NS_ASSUME_NONNULL_BEGIN

extern NSString *const OBSAirPlayUpdaterErrorDomain;

@interface OBSAirPlayReleaseAsset : NSObject
@property(nonatomic, copy) NSString *version;   // "2.4.0", no leading "v"
@property(nonatomic, strong) NSURL *downloadURL;
@property(nonatomic, copy) NSString *sha256;    // lowercase hex
@property(nonatomic) unsigned long long size;
@end

// "arm64" or "x86_64": the architecture this code was built for.
NSString *OBSAirPlayUpdaterArchitecture(void);

// True when `candidate` ("v2.4.0" or "2.4.0") is a later x.y.z than `current`.
BOOL OBSAirPlayIsNewerVersion(NSString *_Nullable candidate, NSString *_Nullable current);

// The installable zip of a GitHub "release" JSON object, or nil when the
// release has no zip for this architecture with a published SHA-256 digest
// and an https://github.com/ download address.
OBSAirPlayReleaseAsset *_Nullable OBSAirPlayInstallableAsset(NSDictionary *release,
                                                            NSString *architecture);

// The plugin bundle that contains the helper at `helperBundleURL`
// (…/obs-airplay.plugin/Contents/Resources/OBS AirPlay Discovery.app), or nil.
NSURL *_Nullable OBSAirPlayPluginURLForHelper(NSURL *helperBundleURL);

// Whether the plugin at `pluginURL` can be replaced without more rights.
BOOL OBSAirPlayCanReplacePlugin(NSURL *pluginURL);

// Checks the downloaded zip, unpacks it beside the plugin and checks the
// bundle inside. Returns the staged bundle, ready for
// OBSAirPlayInstallStagedUpdate, or nil with an error. The zip is not removed.
NSURL *_Nullable OBSAirPlayStageUpdate(NSURL *zipURL, OBSAirPlayReleaseAsset *asset,
                                       NSURL *pluginURL, NSError **error);

// Downloads the asset and stages it with OBSAirPlayStageUpdate. `completion`
// is called once, on a background queue, with nil on success.
void OBSAirPlayDownloadAndStageUpdate(OBSAirPlayReleaseAsset *asset, NSURL *pluginURL,
                                      NSString *userAgent,
                                      void (^completion)(NSError *_Nullable error));

// The version of an update staged earlier for `pluginURL`, or nil.
NSString *_Nullable OBSAirPlayStagedUpdateVersion(NSURL *pluginURL);

// Removes anything staged for `pluginURL`.
void OBSAirPlayDiscardStagedUpdate(NSURL *pluginURL);

// Replaces the plugin with the staged update. Call only when OBS is not
// running. On failure the installed plugin is left as it was.
BOOL OBSAirPlayInstallStagedUpdate(NSURL *pluginURL, NSError **error);

NS_ASSUME_NONNULL_END
