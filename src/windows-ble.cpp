// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows-ble.hpp"

#include <inspectable.h>

namespace windows_ble {

// IBluetoothLEAdvertisementPublisher, in its metadata order (after
// IInspectable). Only get_Status, Start and Stop are called.
struct IPublisher : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Status(int32_t* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Advertisement(void** advertisement) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_StatusChanged(void* handler, int64_t* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_StatusChanged(int64_t token) = 0;
};

namespace {

// {cde820f9-d9fa-43d6-a264-ddd8b7da8b78}
constexpr IID kPublisherIid = {0xcde820f9, 0xd9fa, 0x43d6,
                               {0xa2, 0x64, 0xdd, 0xd8, 0xb7, 0xda, 0x8b, 0x78}};
constexpr wchar_t kPublisherClass[] =
    L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementPublisher";
constexpr int kRoInitMultithreaded = 1;  // RO_INIT_MULTITHREADED

using RoInitializeFn = HRESULT(WINAPI*)(int);
using RoUninitializeFn = void(WINAPI*)();
using RoActivateInstanceFn = HRESULT(WINAPI*)(HSTRING, IInspectable**);
using WindowsCreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, HSTRING*);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);

// combase.dll's WinRT entry points (Windows 8+), loaded once and kept.
struct Combase {
    RoInitializeFn initialize = nullptr;
    RoUninitializeFn uninitialize = nullptr;
    RoActivateInstanceFn activate = nullptr;
    WindowsCreateStringFn create_string = nullptr;
    WindowsDeleteStringFn delete_string = nullptr;

    bool loaded() const
    {
        return initialize && uninitialize && activate && create_string && delete_string;
    }
};

template <class T> T entry(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

const Combase& combase()
{
    static const Combase functions = [] {
        Combase result;
        HMODULE module = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) {
            result.initialize = entry<RoInitializeFn>(module, "RoInitialize");
            result.uninitialize = entry<RoUninitializeFn>(module, "RoUninitialize");
            result.activate = entry<RoActivateInstanceFn>(module, "RoActivateInstance");
            result.create_string = entry<WindowsCreateStringFn>(module, "WindowsCreateString");
            result.delete_string = entry<WindowsDeleteStringFn>(module, "WindowsDeleteString");
        }
        return result;
    }();
    return functions;
}

} // namespace

Publisher::~Publisher()
{
    reset();
    if (m_initialized) combase().uninitialize();
}

HRESULT Publisher::create()
{
    if (m_publisher) return S_OK;
    const Combase& functions = combase();
    if (!functions.loaded()) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (!m_initialized) {
        const HRESULT initialized = functions.initialize(kRoInitMultithreaded);
        // S_FALSE: already initialised on this thread, still to be balanced.
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return initialized;
        m_initialized = SUCCEEDED(initialized);
    }

    HSTRING name = nullptr;
    HRESULT result = functions.create_string(
        kPublisherClass, static_cast<UINT32>(sizeof(kPublisherClass) / sizeof(wchar_t) - 1), &name);
    if (FAILED(result)) return result;
    IInspectable* instance = nullptr;
    result = functions.activate(name, &instance);
    functions.delete_string(name);
    if (FAILED(result)) return result;
    result = instance->QueryInterface(kPublisherIid, reinterpret_cast<void**>(&m_publisher));
    instance->Release();
    if (FAILED(result)) m_publisher = nullptr;
    return result;
}

int32_t Publisher::status() const
{
    int32_t value = -1;
    if (!m_publisher || FAILED(m_publisher->get_Status(&value))) return -1;
    return value;
}

HRESULT Publisher::start()
{
    return m_publisher ? m_publisher->Start() : E_POINTER;
}

HRESULT Publisher::stop()
{
    return m_publisher ? m_publisher->Stop() : E_POINTER;
}

void Publisher::reset()
{
    if (!m_publisher) return;
    const int32_t current = status();
    if (current == Waiting || current == Started) m_publisher->Stop();
    m_publisher->Release();
    m_publisher = nullptr;
}

const char* status_name(int32_t status)
{
    switch (status) {
    case Created: return "created";
    case Waiting: return "waiting";
    case Started: return "started";
    case Stopping: return "stopping";
    case Stopped: return "stopped";
    case Aborted: return "aborted";
    default: return "unknown";
    }
}

} // namespace windows_ble
