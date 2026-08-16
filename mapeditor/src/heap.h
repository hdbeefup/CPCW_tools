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
