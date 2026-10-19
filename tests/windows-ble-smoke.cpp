// SPDX-License-Identifier: GPL-3.0-or-later
// Checks the hand-declared WinRT advertisement publisher (src/windows-ble.cpp)
// against Windows itself: the class activates, answers the declared interface
// and reports a valid status through the declared vtable, before and after
// Start. A machine without a Bluetooth radio (CI) ends in "aborted"; one
// without the WinRT class at all is skipped (exit 77).
#include "windows-ble.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    windows_ble::Publisher publisher;
    const HRESULT created = publisher.create();
    if (created == REGDB_E_CLASSNOTREG || created == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)) {
        std::printf("windows-ble-smoke: SKIP, no WinRT publisher (0x%08lx)\n",
                    static_cast<unsigned long>(created));
        return 77;  // ctest: skipped
    }
    if (FAILED(created) || !publisher.ready()) {
        std::printf("windows-ble-smoke: FAIL, create 0x%08lx\n", static_cast<unsigned long>(created));
        return 1;
    }

    const int32_t initial = publisher.status();
    std::printf("status after create: %s (%d)\n", windows_ble::status_name(initial), initial);
    if (initial != windows_ble::Created) {
        std::printf("windows-ble-smoke: FAIL, a new publisher is not \"created\"\n");
        return 1;
    }

    const HRESULT started = publisher.start();
    std::printf("start: 0x%08lx\n", static_cast<unsigned long>(started));
    int32_t status = publisher.status();
    for (int i = 0; i < 30 && status == windows_ble::Waiting; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        status = publisher.status();
    }
    std::printf("status after start: %s (%d)\n", windows_ble::status_name(status), status);
    if (status < windows_ble::Created || status > windows_ble::Aborted) {
        std::printf("windows-ble-smoke: FAIL, status out of range\n");
        return 1;
    }
    if (SUCCEEDED(started) && status == windows_ble::Created) {
        std::printf("windows-ble-smoke: FAIL, Start succeeded but nothing changed\n");
        return 1;
    }

    publisher.reset();
    if (publisher.ready() || publisher.status() != -1) {
        std::printf("windows-ble-smoke: FAIL, reset kept the publisher\n");
        return 1;
    }
    std::printf("windows-ble-smoke: OK\n");
    return 0;
}
