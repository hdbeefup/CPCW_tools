// Minimal DDS decoder (DXT1/3/5 + simple uncompressed) -> RGBA, top mip only.
// Ported from blendertools/SRM_Blender/dds.py (pixel-exact vs Pillow there).
#pragma once
#include <cstdint>
#include <vector>
#include <string>

struct DdsImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // width*height*4, R,G,B,A per px, top-down
    bool ok = false;
};

// Decode a .dds file from disk. On failure returns DdsImage{ok=false}.
DdsImage dds_load(const std::string& path);
