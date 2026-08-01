#include "vfs.h"
#include "pak.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
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

// Mount every game archive in `dir`. vfs_resolve returns the FIRST pak holding an
// entry, so mount order is priority: later-numbered mainN paks patch earlier ones
// (the Steam install ships main3.pak, dated after main1/main2), then the
// localisation pak, then anything else found.
//
// `main3-beta.pak` / `main3-thqfin.pak` / `main3-thqv1.pak` are *alternate
// distribution builds*, not additive content — mounting them alongside main3 would
// mix builds, so names of the form main<N>-<variant> are skipped.
void vfs_mount_dir(const std::string& dir) {
    struct Cand { int tier; int rank; std::string path; };
    std::vector<Cand> cands;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file(ec)) continue;
        std::string name = de.path().filename().string(), lower;
        for (char c : name) lower += (char)tolower((unsigned char)c);
        if (lower.size() < 5 || lower.compare(lower.size()-4, 4, ".pak") != 0) continue;
        std::string stem = lower.substr(0, lower.size()-4);
        if (stem.compare(0, 4, "main") == 0 && stem.size() > 4) {
            size_t d = 4; while (d < stem.size() && isdigit((unsigned char)stem[d])) d++;
            if (d == stem.size() && d > 4) {                 // main<N>.pak
                cands.push_back({0, -atoi(stem.c_str()+4), de.path().string()});
                continue;
            }
            if (d > 4) continue;                             // main<N>-<variant>.pak: skip
        }
        if (stem == "enus") { cands.push_back({1, 0, de.path().string()}); continue; }
        cands.push_back({2, 0, de.path().string()});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.path < b.path;
    });
    for (const auto& c : cands) {
        PakArchive a;
        if (!a.open(c.path)) continue;
        fprintf(stderr, "vfs: mounted %s (%zu entries)\n", c.path.c_str(), a.count());
        g_paks.push_back(std::move(a));
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
