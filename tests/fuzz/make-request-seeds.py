#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the seed inputs for obs-airplay-fuzz-requests. An input is a list of
# records: one byte naming the request (KINDS, the same order as in
# fuzz-requests.cpp), two bytes of body length (big-endian), the body. The
# harness builds valid headers around each body, so mutations land in the
# bodies (binary plists) and query values rather than in the HTTP framing.
#   python3 tests/fuzz/make-request-seeds.py tests/fuzz/seeds
import os
import plistlib
import struct
import sys

KINDS = ["play", "action", "set-property", "get-property", "rate", "scrub",
         "playback-info", "stop", "info", "setup", "record", "teardown"]
MP4 = bytes([0, 0, 0, 0x18]) + b"ftypisom" + bytes([0, 0, 2, 0]) + b"isomiso2"


def record(kind, body=b""):
    if not isinstance(body, (bytes, bytearray)):
        body = plistlib.dumps(body, fmt=plistlib.FMT_BINARY)
    return bytes([KINDS.index(kind)]) + struct.pack(">H", len(body)) + bytes(body)


def video_session():
    return b"".join([
        record("play", {"Content-Location": "http://192.0.2.10/item.mp4", "Start-Position": 0.0,
                        "uuid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"}),
        record("rate", b"1.000000"),
        record("scrub", b"10.500000"),
        record("playback-info"),
        record("set-property", {"value": [{"MediaSelectionOptionsName": "English",
                                           "MediaSelectionOptionsType": 0}]}),
        record("get-property"),
        record("action", {"type": "playlistInsert",
                          "item": {"Content-Location": "http://192.0.2.10/next.mp4",
                                   "uuid": "bbbbbbbb-cccc-dddd-eeee-ffffffffffff"}}),
        record("action", {"type": "playlistRemove",
                          "item": {"uuid": "bbbbbbbb-cccc-dddd-eeee-ffffffffffff"}}),
        record("play", {"Content-Location": "blob:https://example.test/page-item",
                        "uuid": "cccccccc-dddd-eeee-ffff-000000000000"}),
        record("action", {"type": "unhandledURLResponse",
                          "params": {"FCUP_Response_StatusCode": 0, "FCUP_Response_RequestID": 1,
                                     "FCUP_Response_URL": "blob:https://example.test/page-item",
                                     "FCUP_Response_Data": MP4}}),
        record("stop"),
    ])


def hls_session():
    return b"".join([
        record("play", {"Content-Location": "http://192.0.2.10/hls/master.m3u8",
                        "uuid": "dddddddd-eeee-ffff-0000-111111111111",
                        "clientProcName": "YouTube", "Start-Position-Seconds": 12.5}),
        record("action", {"type": "unhandledURLResponse",
                          "params": {"FCUP_Response_StatusCode": 200, "FCUP_Response_RequestID": 1,
                                     "FCUP_Response_URL": "http://192.0.2.10/hls/master.m3u8",
                                     "FCUP_Response_Data": b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nhttp://192.0.2.10/hls/low.m3u8\n"}}),
        record("playback-info"),
        record("stop"),
    ])


def rtsp_session():
    return b"".join([
        record("info"),
        record("setup", {"streams": [{"type": 96, "ct": 8, "audioFormat": 0x2000000, "spf": 480,
                                      "controlPort": 6001, "usingScreen": True, "isMedia": False}]}),
        record("record"),
        record("teardown", {"streams": [{"type": 96}]}),
    ])


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, data in (("requests-video.bin", video_session()),
                       ("requests-hls.bin", hls_session()),
                       ("requests-rtsp.bin", rtsp_session())):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)


main()
