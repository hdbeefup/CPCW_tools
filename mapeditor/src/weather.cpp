// WTHR decoder/writer. See weather.h for the container layout and why the field
// table lives in exactly one place.
#include "weather.h"
#include <cstring>
#include <cmath>

// The stream order SWeather::Load reads, with each field's offset inside the
// 194-byte body. Deliberately NOT sorted by engine offset: the reflection table
// orders these differently, and a table in reflection order also sums to 194, so
// it would pass a "the walk consumed the chunk exactly" check on all 45 maps
// while decoding every colour into the wrong widget. The order is pinned by
// semantics instead — see --wthrtest.
const WeatherField kWeatherFields[] = {
  // name                tail kind       n  engine  group        note
  {"FogEnabled",           0, WFK_BOOL,  1, 0x08, "Fog",     nullptr},
  {"FogStart",             1, WFK_FLOAT, 1, 0x0c, "Fog",     nullptr},
  {"FogEnd",               5, WFK_FLOAT, 1, 0x10, "Fog",     nullptr},
  {"SunDirection",         9, WFK_DIR,   3, 0x2c, "Sun",
   "Unit vector, the direction light TRAVELS (component 1 is down, < 0 on 219/219 records)."},
  {"SunColor",            21, WFK_RGBA,  4, 0x38, "Colours",
   "Not clamped to 1.0 - shipped presets reach 2.78."},
  {"SunAmbient",          37, WFK_RGBA,  4, 0x48, "Colours",  nullptr},
  {"SunShadow",           53, WFK_RGBA,  4, 0x58, "Colours",
   "The 4th component is NOT an alpha: it varies 0.25 .. 2.58 across the corpus."},
  {"FogColor",            69, WFK_RGBA,  4, 0x1c, "Fog",      nullptr},
  {"Night",               85, WFK_BOOL,  1, 0x78, "Sky",      nullptr},
  {"WindDirection",       86, WFK_VEC2,  2, 0x7c, "Sky",      nullptr},
  {"CloudSpeed",          94, WFK_FLOAT, 1, 0x84, "Sky",      nullptr},
  {"SunSpecular",         98, WFK_RGBA,  4, 0x68, "Colours",  nullptr},
  {"CloudCover",         114, WFK_FLOAT, 1, 0x88, "Sky",      nullptr},
  {"CloudMovementDir",   118, WFK_VEC2,  2, 0x8c, "Sky",
   "Usually WindDirection * CloudSpeed, but 8 of 219 shipped records are not the exact "
   "product - never rewrite it automatically."},
  {"FogBottom",          126, WFK_FLOAT, 1, 0x14, "Fog",      nullptr},
  {"FogTop",             130, WFK_FLOAT, 1, 0x18, "Fog",      nullptr},
  {"EffectCount",        134, WFK_U32,   1, 0x00, "Effects",
   "4 on 219/219 records; it is the length of the Effects array that follows."},
  {"Effects",            138, WFK_RGBA,  4, 0xc0, "Effects",
   "Four floats. Which effect each slot drives is NOT established - shown as Effect 0..3."},
  {"Puddles",            154, WFK_FLOAT, 1, 0x94, "Sky",      nullptr},
  {"BloomMul",           158, WFK_FLOAT, 1, 0xa0, "Post",     nullptr},
  {"BloomAdd",           162, WFK_FLOAT, 1, 0xa4, "Post",     nullptr},
  {"SoftShadows",        166, WFK_FLOAT, 1, 0xb4, "Post",     nullptr},
  {"TimeOfTheDay",       170, WFK_FLOAT, 1, 0xb8, "Sky",
   "Hours; 4.0 .. 24.0 across the corpus."},
  {"Brightness",         174, WFK_FLOAT, 1, 0xa8, "Post",     nullptr},
  {"Contrast",           178, WFK_FLOAT, 1, 0xac, "Post",     nullptr},
  {"Saturation",         182, WFK_FLOAT, 1, 0xb0, "Post",     nullptr},
  // Read at record version 13 but absent from the engine's reflection table, so
  // they have no authored name. Do not invent one.
  {"unknown98",          186, WFK_FLOAT, 1, 0x98, "Unknown",  nullptr},
  {"unknown9c",          190, WFK_FLOAT, 1, 0x9c, "Unknown",  nullptr},
};
const int kWeatherFieldCount = (int)(sizeof(kWeatherFields) / sizeof(kWeatherFields[0]));

namespace {

struct R {
    const unsigned char* d; size_t n;
    bool has(size_t p, size_t k) const { return p + k <= n; }
    uint32_t u32(size_t p) const {
        return has(p,4) ? (uint32_t)(d[p] | (d[p+1]<<8) | (d[p+2]<<16) | ((uint32_t)d[p+3]<<24)) : 0; }
    int32_t  i32(size_t p) const { return (int32_t)u32(p); }
    uint16_t u16(size_t p) const { return has(p,2) ? (uint16_t)(d[p] | (d[p+1]<<8)) : 0; }
    float    f32(size_t p) const { uint32_t v = u32(p); float f; std::memcpy(&f, &v, 4); return f; }
};

// Locate WRLD/WTHR without re-walking the whole chunk tree: SCEN -> WRLD -> WTHR.
long find_wthr(const R& r, long& outSize) {
    if (!r.has(0, 8) || std::memcmp(r.d, "SCEN", 4) != 0) return -1;
    long scenEnd = 8 + (long)r.u32(4);
    long p = 12, wrld = -1, wrldEnd = 0;
    while (p + 8 <= scenEnd && r.has((size_t)p, 8)) {
        long sz = (long)r.u32((size_t)p + 4);
        if (std::memcmp(r.d + p, "WRLD", 4) == 0) { wrld = p; wrldEnd = p + 8 + sz; break; }
        p += 8 + sz;
    }
    if (wrld < 0) return -1;
    p = wrld + 20;                                  // WRLD sub-header: version + w + h
    while (p + 8 <= wrldEnd && r.has((size_t)p, 8)) {
        long sz = (long)r.u32((size_t)p + 4);
        if (std::memcmp(r.d + p, "WTHR", 4) == 0) { outSize = sz; return p; }
        p += 8 + sz;
    }
    return -1;
}

} // namespace

bool read_pool_header(const std::vector<unsigned char>& raw, long chunkOff,
                      PoolHeader& hdr, std::vector<PoolSlot>& slots) {
    hdr = PoolHeader{}; slots.clear();
    R r{ raw.data(), raw.size() };
    if (chunkOff < 0 || !r.has((size_t)chunkOff, 8)) return false;
    long size = (long)r.u32((size_t)chunkOff + 4);
    long end  = chunkOff + 8 + size;
    if (end > (long)raw.size()) return false;
    long p = chunkOff + 8;
    hdr.version = r.u32((size_t)p);
    hdr.live    = r.u32((size_t)p + 4);
    hdr.freeHead = r.i32((size_t)p + 8);
    hdr.freeTail = r.i32((size_t)p + 12);
    hdr.listHead = r.i32((size_t)p + 16);
    hdr.listTail = r.i32((size_t)p + 20);
    hdr.capacity = r.i32((size_t)p + 24);
    p += 28;
    // An empty pool ships as capacity 0 with (-1,-1,-1,-1) — several MPMission
    // maps carry CAMS and PATH exactly that way. That is valid, not a failure.
    if (hdr.capacity < 0 || hdr.capacity > 1 << 20) return false;
    slots.reserve((size_t)hdr.capacity);
    for (long i = 0; i < hdr.capacity; i++) {
        if (p + 9 > end) return false;
        PoolSlot s;
        s.next = r.i32((size_t)p);
        s.prev = r.i32((size_t)p + 4);
        s.free = r.d[p + 8] != 0;
        p += 9;
        s.recOff = s.free ? -1 : p;                 // the record follows its slot header
        slots.push_back(s);
        if (!s.free) return true;                   // record length is caller-specific
    }
    return true;
}

void parse_weather(const std::vector<unsigned char>& raw, Scene& s) {
    s.weather.clear();
    s.wthrOff = -1; s.weatherCap = 0; s.weatherFree = 0;
    s.weatherActive = -1; s.weatherEdited = false;
    if (raw.empty()) return;

    R r{ raw.data(), raw.size() };
    long size = 0;
    long off = find_wthr(r, size);
    if (off < 0) return;
    long end = off + 8 + size;
    if (end > (long)raw.size()) return;

    long p = off + 8;
    long version = (long)r.u32((size_t)p);
    // Chunk version 2 is a flat count+records list, not a pool (the engine routes
    // it through a different reader). Refuse rather than mis-stride it.
    if (version != 3) return;
    long live = (long)r.u32((size_t)p + 4);
    long cap  = (long)r.i32((size_t)p + 24);
    p += 28;
    if (cap < 0 || cap > 4096) return;

    std::vector<WeatherPreset> out;
    std::vector<long> nextOf((size_t)cap, -1);
    std::vector<int>  indexOfSlot((size_t)cap, -1);
    for (long i = 0; i < cap; i++) {
        if (p + 9 > end) return;
        long nxt = r.i32((size_t)p);
        bool free = r.d[p + 8] != 0;
        p += 9;
        nextOf[(size_t)i] = nxt;
        if (free) continue;
        if (p + 4 > end) return;
        long recVer = (long)r.u32((size_t)p); p += 4;
        // The reader is an `if (N < version)` cascade, so a lower record version
        // is a SHORTER record: the fixed 194 stride would silently desync.
        if (recVer != 13) return;
        if (p + 2 > end) return;
        long nameOff = p;
        int nlen = r.u16((size_t)p); p += 2;
        if (p + nlen > end) return;
        std::string name((const char*)raw.data() + p, (size_t)nlen); p += nlen;
        if (p + kWeatherRecordBytes > end) return;

        WeatherPreset wp;
        wp.name = name; wp.slot = (int)i;
        wp.nameOff = nameOff; wp.tailOff = p;
        wp.values.assign((size_t)kWeatherFieldCount, {0, 0, 0, 0});
        wp.dirty.assign((size_t)kWeatherFieldCount, 0);
        for (int f = 0; f < kWeatherFieldCount; f++) {
            const WeatherField& F = kWeatherFields[f];
            long q = p + F.tail;
            switch (F.kind) {
                case WFK_BOOL: wp.values[f][0] = (float)r.d[q]; break;
                case WFK_U32:  wp.values[f][0] = (float)r.u32((size_t)q); break;
                default: for (int c = 0; c < F.comps; c++) wp.values[f][c] = r.f32((size_t)q + 4 * c);
            }
        }
        indexOfSlot[(size_t)i] = (int)out.size();
        out.push_back(std::move(wp));
        p += kWeatherRecordBytes;
    }
    // The walk must land exactly on the chunk end. Anything else means the record
    // layout is wrong for this map, and a half-decoded pool is worse than none.
    if (p != end) return;
    if ((long)out.size() != live) return;

    // Present presets in list-chain order, which is the order the artists see —
    // it is not slot order (M_17's chain is 7 -> 0 -> 1 -> 4).
    std::vector<WeatherPreset> chained;
    chained.reserve(out.size());
    {
        long guard = 0, cur = (long)r.i32((size_t)(off + 8 + 16));   // listHead
        std::vector<char> seen((size_t)cap, 0);
        while (cur >= 0 && cur < cap && !seen[(size_t)cur] && guard++ <= cap) {
            seen[(size_t)cur] = 1;
            int idx = indexOfSlot[(size_t)cur];
            if (idx >= 0) chained.push_back(out[(size_t)idx]);
            cur = nextOf[(size_t)cur];
        }
    }
    s.weather = (chained.size() == out.size()) ? std::move(chained) : std::move(out);

    s.wthrOff = off;
    s.weatherCap = (int)cap;
    s.weatherFree = (int)(cap - live);
    // The engine binds the preset literally NAMED "Default" (LoadWeathers builds
    // that string and only assigns on a find-by-name hit). Never slot 0, never
    // the list head — M_17 has no "Default" at all, and (6) Breakthrough ships
    // both "Default" and "Default2".
    for (size_t i = 0; i < s.weather.size(); i++)
        if (s.weather[i].name == "Default") { s.weatherActive = (int)i; break; }
}

void apply_weather_inplace(const Scene& s, std::vector<unsigned char>& b) {
    if (!s.weatherEdited || s.weather.empty()) return;
    for (const WeatherPreset& wp : s.weather) {
        if (wp.tailOff < 0) continue;
        for (int f = 0; f < kWeatherFieldCount && f < (int)wp.dirty.size(); f++) {
            if (!wp.dirty[(size_t)f]) continue;
            const WeatherField& F = kWeatherFields[f];
            long q = wp.tailOff + F.tail;
            if (q < 0 || q + 4 * F.comps > (long)b.size()) continue;
            switch (F.kind) {
                case WFK_BOOL:
                    b[(size_t)q] = wp.values[(size_t)f][0] != 0.0f ? 1 : 0;
                    break;
                case WFK_U32: {
                    uint32_t v = (uint32_t)wp.values[(size_t)f][0];
                    std::memcpy(&b[(size_t)q], &v, 4);
                    break;
                }
                default:
                    for (int c = 0; c < F.comps; c++) {
                        float v = wp.values[(size_t)f][c];
                        std::memcpy(&b[(size_t)q + 4 * c], &v, 4);
                    }
            }
        }
    }
}
