// Tiny virtual filesystem over the game's .pak archives. Lets the path-based
// loaders (srm_parse / dds_load / protodb / load_map_native) work unchanged: a
// logical path resolves to a real disk path (extracted content), or is extracted
// from a mounted pak into a temp cache and that path is returned.
#pragma once
#include <string>
#include <vector>

void vfs_mount_dir(const std::string& dir);   // mount main1/main2/enUS.pak found in dir
bool vfs_any_mounted();

// Resolve a logical relative path (e.g. "Vehicles/Civilian/x.srm") to a real file
// path: diskRoot/logical if it exists on disk, else extracted-from-pak temp path,
// else "". `diskRoot` may be empty to skip the disk check.
std::string vfs_resolve(const std::string& logical, const std::string& diskRoot);

// All mounted-pak entry names ending with `suffix` (case-insensitive), e.g. ".map".
std::vector<std::string> vfs_list_suffix(const std::string& suffix);
