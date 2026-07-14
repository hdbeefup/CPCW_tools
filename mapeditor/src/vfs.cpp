#include "vfs.h"
#include "pak.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::vector<PakArchive> g_paks;
static std::string g_cache;

static std::string cacheDir() {
    if (!g_cache.empty()) return g_cache;
    const char* t = getenv("TEMP"); if (!t) t = getenv("TMP"); if (!t) t = ".";
    g_cache = std::string(t) + "/cpcw_mapedit_cache";
    std::error_code ec; fs::create_directories(g_cache, ec);
    return g_cache;
}

void vfs_mount_dir(const std::string& dir) {
    for (const char* n : {"main1.pak", "main2.pak", "enUS.pak", "enus.pak"}) {
        std::string p = dir + "/" + n;
        std::error_code ec;
        if (fs::exists(p, ec)) { PakArchive a; if (a.open(p)) g_paks.push_back(std::move(a)); }
    }
}
bool vfs_any_mounted() { return !g_paks.empty(); }

static std::string sanitize(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) { if (c == '/' || c == '\\' || c == ':') c = '_'; r += c; }
    return r;
}

std::string vfs_resolve(const std::string& logical, const std::string& diskRoot) {
    std::error_code ec;
    if (!diskRoot.empty()) {
        std::string dp = diskRoot + "/" + logical;
        if (fs::exists(dp, ec) && fs::is_regular_file(dp, ec)) return dp;
    }
    for (auto& pak : g_paks) {
        if (!pak.has(logical)) continue;
        std::string tp = cacheDir() + "/" + sanitize(logical);
        if (!fs::exists(tp, ec)) {
            auto data = pak.read(logical);
            if (data.empty()) continue;
            std::ofstream f(tp, std::ios::binary);
            f.write((const char*)data.data(), (std::streamsize)data.size());
        }
        return tp;
    }
    return "";
}

std::vector<std::string> vfs_list_suffix(const std::string& suffix) {
    std::vector<std::string> out;
    std::string suf = pak_norm(suffix);
    for (auto& pak : g_paks)
        for (auto& kv : pak.all()) {
            const std::string& k = kv.first;   // already pak_norm'd (lower, '/')
            if (k.size() >= suf.size() &&
                k.compare(k.size() - suf.size(), suf.size(), suf) == 0)
                out.push_back(k);
        }
    return out;
}
