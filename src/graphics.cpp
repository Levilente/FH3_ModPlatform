// Graphics (API 18): the game's shaders as it creates them, and the game's own files.
//
// FH3 is an Xbox One game: it creates its shaders through a D3D11.x device, which Azimuth implements (and records into
// D3D12). Azimuth fills that device's method table at runtime, so the platform hooks the six Create*Shader methods
// themselves, inside azimuth.exe. Their places are known for Azimuth 1.0.4 (azimuth.exe of TimeDateStamp 0x6AE393EB);
// each one is checked against its first bytes before it is hooked, and with another Azimuth nothing is hooked at all.
// The methods take (this, bytecode, length, class linkage, out shader), like D3D11's.
#include "platform.hpp"
#include "strings.hpp"
#include "updates.hpp"

#include "MinHook.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fh3platform {

namespace {

namespace fs = std::filesystem;

using CreateShaderFn = HRESULT (*)(void* self, const void* code, SIZE_T size, void* linkage, void** out);

constexpr uint32_t kAzimuthStamp = 0x6AE393EBu; // Azimuth 1.0.4
constexpr uint32_t kAzimuthImage = 0x008A2000u;
// RVAs in azimuth.exe 1.0.4, by FH3_SHADER_... stage; found from the method table Azimuth builds at runtime (the code
// at azimuth+0x21A79D stores them at device method slots 12, 16, 17, 13, 15, 18).
constexpr std::array<uint32_t, 6> kCreateRva = {0x67050, 0x67AA0, 0x68500, 0x68F60, 0x699C0, 0x6A420};
constexpr const char* kStageName[6]          = {"vertex", "hull", "domain", "geometry", "pixel", "compute"};
constexpr unsigned char kPrologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57,
                                         0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

struct ShaderHook {
    FH3Mod* mod;
    int (*fn)(void*, FH3Shader*);
    void* user;
};

constexpr int kMaxHooks = 64;
ShaderHook g_hooks[kMaxHooks];
std::atomic<int> g_hook_count{0};
std::mutex g_add_mu;
int g_state = 0; // 0 = not tried, 1 = hooked, -1 = not available
std::array<CreateShaderFn, 6> g_original{};
std::atomic<uint64_t> g_created{0}, g_replaced{0};

std::mutex g_seen_mu;
std::set<uint64_t> g_refused; // shaders whose replacement Azimuth refused (logged once each)

std::string hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(v));
    return b;
}

HRESULT create(int stage, void* self, const void* code, SIZE_T size, void* linkage, void** out) {
    const CreateShaderFn original = g_original[stage];
    const int n                   = g_hook_count.load(std::memory_order_acquire);
    if (!n || !code || size < 20) return original(self, code, size, linkage, out);
    g_created.fetch_add(1, std::memory_order_relaxed);

    FH3Shader s{};
    s.size          = sizeof s;
    s.stage         = static_cast<uint32_t>(stage);
    s.bytecode      = code;
    s.bytecode_size = size;
    std::memcpy(s.hash, static_cast<const char*>(code) + 4, 16);
    std::memcpy(&s.hash64, s.hash, 8);
    std::vector<char> mine;
    for (int i = 0; i < n; ++i) {
        const ShaderHook& h = g_hooks[i];
        s.replacement       = nullptr;
        s.replacement_size  = 0;
        if (!h.fn(h.user, &s) || !s.replacement || s.replacement_size < 20 || s.replaced) continue;
        mine.assign(static_cast<const char*>(s.replacement), static_cast<const char*>(s.replacement) + s.replacement_size);
        s.replaced = 1;
    }
    if (mine.empty()) return original(self, code, size, linkage, out);
    const HRESULT hr = original(self, mine.data(), mine.size(), linkage, out);
    if (SUCCEEDED(hr)) {
        g_replaced.fetch_add(1, std::memory_order_relaxed);
        vplog("graphics: " + std::string(kStageName[stage]) + " shader " + hex64(s.hash64) + " replaced");
        return hr;
    }
    {
        std::scoped_lock lk{g_seen_mu};
        if (g_refused.insert(s.hash64).second)
            plog("graphics: Azimuth refused the replacement of " + std::string(kStageName[stage]) + " shader " + hex64(s.hash64) +
                 " (error 0x" + hex64(static_cast<uint32_t>(hr)).substr(8) + "); the game's own shader is used");
    }
    if (out) *out = nullptr;
    return original(self, code, size, linkage, out);
}

template <int Stage>
HRESULT detour(void* self, const void* code, SIZE_T size, void* linkage, void** out) {
    return create(Stage, self, code, size, linkage, out);
}
constexpr std::array<CreateShaderFn, 6> kDetours = {&detour<0>, &detour<1>, &detour<2>, &detour<3>, &detour<4>, &detour<5>};

// Hooks the six methods, once. Under g_add_mu.
bool start() {
    if (g_state) return g_state > 0;
    g_state = -1;
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    wchar_t exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!len || lstrcmpiW(fs::path{std::wstring{exe, len}}.filename().c_str(), L"azimuth.exe")) {
        plog("graphics: the game doesn't run through azimuth.exe here; the shader hooks are off");
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kAzimuthStamp || nt->OptionalHeader.SizeOfImage != kAzimuthImage) {
        char b[160];
        std::snprintf(b, sizeof b,
                      "graphics: this azimuth.exe (TimeDateStamp 0x%08lX) isn't Azimuth 1.0.4, the one the shader hooks know; "
                      "they are off",
                      static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        plog(b);
        return false;
    }
    for (std::size_t i = 0; i < kCreateRva.size(); ++i) {
        unsigned char got[16];
        if (!unpatched_bytes(base + kCreateRva[i], got, sizeof got) || std::memcmp(got, kPrologue, sizeof got)) {
            plog(std::string("graphics: Azimuth's Create") + kStageName[i] + "Shader isn't as expected; the shader hooks are off");
            return false;
        }
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        plog(std::string("graphics: MinHook didn't start: ") + MH_StatusToString(init));
        return false;
    }
    for (std::size_t i = 0; i < kCreateRva.size(); ++i) {
        void* tramp  = nullptr;
        MH_STATUS st = MH_CreateHook(base + kCreateRva[i], reinterpret_cast<void*>(kDetours[i]), &tramp);
        if (st == MH_OK) {
            g_original[i] = reinterpret_cast<CreateShaderFn>(tramp);
            st            = MH_EnableHook(base + kCreateRva[i]); // one by one: the mods' hooks use MinHook's queue under their own lock
        }
        if (st != MH_OK) {
            plog(std::string("graphics: couldn't hook Create") + kStageName[i] + "Shader: " + MH_StatusToString(st));
            for (std::size_t j = 0; j <= i; ++j) MH_RemoveHook(base + kCreateRva[j]);
            return false;
        }
    }
    g_state = 1;
    plog("graphics: watching the shaders the game creates (Azimuth 1.0.4)");
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

} // namespace

int api_add_shader_hook(FH3Mod* mod, int (*fn)(void*, FH3Shader*), void* user) {
    if (!mod || !fn) return 0;
    std::scoped_lock lk{g_add_mu};
    const int n = g_hook_count.load();
    for (int i = 0; i < n; ++i)
        if (g_hooks[i].mod == mod && reload_rebinding(mod)) { // hot reload: swap the callback in place
            g_hooks[i].fn   = fn;
            g_hooks[i].user = user;
            return 1;
        }
    if (n >= kMaxHooks) {
        log_line(mod->name, "add_shader_hook: too many shader hooks");
        return 0;
    }
    if (!start()) {
        log_line(mod->name, "add_shader_hook: shader hooks aren't available (see the platform's lines above)");
        return 0;
    }
    g_hooks[n] = {mod, fn, user};
    g_hook_count.store(n + 1, std::memory_order_release);
    log_line(mod->name, "added a shader hook");
    return 1;
}

void* api_read_game_file(const char* media_path, size_t* size) {
    if (size) *size = 0;
    if (!media_path || !*media_path) return nullptr;
    std::string p = media_path;
    for (auto& c : p)
        if (c == '\\') c = '/';
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    if (lower(p.substr(0, 6)) == "media/") p.erase(0, 6);
    if (p.empty() || p.find("..") != std::string::npos || p.find(':') != std::string::npos) return nullptr;
    const fs::path media = game_mount() / "media";
    std::vector<char> data;
    std::error_code ec;
    const fs::path plain = media / fs::path{widen(p)};
    if (fs::is_regular_file(plain, ec)) {
        if (FILE* f = _wfopen(plain.c_str(), L"rb")) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0) {
                data.resize(static_cast<std::size_t>(n));
                if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
            }
            std::fclose(f);
        }
    } else {
        // a/b/c: media\a.zip holding "b/c", then media\a\b.zip holding "c", ...
        for (std::size_t cut = p.find('/'); cut != std::string::npos && data.empty(); cut = p.find('/', cut + 1)) {
            const fs::path zip = media / fs::path{widen(p.substr(0, cut) + ".zip")};
            if (fs::is_regular_file(zip, ec)) data = unzip_entry(zip, p.substr(cut + 1));
        }
    }
    if (data.empty()) return nullptr;
    void* out = HeapAlloc(GetProcessHeap(), 0, data.size());
    if (!out) return nullptr;
    std::memcpy(out, data.data(), data.size());
    if (size) *size = data.size();
    return out;
}

void api_free_game_file(void* data) {
    if (data) HeapFree(GetProcessHeap(), 0, data);
}

} // namespace fh3platform
