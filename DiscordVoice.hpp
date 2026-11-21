// Knows when your collab partner is talking in your Discord voice call, through
// the Discord app's local RPC connection (named pipe discord-ipc-N). Needs a
// Discord application (client ID + secret, entered once, stored with DPAPI);
// Discord asks to authorize it the first time.
#pragma once
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace discord {

struct Member { std::string id, name; };

static std::wstring widen(const std::string& s) {
    if(s.empty()) return {};
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), int(out.size()));
    return out;
}
static std::string narrow(const std::wstring& s) {
    if(s.empty()) return {};
    std::string out(WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}

// --- Minimal JSON reading: values of top-level keys only, so nested fields
// with the same name never get picked up by mistake.

static void appendUtf8(std::string& out, uint32_t c) {
    if(c < 0x80) out += char(c);
    else if(c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
    else if(c < 0x10000) { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xF0 | c >> 18); out += char(0x80 | (c >> 12 & 0x3F)); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}
// Reads the string starting at json[at] == '"'; leaves at just past it.
static std::string readString(const std::string& json, size_t& at) {
    std::string out;
    for(++at; at < json.size() && json[at] != '"'; ++at) {
        if(json[at] != '\\' || at + 1 >= json.size()) { out += json[at]; continue; }
        char e = json[++at];
        if(e == 'n') out += '\n'; else if(e == 't') out += '\t'; else if(e == 'r') out += '\r';
        else if(e == 'u' && at + 4 < json.size()) {
            uint32_t c = std::stoul(json.substr(at + 1, 4), nullptr, 16); at += 4;
            if(c >= 0xD800 && c < 0xDC00 && at + 6 < json.size() && json[at + 1] == '\\' && json[at + 2] == 'u') {
                uint32_t low = std::stoul(json.substr(at + 3, 4), nullptr, 16);
                if(low >= 0xDC00 && low < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00); at += 6; }
            }
            appendUtf8(out, c);
        } else out += e;
    }
    ++at;
    return out;
}
// End (one past) of the value starting at json[at].
static size_t skipValue(const std::string& json, size_t at) {
    if(at >= json.size()) return at;
    if(json[at] == '"') { readString(json, at); return at; }
    if(json[at] != '{' && json[at] != '[') return json.find_first_of(",}]", at) == std::string::npos ? json.size() : json.find_first_of(",}]", at);
    int depth = 0;
    for(; at < json.size(); ++at) {
        char c = json[at];
        if(c == '"') { readString(json, at); --at; continue; }
        if(c == '{' || c == '[') ++depth;
        else if((c == '}' || c == ']') && --depth == 0) return at + 1;
    }
    return at;
}
static size_t skipSpace(const std::string& json, size_t at) { while(at < json.size() && strchr(" \t\r\n", json[at])) ++at; return at; }
// Value of a top-level key of the object `json`: strings unescaped, objects and
// arrays as raw text, null as empty.
static std::string member(const std::string& json, const std::string& key) {
    size_t at = skipSpace(json, 0);
    if(at >= json.size() || json[at] != '{') return {};
    for(++at;;) {
        at = skipSpace(json, at);
        if(at >= json.size() || json[at] != '"') return {};
        std::string name = readString(json, at);
        at = skipSpace(json, at); if(at >= json.size() || json[at] != ':') return {};
        at = skipSpace(json, at + 1);
        size_t end = skipValue(json, at);
        if(name == key) {
            if(json[at] == '"') { size_t p = at; return readString(json, p); }
            auto raw = json.substr(at, end - at);
            while(!raw.empty() && strchr(" \t\r\n", raw.back())) raw.pop_back();
            return raw == "null" ? std::string() : raw;
        }
        at = skipSpace(json, end);
        if(at >= json.size() || json[at] != ',') return {};
        ++at;
    }
}
// Objects of a JSON array given as raw text.
static std::vector<std::string> elements(const std::string& array) {
    std::vector<std::string> out;
    size_t at = skipSpace(array, 0);
    if(at >= array.size() || array[at] != '[') return out;
    for(++at;;) {
        at = skipSpace(array, at);
        if(at >= array.size() || array[at] == ']') return out;
        size_t end = skipValue(array, at);
        out.push_back(array.substr(at, end - at));
        at = skipSpace(array, end);
        if(at >= array.size() || array[at] != ',') return out;
        ++at;
    }
}
static std::string quote(const std::string& s) {
    std::string out = "\"";
    for(char c : s) { if(c == '"' || c == '\\') out += '\\'; if(uint8_t(c) >= 0x20) out += c; }
    return out + "\"";
}

// --- Credentials and tokens, encrypted for this Windows user.

struct Secrets { std::string clientId, clientSecret, accessToken, refreshToken; };

static bool saveSecrets(const std::filesystem::path& file, const Secrets& s) {
    std::string plain = s.clientId + "\n" + s.clientSecret + "\n" + s.accessToken + "\n" + s.refreshToken;
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(plain.data())}, out{};
    if(!CryptProtectData(&in, L"Auto-Vtuber Discord", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    SecureZeroMemory(plain.data(), plain.size());
    return true;
}
static Secrets loadSecrets(const std::filesystem::path& file) {
    Secrets s;
    std::ifstream f(file, std::ios::binary);
    std::string blob((std::istreambuf_iterator<char>(f)), {});
    DATA_BLOB in{DWORD(blob.size()), reinterpret_cast<BYTE*>(blob.data())}, out{};
    if(blob.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return s;
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData);
    std::string* parts[] = {&s.clientId, &s.clientSecret, &s.accessToken, &s.refreshToken};
    size_t at = 0;
    for(auto* part : parts) { size_t end = plain.find('\n', at); *part = plain.substr(at, end == std::string::npos ? std::string::npos : end - at); if(end == std::string::npos) break; at = end + 1; }
    return s;
}

// --- OAuth2 token exchange over HTTPS.

static std::string formEncode(const std::string& s) {
    std::string out; char hex[4];
    for(unsigned char c : s) {
        if(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { snprintf(hex, sizeof(hex), "%%%02X", c); out += hex; }
    }
    return out;
}
static std::string postToken(const std::string& body) {
    std::string reply;
    HINTERNET session = WinHttpOpen(L"Auto-Vtuber", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", L"/api/oauth2/token", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    const wchar_t* headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    if(request && WinHttpSendRequest(request, headers, DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0) && WinHttpReceiveResponse(request, nullptr)) {
        char buffer[4096]; DWORD read = 0;
        while(WinHttpReadData(request, buffer, sizeof(buffer), &read) && read) reply.append(buffer, read);
    }
    if(request) WinHttpCloseHandle(request);
    if(connection) WinHttpCloseHandle(connection);
    if(session) WinHttpCloseHandle(session);
    return reply;
}

// --- Discord's local IPC pipe: frames of [opcode, length] (little-endian
// 32-bit) followed by JSON.

class Pipe {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool readExact(char* out, DWORD size) {
        while(size) { DWORD got = 0; if(!ReadFile(handle, out, size, &got, nullptr) || !got) return false; out += got; size -= got; }
        return true;
    }
public:
    ~Pipe() { if(handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    bool open() {
        for(int n = 0; n < 10; ++n) {
            auto name = L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(n);
            handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if(handle != INVALID_HANDLE_VALUE) return true;
        }
        return false;
    }
    bool send(int32_t op, const std::string& body) {
        std::string frame(8, '\0');
        int32_t length = int32_t(body.size());
        memcpy(frame.data(), &op, 4); memcpy(frame.data() + 4, &length, 4);
        frame += body;
        DWORD written = 0;
        return WriteFile(handle, frame.data(), DWORD(frame.size()), &written, nullptr) && written == frame.size();
    }
    // One frame, waiting up to `ms`. got=false on timeout; returns false when the pipe broke.
    bool receive(int32_t& op, std::string& body, int ms, bool& got) {
        got = false;
        for(int waited = 0;; waited += 20) {
            DWORD available = 0;
            if(!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) return false;
            if(available >= 8) break;
            if(waited >= ms) return true;
            Sleep(20);
        }
        char header[8];
        if(!readExact(header, 8)) return false;
        int32_t length = 0; memcpy(&op, header, 4); memcpy(&length, header + 4, 4);
        if(length < 0 || length > (1 << 24)) return false;
        body.assign(size_t(length), '\0');
        if(length && !readExact(body.data(), DWORD(length))) return false;
        got = true;
        return true;
    }
};

class Voice {
    using Clock = std::chrono::steady_clock;
    std::thread thread;
    std::atomic<bool> quit{false}, reconnect{false}, needChannel{false}, declined{false};
    std::atomic<bool> talking{false};
    std::atomic<long long> stoppedAt{0};
    std::mutex lock;
    std::filesystem::path secretsFile;
    Secrets secrets;
    std::string selfId, channelId, partnerId;
    std::vector<Member> members;
    std::wstring status = L"Discord: starting";
    int nonce = 0;

    static long long nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
    void setStatus(const std::wstring& s) { std::lock_guard<std::mutex> g(lock); status = s; }
    void waitFor(int ms) { for(int i = 0; i < ms / 50 && !quit && !reconnect; ++i) Sleep(50); }

    void dispatch(const std::string& message) {
        auto evt = member(message, "evt"); auto data = member(message, "data");
        if(evt == "SPEAKING_START" || evt == "SPEAKING_STOP") {
            std::string id = member(data, "user_id"), partner;
            { std::lock_guard<std::mutex> g(lock); partner = partnerId; }
            if(!partner.empty() && id == partner) {
                talking = evt == "SPEAKING_START";
                if(!talking) stoppedAt = nowMs();
            }
        } else if(evt == "VOICE_CHANNEL_SELECT" || evt == "VOICE_STATE_CREATE" || evt == "VOICE_STATE_DELETE" || evt == "VOICE_STATE_UPDATE") {
            needChannel = true;
        }
    }
    // Sends a command and waits for its reply, handling events meanwhile.
    bool call(Pipe& pipe, const std::string& cmd, const std::string& args, std::string& reply, const std::string& evt = {}, int timeoutMs = 10000) {
        std::string id = std::to_string(++nonce);
        std::string body = "{\"cmd\":" + quote(cmd) + ",\"args\":" + args + (evt.empty() ? "" : ",\"evt\":" + quote(evt)) + ",\"nonce\":" + quote(id) + "}";
        if(!pipe.send(1, body)) return false;
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while(!quit && Clock::now() < deadline) {
            int32_t op = 0; std::string message; bool got = false;
            if(!pipe.receive(op, message, 200, got)) return false;
            if(!got) continue;
            if(op == 3) { pipe.send(4, message); continue; }
            if(op == 2) return false;
            if(member(message, "cmd") == "DISPATCH") { dispatch(message); continue; }
            if(member(message, "nonce") == id) { reply = message; return true; }
        }
        return false;
    }
    static bool failed(const std::string& reply) { return member(reply, "evt") == "ERROR"; }

    bool authenticate(Pipe& pipe, const std::string& token) {
        std::string reply;
        if(token.empty() || !call(pipe, "AUTHENTICATE", "{\"access_token\":" + quote(token) + "}", reply) || failed(reply)) return false;
        std::lock_guard<std::mutex> g(lock);
        selfId = member(member(member(reply, "data"), "user"), "id");
        return true;
    }
    bool storeTokens(const std::string& reply) {
        auto access = member(reply, "access_token"), refresh = member(reply, "refresh_token");
        if(access.empty()) return false;
        std::lock_guard<std::mutex> g(lock);
        secrets.accessToken = access;
        if(!refresh.empty()) secrets.refreshToken = refresh;
        saveSecrets(secretsFile, secrets);
        return true;
    }
    bool login(Pipe& pipe, const Secrets& s) {
        if(authenticate(pipe, s.accessToken)) return true;
        std::string base = "client_id=" + formEncode(s.clientId) + "&client_secret=" + formEncode(s.clientSecret);
        if(!s.refreshToken.empty() && storeTokens(postToken(base + "&grant_type=refresh_token&refresh_token=" + formEncode(s.refreshToken)))) {
            std::string token; { std::lock_guard<std::mutex> g(lock); token = secrets.accessToken; }
            if(authenticate(pipe, token)) return true;
        }
        setStatus(L"Discord: click Authorize in the Discord window");
        std::string reply;
        bool answered = call(pipe, "AUTHORIZE", "{\"client_id\":" + quote(s.clientId) + ",\"scopes\":[\"rpc\",\"identify\",\"rpc.voice.read\"]}", reply, {}, 180000);
        if(!answered || failed(reply)) {
            // Declined (or not allowed): don't pop the request up again until set up again.
            declined = answered;
            setStatus(L"Discord: not authorized. Use Set up Discord in the tray menu to try again.");
            return false;
        }
        auto code = member(member(reply, "data"), "code");
        // Discord's docs don't say whether RPC needs a redirect URI here; try
        // without, then with the one the README asks to register.
        auto exchange = base + "&grant_type=authorization_code&code=" + formEncode(code);
        if(!storeTokens(postToken(exchange)) && !storeTokens(postToken(exchange + "&redirect_uri=" + formEncode("http://localhost")))) {
            setStatus(L"Discord: token exchange failed. Check the client secret and the http://localhost redirect.");
            return false;
        }
        std::string token; { std::lock_guard<std::mutex> g(lock); token = secrets.accessToken; }
        return authenticate(pipe, token);
    }
    void subscribeChannel(Pipe& pipe, const std::string& channel, bool on) {
        std::string reply, args = "{\"channel_id\":" + quote(channel) + "}";
        for(auto evt : {"SPEAKING_START", "SPEAKING_STOP", "VOICE_STATE_CREATE", "VOICE_STATE_DELETE", "VOICE_STATE_UPDATE"})
            call(pipe, on ? "SUBSCRIBE" : "UNSUBSCRIBE", args, reply, evt);
    }
    void refreshChannel(Pipe& pipe) {
        needChannel = false;
        std::string reply;
        if(!call(pipe, "GET_SELECTED_VOICE_CHANNEL", "{}", reply)) return;
        auto data = member(reply, "data");
        auto id = member(data, "id");
        std::vector<Member> list; std::string self;
        { std::lock_guard<std::mutex> g(lock); self = selfId; }
        for(auto& state : elements(member(data, "voice_states"))) {
            auto user = member(state, "user");
            Member m{member(user, "id"), member(state, "nick")};
            if(m.name.empty()) m.name = member(user, "global_name");
            if(m.name.empty()) m.name = member(user, "username");
            if(!m.id.empty() && m.id != self) list.push_back(m);
        }
        if(id != channelId) {
            if(!channelId.empty()) subscribeChannel(pipe, channelId, false);
            channelId = id; talking = false;
            if(!channelId.empty()) subscribeChannel(pipe, channelId, true);
        }
        std::lock_guard<std::mutex> g(lock);
        members = list;
        status = channelId.empty() ? L"Discord: connected, not in a voice call" : L"Discord: in a voice call with " + std::to_wstring(list.size()) + (list.size() == 1 ? L" other person" : L" other people");
    }
    void session(Pipe& pipe, const Secrets& s) {
        reconnect = false; declined = false; channelId.clear();
        if(!pipe.send(0, "{\"v\":1,\"client_id\":" + quote(s.clientId) + "}")) return;
        // Wait for READY.
        for(auto deadline = Clock::now() + std::chrono::seconds(10);;) {
            if(quit || Clock::now() > deadline) return;
            int32_t op = 0; std::string message; bool got = false;
            if(!pipe.receive(op, message, 200, got)) { setStatus(L"Discord: connection refused (check the client ID)"); return; }
            if(got && op == 1 && member(message, "evt") == "READY") break;
        }
        if(!login(pipe, s)) {
            if(declined) while(!quit && !reconnect) Sleep(100);
            else waitFor(60000);
            return;
        }
        std::string reply;
        call(pipe, "SUBSCRIBE", "{}", reply, "VOICE_CHANNEL_SELECT");
        refreshChannel(pipe);
        while(!quit && !reconnect) {
            if(needChannel) refreshChannel(pipe);
            int32_t op = 0; std::string message; bool got = false;
            if(!pipe.receive(op, message, 200, got)) break;
            if(!got) continue;
            if(op == 3) pipe.send(4, message);
            else if(op == 2) break;
            else if(member(message, "cmd") == "DISPATCH") dispatch(message);
        }
        talking = false;
    }
    void run() {
        while(!quit) {
            Secrets s; { std::lock_guard<std::mutex> g(lock); s = secrets; }
            if(s.clientId.empty() || s.clientSecret.empty()) { setStatus(L"Discord: not set up"); reconnect = false; waitFor(1000); continue; }
            Pipe pipe;
            if(!pipe.open()) { setStatus(L"Discord: the Discord app isn't running"); waitFor(5000); continue; }
            setStatus(L"Discord: connecting");
            session(pipe, s);
            { std::lock_guard<std::mutex> g(lock); members.clear(); }
            if(!quit && !reconnect) { setStatus(L"Discord: disconnected, retrying"); waitFor(3000); }
        }
    }
public:
    ~Voice() { stop(); }
    void start(const std::filesystem::path& file) {
        secretsFile = file; secrets = loadSecrets(file);
        quit = false; thread = std::thread([this] { run(); });
    }
    void stop() { quit = true; if(thread.joinable()) thread.join(); }
    bool configured() { std::lock_guard<std::mutex> g(lock); return !secrets.clientId.empty() && !secrets.clientSecret.empty(); }
    std::string clientId() { std::lock_guard<std::mutex> g(lock); return secrets.clientId; }
    // New app credentials: forget old tokens and reconnect (Discord asks to authorize).
    void setCredentials(const std::string& id, const std::string& secret) {
        { std::lock_guard<std::mutex> g(lock); secrets = {id, secret, {}, {}}; saveSecrets(secretsFile, secrets); }
        reconnect = true;
    }
    void setPartner(const std::string& id) { { std::lock_guard<std::mutex> g(lock); partnerId = id; } talking = false; }
    std::vector<Member> people() { std::lock_guard<std::mutex> g(lock); return members; }
    std::wstring statusText() { std::lock_guard<std::mutex> g(lock); return status; }
    // Talking now, or stopped less than a moment ago (bridges pauses between words).
    bool partnerTalking() { return talking || nowMs() - stoppedAt < 1200; }
};

} // namespace discord
