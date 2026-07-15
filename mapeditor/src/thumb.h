// Native THMB thumbnail decoder — reads the preview image the game's authoring
// tool bakes into every .srm and returns it as RGBA for a GL texture. See
// thumb.cpp; format documented in docs/FORMAT_SRM.md.
#pragma once
#include <string>
#include <vector>

// Decode the THMB chunk of the .srm at `path`. On success fills w/h and `rgba`
// (w*h*4 bytes, top-to-bottom, opaque) and returns true. Returns false if the
// file is missing or has no usable thumbnail.
bool load_thmb(const std::string& path, int& w, int& h,
               std::vector<unsigned char>& rgba);
