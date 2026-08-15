// Versioned key/value settings, stored beside imgui.ini.
//
// imgui.ini already persists the dock layout, and nothing else survived a
// restart: panel visibility, snap steps, brush parameters, favourites, the data
// root and the recent-map list were all lost on exit.
//
// Format is one `key value` per line, `#` comments, values run to end of line so
// paths with spaces work unquoted. Unknown keys are PRESERVED on save, so an
// older build cannot silently drop a newer build's settings. `version` gets one
// `if (v < N)` migration block per bump.
#pragma once
#include <string>
#include <vector>

class Settings {
public:
    // Load from `path`. Missing file is not an error — everything keeps its
    // default and the file appears on the first save.
    void load(const std::string& path);
    bool save(const std::string& path) const;

    // Typed accessors. `get*` returns `def` when the key is absent or unparsable,
    // so a corrupt line degrades to the default instead of a zero.
    bool        getBool (const char* key, bool def) const;
    int         getInt  (const char* key, int def) const;
    float       getFloat(const char* key, float def) const;
    std::string getStr  (const char* key, const std::string& def = std::string()) const;

    void setBool (const char* key, bool v);
    void setInt  (const char* key, int v);
    void setFloat(const char* key, float v);
    void setStr  (const char* key, const std::string& v);

    // Recent maps, most recent first, de-duplicated, capped.
    std::vector<std::string> recentMaps() const;
    void pushRecentMap(const std::string& path);

    // Favourite prototype GUIDs (stored as one comma-separated value).
    std::vector<std::string> favourites() const;
    void setFavourites(const std::vector<std::string>& guids);

    static const int kVersion = 1;
    static const int kMaxRecent = 10;

private:
    struct Entry { std::string key, value; };
    std::vector<Entry> kv_;          // insertion-ordered, so saves stay stable
    const std::string* find(const char* key) const;
    void set(const char* key, const std::string& v);
    void migrate(int fromVersion);
};

// The settings file lives beside the executable, like the .pak auto-mount.
std::string settings_default_path();
