// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

// Which URLs the media player may fetch for an AirPlay video item.
//
// The receiver (uxplay/lib/media_url_policy.h) checks the URL a sender puts
// in POST /play, knowing the sender's address. Everything the player fetches
// after that is checked here against the resolved addresses: the item's own
// host when it is a name, redirect targets, and the playlists, segments and
// keys an HLS playlist points at.
namespace media_url_policy {

// Ports allowed on hosts other than the item's own (default 80, 443, 8080,
// 8443). Overridden by OBS_AIRPLAY_MEDIA_PORTS="80,443,..." at load.
void setAllowedPorts(std::vector<unsigned short> ports);
std::vector<unsigned short> allowedPorts();
// Parses "80, 443,8080"; returns an empty list when nothing valid is found.
std::vector<unsigned short> parsePortList(const std::string& text);

// Test hooks: the smokes serve media from loopback and play local files.
void setLoopbackAllowedForTesting(bool allowed);
void setLocalFilesAllowedForTesting(bool allowed);

// Empty when allowed, otherwise the reason (for the log).
//
// checkItem: the URL the receiver accepted for the item. A literal address
// was already checked with the sender's address in mind; a host name is
// resolved, and refused if any of its addresses is loopback, unspecified,
// multicast, reserved or link-local. "localhost" is only produced by the
// receiver's own HLS proxy.
std::string checkItem(const std::string& url);
// checkNested: any other URL fetched for the item. Same host and port as the
// item are allowed; otherwise the address rules apply to every resolved
// address and the port must be allowed.
std::string checkNested(const std::string& url, const std::string& item_url);
// A non-HTTP item must be the receiver's own spool file (obs-airplay-fcup-*).
std::string checkLocalFile(const std::string& location);

} // namespace media_url_policy
