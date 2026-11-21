// Watches the NitLink capture card viewer window while the mouse is idle:
// finds where the action is in the game picture (10 times a second, on a tiny
// GPU-shrunk copy) and listens to NitLink's own audio for loud moments.
// Publishes a focus point and a "startle" counter for the gaze.
#pragma once
#include <unknwn.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace game {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
using WatchClock = std::chrono::steady_clock;

// What the gaze reads. x/y are 0..1 across the game picture, y down.
struct Focus {
    bool active = false, has = false;
    float x = 0.5f, y = 0.5f;
    unsigned startles = 0;
};

static std::wstring lower(std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s; }

static std::wstring processName(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if(!process) return {};
    wchar_t path[MAX_PATH]; DWORD size = MAX_PATH; std::wstring name;
    if(QueryFullProcessImageNameW(process, 0, path, &size)) name = std::filesystem::path(path).filename().wstring();
    CloseHandle(process);
    return name;
}

// NitLink's main window: its program name first, then a window title (not a
// browser, so a "NitLink" web page doesn't count); largest one wins.
static HWND findNitLink(DWORD& pid) {
    struct Search { HWND best = nullptr; long long area = -1; DWORD pid = 0; } search;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(l);
        if(!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        wchar_t title[256] = {}; GetWindowTextW(h, title, 256);
        DWORD owner = 0; GetWindowThreadProcessId(h, &owner);
        auto program = lower(processName(owner));
        bool byProgram = program.find(L"nitlink") != std::wstring::npos;
        bool browser = program == L"chrome.exe" || program == L"msedge.exe" || program == L"firefox.exe" || program == L"opera.exe" || program == L"brave.exe" || program == L"steamwebhelper.exe";
        if(!byProgram && (browser || lower(title).find(L"nitlink") == std::wstring::npos)) return TRUE;
        RECT r{}; GetWindowRect(h, &r);
        long long area = IsIconic(h) ? 0 : (long long)(r.right - r.left) * (r.bottom - r.top);
        if(byProgram) area += 1LL << 40;
        if(area > s.area) { s.area = area; s.best = h; s.pid = owner; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    pid = search.pid;
    return search.best;
}

// Grid the motion finder works on (16:9).
static constexpr int GridW = 48, GridH = 27;

// Windows Graphics Capture of one window. Each frame is shrunk on the GPU
// (mip chain) and only the ~150 px copy is read back, one call later so the
// CPU never waits for the GPU.
class WindowCapture {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wdx::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    winrt::com_ptr<ID3D11Texture2D> mips, staging;
    winrt::com_ptr<ID3D11ShaderResourceView> mipView;
    UINT texW = 0, texH = 0, level = 0, smallW = 0, smallH = 0;
    bool pending = false;
    HWND window = nullptr;

    bool makeTextures(UINT w, UINT h) {
        mips = nullptr; staging = nullptr; mipView = nullptr; pending = false;
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if(FAILED(device->CreateTexture2D(&d, nullptr, mips.put())) || FAILED(device->CreateShaderResourceView(mips.get(), nullptr, mipView.put()))) return false;
        level = 0; while((w >> level) > 192 && (w >> (level + 1)) > 0 && (h >> (level + 1)) > 0) ++level;
        smallW = std::max(1u, w >> level); smallH = std::max(1u, h >> level);
        D3D11_TEXTURE2D_DESC s{}; s.Width = smallW; s.Height = smallH; s.MipLevels = 1; s.ArraySize = 1; s.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        s.SampleDesc.Count = 1; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateTexture2D(&s, nullptr, staging.put()))) return false;
        texW = w; texH = h;
        return true;
    }
    // Box-average the small image into the grid as lifted luma (sqrt brings
    // out detail in dark games).
    void toGrid(const D3D11_MAPPED_SUBRESOURCE& m, std::vector<float>& grid) const {
        grid.assign(GridW * GridH, 0.f);
        for(int gy = 0; gy < GridH; ++gy) for(int gx = 0; gx < GridW; ++gx) {
            UINT x0 = gx * smallW / GridW, x1 = std::max(x0 + 1, (gx + 1) * smallW / GridW);
            UINT y0 = gy * smallH / GridH, y1 = std::max(y0 + 1, (gy + 1) * smallH / GridH);
            float sum = 0; int count = 0;
            for(UINT y = y0; y < y1 && y < smallH; ++y) {
                auto row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
                for(UINT x = x0; x < x1 && x < smallW; ++x) { auto p = row + x * 4; sum += (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) / 255.f; ++count; }
            }
            grid[gy * GridW + gx] = std::sqrt(count ? sum / count : 0.f);
        }
    }
public:
    ~WindowCapture() { stop(); }
    HWND target() const { return window; }
    bool start(HWND h) {
        stop();
        try {
            if(!device) {
                if(FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()))) return false;
                winrt::com_ptr<::IInspectable> inspectable;
                if(FAILED(CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(), inspectable.put()))) return false;
                winrtDevice = inspectable.as<wdx::Direct3D11::IDirect3DDevice>();
            }
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForWindow(h, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
            poolSize = item.Size();
            pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
            session = pool.CreateCaptureSession(item);
            try { session.IsCursorCaptureEnabled(false); } catch(...) {}
            // Windows 11 can drop the yellow capture border if allowed.
            using winrt::Windows::Foundation::Metadata::ApiInformation;
            if(ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
                try { wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get(); session.IsBorderRequired(false); } catch(...) {}
            }
            session.StartCapture();
            window = h;
            return true;
        } catch(...) { stop(); return false; }
    }
    void stop() {
        try { if(session) session.Close(); if(pool) pool.Close(); } catch(...) {}
        session = nullptr; pool = nullptr; item = nullptr; window = nullptr;
        mips = nullptr; staging = nullptr; mipView = nullptr; texW = texH = 0; pending = false;
    }
    // True when a new grid is ready.
    bool grab(std::vector<float>& grid) {
        if(!pool) return false;
        bool got = false;
        if(pending) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) { toGrid(m, grid); context->Unmap(staging.get(), 0); got = true; }
            pending = false;
        }
        try {
            wgc::Direct3D11CaptureFrame frame{nullptr};
            for(auto next = pool.TryGetNextFrame(); next; next = pool.TryGetNextFrame()) frame = next;
            if(!frame) return got;
            auto content = frame.ContentSize();
            if(content.Width < 16 || content.Height < 16) return got;
            if(content.Width != poolSize.Width || content.Height != poolSize.Height) {
                poolSize = content;
                pool.Recreate(winrtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
                return got;
            }
            winrt::com_ptr<ID3D11Texture2D> texture;
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
            D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
            if((d.Width != texW || d.Height != texH) && !makeTextures(d.Width, d.Height)) return got;
            context->CopySubresourceRegion(mips.get(), 0, 0, 0, 0, texture.get(), 0, nullptr);
            context->GenerateMips(mipView.get());
            context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, mips.get(), level, nullptr);
            pending = true;
        } catch(...) {}
        return got;
    }
};

// Finds the strongest local movement after cancelling camera pans, and
// spots scene cuts and frames where everything moves (fast camera turns).
class MotionFinder {
    std::vector<float> prev, residual, blurred;
    float at(const std::vector<float>& g, int x, int y) const { return g[std::clamp(y, 0, GridH - 1) * GridW + std::clamp(x, 0, GridW - 1)]; }
public:
    struct Result { bool valid = false, cut = false, chaotic = false; float x = 0.5f, y = 0.5f, strength = 0; };
    void reset() { prev.clear(); }
    Result analyse(const std::vector<float>& cur) {
        Result out;
        if(prev.size() != cur.size()) { prev = cur; return out; }
        out.valid = true;
        // Camera pan: the whole-picture shift that best lines up the frames.
        const int margin = 4; int bestDx = 0, bestDy = 0; float best = 1e9f, raw = 0;
        for(int dy = -margin; dy <= margin; ++dy) for(int dx = -margin; dx <= margin; ++dx) {
            float sad = 0;
            for(int y = margin; y < GridH - margin; ++y) for(int x = margin; x < GridW - margin; ++x)
                sad += std::abs(cur[y * GridW + x] - prev[(y + dy) * GridW + x + dx]);
            sad /= float((GridW - 2 * margin) * (GridH - 2 * margin));
            if(dx == 0 && dy == 0) raw = sad;
            if(sad < best - 1e-5f) { best = sad; bestDx = dx; bestDy = dy; }
        }
        // Scene cut or loading screen: nothing lines up.
        if(raw > 0.16f && best > 0.11f) { out.cut = true; prev = cur; return out; }
        // What still moves once the pan is removed. The player character sits
        // near the middle in third-person games, so that area counts less.
        residual.assign(cur.size(), 0.f); int moving = 0;
        for(int y = 0; y < GridH; ++y) for(int x = 0; x < GridW; ++x) {
            float r = std::max(0.f, std::abs(cur[y * GridW + x] - at(prev, x + bestDx, y + bestDy)) - 0.03f);
            float ex = (x + 0.5f) / GridW - 0.5f, ey = (y + 0.5f) / GridH - 0.62f;
            float centre = std::exp(-(ex * ex / (0.12f * 0.12f) + ey * ey / (0.2f * 0.2f)));
            residual[y * GridW + x] = r * (1 - 0.7f * centre);
            if(r > 0.05f) ++moving;
        }
        prev = cur;
        if(moving > GridW * GridH / 2) { out.chaotic = true; return out; }
        // Peak of the blurred motion, then a centroid around it so two
        // separate movements don't average into empty space.
        blurred.assign(cur.size(), 0.f); int peak = 0;
        for(int y = 0; y < GridH; ++y) for(int x = 0; x < GridW; ++x) {
            float sum = 0;
            for(int oy = -1; oy <= 1; ++oy) for(int ox = -1; ox <= 1; ++ox) sum += at(residual, x + ox, y + oy);
            blurred[y * GridW + x] = sum / 9;
            if(blurred[y * GridW + x] > blurred[peak]) peak = y * GridW + x;
        }
        int px = peak % GridW, py = peak / GridW; float wx = 0, wy = 0, wsum = 0;
        for(int y = std::max(0, py - 3); y <= std::min(GridH - 1, py + 3); ++y) for(int x = std::max(0, px - 4); x <= std::min(GridW - 1, px + 4); ++x) {
            float w = residual[y * GridW + x]; w *= w; wx += w * (x + 0.5f); wy += w * (y + 0.5f); wsum += w;
        }
        out.strength = blurred[peak];
        if(wsum > 0) { out.x = wx / wsum / GridW; out.y = wy / wsum / GridH; }
        return out;
    }
};

// Holds on to one point of interest for a while instead of darting around.
class Attention {
    float strength = 0; double held = 0;
public:
    bool has = false; float x = 0.5f, y = 0.5f;
    void drop() { has = false; strength = 0; }
    void update(const MotionFinder::Result& r, float dt) {
        strength *= std::exp(-dt / 0.8f); held += dt;
        if(!r.valid) return;
        if(r.cut) { drop(); return; }
        float cx = r.x, cy = r.y, cs = r.strength;
        if(r.chaotic) { cx = 0.5f; cy = 0.45f; cs = 0.03f; } // fast camera turn: watch the middle
        if(cs < 0.025f) { if(strength < 0.015f) has = false; return; }
        float distance = std::hypot(cx - x, cy - y);
        if(!has || (held > 1.0 && cs > strength * 0.8f) || cs > strength * 2.0f) {
            if(!has || distance > 0.12f) held = 0;
            has = true; x = cx; y = cy; strength = cs;
        } else if(distance < 0.12f) {
            // Same thing moving a little: track it.
            x += (cx - x) * 0.35f; y += (cy - y) * 0.35f; strength = std::max(strength, cs);
        }
    }
};

// Loopback capture of only NitLink's audio (and its child processes).
class AudioWatch {
    // winrt::implements is agile by default, as process loopback activation requires.
    struct Handler : winrt::implements<Handler, IActivateAudioInterfaceCompletionHandler> {
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Handler() { CloseHandle(done); }
        STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation*) override { SetEvent(done); return S_OK; }
    };
    std::thread thread;
    std::atomic<bool> quit{false};
    void run(DWORD target) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = target;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT activation{}; activation.vt = VT_BLOB; activation.blob.cbSize = sizeof(params); activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
        auto handler = winrt::make_self<Handler>();
        winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
        if(FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation, handler.get(), operation.put()))) { audioOk = false; return; }
        WaitForSingleObject(handler->done, 5000);
        HRESULT result = E_FAIL; winrt::com_ptr<::IUnknown> unknown;
        if(FAILED(operation->GetActivateResult(&result, unknown.put())) || FAILED(result) || !unknown) { audioOk = false; return; }
        auto client = unknown.as<IAudioClient>();
        WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2; format.nSamplesPerSec = 44100; format.wBitsPerSample = 16;
        format.nBlockAlign = 4; format.nAvgBytesPerSec = 44100 * 4;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        winrt::com_ptr<IAudioCaptureClient> capture;
        if(FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 200000, 0, &format, nullptr))
            || FAILED(client->SetEventHandle(ready)) || FAILED(client->GetService(__uuidof(IAudioCaptureClient), capture.put_void())) || FAILED(client->Start())) {
            audioOk = false; CloseHandle(ready); return;
        }
        audioOk = true;
        // 50 ms loudness windows; a spike well above the recent average is a "startle".
        double sum = 0; UINT32 count = 0; float slow = -60.f; auto lastStartle = WatchClock::now() - std::chrono::seconds(10);
        auto measure = [&](float db) {
            loudness = db;
            auto now = WatchClock::now();
            if(db > -32.f && db > slow + 9.f && now - lastStartle > std::chrono::milliseconds(2500)) { ++startles; lastStartle = now; }
            slow += (db - slow) * 0.0125f; // ~4 s average at 20 windows per second
        };
        while(!quit) {
            if(WaitForSingleObject(ready, 100) == WAIT_TIMEOUT) { measure(-90.f); continue; }
            UINT32 frames = 0;
            while(SUCCEEDED(capture->GetNextPacketSize(&frames)) && frames) {
                BYTE* data = nullptr; DWORD flags = 0;
                if(FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                auto samples = reinterpret_cast<const int16_t*>(data);
                for(UINT32 i = 0; i < frames * 2; ++i) {
                    double v = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0 : samples[i] / 32768.0;
                    sum += v * v;
                    if(++count >= 4410) { measure(float(10 * std::log10(sum / count + 1e-9))); sum = 0; count = 0; }
                }
                capture->ReleaseBuffer(frames);
            }
        }
        client->Stop(); CloseHandle(ready);
    }
public:
    std::atomic<unsigned> startles{0};
    std::atomic<float> loudness{-90.f};
    std::atomic<bool> audioOk{false};
    DWORD pid = 0;
    ~AudioWatch() { stop(); }
    void start(DWORD process) { stop(); pid = process; quit = false; thread = std::thread([this, process] { run(process); }); }
    void stop() { quit = true; if(thread.joinable()) thread.join(); pid = 0; audioOk = false; loudness = -90.f; }
};

// Runs the window watch and audio on their own threads.
class Watcher {
    std::thread thread;
    std::atomic<bool> quit{false};
    std::mutex lock;
    Focus focus;
    std::wstring status = L"NitLink: off";
    std::filesystem::path logFile;
    void setStatus(const std::wstring& s) { std::lock_guard<std::mutex> g(lock); status = s; }
    void run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        WindowCapture capture; MotionFinder motion; Attention attention; AudioWatch audio;
        std::vector<float> grid;
        auto lastSearch = WatchClock::now() - std::chrono::seconds(10), lastLog = WatchClock::now(), last = WatchClock::now();
        // Measurement log for tuning and performance checks.
        double analyseMs = 0, peakSum = 0; int ticks = 0, frames = 0, cuts = 0, chaotic = 0, tracked = 0;
        FILETIME c, e, kernel0, user0; GetProcessTimes(GetCurrentProcess(), &c, &e, &kernel0, &user0);
        auto ftSeconds = [](FILETIME f) { return (double(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) / 1e7; };
        while(!quit) {
            auto tickStart = WatchClock::now();
            float dt = std::clamp(std::chrono::duration<float>(tickStart - last).count(), 0.01f, 0.5f); last = tickStart;
            if(!enabled) {
                if(capture.target()) { capture.stop(); audio.stop(); motion.reset(); attention.drop(); }
                { std::lock_guard<std::mutex> g(lock); focus = {}; status = L"NitLink: watching is off"; }
                Sleep(250); continue;
            }
            HWND window = capture.target();
            if(window && !IsWindow(window)) { capture.stop(); audio.stop(); motion.reset(); attention.drop(); window = nullptr; }
            if(!window && tickStart - lastSearch > std::chrono::seconds(2)) {
                lastSearch = tickStart; DWORD pid = 0;
                HWND found = findNitLink(pid);
                if(found && capture.start(found)) { window = found; if(pid != audio.pid) audio.start(pid); }
            }
            bool minimised = window && IsIconic(window);
            if(window && !minimised && capture.grab(grid)) {
                auto t0 = WatchClock::now();
                auto result = motion.analyse(grid);
                attention.update(result, dt);
                analyseMs += std::chrono::duration<double, std::milli>(WatchClock::now() - t0).count();
                ++frames; cuts += result.cut; chaotic += result.chaotic; tracked += attention.has; peakSum += result.strength;
            } else if(minimised) { motion.reset(); attention.drop(); }
            ++ticks;
            {
                std::lock_guard<std::mutex> g(lock);
                focus.active = window != nullptr && !minimised;
                focus.has = focus.active && attention.has;
                focus.x = attention.x; focus.y = attention.y;
                focus.startles = audio.startles;
                status = !window ? L"NitLink: not found (open it in a window)"
                    : minimised ? L"NitLink: minimised, can't see the game"
                    : std::wstring(L"NitLink: watching") + (audio.audioOk ? L" and listening" : L" (no audio)");
            }
            if(tickStart - lastLog > std::chrono::seconds(30)) {
                FILETIME kernel1, user1; GetProcessTimes(GetCurrentProcess(), &c, &e, &kernel1, &user1);
                double busy = ftSeconds(kernel1) + ftSeconds(user1) - ftSeconds(kernel0) - ftSeconds(user0);
                double wall = std::chrono::duration<double>(tickStart - lastLog).count();
                SYSTEM_INFO info; GetSystemInfo(&info);
                if(std::ofstream log{logFile, std::ios::app}) {
                    char line[400];
                    snprintf(line, sizeof(line), "frames %d/%d, analyse %.3f ms avg, cuts %d, fast-camera %d, focused %d%%, peak %.3f avg, loudness %.1f dB, startles %u, helper CPU %.2f%% of whole PC (%.1f%% of one core)\n",
                        frames, ticks, frames ? analyseMs / frames : 0.0, cuts, chaotic, frames ? tracked * 100 / frames : 0, frames ? peakSum / frames : 0.0,
                        audio.loudness.load(), audio.startles.load(), busy / wall / info.dwNumberOfProcessors * 100, busy / wall * 100);
                    log << line;
                }
                kernel0 = kernel1; user0 = user1; lastLog = tickStart;
                analyseMs = peakSum = 0; ticks = frames = cuts = chaotic = tracked = 0;
            }
            auto spent = WatchClock::now() - tickStart;
            auto wait = std::chrono::milliseconds(100) - std::chrono::duration_cast<std::chrono::milliseconds>(spent);
            if(wait.count() > 0) Sleep(DWORD(wait.count()));
        }
        capture.stop(); audio.stop();
    }
public:
    std::atomic<bool> enabled{true};
    ~Watcher() { stop(); }
    void start(const std::filesystem::path& log) { logFile = log; quit = false; thread = std::thread([this] { run(); }); }
    void stop() { quit = true; if(thread.joinable()) thread.join(); }
    Focus current() { std::lock_guard<std::mutex> g(lock); return focus; }
    std::wstring statusText() { std::lock_guard<std::mutex> g(lock); return status; }
};

} // namespace game
