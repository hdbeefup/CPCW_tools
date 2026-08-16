// Strict validator for the scenario object trees. See heap.h for why this walk
// is independent of mapfile.cpp's.
//
// ---------------------------------------------------------------------------
// The container encoding
// ---------------------------------------------------------------------------
// A schema field's type id is composite. The LOW byte selects the container
// kind and the bytes above it give the element (and for HASH, the key) type:
//
//     0x8A / 0x90 / 0x9C  -> ARRY      0xA5 -> HEAP      0xA6 -> HASH
//
// so 0x898A is "ARRY of objects", 0x148A "ARRY of entrefs", 0x039C "ARRY of
// bools" (the type the tools called FLAGS), 0x89A5 "HEAP of objects" and
// 0x8904A6 "HASH from string to object". 0x??90 and 0x??8A are the same thing on
// disk — both write a plain ARRY chunk; the distinction is a runtime one
// (fixed-capacity vs growable) that leaves no trace in the file.
//
// HEAP is the same slot pool the engine uses for WTHR/PATH/CAMS and GROL/GDCL/
// GRVL, but its six header dwords are in a DIFFERENT ORDER — slotCount first
// and no trailing capacity:
//
//     'HEAP' u32 size
//     u32 slotCount, u32 usedCount,
//     i32 freeHead, i32 freeTail, i32 listHead, i32 listTail
//     slotCount x { i32 prev, i32 next, u8 isFree [, OBJT record] }
//
// A free slot carries no record. isFree==0 means live. Empty pools ship
// slotCount 0 with all four indices -1. SLocation has a field literally named
// HeapIndex, which is the engine's own name for the slot key: slot indices are
// stable, positions in the used chain are not.
#include "heap.h"
#include <cstring>
#include <cstdio>

namespace {

struct D {
    const unsigned char* d = nullptr; size_t n = 0;
    unsigned char  u8 (size_t p) const { return p < n ? d[p] : 0; }
    unsigned short u16(size_t p) const { return p+2<=n ? (unsigned short)(d[p]|(d[p+1]<<8)) : 0; }
    unsigned int   u32(size_t p) const {
        return p+4<=n ? (unsigned)(d[p]|(d[p+1]<<8)|(d[p+2]<<16)|((unsigned)d[p+3]<<24)) : 0; }
    int            i32(size_t p) const { return (int)u32(p); }
    bool tag(size_t p, const char* t) const { return p+4<=n && std::memcmp(d+p,t,4)==0; }
};

struct Field { std::string name; unsigned type, size; };
struct Schema { std::string name; unsigned short type_id=0, version=0; std::vector<Field> fields; };

// Field types that occupy a fixed number of bytes. GUID (0x11) and ref (0x12)
// are NOT here: they are u16-length-prefixed strings. Fixing them at 4 bytes
// desynchronised 133 of 225 sections.
int fixedWidth(unsigned ft) {
    switch (ft) {
        case 0x0001: case 0x0002: return 4;          // int32, float
        case 0x0003: return 1;                        // bool
        case 0x0005: return 8;                        // vec2f (NOT a double)
        case 0x0006: return 12;                       // vec3
        case 0x0013: case 0x0014: return 4;           // IID, entref
        case 0x0015: case 0x0016: return 8;           // vec2f, vec2i
        case 0x0017: return 1;                        // uint8
        case 0x0018: return 4;                        // color
        case 0x0019: return 2;                        // int16
        default: return -1;
    }
}
bool strLike(unsigned ft) { return ft==0x0004 || ft==0x0011 || ft==0x0012; }

// A captured record: the schema name plus where each of its fields starts.
// Values are read afterwards, so the walk itself stays a pure structural check.
struct FieldHit { size_t off; unsigned type; };
struct RecordHit { std::string type; std::map<std::string, FieldHit> fields; };

struct Walker {
    D D_;
    std::map<unsigned short, Schema> schemas;
    HeapReport* rep = nullptr;
    bool failed = false;
    std::string err;
    // Set to a schema name to capture that record's field offsets. Only the
    // OUTERMOST VOBJ of that name is captured: SLocation.Triggers holds nested
    // HandlerEntryType objects whose fields would otherwise overwrite the
    // parent's by name.
    const char* capture = nullptr;
    std::vector<RecordHit>* hits = nullptr;
    bool capturing = false;

    void fail(const std::string& m) { if (!failed) { failed = true; err = m; } }

    // -- containers ---------------------------------------------------------

    size_t objt(size_t pos) {
        if (failed) return pos;
        if (!D_.tag(pos, "OBJT")) { fail("expected OBJT at " + std::to_string(pos)); return pos; }
        size_t end = pos + 8 + D_.u32(pos + 4);
        if (end > D_.n) { fail("OBJT runs past EOF at " + std::to_string(pos)); return pos; }
        rep->objtRecords++;
        size_t p = pos + 10;
        while (!failed && p + 8 <= end && D_.tag(p, "VOBJ")) p = vobj(p);
        if (!failed && p != end)
            fail("OBJT @" + std::to_string(pos) + " consumed to " + std::to_string(p) +
                 ", ends " + std::to_string(end));
        return end;
    }

    size_t vobj(size_t pos) {
        size_t end = pos + 8 + D_.u32(pos + 4);
        unsigned short tid = D_.u16(pos + 8);
        size_t p = pos + 10;
        if (p + 2 > end) return end;
        p += 2;                                     // u16 version
        auto it = schemas.find(tid);
        if (it == schemas.end()) return end;         // no schema: skip the span
        const bool mine = capture && !capturing && hits && it->second.name == capture;
        if (mine) { capturing = true; hits->push_back(RecordHit{ it->second.name, {} }); }
        for (const Field& f : it->second.fields) {
            if (failed) { if (mine) capturing = false; return end; }
            if (p > end) { fail(it->second.name + "." + f.name + " starts past end");
                           if (mine) capturing = false; return end; }
            if (mine) hits->back().fields[f.name] = FieldHit{ p, f.type };
            p = field(p, f.type, f.size, end, it->second.name + "." + f.name);
        }
        if (mine) capturing = false;
        if (!failed && p != end)
            fail("VOBJ " + it->second.name + " @" + std::to_string(pos) + " consumed to " +
                 std::to_string(p) + ", ends " + std::to_string(end));
        return end;
    }

    size_t arry(size_t pos, unsigned elem, const std::string& what) {
        if (!D_.tag(pos, "ARRY")) { fail(what + ": expected ARRY at " + std::to_string(pos)); return pos; }
        size_t end = pos + 8 + D_.u32(pos + 4);
        unsigned cnt = D_.u32(pos + 8);
        size_t p = pos + 12;
        rep->arrays++;
        for (unsigned k = 0; k < cnt && !failed; k++)
            p = (elem == 0x88 || elem == 0x89) ? objt(p) : field(p, elem, 0, end, what + "[]");
        if (!failed && p != end)
            fail(what + ": ARRY n=" + std::to_string(cnt) + " consumed to " + std::to_string(p) +
                 ", ends " + std::to_string(end));
        return end;
    }

    size_t heap(size_t pos, const std::string& what) {
        if (!D_.tag(pos, "HEAP")) { fail(what + ": expected HEAP at " + std::to_string(pos)); return pos; }
        size_t end = pos + 8 + D_.u32(pos + 4);
        if (end > D_.n) { fail(what + ": HEAP runs past EOF"); return pos; }
        rep->heaps++;
        unsigned slotCount = D_.u32(pos + 8);
        unsigned usedCount = D_.u32(pos + 12);
        int freeHead = D_.i32(pos + 16), freeTail = D_.i32(pos + 20);
        int listHead = D_.i32(pos + 24), listTail = D_.i32(pos + 28);
        size_t p = pos + 32;

        std::vector<int> prev(slotCount), next(slotCount);
        std::vector<unsigned char> isFree(slotCount);
        unsigned live = 0;
        for (unsigned s = 0; s < slotCount && !failed; s++) {
            if (p + 9 > end) { fail(what + ": slot header overruns chunk"); return end; }
            prev[s] = D_.i32(p); next[s] = D_.i32(p + 4); isFree[s] = D_.u8(p + 8);
            p += 9;
            if (isFree[s] > 1) { fail(what + ": isFree not a bool"); return end; }
            if (isFree[s] == 0) {
                unsigned short tid = D_.u16(p + 8);
                auto it = schemas.find(tid);
                rep->heapRecordsByType[it != schemas.end() ? it->second.name : "?"]++;
                p = objt(p);
                live++;
            }
        }
        if (failed) return end;
        if (p != end) {
            fail(what + ": HEAP slots=" + std::to_string(slotCount) + " consumed to " +
                 std::to_string(p) + ", ends " + std::to_string(end));
            return end;
        }
        if (live != usedCount) {
            fail(what + ": usedCount=" + std::to_string(usedCount) + " but " +
                 std::to_string(live) + " live slots");
            return end;
        }
        rep->heapsExact++;
        rep->slotsTotal += slotCount; rep->slotsLive += live;
        rep->slotsFree += (long)slotCount - (long)live;

        // Both chains: exact length, prev/next mutually inverse, -1 at the ends,
        // no slot on both chains, and free/live agreeing with the isFree byte.
        auto chain = [&](int head, int tail, unsigned want, bool wantFree,
                         std::vector<char>& seen) -> bool {
            int cur = head, last = -1; unsigned n = 0;
            while (cur != -1) {
                if (cur < 0 || (unsigned)cur >= slotCount) return false;
                if (seen[cur]) return false;
                seen[cur] = 1;
                if ((isFree[cur] != 0) != wantFree) return false;
                if (++n > slotCount) return false;
                int nx = next[cur];
                if (nx != -1 && ((unsigned)nx >= slotCount || prev[nx] != cur)) return false;
                last = cur; cur = nx;
            }
            if (n != want) return false;
            if (n) { if (prev[head] != -1 || last != tail || next[tail] != -1) return false; }
            else if (head != -1 || tail != -1) return false;
            return true;
        };
        std::vector<char> seenU(slotCount, 0), seenF(slotCount, 0);
        bool okU = chain(listHead, listTail, live, false, seenU);
        bool okF = chain(freeHead, freeTail, slotCount - live, true, seenF);
        bool overlap = false;
        for (unsigned s = 0; s < slotCount; s++) if (seenU[s] && seenF[s]) overlap = true;
        if (okU && okF && !overlap) rep->heapsListOk++;
        else if (rep->issues.size() < 24)
            rep->issues.push_back(what + ": list invariants failed (used=" +
                std::to_string((int)okU) + " free=" + std::to_string((int)okF) +
                " overlap=" + std::to_string((int)overlap) + ")");
        return end;
    }

    size_t hash(size_t pos, unsigned keyft, unsigned valft, const std::string& what) {
        if (!D_.tag(pos, "HASH")) { fail(what + ": expected HASH at " + std::to_string(pos)); return pos; }
        size_t end = pos + 8 + D_.u32(pos + 4);
        unsigned cnt = D_.u32(pos + 8);
        size_t p = pos + 12;
        rep->hashes++;
        for (unsigned k = 0; k < cnt && !failed; k++) {
            p = field(p, keyft, 0, end, what + ".key");
            p = (valft == 0x88 || valft == 0x89) ? objt(p) : field(p, valft, 0, end, what + ".val");
        }
        if (!failed && p != end)
            fail(what + ": HASH n=" + std::to_string(cnt) + " consumed to " + std::to_string(p) +
                 ", ends " + std::to_string(end));
        return end;
    }

    // -- fields -------------------------------------------------------------

    size_t field(size_t p, unsigned ft, unsigned fs, size_t end, const std::string& what) {
        if (failed) return p;
        int w = fixedWidth(ft);
        if (w >= 0) return p + (size_t)w;
        if (strLike(ft)) return p + 2 + D_.u16(p);
        if (ft == 0x002B) return p + 4 + 2 * (size_t)D_.u32(p);   // LOCSTR, UTF-16LE
        unsigned kind = ft & 0xFF;
        if (kind == 0x8A || kind == 0x90 || kind == 0x9C) return arry(p, (ft >> 8) & 0xFF, what);
        if (kind == 0xA5) return heap(p, what);
        if (kind == 0xA6) return hash(p, (ft >> 8) & 0xFF, (ft >> 16) & 0xFF, what);
        if (ft == 0x0088 || ft == 0x0089) {
            if (D_.tag(p, "OBJT")) return objt(p);
            if (D_.tag(p, "VOBJ")) return vobj(p);
            if (D_.tag(p, "ARRY")) return arry(p, 0x89, what);
            if (D_.tag(p, "HEAP")) return heap(p, what);
            if (D_.tag(p, "HASH")) return hash(p, 0x04, 0x89, what);
            fail(what + ": inline tag not a known container at " + std::to_string(p));
            return p;
        }
        fail(what + ": unhandled field type 0x" + std::to_string(ft));
        return p;
    }

    // -- schema tables ------------------------------------------------------

    void parseSchd(size_t pos, size_t limit) {
        unsigned short count = D_.u16(pos); pos += 4;
        for (int s = 0; s < count; s++) {
            if (pos >= limit || !D_.tag(pos, "SCHM")) break;
            size_t cs = pos + 8, ce = cs + D_.u32(pos + 4), p = cs;
            Schema sc;
            unsigned short L = D_.u16(p); sc.name.assign((const char*)D_.d + p + 2, L); p += 2 + L;
            sc.type_id = D_.u16(p); p += 2; sc.version = D_.u16(p); p += 2;
            unsigned short fc = D_.u16(p); p += 2;
            for (int fi = 0; fi < fc; fi++) {
                if (p + 2 > ce) break;
                Field f;
                unsigned short fl = D_.u16(p); f.name.assign((const char*)D_.d + p + 2, fl); p += 2 + fl;
                if (p + 8 > ce) break;
                f.type = D_.u32(p); p += 4; f.size = D_.u32(p); p += 4;
                sc.fields.push_back(std::move(f));
            }
            schemas[sc.type_id] = std::move(sc);
            pos = ce;
        }
    }
};

// Collect every OBJS section (offset, schemaOffset, end) and every SCHD span.
void chunkWalk(const D& d, size_t pos, size_t end,
               std::vector<std::pair<size_t, size_t>>& schd,
               std::vector<std::pair<size_t, size_t>>& objs) {
    while (pos + 8 <= end) {
        bool ascii = true;
        for (int k = 0; k < 4; k++) { unsigned char b = d.u8(pos + k); if (b < 0x20 || b >= 0x7F) { ascii = false; break; } }
        if (!ascii) break;
        std::string tag((const char*)d.d + pos, 4);
        size_t size = d.u32(pos + 4), chunkEnd = pos + 8 + size;
        if (chunkEnd > end + 4) break;
        if (tag == "PREC" || tag == "SETS" || tag == "OJTS") chunkWalk(d, pos + 12, chunkEnd, schd, objs);
        else if (tag == "OBJS") { objs.push_back({pos, chunkEnd}); chunkWalk(d, pos + 12, chunkEnd, schd, objs); }
        else if (tag == "WRLD") chunkWalk(d, pos + 20, chunkEnd, schd, objs);
        else if (tag == "GTRN") chunkWalk(d, pos + 9, chunkEnd, schd, objs);
        else if (tag == "UNTS") chunkWalk(d, pos + 16, chunkEnd, schd, objs);
        else if (tag == "SCHD") schd.push_back({pos + 8, chunkEnd});
        pos = chunkEnd;
    }
}

}  // namespace

bool map_heap_scan(const std::vector<unsigned char>& raw, HeapReport& out) {
    D d; d.d = raw.data(); d.n = raw.size();
    if (!d.tag(0, "SCEN")) return false;

    std::vector<std::pair<size_t, size_t>> schd, objs;
    chunkWalk(d, 12, std::min((size_t)8 + d.u32(4), d.n), schd, objs);

    Walker w; w.D_ = d; w.rep = &out;
    for (auto& s : schd) w.parseSchd(s.first, s.second);   // schemas are merged map-wide

    out.objsSections = (int)objs.size();
    for (auto& sec : objs) {
        size_t dataOff = sec.first + 8;
        size_t schemaOff = d.u32(dataOff);
        size_t p = dataOff + 4;
        if (schemaOff <= p || schemaOff > sec.second) {
            if (out.issues.size() < 24)
                out.issues.push_back("OBJS @" + std::to_string(sec.first) + ": schema_offset " +
                                     std::to_string(schemaOff) + " outside the section");
            continue;
        }
        w.failed = false; w.err.clear();
        while (!w.failed && p < schemaOff) p = w.objt(p);
        if (!w.failed && p != schemaOff)
            w.fail("section consumed to " + std::to_string(p) + ", SCHD at " +
                   std::to_string(schemaOff));
        if (w.failed) {
            if (out.issues.size() < 24)
                out.issues.push_back("OBJS @" + std::to_string(sec.first) + ": " + w.err);
        } else {
            out.objsExact++;
        }
    }
    out.ok = (out.objsExact == out.objsSections) &&
             (out.heapsExact == out.heaps) &&
             (out.heapsListOk == out.heaps) &&
             out.issues.empty();
    return true;
}

// ---- scenario records --------------------------------------------------------

namespace {

// Value readers. Each returns a default rather than throwing when the field is
// absent — a schema version that predates a field is normal, not an error.
struct Vals {
    const D* d;
    const std::map<std::string, FieldHit>* f;
    const FieldHit* get(const char* n, unsigned wantType = 0) const {
        auto it = f->find(n);
        if (it == f->end()) return nullptr;
        if (wantType && it->second.type != wantType) return nullptr;
        return &it->second;
    }
    std::string str(const char* n) const {
        const FieldHit* h = get(n, 0x0004);
        if (!h) return {};
        unsigned short L = d->u16(h->off);
        if (h->off + 2 + L > d->n) return {};
        return std::string((const char*)d->d + h->off + 2, L);
    }
    int i32(const char* n) const { const FieldHit* h = get(n, 0x0001); return h ? d->i32(h->off) : 0; }
    unsigned u32v(const char* n, unsigned t) const { const FieldHit* h = get(n, t); return h ? d->u32(h->off) : 0u; }
    bool boolean(const char* n) const { const FieldHit* h = get(n, 0x0003); return h && d->u8(h->off) != 0; }
    int u8v(const char* n) const { const FieldHit* h = get(n, 0x0017); return h ? (int)d->u8(h->off) : 0; }
    float f32(const char* n) const {
        const FieldHit* h = get(n, 0x0002);
        if (!h) return 0.0f;
        unsigned v = d->u32(h->off); float r; std::memcpy(&r, &v, 4); return r;
    }
    void vec3(const char* n, float o[3]) const {
        const FieldHit* h = get(n, 0x0006);
        if (!h) return;
        for (int k = 0; k < 3; k++) { unsigned v = d->u32(h->off + 4*k); std::memcpy(&o[k], &v, 4); }
    }
    void vec2(const char* n, float o[2]) const {
        const FieldHit* h = get(n, 0x0005);       // vec2f, NOT a double
        if (!h) return;
        for (int k = 0; k < 2; k++) { unsigned v = d->u32(h->off + 4*k); std::memcpy(&o[k], &v, 4); }
    }
    long offOf(const char* n) const { auto it = f->find(n); return it == f->end() ? -1 : (long)it->second.off; }
    // Element count of an on-disk ARRY field, without walking it.
    int arrayCount(const char* n) const {
        auto it = f->find(n);
        if (it == f->end()) return 0;
        size_t p = it->second.off;
        if (!d->tag(p, "ARRY")) return 0;
        return (int)d->u32(p + 8);
    }
};

// Run the full walk once with `capture` set, returning the captured records.
bool collect(const D& d, Walker& w,
             const std::vector<std::pair<size_t, size_t>>& objs,
             const char* type, std::vector<RecordHit>& out) {
    out.clear();
    w.capture = type; w.hits = &out; w.capturing = false;
    bool allExact = true;
    for (auto& sec : objs) {
        size_t dataOff = sec.first + 8;
        size_t schemaOff = d.u32(dataOff);
        size_t p = dataOff + 4;
        if (schemaOff <= p || schemaOff > sec.second) { allExact = false; continue; }
        w.failed = false; w.err.clear(); w.capturing = false;
        while (!w.failed && p < schemaOff) p = w.objt(p);
        if (w.failed || p != schemaOff) allExact = false;
    }
    w.capture = nullptr; w.hits = nullptr;
    return allExact;
}

}  // namespace

bool map_scenario_read(const std::vector<unsigned char>& raw, ScenarioData& out) {
    D d; d.d = raw.data(); d.n = raw.size();
    if (!d.tag(0, "SCEN")) return false;

    std::vector<std::pair<size_t, size_t>> schd, objs;
    chunkWalk(d, 12, std::min((size_t)8 + d.u32(4), d.n), schd, objs);

    HeapReport sink;                       // the walk needs somewhere to count
    Walker w; w.D_ = d; w.rep = &sink;
    for (auto& s : schd) w.parseSchd(s.first, s.second);

    bool ok = true;
    std::vector<RecordHit> hits;

    ok &= collect(d, w, objs, "SLocation", hits);
    for (const RecordHit& r : hits) {
        Vals v{ &d, &r.fields };
        ScenLocation L;
        L.name = v.str("Name");
        v.vec3("Pos", L.pos); v.vec3("Dir", L.dir); v.vec2("Size", L.size);
        L.color = v.u32v("Color", 0x0018);
        L.isStart = v.boolean("IsStartLocaion");     // sic: the engine's spelling
        L.startId = v.u8v("StartLocationID");
        L.startTeam = v.i32("StartLocationTeam");
        L.active = v.boolean("IsActive");
        L.heapIndex = v.i32("HeapIndex");
        L.triggerCount = v.arrayCount("Triggers");
        L.posOff = v.offOf("Pos");
        out.locations.push_back(std::move(L));
    }

    ok &= collect(d, w, objs, "SObjective", hits);
    for (const RecordHit& r : hits) {
        Vals v{ &d, &r.fields };
        ScenObjective O;
        O.id = v.str("ID");
        O.type = v.i32("Type"); O.prestige = v.i32("Prestige");
        O.messageId = v.i32("MessageId"); O.status = v.i32("Status");
        O.hidden = v.boolean("Hidden");
        out.objectives.push_back(std::move(O));
    }

    ok &= collect(d, w, objs, "TriggerVar", hits);
    for (const RecordHit& r : hits) {
        Vals v{ &d, &r.fields };
        ScenTriggerVar T;
        T.name = v.str("Name"); T.trigger = v.str("Trigger");
        T.value = v.i32("Value"); T.delta = v.i32("Delta");
        T.active = v.boolean("IsActive");
        out.vars.push_back(std::move(T));
    }

    ok &= collect(d, w, objs, "SGroup", hits);
    for (const RecordHit& r : hits) {
        Vals v{ &d, &r.fields };
        ScenGroup G;
        G.name = v.str("Name");
        G.type = v.i32("Type"); G.index = v.i32("Index");
        G.player = v.u8v("PlayerIndex");
        G.members = v.arrayCount("Members");
        out.groups.push_back(std::move(G));
    }

    ok &= collect(d, w, objs, "SCameraPath", hits);
    for (const RecordHit& r : hits) {
        Vals v{ &d, &r.fields };
        ScenCameraPath C;
        C.name = v.str("Name");
        C.eyeIndex = v.i32("EyeIndex"); C.targetIndex = v.i32("TargetIndex");
        C.seconds = v.f32("TotalPlayingTimeInSec");
        out.cameras.push_back(std::move(C));
    }

    // The trigger table is one HASH per map (SLuaHandler.Triggers); its entry
    // count is the honest "how many trigger handlers" number, so read it rather
    // than infer it. One forward pass.
    out.triggerHandlers = 0;
    for (size_t k = 0; k + 12 <= d.n; k++)
        if (d.tag(k, "HASH")) { out.triggerHandlers += (int)d.u32(k + 8); k += 3; }

    out.ok = ok;
    return true;
}
