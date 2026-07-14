// Native CPCW .pak reader — the whole archive is byte-rotated (+0xBD to decrypt);
// a footer gives the INFO directory of folders/files; file data is raw (type 1)
// or zlib (type 2). Lets the editor read maps/models/textures/ProtoDB straight
// from the game's .pak files. Port of cpcw_pak.py; see docs/FORMAT_PAK.md.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct PakEntry { uint32_t offset=0, zsize=0, type=0, size=0; };

class PakArchive {
public:
    bool open(const std::string& path);                 // read footer + INFO index
    bool has(const std::string& name) const;            // name matched case-insensitively
    std::vector<unsigned char> read(const std::string& name) const;  // decrypt (+ inflate)
    size_t count() const { return index.size(); }
    const std::map<std::string, PakEntry>& all() const { return index; }
private:
    std::string pakPath;
    std::map<std::string, PakEntry> index;   // pak_norm(name) -> entry
};

// lower-case + backslashes -> '/', for case/style-insensitive lookup
std::string pak_norm(const std::string& s);
