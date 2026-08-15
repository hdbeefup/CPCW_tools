// WRLD/WTHR — the map's named lighting/weather presets.
//
// This is NOT the SCHD/OBJT schema system that mapfile.cpp implements. WTHR is a
// hand-rolled slot pool of a fixed C struct, so it gets its own decoder:
//
//   WTHR + u32 size + u32 version(3) + u32 liveCount
//        + i32 freeHead, freeTail, listHead, listTail, capacity
//        + capacity x { i32 next, i32 prev, u8 isFree
//                       [, u32 recordVersion(13) + u16-prefixed name + 194 bytes] }
//
// The record body is read in STREAM order, which is *not* the struct/reflection
// order — SWeather::Load (FUN_004f52a0) reads FogColor 8th and SunSpecular 12th.
// kWeatherFields[] below is that stream order; every consumer (decoder, writer,
// UI, harness) iterates it, so the order exists in exactly one place.
//
// Verified on the shipped corpus: the walk consumes each WTHR chunk to its exact
// end on 45/45 maps, 219 live records. See docs/MAP_FORMAT.md §11.
#pragma once
#include "scene.h"
#include <string>
#include <vector>

enum WFieldKind {
    WFK_BOOL,    // u8, 0/1
    WFK_FLOAT,   // 1 float
    WFK_VEC2,    // 2 floats
    WFK_DIR,     // 3 floats, a unit vector
    WFK_RGBA,    // 4 floats, colour (w is NOT always an alpha — see SunShadow)
    WFK_U32,     // u32 count
};

struct WeatherField {
    const char* name;
    int         tail;       // byte offset inside the 194-byte record body
    WFieldKind  kind;
    int         comps;      // 1..4
    int         engineOff;  // offset in SWeather, for docs and tooltips
    const char* group;      // UI grouping
    const char* note;       // shown as a tooltip; nullptr when unremarkable
};

extern const WeatherField kWeatherFields[];
extern const int          kWeatherFieldCount;
const int kWeatherRecordBytes = 194;

// Decode WRLD/WTHR out of `raw` into `s.weather` (+ wthrOff/weatherCap/
// weatherFree/weatherActive). Leaves s.weather empty and s.wthrOff = -1 when the
// chunk is absent, is chunk version 2 (a different, flat layout), or fails to
// walk exactly — this decoder never guesses at a partial pool.
void parse_weather(const std::vector<unsigned char>& raw, Scene& s);

// Write every preset's dirty fields back into `b` in place. Size-preserving, so
// the save stays byte-faithful; only the dirty fields' bytes change.
void apply_weather_inplace(const Scene& s, std::vector<unsigned char>& b);

// A generic read of the same slot-pool header, so CAMS and PATH can be checked
// with the same walker. `slotHdrs` gets {next, prev, isFree} per slot.
struct PoolHeader { long version = 0, live = 0, freeHead = 0, freeTail = 0,
                    listHead = 0, listTail = 0, capacity = 0; };
struct PoolSlot   { long next = 0, prev = 0; bool free = true; long recOff = -1; };
bool read_pool_header(const std::vector<unsigned char>& raw, long chunkOff,
                      PoolHeader& hdr, std::vector<PoolSlot>& slots);
