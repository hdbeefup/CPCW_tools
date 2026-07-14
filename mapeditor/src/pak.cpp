#include "pak.h"
#include <cstring>
#include <fstream>
#include <zlib.h>

std::string pak_norm(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) {
        if (c == '\\') c = '/';
        r += (char)tolower((unsigned char)c);
    }
    return r;
}

static inline void decrypt(unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)((p[i] + 0xBD) & 0xFF);
}
static inline uint32_t rd32(const unsigned char* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}
static inline uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool PakArchive::open(const std::string& path) {
    pakPath = path; index.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < 8) return false;

    unsigned char foot[8];
    f.seekg(size - 8); f.read((char*)foot, 8); decrypt(foot, 8);
    uint32_t infoSize = rd32(foot), ver = rd32(foot + 4);
    if (ver != 3) return false;
    std::streamoff infoOff = size - 8 - (std::streamoff)infoSize;
    if (infoOff < 0) return false;

    std::vector<unsigned char> info(infoSize);
    f.seekg(infoOff); f.read((char*)info.data(), infoSize); decrypt(info.data(), infoSize);

    size_t pos = 0;
    auto need = [&](size_t n) { return pos + n <= info.size(); };
    if (!need(4)) return false;
    uint32_t folders = rd32(&info[pos]); pos += 4;
    for (uint32_t fo = 0; fo < folders; fo++) {
        if (!need(2)) break;
        uint16_t nl = rd16(&info[pos]); pos += 2;
        if (!need(nl + 4)) break;
        pos += nl;                                   // folder name (grouping only)
        uint32_t files = rd32(&info[pos]); pos += 4;
        for (uint32_t fi = 0; fi < files; fi++) {
            if (!need(2)) break;
            uint16_t fnl = rd16(&info[pos]); pos += 2;
            if (!need((size_t)fnl + 16 + 8 + 4)) break;
            std::string name((const char*)&info[pos], fnl); pos += fnl;
            PakEntry e;
            e.offset = rd32(&info[pos]);            // offset
            e.zsize  = rd32(&info[pos + 8]);        // (skip dummy @+4)
            e.type   = rd32(&info[pos + 12]);
            pos += 16;
            pos += 8;                                // timestamp
            e.size   = rd32(&info[pos]); pos += 4;
            if (e.type == 1 || e.type == 2) index[pak_norm(name)] = e;
        }
    }
    return !index.empty();
}

bool PakArchive::has(const std::string& name) const {
    return index.find(pak_norm(name)) != index.end();
}

std::vector<unsigned char> PakArchive::read(const std::string& name) const {
    std::vector<unsigned char> out;
    auto it = index.find(pak_norm(name));
    if (it == index.end()) return out;
    const PakEntry& e = it->second;
    std::ifstream f(pakPath, std::ios::binary);
    if (!f) return out;
    f.seekg(e.offset);
    if (e.type == 1) {
        out.resize(e.size);
        f.read((char*)out.data(), e.size);
        decrypt(out.data(), out.size());
    } else if (e.type == 2) {
        if (e.zsize == 0) return out;
        std::vector<unsigned char> comp(e.zsize);
        f.read((char*)comp.data(), e.zsize);
        decrypt(comp.data(), comp.size());
        out.resize(e.size);
        uLongf destLen = e.size;
        if (uncompress(out.data(), &destLen, comp.data(), e.zsize) != Z_OK || destLen != e.size)
            out.clear();
    }
    return out;
}
