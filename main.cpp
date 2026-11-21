// One-click launcher for VTube Studio plus a gaze helper plugin.
// Starts VTube Studio through Steam, connects to its local API (port 8001)
// and drives four custom inputs at 30 Hz:
//   CompanionLookX/Y  eyes and body: follow the mouse; once it has been still
//                     for 3 seconds, watch the action in the NitLink window
//                     (console games), or glance around when there is none
//   CompanionHeadY    head up/down: same as LookY, but looking up is halved
//   CompanionTilt     small idle head tilt
// Lives in the tray and exits when VTube Studio closes.
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include "GameWatch.hpp"
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static const wchar_t* VtsSteamUrl = L"steam://rungameid/1325860";
static const char* PluginName = "Lily Gaze Helper";
static const char* PluginDeveloper = "CodexCompanion";
static constexpr double IdleAfterSeconds = 3.0;
static constexpr UINT TrayMessage = WM_APP + 1;

static std::atomic<bool> stopping{false};
static std::mutex statusLock;
static std::wstring statusText = L"Starting";
static std::wstring pendingBalloon;
static HWND trayWindow = nullptr;
static fs::path dataDir;
static game::Watcher gameWatcher;

// settings.txt holds one line: watchGame=0 or 1.
static void loadSettings() { std::ifstream f(dataDir / L"settings.txt"); std::string line; std::getline(f, line); gameWatcher.enabled = line != "watchGame=0"; }
static void saveSettings() { std::ofstream(dataDir / L"settings.txt") << (gameWatcher.enabled ? "watchGame=1" : "watchGame=0"); }

static void setStatus(const std::wstring& text, bool balloon = false) {
    std::lock_guard<std::mutex> lock(statusLock);
    statusText = text;
    if(balloon) pendingBalloon = text;
}

static bool vtsRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)}; bool found = false;
    for(BOOL ok = Process32FirstW(snap, &entry); ok && !found; ok = Process32NextW(snap, &entry))
        found = _wcsicmp(entry.szExeFile, L"VTube Studio.exe") == 0;
    CloseHandle(snap);
    return found;
}

// Minimal JSON field reader for the flat replies this helper needs.
static std::string field(const std::string& json, const std::string& key, size_t from = 0) {
    size_t at = json.find("\"" + key + "\"", from); if(at == std::string::npos) return {};
    at = json.find(':', at); if(at == std::string::npos) return {};
    ++at; while(at < json.size() && (json[at] == ' ' || json[at] == '\t' || json[at] == '\r' || json[at] == '\n')) ++at;
    if(at < json.size() && json[at] == '"') {
        std::string out;
        for(++at; at < json.size() && json[at] != '"'; ++at) { if(json[at] == '\\' && at + 1 < json.size()) ++at; out += json[at]; }
        return out;
    }
    size_t end = json.find_first_of(",}\r\n", at);
    return json.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

class Socket {
    HINTERNET session = nullptr, connection = nullptr, socket = nullptr;
    int nextId = 0;
public:
    ~Socket() { close(); }
    void close() {
        if(socket) { WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); WinHttpCloseHandle(socket); }
        if(connection) WinHttpCloseHandle(connection);
        if(session) WinHttpCloseHandle(session);
        socket = connection = session = nullptr;
    }
    bool open(INTERNET_PORT port) {
        close();
        session = WinHttpOpen(L"LilyVTubeLauncher", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if(!session) return false;
        // Long receive timeout: the token request waits for the user to click Allow.
        WinHttpSetTimeouts(session, 2000, 2000, 2000, 180000);
        connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
        if(!connection) return false;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if(!request) return false;
        bool ok = WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
            && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(request, nullptr);
        if(ok) socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        WinHttpCloseHandle(request);
        return socket != nullptr;
    }
    bool call(const char* type, const std::string& data, std::string& reply) {
        std::ostringstream message;
        message << "{\"apiName\":\"VTubeStudioPublicAPI\",\"apiVersion\":\"1.0\",\"requestID\":\"r" << nextId++
                << "\",\"messageType\":\"" << type << "\",\"data\":" << data << "}";
        auto text = message.str();
        if(WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, text.data(), DWORD(text.size())) != NO_ERROR) return false;
        reply.clear(); char buffer[8192];
        for(;;) {
            DWORD read = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE kind;
            if(WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &read, &kind) != NO_ERROR) return false;
            if(kind == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
            reply.append(buffer, read);
            if(kind == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || kind == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return true;
        }
    }
};

// VTube Studio's install folder: from the running program, else from Steam's
// library folders. Empty when it can't be found.
static fs::path vtsFolder() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof(entry)}; fs::path found;
        for(BOOL ok = Process32FirstW(snap, &entry); ok && found.empty(); ok = Process32NextW(snap, &entry)) {
            if(_wcsicmp(entry.szExeFile, L"VTube Studio.exe") != 0) continue;
            if(HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
                wchar_t path[MAX_PATH]; DWORD size = MAX_PATH;
                if(QueryFullProcessImageNameW(process, 0, path, &size)) found = fs::path(path).parent_path();
                CloseHandle(process);
            }
        }
        CloseHandle(snap);
        if(!found.empty()) return found;
    }
    wchar_t steam[MAX_PATH]; DWORD bytes = sizeof(steam);
    if(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &bytes) != ERROR_SUCCESS) return {};
    std::vector<fs::path> libraries{steam};
    std::ifstream file(fs::path(steam) / L"steamapps/libraryfolders.vdf", std::ios::binary);
    std::string vdf((std::istreambuf_iterator<char>(file)), {});
    for(size_t at = vdf.find("\"path\""); at != std::string::npos; at = vdf.find("\"path\"", at + 6)) {
        auto path = field(vdf, "path", at);
        if(!path.empty()) libraries.push_back(fs::u8path(path));
    }
    std::error_code ignored;
    for(auto& library : libraries) {
        auto folder = library / L"steamapps/common/VTube Studio";
        if(fs::exists(folder / L"VTube Studio.exe", ignored)) return folder;
    }
    return {};
}

// Same screen area VTube Studio uses for its own mouse inputs (its config), or
// the primary monitor when the config cannot be read.
struct MouseArea {
    double left = 0, top = 0, right = GetSystemMetrics(SM_CXSCREEN), bottom = GetSystemMetrics(SM_CYSCREEN);
    void load() {
        auto folder = vtsFolder(); if(folder.empty()) return;
        std::ifstream file(folder / L"VTube Studio_Data/StreamingAssets/Config/vts_config.json", std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(file)), {});
        auto value = [&](const char* key, double fallback) {
            size_t at = json.find(std::string("\"") + key + "\""); if(at == std::string::npos) return fallback;
            auto text = field(json, "Value", at); return text.empty() ? fallback : atof(text.c_str());
        };
        double l = value("Config_MouseXMin", left), r = value("Config_MouseXMax", right), t = value("Config_MouseYMin", top), b = value("Config_MouseYMax", bottom);
        if(r - l > 10 && b - t > 10) { left = l; right = r; top = t; bottom = b; }
    }
    // -1..1 with +x right and +y up, matching VTube Studio's MousePositionX/Y.
    void normalize(POINT p, float& x, float& y) const {
        x = float(std::clamp((p.x - left) / (right - left) * 2 - 1, -1.0, 1.0));
        y = float(std::clamp(1 - (p.y - top) / (bottom - top) * 2, -1.0, 1.0));
    }
};

class Gaze {
    double time = 0, glanceIn = 0;
    float weight = 1, weightV = 0, idleX = 0, idleY = 0, idleTilt = 0;
    float x = 0, y = 0, vx = 0, vy = 0, tilt = 0, tiltV = 0;
    float startle = 0, startleSide = 1;
    unsigned lastStartles = 0;
    uint32_t randomState = 0x629bf031;
    float random() { randomState ^= randomState << 13; randomState ^= randomState >> 17; randomState ^= randomState << 5; return float(randomState >> 8) / 16777216.f; }
    float sign() { return random() < 0.5f ? -1.f : 1.f; }
    // Exact critically damped spring: smooth at any frame rate, no overshoot from rest.
    static void smooth(float& value, float& velocity, float target, float rate, float dt) {
        float offset = value - target, impulse = velocity + rate * offset, decay = std::exp(-rate * dt);
        value = target + (offset + impulse * dt) * decay;
        velocity = (velocity - rate * impulse * dt) * decay;
    }
public:
    float lookX = 0, lookY = 0, headY = 0, headTilt = 0;
    explicit Gaze(uint32_t seed) { randomState = seed ? seed : 0x629bf031; }
    void update(float dt, float mouseX, float mouseY, bool idle, const game::Focus& focus) {
        time += dt;
        // A loud moment in the game: a quick little jolt of the head.
        if(focus.startles != lastStartles) { lastStartles = focus.startles; startle = 1; startleSide = sign(); }
        startle *= std::exp(-2.5f * dt);
        smooth(weight, weightV, idle ? 0.f : 1.f, idle ? 2.5f : 3.f, dt);
        weight = std::clamp(weight, 0.f, 1.f);
        if(!idle) {
            // Coming back from idle eases over about a second instead of
            // snapping: the idle point glides to the cursor while the blend
            // shifts to the live mouse. Once settled, output is the raw mouse.
            smooth(x, vx, mouseX, 3.5f, dt); smooth(y, vy, mouseY, 3.5f, dt);
            glanceIn = 0.6; idleTilt = 0;
        } else if(focus.has) {
            // Watching the console game: look at the action. She sits bottom-left
            // on stream, so the whole game picture maps to camera right (left
            // edge of the game = barely right of straight ahead).
            float gameX = 0.1f + focus.x * 0.8f;
            float gameY = std::clamp((1 - focus.y * 2) * 0.85f + 0.1f, -1.f, 1.f);
            smooth(x, vx, gameX, 4.5f, dt); smooth(y, vy, gameY, 4.5f, dt);
            glanceIn = 1.5; idleTilt = 0;
        } else {
            // Hold each glance for a few seconds, off-centre, sometimes with a tilt.
            glanceIn -= dt;
            if(glanceIn <= 0) {
                // With the game watched, glance mostly to camera right, towards
                // the game; only now and then a slight look left.
                if(focus.active) idleX = random() < 0.15f ? -0.1f * random() : 0.15f + random() * 0.55f;
                else idleX = sign() * (0.25f + random() * 0.35f);
                idleY = sign() * (0.05f + random() * 0.25f);
                idleTilt = random() > 0.6f ? sign() * (0.3f + random() * 0.4f) : 0.f;
                glanceIn = 2.8 + random() * 4.2;
            }
            smooth(x, vx, idleX, 3.f, dt); smooth(y, vy, idleY, 3.f, dt);
        }
        smooth(tilt, tiltV, idleTilt, 3.f, dt);
        float idleAmount = 1 - weight;
        lookX = std::clamp(weight * mouseX + idleAmount * (x + 0.04f * float(std::sin(time * 0.71))), -1.f, 1.f);
        lookY = std::clamp(weight * mouseY + idleAmount * (y + 0.03f * float(std::sin(time * 0.53)) + 0.12f * startle), -1.f, 1.f);
        headY = lookY > 0 ? lookY * 0.5f : lookY;
        headTilt = std::clamp((tilt + 0.45f * startleSide * startle) * idleAmount, -1.f, 1.f);
    }
};

static std::string readToken() { std::ifstream f(dataDir / L"token.txt"); std::string t; std::getline(f, t); return t; }
static void saveToken(const std::string& t) { std::ofstream(dataDir / L"token.txt") << t; }

static std::string pluginIdentity() { return std::string("\"pluginName\":\"") + PluginName + "\",\"pluginDeveloper\":\"" + PluginDeveloper + "\""; }

static bool authenticate(Socket& socket) {
    std::string reply, token = readToken();
    for(int attempt = 0; attempt < 2; ++attempt) {
        if(token.empty()) {
            setStatus(L"Click Allow in VTube Studio to let the gaze helper connect", true);
            if(!socket.call("AuthenticationTokenRequest", "{" + pluginIdentity() + "}", reply)) return false;
            token = field(reply, "authenticationToken");
            if(token.empty()) { setStatus(L"Connection was not allowed in VTube Studio. Exit and reopen the launcher to try again.", true); stopping = true; return false; }
            saveToken(token);
        }
        if(!socket.call("AuthenticationRequest", "{" + pluginIdentity() + ",\"authenticationToken\":\"" + token + "\"}", reply)) return false;
        if(field(reply, "authenticated") == "true") return true;
        token.clear(); // Revoked or stale: ask again once.
    }
    return false;
}

static bool createParameters(Socket& socket) {
    struct { const char* name; const char* explanation; } list[] = {
        {"CompanionLookX", "Gaze left/right: mouse, or idle glances"},
        {"CompanionLookY", "Gaze up/down: mouse, or idle glances"},
        {"CompanionHeadY", "Head up/down: like LookY with looking up halved"},
        {"CompanionTilt", "Small idle head tilt"},
    };
    std::string reply;
    for(auto& p : list) {
        std::string data = std::string("{\"parameterName\":\"") + p.name + "\",\"explanation\":\"" + p.explanation + "\",\"min\":-1,\"max\":1,\"defaultValue\":0}";
        if(!socket.call("ParameterCreationRequest", data, reply) || field(reply, "messageType") == "APIError") return false;
    }
    return true;
}

static void sleepFor(int ms) { for(int i = 0; i < ms / 50 && !stopping; ++i) Sleep(50); }

static DWORD WINAPI worker(void*) {
    if(!vtsRunning()) {
        setStatus(L"Starting VTube Studio");
        ShellExecuteW(nullptr, L"open", VtsSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);
        auto deadline = Clock::now() + std::chrono::minutes(3);
        while(!stopping && !vtsRunning() && Clock::now() < deadline) sleepFor(500);
        if(!vtsRunning()) { if(!stopping) setStatus(L"VTube Studio did not start. Open it from Steam, then run the launcher again.", true); sleepFor(8000); PostMessageW(trayWindow, WM_CLOSE, 0, 0); return 0; }
    }
    MouseArea area; area.load();
    Gaze gaze{static_cast<uint32_t>(GetTickCount64())};
    POINT lastCursor{}; GetCursorPos(&lastCursor); auto lastMove = Clock::now();
    bool announced = false;
    while(!stopping && vtsRunning()) {
        Socket socket;
        if(!socket.open(8001)) { setStatus(L"Waiting for VTube Studio to finish loading"); sleepFor(2000); continue; }
        if(!authenticate(socket) || !createParameters(socket)) { if(!stopping) { setStatus(L"Reconnecting to VTube Studio"); sleepFor(2000); } continue; }
        setStatus(L"Connected: following the mouse, watching NitLink or looking around after 3 s idle", !announced); announced = true;
        auto last = Clock::now(), next = last; std::string reply;
        while(!stopping) {
            auto now = Clock::now();
            if(now < next) { Sleep(DWORD(std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count()))); continue; }
            next = now + std::chrono::milliseconds(33);
            float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.001f, 0.1f); last = now;
            POINT cursor{};
            if(GetCursorPos(&cursor) && std::abs(cursor.x - lastCursor.x) + std::abs(cursor.y - lastCursor.y) >= 2) { lastCursor = cursor; lastMove = now; }
            float mouseX = 0, mouseY = 0; area.normalize(lastCursor, mouseX, mouseY);
            gaze.update(dt, mouseX, mouseY, std::chrono::duration<double>(now - lastMove).count() >= IdleAfterSeconds, gameWatcher.current());
            char data[400];
            snprintf(data, sizeof(data), "{\"faceFound\":false,\"mode\":\"set\",\"parameterValues\":[{\"id\":\"CompanionLookX\",\"value\":%.4f},{\"id\":\"CompanionLookY\",\"value\":%.4f},{\"id\":\"CompanionHeadY\",\"value\":%.4f},{\"id\":\"CompanionTilt\",\"value\":%.4f}]}",
                gaze.lookX, gaze.lookY, gaze.headY, gaze.headTilt);
            if(!socket.call("InjectParameterDataRequest", data, reply)) break;
        }
        if(!stopping) setStatus(L"Lost VTube Studio; reconnecting");
    }
    // VTube Studio closed: the launcher's job is done.
    PostMessageW(trayWindow, WM_CLOSE, 0, 0);
    return 0;
}

static LRESULT CALLBACK trayProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    static NOTIFYICONDATAW icon{};
    switch(msg) {
    case WM_CREATE: {
        icon.cbSize = sizeof(icon); icon.hWnd = h; icon.uID = 1; icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; icon.uCallbackMessage = TrayMessage;
        auto exe = vtsFolder() / L"VTube Studio.exe";
        icon.hIcon = ExtractIconW(GetModuleHandleW(nullptr), exe.c_str(), 0);
        if(!icon.hIcon || icon.hIcon == reinterpret_cast<HICON>(1)) icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(icon.szTip, L"Lily VTube launcher: starting");
        Shell_NotifyIconW(NIM_ADD, &icon);
        SetTimer(h, 1, 500, nullptr);
        return 0;
    }
    case WM_TIMER: {
        std::wstring status, balloon;
        { std::lock_guard<std::mutex> lock(statusLock); status = statusText; balloon.swap(pendingBalloon); }
        icon.uFlags = NIF_TIP;
        wcsncpy_s(icon.szTip, (L"Lily VTube: " + status).c_str(), _TRUNCATE);
        if(!balloon.empty()) { icon.uFlags |= NIF_INFO; wcscpy_s(icon.szInfoTitle, L"Lily VTube launcher"); wcsncpy_s(icon.szInfo, balloon.c_str(), _TRUNCATE); icon.dwInfoFlags = NIIF_INFO; }
        Shell_NotifyIconW(NIM_MODIFY, &icon);
        return 0;
    }
    case TrayMessage:
        if(LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP) {
            std::wstring status; { std::lock_guard<std::mutex> lock(statusLock); status = statusText; }
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, gameWatcher.statusText().c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING | (gameWatcher.enabled ? MF_CHECKED : 0), 2, L"Watch the game in NitLink when the mouse is idle");
            AppendMenuW(menu, MF_STRING, 1, L"Stop gaze helper (VTube Studio keeps running)");
            POINT p; GetCursorPos(&p); SetForegroundWindow(h);
            switch(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr)) {
            case 1: PostMessageW(h, WM_CLOSE, 0, 0); break;
            case 2: gameWatcher.enabled = !gameWatcher.enabled; saveSettings(); break;
            }
            DestroyMenu(menu);
        }
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: Shell_NotifyIconW(NIM_DELETE, &icon); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // Physical cursor pixels, like VTube Studio.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"LilyVTubeLauncher");
    if(GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: just make sure VTube Studio is open.
        if(!vtsRunning()) ShellExecuteW(nullptr, L"open", VtsSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    PWSTR local = nullptr;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) { dataDir = fs::path(local) / L"LilyVTubeLauncher"; CoTaskMemFree(local); }
    std::error_code ignored; fs::create_directories(dataDir, ignored);
    loadSettings();
    gameWatcher.start(dataDir / L"gamewatch.log");
    WNDCLASSW wc{}; wc.lpfnWndProc = trayProc; wc.hInstance = instance; wc.lpszClassName = L"LilyVTubeLauncherTray";
    RegisterClassW(&wc);
    // Hidden (never shown) top-level window, so the tray menu can take focus and dismiss properly.
    trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Lily VTube launcher", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    MSG msg;
    while(GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    stopping = true;
    // The worker may be blocked waiting for the Allow click; don't hang on exit.
    if(thread) { WaitForSingleObject(thread, 1500); CloseHandle(thread); }
    gameWatcher.stop();
    if(single) CloseHandle(single);
    return 0;
}
