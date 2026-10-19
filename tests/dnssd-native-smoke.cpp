// SPDX-License-Identifier: GPL-3.0-or-later
// Windows only: registers _airplay._tcp and _raop._tcp through UxPlay's
// Windows DNS service backend (no Bonjour), then resolves the AirPlay
// service back over mDNS with DnsServiceResolve and checks its port and TXT
// record, as an iPhone would see them.
//   obs-airplay-dnssd-native-smoke [--register-only]
#include <winsock2.h>
#include <windows.h>
#include <windns.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "dnssd.h"
}

namespace {

struct Resolved {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<DWORD> status{0};
    WORD port = 0;
    std::wstring host;
    std::wstring deviceid;
    std::wstring features;
    DWORD properties = 0;
};

void WINAPI resolveComplete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
    auto* resolved = static_cast<Resolved*>(context);
    resolved->status = status;
    if (instance) {
        resolved->port = instance->wPort;
        if (instance->pszHostName) resolved->host = instance->pszHostName;
        resolved->properties = instance->dwPropertyCount;
        for (DWORD i = 0; i < instance->dwPropertyCount; ++i) {
            if (!instance->keys[i] || !instance->values[i]) continue;
            if (!wcscmp(instance->keys[i], L"deviceid")) resolved->deviceid = instance->values[i];
            if (!wcscmp(instance->keys[i], L"features")) resolved->features = instance->values[i];
        }
        DnsServiceFreeInstance(instance);
    }
    SetEvent(resolved->done);
}

} // namespace

int main(int argc, char** argv)
{
    const bool register_only = argc == 2 && !std::strcmp(argv[1], "--register-only");
    _putenv("UXPLAY_DNSSD_BACKEND=windows");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    char name[64];
    std::snprintf(name, sizeof(name), "OBS AirPlay CI %lu", GetCurrentProcessId());
    const char hw_addr[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    int error = 0;
    dnssd_t* dnssd = dnssd_init(name, static_cast<int>(std::strlen(name)), hw_addr,
                                sizeof(hw_addr), &error, 0);
    if (!dnssd || error != DNSSD_ERROR_NOERROR) {
        std::printf("dnssd_init failed (%d): no Windows DNS service API\n", error);
        return 1;
    }
    char pk[] = "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7";
    dnssd_set_pk(dnssd, pk);
    std::printf("backend=%s\n", dnssd_get_backend(dnssd));
    if (std::strcmp(dnssd_get_backend(dnssd), "Windows DNS service") != 0) return 1;

    const unsigned short port = 47123;
    const int airplay = dnssd_register_airplay(dnssd, port);
    const int raop = dnssd_register_raop(dnssd, port);
    int txt_length = 0;
    const char* txt = dnssd_get_airplay_txt(dnssd, &txt_length);
    const bool txt_ok = txt && txt_length > 20 &&
        std::string(txt, static_cast<size_t>(txt_length)).find("deviceid=02:11:22:33:44:55") !=
            std::string::npos;
    std::printf("register airplay=%d raop=%d txt_bytes=%d txt=%s\n", airplay, raop, txt_length,
                txt_ok ? "ok" : "BAD");
    if (airplay != 0 || raop != 0 || !txt_ok) return 1;

    int failures = 0;
    if (!register_only) {
        // Probing and announcing take a moment; resolving waits for answers.
        Sleep(3000);
        Resolved resolved;
        wchar_t query[128];
        std::swprintf(query, 128, L"OBS AirPlay CI %lu._airplay._tcp.local",
                      GetCurrentProcessId());
        DNS_SERVICE_RESOLVE_REQUEST request = {};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.QueryName = query;
        request.pResolveCompletionCallback = resolveComplete;
        request.pQueryContext = &resolved;
        DNS_SERVICE_CANCEL cancel = {};
        const DNS_STATUS started = DnsServiceResolve(&request, &cancel);
        bool completed = false;
        if (started == DNS_REQUEST_PENDING) {
            completed = WaitForSingleObject(resolved.done, 10000) == WAIT_OBJECT_0;
            if (!completed) DnsServiceResolveCancel(&cancel);
        }
        const bool resolve_ok = completed && resolved.status == ERROR_SUCCESS &&
            resolved.port == port && resolved.deviceid == L"02:11:22:33:44:55" &&
            !resolved.features.empty();
        std::printf("resolve start=%ld completed=%d status=%lu port=%u host=%ls properties=%lu "
                    "deviceid=%ls -> %s\n",
                    static_cast<long>(started), completed ? 1 : 0,
                    static_cast<unsigned long>(resolved.status.load()), resolved.port,
                    resolved.host.c_str(), static_cast<unsigned long>(resolved.properties),
                    resolved.deviceid.c_str(), resolve_ok ? "ok" : "FAILED");
        if (!resolve_ok) ++failures;
    }

    dnssd_unregister_airplay(dnssd);
    dnssd_unregister_raop(dnssd);
    dnssd_destroy(dnssd);
    std::printf("dnssd-native: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
