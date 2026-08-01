// Native ProtoDB.bin reader — resolves entity Prototype GUIDs to model .srm
// paths. ProtoDB uses the same OBJS/OBJT/VOBJ/SCHD container as .map files.
#pragma once
#include <map>
#include <string>

// Build { guid(lower) -> model .srm path } for every prototype naming a model.
// Returns an empty map on failure.
std::map<std::string, std::string> protodb_model_index(const std::string& path);

// Everything the browser needs about one prototype.
struct ProtoInfo {
    std::string model;    // ModelName -> .srm path, forward slashes ("" if none)
    std::string name;     // designer-facing Name
    std::string schema;   // ProtoDB schema, e.g. "SPDoodad", "SPBuildingUnit"
};

// Full prototype index, including entries with no model. The map-side entity
// schema of a prototype is derivable from `schema`: ProtoDB "SP<X>" corresponds to
// the .map "S<X>Desc" (SPDoodad -> SDoodadDesc, SPVehicleUnit -> SVehicleUnitDesc,
// ...), which is how the browser knows which existing entity to clone as a
// byte template when placing a prototype the map has never used.
std::map<std::string, ProtoInfo> protodb_full_index(const std::string& path);

// "SPDoodad" -> "SDoodadDesc". Empty if `schema` is not an SP* prototype schema.
std::string protodb_map_schema(const std::string& schema);
