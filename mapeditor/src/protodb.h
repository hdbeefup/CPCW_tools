// Native ProtoDB.bin reader — resolves entity Prototype GUIDs to model .srm
// paths. ProtoDB uses the same OBJS/OBJT/VOBJ/SCHD container as .map files.
#pragma once
#include <map>
#include <string>

// Build { guid(lower) -> model .srm path } for every prototype naming a model.
// Returns an empty map on failure.
std::map<std::string, std::string> protodb_model_index(const std::string& path);
