// SPDX-License-Identifier: GPL-3.0-or-later
// Windows counterpart of the macOS discovery helper (airplay-ble-discovery.mm),
// behind the same interface (airplay-ble-helper.hpp). It runs in OBS's own
// process: Windows asks no per-app permission for Bluetooth, and the plugin
// is replaced by running the release's installer, not by swapping files.
//
// - Bluetooth LE: the same nameless advertisement with no data the macOS
//   helper publishes (AirServer's), through WinRT's
//   BluetoothLEAdvertisementPublisher.
// - Updates: at most once a day (the source's "Check for updates" option),
//   the latest GitHub release is checked. A newer one is offered when OBS is
//   not streaming or recording: Install, View Release, Later (a week), Skip
//   This Version. Install downloads the release's installer, checks it
//   (windows-update.hpp: published size and SHA-256, Ed25519 release
//   signature) and starts it with administrator rights; the installer waits
//   until OBS has quit (/WAITPID) and then installs silently.
#include <winsock2.h>  // before windows.h, which windows-ble.hpp includes

#include "airplay-ble-helper.hpp"
#include "windows-ble.hpp"
#include "windows-update.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const wchar_t* kLatestReleaseHost = L"api.github.com";
constexpr const wchar_t* kLatestReleasePath = L"/repos/LZlamian/OBS-AirPlay-Plugin/releases/latest";
constexpr const wchar_t* kLatestReleasePage =
    L"https://github.com/LZlamian/OBS-AirPlay-Plugin/releases/latest";
constexpr int64_t kCheckIntervalSeconds = 24 * 60 * 60;
constexpr int64_t kSnoozeSeconds = 7 * 24 * 60 * 60;
// After OBS starts, before the first check: not while OBS is loading.
constexpr auto kFirstCheckDelay = std::chrono::seconds(60);
constexpr auto kBusyRetry = std::chrono::seconds(60);
constexpr uint64_t kMaxReleaseJson = 1024 * 1024;

// ---- shared state ---------------------------------------------------------

std::mutex g_mutex;  // guards everything below
std::condition_variable g_cv;
bool g_running = false;
bool g_quit = false;
std::thread g_ble_thread;
std::thread g_update_thread;
std::string g_bluetooth_state;
HINTERNET g_request = nullptr;   // the request in progress, closed by stop() to cancel it
HWND g_dialog = nullptr;         // the prompt on screen, closed by stop()

std::atomic<bool> g_update_checks{true};
std::atomic<bool> g_outputs_active{false};

// Waits up to `duration`; false once the helper is stopping.
template <class Duration> bool sleep_unless_stopping(Duration duration)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    return !g_cv.wait_for(lock, duration, [] { return g_quit; });
}

bool stopping()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_quit;
}

std::wstring widen(const std::string& text)
{
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
    std::wstring wide(static_cast<size_t>(count > 0 ? count : 0), L'\0');
    if (count > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            count);
    }
    return wide;
}

// ---- Bluetooth LE -----------------------------------------------------------

void set_bluetooth_state(const char* state)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bluetooth_state = state;
}

void ble_thread_func()
{
    windows_ble::Publisher publisher;
    auto next_start = std::chrono::steady_clock::now();
    do {
        if (!publisher.ready()) {
            const HRESULT created = publisher.create();
            if (FAILED(created)) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    blog(LOG_INFO, "[BLE] Bluetooth LE advertising is not available (0x%08lx)",
                         static_cast<unsigned long>(created));
                }
                set_bluetooth_state("unavailable");
                continue;  // tried again after the wait below
            }
        }
        int32_t status = publisher.status();
        const auto now = std::chrono::steady_clock::now();
        if ((status == windows_ble::Created || status == windows_ble::Stopped ||
             status == windows_ble::Aborted) && now >= next_start) {
            // Also after Bluetooth was turned off and on: Windows aborts the
            // advertisement with the radio, so it is requested again.
            next_start = now + std::chrono::seconds(30);
            const HRESULT started = publisher.start();
            if (FAILED(started)) {
                blog(LOG_INFO, "[BLE] advertising could not start (0x%08lx)",
                     static_cast<unsigned long>(started));
                publisher.reset();  // a fresh publisher next time
                set_bluetooth_state("unavailable");
                continue;
            }
            status = publisher.status();
        }
        set_bluetooth_state(status == windows_ble::Started   ? "on"
                            : status == windows_ble::Aborted ? "off or not supported"
                                                             : "starting");
    } while (sleep_unless_stopping(std::chrono::seconds(5)));
}

// ---- HTTPS (WinHTTP: Windows' TLS and certificate store) --------------------

class Http {
public:
    Http()
    {
        const std::wstring agent = L"OBS-AirPlay-Update-Checker/" + widen(PLUGIN_VERSION);
        m_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session) {  // before Windows 8.1
            m_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (m_session) WinHttpSetTimeouts(m_session, 10000, 10000, 30000, 30000);
    }
    ~Http()
    {
        if (m_session) WinHttpCloseHandle(m_session);
    }
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    // GET https://host/path; each chunk of the body goes to `sink` (false
    // stops). Redirects are followed, never from HTTPS to HTTP.
    bool get(const std::wstring& url, uint64_t max_size,
             const std::function<bool(const char*, size_t)>& sink, bool github_api = false)
    {
        URL_COMPONENTS parts = {};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2048;
        if (!m_session || !WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS) {
            return false;
        }
        HINTERNET connection = WinHttpConnect(m_session, host, parts.nPort, 0);
        if (!connection) return false;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        bool ok = false;
        if (request) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_quit) {
                    WinHttpCloseHandle(request);
                    request = nullptr;
                } else {
                    g_request = request;
                }
            }
            const wchar_t* headers = github_api
                ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                : WINHTTP_NO_ADDITIONAL_HEADERS;
            DWORD status = 0;
            DWORD status_size = sizeof(status);
            ok = request &&
                 WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                                    0, 0, 0) &&
                 WinHttpReceiveResponse(request, nullptr) &&
                 WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                     WINHTTP_NO_HEADER_INDEX) &&
                 status == 200;
            uint64_t total = 0;
            std::vector<char> buffer(64 * 1024);
            while (ok) {
                DWORD read = 0;
                if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                    ok = false;
                    break;
                }
                if (read == 0) break;
                total += read;
                if (total > max_size || !sink(buffer.data(), read)) ok = false;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            if (request && g_request == request) {  // not already closed by stop()
                WinHttpCloseHandle(request);
                g_request = nullptr;
            }
        }
        WinHttpCloseHandle(connection);
        return ok && !stopping();
    }

private:
    HINTERNET m_session = nullptr;
};

// ---- update state (plugin config folder: updater.json) ----------------------

struct UpdateState {
    int64_t last_check = 0;
    int64_t snoozed_until = 0;
    std::string skipped_version;
};

std::string state_path()
{
    char* path = obs_module_config_path("updater.json");
    std::string result = path ? path : "";
    bfree(path);
    return result;
}

UpdateState load_state()
{
    UpdateState state;
    obs_data_t* data = obs_data_create_from_json_file_safe(state_path().c_str(), "bak");
    if (data) {
        state.last_check = obs_data_get_int(data, "last_check");
        state.snoozed_until = obs_data_get_int(data, "snoozed_until");
        state.skipped_version = obs_data_get_string(data, "skipped_version");
        obs_data_release(data);
    }
    return state;
}

void save_state(const UpdateState& state)
{
    char* dir = obs_module_config_path("");
    if (dir) {
        os_mkdirs(dir);
        bfree(dir);
    }
    obs_data_t* data = obs_data_create();
    obs_data_set_int(data, "last_check", state.last_check);
    obs_data_set_int(data, "snoozed_until", state.snoozed_until);
    obs_data_set_string(data, "skipped_version", state.skipped_version.c_str());
    obs_data_save_json_safe(data, state_path().c_str(), "tmp", "bak");
    obs_data_release(data);
}

int64_t now_seconds()
{
    return static_cast<int64_t>(std::time(nullptr));
}

// ---- prompt -------------------------------------------------------------------

enum class Choice { None, Install, ViewRelease, Later, Skip };

// The dialog on screen, for stop() to close; closed at once if OBS is
// already quitting (stop() may have run while it was being created).
void register_dialog(HWND window)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dialog = window;
    if (g_quit) PostMessageW(window, WM_CLOSE, 0, 0);
}

HRESULT CALLBACK prompt_callback(HWND window, UINT notification, WPARAM, LPARAM, LONG_PTR)
{
    if (notification == TDN_CREATED) {
        // Above OBS, without taking the keyboard focus from it.
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        register_dialog(window);
    } else if (notification == TDN_DESTROYED) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dialog = nullptr;
    }
    return S_OK;
}

BOOL CALLBACK find_message_box(HWND window, LPARAM found)
{
    wchar_t name[16] = {};
    GetClassNameW(window, name, 16);
    if (wcscmp(name, L"#32770") == 0) {  // a dialog: the message box
        *reinterpret_cast<HWND*>(found) = window;
        return FALSE;
    }
    return TRUE;
}

// MessageBox on this thread, closable by stop() like the task dialog.
int message_box(const std::wstring& text, UINT flags)
{
    const DWORD thread = GetCurrentThreadId();
    std::atomic<bool> done{false};
    std::thread finder([thread, &done] {
        while (!done) {
            HWND found = nullptr;
            EnumThreadWindows(thread, find_message_box, reinterpret_cast<LPARAM>(&found));
            if (found) {
                register_dialog(found);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    // Topmost, but it does not take the keyboard focus from OBS.
    const int result = MessageBoxW(nullptr, text.c_str(), L"OBS AirPlay", flags | MB_TOPMOST);
    done = true;
    finder.join();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dialog = nullptr;
    return result;
}

Choice prompt(const std::string& latest, bool installable)
{
    const std::wstring title = L"OBS AirPlay " + widen(latest) + L" is available";
    const std::wstring body = installable
        ? L"You are using " + widen(PLUGIN_VERSION) +
              L". The update is downloaded now and installed when you quit OBS (Windows asks "
              L"for permission first)."
        : L"You are using " + widen(PLUGIN_VERSION) + L". Install it from the release page.";

    using TaskDialogIndirect_t = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    // Version 6 of the common controls only (OBS's manifest asks for it).
    HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
    auto task_dialog = comctl ? reinterpret_cast<TaskDialogIndirect_t>(reinterpret_cast<void*>(
                                    GetProcAddress(comctl, "TaskDialogIndirect")))
                              : nullptr;
    if (task_dialog) {
        std::vector<TASKDIALOG_BUTTON> buttons;
        if (installable) buttons.push_back({100, L"Install Update"});
        buttons.push_back({101, L"View Release"});
        buttons.push_back({102, L"Later"});
        buttons.push_back({103, L"Skip This Version"});
        TASKDIALOGCONFIG config = {};
        config.cbSize = sizeof(config);
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.pszWindowTitle = L"OBS AirPlay";
        config.pszMainIcon = TD_INFORMATION_ICON;
        config.pszMainInstruction = title.c_str();
        config.pszContent = body.c_str();
        config.pButtons = buttons.data();
        config.cButtons = static_cast<UINT>(buttons.size());
        config.nDefaultButton = buttons.front().nButtonID;
        config.pfCallback = prompt_callback;
        int button = 0;
        if (SUCCEEDED(task_dialog(&config, &button, nullptr, nullptr))) {
            switch (button) {
            case 100: return Choice::Install;
            case 101: return Choice::ViewRelease;
            case 102: return Choice::Later;
            case 103: return Choice::Skip;
            default: return Choice::None;  // closed, or OBS is quitting
            }
        }
    }
    // Without the task dialog: Yes / No / Cancel.
    const std::wstring text = title + L"\n\n" + body +
        (installable ? L"\n\nYes: install the update.  No: remind me in a week.  "
                       L"Cancel: skip this version."
                     : L"\n\nYes: open the release page.  No: remind me in a week.  "
                       L"Cancel: skip this version.");
    switch (message_box(text, MB_YESNOCANCEL | MB_ICONINFORMATION)) {
    case IDYES: return installable ? Choice::Install : Choice::ViewRelease;
    case IDNO: return Choice::Later;
    case IDCANCEL: return stopping() ? Choice::None : Choice::Skip;
    default: return Choice::None;
    }
}

// ---- release check and install -----------------------------------------------

std::wstring updates_folder()
{
    wchar_t local[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    std::wstring folder = std::wstring(local) + L"\\obs-airplay";
    CreateDirectoryW(folder.c_str(), nullptr);
    folder += L"\\updates";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

// Installers downloaded a day or more ago (installed, or not wanted).
void remove_old_downloads()
{
    const std::wstring folder = updates_folder();
    if (folder.empty()) return;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW((folder + L"\\obs-airplay-*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    FILETIME now_time;
    GetSystemTimeAsFileTime(&now_time);
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            ticks(now_time) - ticks(entry.ftLastWriteTime) > 24ull * 3600 * 10000000) {
            DeleteFileW((folder + L"\\" + entry.cFileName).c_str());  // fails while it runs
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

std::string sha256_of(HANDLE file, uint64_t* size)
{
    windows_update::Sha256 hash;
    std::vector<char> buffer(1 << 20);
    uint64_t total = 0;
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            return {};
        }
        if (read == 0) break;
        hash.update(buffer.data(), read);
        total += read;
    }
    *size = total;
    return hash.hex();
}

// Downloads, checks and starts the installer; returns why not, or empty.
std::string install(Http& http, const windows_update::Update& update)
{
    // The signature first: small, and without it the installer is useless.
    std::string signature;
    if (!http.get(widen(update.signature_url), windows_update::kMaxSignatureSize,
                  [&](const char* data, size_t size) {
                      signature.append(data, size);
                      return true;
                  })) {
        return "the release signature could not be downloaded.";
    }
    if (!windows_update::verify_signature(update, signature)) {
        return "the release is not signed by the OBS AirPlay project.";
    }

    const std::wstring folder = updates_folder();
    if (folder.empty()) return "there is no folder to download it to.";
    const std::wstring path = folder + L"\\" + widen(update.name);
    DeleteFileW(path.c_str());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "the download could not be saved.";
    bool written = http.get(widen(update.url), update.size, [&](const char* data, size_t size) {
        DWORD wrote = 0;
        return WriteFile(file, data, static_cast<DWORD>(size), &wrote, nullptr) && wrote == size;
    });
    CloseHandle(file);
    if (!written) {
        DeleteFileW(path.c_str());
        return "the download failed.";
    }

    // Opened again so nothing can change it (no write or delete sharing)
    // between this check and the installer starting.
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "the download could not be opened.";
    uint64_t size = 0;
    const std::string digest = sha256_of(file, &size);
    if (size != update.size || digest != update.sha256) {
        CloseHandle(file);
        DeleteFileW(path.c_str());
        return "the download does not match the published release.";
    }

    const std::wstring parameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS "
                                    L"/LOG /WAITPID=" + std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW execute = {};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";  // the installer writes to ProgramData and the firewall
    execute.lpFile = path.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    const BOOL started = ShellExecuteExW(&execute);
    const DWORD error = started ? 0 : GetLastError();
    CloseHandle(file);
    if (!started) {
        return error == ERROR_CANCELLED ? "Windows did not get permission to install it."
                                        : "the installer could not be started.";
    }
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return {};
}

void open_release_page()
{
    ShellExecuteW(nullptr, L"open", kLatestReleasePage, nullptr, nullptr, SW_SHOWNORMAL);
}

// One check, as the macOS helper's checkForUpdates.
void check_for_updates()
{
    if (!g_update_checks) {
        blog(LOG_INFO, "[Update] update checks are turned off");
        return;
    }
    UpdateState state = load_state();
    const int64_t now = now_seconds();
    if (state.snoozed_until > now) return;
    if (state.last_check && now - state.last_check < kCheckIntervalSeconds && now >= state.last_check) {
        return;
    }
    // Attempts count, not only successes: no hammering GitHub in an outage.
    state.last_check = now;
    save_state(state);
    remove_old_downloads();

    Http http;
    std::string json;
    if (!http.get(std::wstring(L"https://") + kLatestReleaseHost + kLatestReleasePath,
                  kMaxReleaseJson,
                  [&](const char* data, size_t size) {
                      json.append(data, size);
                      return true;
                  },
                  true)) {
        if (!stopping()) blog(LOG_INFO, "[Update] the release check failed");
        return;
    }
    obs_data_t* release = obs_data_create_from_json(json.c_str());
    if (!release) return;
    const std::string tag = obs_data_get_string(release, "tag_name");
    std::vector<windows_update::ReleaseAsset> assets;
    obs_data_array_t* list = obs_data_get_array(release, "assets");
    for (size_t i = 0; list && i < obs_data_array_count(list); ++i) {
        obs_data_t* item = obs_data_array_item(list, i);
        windows_update::ReleaseAsset asset;
        asset.name = obs_data_get_string(item, "name");
        asset.url = obs_data_get_string(item, "browser_download_url");
        const long long size = obs_data_get_int(item, "size");
        asset.size = size > 0 ? static_cast<uint64_t>(size) : 0;
        asset.digest = obs_data_get_string(item, "digest");
        assets.push_back(asset);
        obs_data_release(item);
    }
    obs_data_array_release(list);
    obs_data_release(release);

    const std::string latest = windows_update::plain_version(tag);
    if (!windows_update::is_newer(latest, PLUGIN_VERSION) || state.skipped_version == latest) return;
    const auto update = windows_update::installable(tag, assets);
    blog(LOG_INFO, "[Update] %s is available (%s)", latest.c_str(),
         update ? "signed installer found" : "no installable Windows asset");

    // Never in the middle of a show.
    while (g_outputs_active || !g_update_checks) {
        if (!g_update_checks || !sleep_unless_stopping(kBusyRetry)) return;
    }
    if (stopping()) return;

    switch (prompt(latest, update.has_value())) {
    case Choice::Install: {
        const std::string failure = install(http, *update);
        if (stopping()) return;
        if (failure.empty()) {
            blog(LOG_INFO, "[Update] the %s installer is waiting for OBS to quit", latest.c_str());
            message_box(L"OBS AirPlay " + widen(latest) +
                            L" is ready. It will be installed when you quit OBS.",
                        MB_OK | MB_ICONINFORMATION);
        } else {
            blog(LOG_WARNING, "[Update] %s not installed: %s", latest.c_str(), failure.c_str());
            if (message_box(L"OBS AirPlay " + widen(latest) + L" could not be installed: " +
                                widen(failure) +
                                L" Nothing was changed.\n\nOpen the release page to install it?",
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                open_release_page();
            }
        }
        break;
    }
    case Choice::ViewRelease:
        open_release_page();
        break;
    case Choice::Later:
        state.snoozed_until = now_seconds() + kSnoozeSeconds;
        save_state(state);
        break;
    case Choice::Skip:
        state.skipped_version = latest;
        save_state(state);
        break;
    case Choice::None:
        break;
    }
}

void update_thread_func()
{
    if (!sleep_unless_stopping(kFirstCheckDelay)) return;
    check_for_updates();
}

} // namespace

bool start_airplay_ble_helper()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running) return true;
    g_running = true;
    g_quit = false;
    g_ble_thread = std::thread(ble_thread_func);
    g_update_thread = std::thread(update_thread_func);
    return true;
}

void stop_airplay_ble_helper()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running) return;
        g_quit = true;
        // Cancels a request in progress and closes a prompt still open.
        if (g_request) {
            WinHttpCloseHandle(g_request);
            g_request = nullptr;
        }
        if (g_dialog) PostMessageW(g_dialog, WM_CLOSE, 0, 0);
    }
    g_cv.notify_all();
    if (g_ble_thread.joinable()) g_ble_thread.join();
    if (g_update_thread.joinable()) g_update_thread.join();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_running = false;
}

void set_helper_update_checks_enabled(bool enabled)
{
    g_update_checks = enabled;
}

void set_helper_obs_outputs_active(bool active)
{
    g_outputs_active = active;
}

std::string read_helper_bluetooth_state()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bluetooth_state;
}
