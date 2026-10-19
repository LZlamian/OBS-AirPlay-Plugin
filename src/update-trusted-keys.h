// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Ed25519 public keys whose signatures make a release installable by the
// self-updaters (src/plugin-updater.mm on macOS, src/windows-update.cpp on
// Windows). The private key is held by the maintainer outside the repository
// (scripts/sign-release.sh). To rotate, ship a release signed by the old key
// that lists both, then drop the old one a release later.
static const char *const kTrustedUpdateKeys[] = {
    "ca4d3512515ca43033f9994d36fe58c0dac5da39ede4f645a375f740310372af",
};
