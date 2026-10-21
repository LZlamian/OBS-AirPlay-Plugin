// SPDX-License-Identifier: GPL-3.0-or-later
// HLS master playlists a sender returns over the reverse channel
// (uxplay/lib/airplay_video.c), through the steps POST /action takes for a
// .../master.m3u8 reply: the audio-language choice, the media URI table and
// the rewrite to the receiver's local prefix. Run once with no preferred
// language and once with a list, as raop->lang may be.
extern "C" {
// raop.h declares airplay_video_init with the type airplay_video.h defines.
typedef struct airplay_video_s airplay_video_t;
#include "airplay_video.h"
}

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char kUriPrefix[] = "https://manifest.example.test/api/manifest/hls_playlist";

void runMasterPath(const char* lang, const uint8_t* data, size_t size)
{
    // airplay_video_init only keeps the raop pointer; nothing here uses it.
    static int fake_raop;
    airplay_video_t* video =
        airplay_video_init(reinterpret_cast<raop_t*>(&fake_raop), 7100, lang);
    if (!video) return;
    set_uri_prefix(video, kUriPrefix, sizeof(kUriPrefix) - 1);

    // The handler copies the reply into a NUL-terminated buffer of its own
    // length, so an embedded NUL ends the playlist early, as it does there.
    char* playlist = static_cast<char*>(std::malloc(size + 1));
    if (!playlist) {
        airplay_video_destroy(video);
        return;
    }
    std::memcpy(playlist, data, size);
    playlist[size] = '\0';

    playlist = select_master_playlist_language(video, playlist);
    const int playlist_len = static_cast<int>(std::strlen(playlist));
    char** uri_list = nullptr;
    int num_uri = 0;
    create_media_uri_table(kUriPrefix, playlist, playlist_len, &uri_list, &num_uri);
    char* new_master = adjust_master_playlist(playlist, playlist_len, kUriPrefix,
                                              get_uri_local_prefix(video));
    std::free(playlist);
    store_master_playlist(video, new_master);
    create_media_data_store(video, uri_list, num_uri);
    std::free(uri_list);
    // What the following /play, /playback-info and media requests read.
    get_duration(video);
    int count = 0;
    float duration = 0.0f;
    get_media_playlist(video, &count, &duration, "index.m3u8");
    airplay_video_destroy(video);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // The language choice prints to stdout on every input.
    static const bool quiet = std::freopen("/dev/null", "w", stdout) != nullptr;
    (void)quiet;
    runMasterPath(nullptr, data, size);
    runMasterPath("fr:en", data, size);
    return 0;
}
