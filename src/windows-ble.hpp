// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// WinRT's BluetoothLEAdvertisementPublisher with an empty advertisement, the
// Windows side of the macOS helper's "fast discovery" (src/windows-helper.cpp
// runs it). WinRT is reached through combase.dll, loaded at run time, and the
// one interface used is declared here, so neither the Windows SDK's C++/WinRT
// headers nor runtimeobject.lib are needed. No libobs: the smoke test
// (tests/windows-ble-smoke.cpp) runs it on a Windows runner.
//
// Not thread-safe: one thread creates, uses and destroys a Publisher (it
// initialises WinRT on that thread).

#include <windows.h>

#include <cstdint>

namespace windows_ble {

// Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementPublisherStatus
enum PublisherStatus : int32_t {
    Created = 0,
    Waiting = 1,
    Started = 2,
    Stopping = 3,
    Stopped = 4,
    Aborted = 5,
};

struct IPublisher;

class Publisher {
public:
    Publisher() = default;
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    // Initialises WinRT on this thread (once) and activates a publisher.
    HRESULT create();
    bool ready() const { return m_publisher != nullptr; }
    // A PublisherStatus, or -1 without a publisher or when it cannot be read.
    int32_t status() const;
    HRESULT start();
    HRESULT stop();
    // Stops and releases the publisher (WinRT stays initialised).
    void reset();

private:
    IPublisher* m_publisher = nullptr;
    bool m_initialized = false;
};

// Name of a status, for logs and tests.
const char* status_name(int32_t status);

} // namespace windows_ble
