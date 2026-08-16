// Strict, read-only validator for the scenario object trees — the HEAP/HASH/ARRY
// containers behind the trigger, objective, group, location and camera-path data.
//
// This is a VERIFIER, not a loader: it walks every OBJS section of a map and
// demands that the walk land on exactly the byte where that section's SCHD table
// begins, and that every HEAP land on exactly its own chunk end. Nothing is
// interpreted into Scene and nothing is written. A wrong width for any field of
// any schema desynchronises the rest of the tree, so "225/225 sections exact" is
// a statement about every field of every record, not just about the containers.
//
// It deliberately re-implements the chunk walk and the schema table rather than
// calling into mapfile.cpp. An independent implementation is what makes the
// check worth running: a shared helper with a wrong assumption would agree with
// itself and report green.
#pragma once
#include <map>
#include <string>
#include <vector>

struct HeapReport {
    // OBJS sections and how many consumed exactly up to their SCHD offset.
    int objsSections = 0, objsExact = 0;
    // Containers seen anywhere in those trees.
    int heaps = 0, heapsExact = 0, heapsListOk = 0;
    int hashes = 0, arrays = 0;
    long objtRecords = 0;
    // Slot-pool totals over every HEAP.
    long slotsTotal = 0, slotsLive = 0, slotsFree = 0;
    // Live HEAP records by schema name, e.g. {"SLocation", 73}.
    std::map<std::string, int> heapRecordsByType;
    std::vector<std::string> issues;    // capped; first failures only
    bool ok = false;
};

// Walk every OBJS section in `raw`. Returns false only when the file is not a
// map at all; a structural mismatch is reported through HeapReport::ok/issues so
// the caller can print every failure rather than the first.
bool map_heap_scan(const std::vector<unsigned char>& raw, HeapReport& out);

// ---- scenario records (read-only) -------------------------------------------
// The five headline types that live in HEAP slot pools. Everything here is
// READ-ONLY: byte offsets are kept so a later milestone can write them back, but
// nothing in this file writes.

struct ScenLocation {
    std::string name;
    float pos[3] = {0,0,0};       // map coords: x, y, elevation as stored
    float dir[3] = {0,0,0};
    float size[2] = {0,0};        // ellipse HALF-extents (0x0005 is a vec2f)
    unsigned color = 0;           // packed RGBA
    int  startId = 0, startTeam = 0, heapIndex = -1;
    bool isStart = false, active = false;
    int  triggerCount = 0;
    long posOff = -1;             // byte offset of Pos, for a future write path
};
struct ScenObjective {
    std::string id;
    int type = 0, prestige = 0, messageId = 0, status = 0;
    bool hidden = false;
};
struct ScenTriggerVar {
    std::string name, trigger;
    int value = 0, delta = 0;
    bool active = false;
};
struct ScenGroup {
    std::string name;
    int type = 0, index = 0, player = 0, members = 0;
};
struct ScenCameraPath {
    std::string name;
    int eyeIndex = 0, targetIndex = 0;
    float seconds = 0;
};
struct ScenarioData {
    std::vector<ScenLocation>   locations;
    std::vector<ScenObjective>  objectives;
    std::vector<ScenTriggerVar> vars;
    std::vector<ScenGroup>      groups;
    std::vector<ScenCameraPath> cameras;
    int  triggerHandlers = 0;     // entries in the SLuaHandler.Triggers HASH
    bool ok = false;              // false when a tree failed to walk exactly
};

// Read the scenario records out of `raw`. Same walk as map_heap_scan, so it is
// only as trustworthy as --heaptest says it is: if that fails on a map, `ok` is
// false here and the caller must show the data as unreliable rather than pretend.
bool map_scenario_read(const std::vector<unsigned char>& raw, ScenarioData& out);
