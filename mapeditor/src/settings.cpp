#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

std::string settings_default_path() {
#ifdef _WIN32
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        std::string p(exe);
        size_t sl = p.find_last_of("/\\");
        if (sl != std::string::npos) return p.substr(0, sl + 1) + "cpcw_mapeditor.ini";
    }
#endif
    return "cpcw_mapeditor.ini";
}

const std::string* Settings::find(const char* key) const {
    for (const Entry& e : kv_) if (e.key == key) return &e.value;
    return nullptr;
}

void Settings::set(const char* key, const std::string& v) {
    for (Entry& e : kv_) if (e.key == key) { e.value = v; return; }
    kv_.push_back({ key, v });
}

bool Settings::getBool(const char* key, bool def) const {
    const std::string* v = find(key);
    if (!v) return def;
    return *v == "1" || *v == "true" || *v == "yes";
}
int Settings::getInt(const char* key, int def) const {
    const std::string* v = find(key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    long r = strtol(v->c_str(), &end, 10);
    return (end && end != v->c_str()) ? (int)r : def;
}
float Settings::getFloat(const char* key, float def) const {
    const std::string* v = find(key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    double r = strtod(v->c_str(), &end);
    return (end && end != v->c_str()) ? (float)r : def;
}
std::string Settings::getStr(const char* key, const std::string& def) const {
    const std::string* v = find(key);
    return v ? *v : def;
}

void Settings::setBool (const char* key, bool v)  { set(key, v ? "1" : "0"); }
void Settings::setInt  (const char* key, int v)   { set(key, std::to_string(v)); }
void Settings::setFloat(const char* key, float v) {
    char b[64]; snprintf(b, sizeof(b), "%.6g", v); set(key, b);
}
void Settings::setStr  (const char* key, const std::string& v) { set(key, v); }

void Settings::load(const std::string& path) {
    kv_.clear();
    std::ifstream f(path);
    if (!f) return;                       // no file yet: defaults, saved on exit
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        size_t sp = line.find(' ', b);
        if (sp == std::string::npos) continue;
        std::string key = line.substr(b, sp - b);
        // Value runs to end of line, so a data root with spaces needs no quoting.
        std::string val = line.substr(sp + 1);
        size_t e = val.find_last_not_of(" \t");
        val = (e == std::string::npos) ? std::string() : val.substr(0, e + 1);
        if (!key.empty()) kv_.push_back({ key, val });
    }
    int v = getInt("version", 0);
    if (v != kVersion) { migrate(v); setInt("version", kVersion); }
}

void Settings::migrate(int fromVersion) {
    // One block per bump; they run in order and must be idempotent.
    // v0 = a file written before `version` existed. Nothing to rewrite yet, but
    // the block stays so the next bump has an obvious place to land.
    (void)fromVersion;
}

bool Settings::save(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# CPCW map editor settings. Written on exit; edit by hand if you like.\n"
      << "# Unknown keys are preserved, so an older build will not drop a newer\n"
      << "# build's settings.\n";
    bool wroteVersion = false;
    for (const Entry& e : kv_) {
        if (e.key == "version") wroteVersion = true;
        f << e.key << ' ' << e.value << '\n';
    }
    if (!wroteVersion) f << "version " << kVersion << '\n';
    return (bool)f;
}

std::vector<std::string> Settings::recentMaps() const {
    std::vector<std::string> out;
    for (int i = 0; i < kMaxRecent; i++) {
        char k[32]; snprintf(k, sizeof(k), "recent.%d", i);
        std::string v = getStr(k);
        if (!v.empty()) out.push_back(v);
    }
    return out;
}

void Settings::pushRecentMap(const std::string& path) {
    if (path.empty()) return;
    std::vector<std::string> r = recentMaps();
    for (size_t i = 0; i < r.size(); i++)
        if (r[i] == path) { r.erase(r.begin() + (long)i); break; }
    r.insert(r.begin(), path);
    if ((int)r.size() > kMaxRecent) r.resize(kMaxRecent);
    for (int i = 0; i < kMaxRecent; i++) {
        char k[32]; snprintf(k, sizeof(k), "recent.%d", i);
        set(k, i < (int)r.size() ? r[(size_t)i] : std::string());
    }
}

std::vector<std::string> Settings::favourites() const {
    std::vector<std::string> out;
    std::stringstream ss(getStr("browser.favourites"));
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) out.push_back(tok);
    return out;
}

void Settings::setFavourites(const std::vector<std::string>& guids) {
    std::string s;
    for (size_t i = 0; i < guids.size(); i++) { if (i) s += ','; s += guids[i]; }
    set("browser.favourites", s);
}
