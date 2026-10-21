// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#import <Foundation/Foundation.h>

// Downloading and installing a plugin update from a GitHub release.
//
// The update is the release's zip asset. It must carry an Ed25519 signature
// (the "<zip>.sig" asset) by a key built into the plugin over its name,
// version, size and SHA-256; it is checked against that digest and the one
// GitHub publishes, unpacked next to the installed plugin, checked again as
// a bundle (identifier, version, architecture, code seal), and swapped in
// once OBS has quit, when the bundle is checked once more. Nothing here
// needs administrator rights: only a plugin in the user's own home folder
// is updated this way.

NS_ASSUME_NONNULL_BEGIN

extern NSString *const OBSAirPlayUpdaterErrorDomain;

@interface OBSAirPlayReleaseAsset : NSObject
@property(nonatomic, copy) NSString *version;   // "2.4.0", no leading "v"
@property(nonatomic, strong) NSURL *downloadURL;
@property(nonatomic, copy) NSString *sha256;    // lowercase hex
@property(nonatomic) unsigned long long size;
@property(nonatomic, copy) NSString *name;      // "obs-airplay-v2.6.0-macos-arm64.zip"
@property(nonatomic, strong, nullable) NSURL *signatureURL;
@property(nonatomic, strong, nullable) NSData *signature; // the downloaded .sig asset
@end

// The text an update signature signs, for `asset`.
NSString *OBSAirPlayUpdateSignedMessage(OBSAirPlayReleaseAsset *asset);
// Whether `asset.signature` is a valid signature of the asset by a trusted key.
BOOL OBSAirPlayVerifyUpdateSignature(OBSAirPlayReleaseAsset *asset);
// Replaces the built-in trusted keys (32-byte Ed25519 public keys). Tests only.
void OBSAirPlaySetTrustedUpdateKeysForTesting(NSArray<NSData *> *_Nullable keys);
// Folder under which a plugin may update itself (default: the home folder).
// Tests only.
void OBSAirPlaySetUpdateRootForTesting(NSString *_Nullable root);

// "arm64" or "x86_64": the architecture this code was built for.
NSString *OBSAirPlayUpdaterArchitecture(void);

// True when `candidate` ("v2.4.0" or "2.4.0") is a later x.y.z than `current`.
BOOL OBSAirPlayIsNewerVersion(NSString *_Nullable candidate, NSString *_Nullable current);

// The installable zip of a GitHub "release" JSON object, or nil when the
// release has no zip for this architecture with a published SHA-256 digest,
// a signature asset, and download addresses under this project's
// https://github.com/LZlamian/OBS-AirPlay-Plugin/releases/download/.
OBSAirPlayReleaseAsset *_Nullable OBSAirPlayInstallableAsset(NSDictionary *release,
                                                            NSString *architecture);

// The plugin bundle that contains the helper at `helperBundleURL`
// (…/obs-airplay.plugin/Contents/Resources/OBS AirPlay Discovery.app), or nil.
NSURL *_Nullable OBSAirPlayPluginURLForHelper(NSURL *helperBundleURL);

// Whether the plugin at `pluginURL` can be replaced without more rights: it
// is in the user's home folder, owned by the user, and writable.
BOOL OBSAirPlayCanReplacePlugin(NSURL *pluginURL);

// Checks the downloaded zip, unpacks it beside the plugin and checks the
// bundle inside. Returns the staged bundle, ready for
// OBSAirPlayInstallStagedUpdate, or nil with an error. The zip is not removed.
NSURL *_Nullable OBSAirPlayStageUpdate(NSURL *zipURL, OBSAirPlayReleaseAsset *asset,
                                       NSURL *pluginURL, NSError **error);

// Downloads the signature and the asset and stages it with
// OBSAirPlayStageUpdate. `completion` is called once, on a background queue,
// with nil on success.
void OBSAirPlayDownloadAndStageUpdate(OBSAirPlayReleaseAsset *asset, NSURL *pluginURL,
                                      NSString *userAgent,
                                      void (^completion)(NSError *_Nullable error));

// GET with a byte limit, enforced while the data arrives (a response is
// cancelled once it is larger): NSURLSession itself takes whatever a server
// or an interposer sends. `completion` runs once, on a background queue,
// with the body or an error.
void OBSAirPlayFetchLimited(NSURLRequest *request, NSUInteger limit,
                            void (^completion)(NSData *_Nullable data,
                                               NSHTTPURLResponse *_Nullable response,
                                               NSError *_Nullable error));

// The version of an update staged earlier for `pluginURL`, or nil.
NSString *_Nullable OBSAirPlayStagedUpdateVersion(NSURL *pluginURL);

// Removes anything staged for `pluginURL`.
void OBSAirPlayDiscardStagedUpdate(NSURL *pluginURL);

// Replaces the plugin with the staged update. Call only when OBS is not
// running. The staged bundle is checked again, and the swap holds a lock so
// two helpers cannot interleave it. On failure the installed plugin is left
// as it was. The replaced version is kept (OBSAirPlayPreviousVersion) until
// OBSAirPlayForgetPreviousVersion.
BOOL OBSAirPlayInstallStagedUpdate(NSURL *pluginURL, NSError **error);

// The version kept from the last update, or nil. Kept beside the plugin as
// .obs-airplay.previous/obs-airplay.plugin so it can be put back by hand.
NSString *_Nullable OBSAirPlayPreviousVersion(NSURL *pluginURL);
// Removes the kept version once a newer plugin has started successfully.
void OBSAirPlayForgetPreviousVersion(NSURL *pluginURL);

NS_ASSUME_NONNULL_END
