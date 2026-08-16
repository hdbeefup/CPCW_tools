// Native .map loader — port of cpcw_map.py (kept in lockstep; Python is the
// oracle). Parses the SCEN chunk tree, SCHD schemas, UNTS/OBJS entities, the
// GTRD terrain (heightmap locator + splatmap colormap) directly into a Scene.
#include "mapfile.h"
#include "overlays.h"
#include "weather.h"
#include "heap.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace {

// ---- field type ids (D3DDECLUSAGE-like schema field types) -----------------
enum {
    FT_INT32=0x0001, FT_FLOAT=0x0002, FT_BOOL=0x0003, FT_STRING=0x0004,
    FT_FLOAT64=0x0005, FT_VEC3=0x0006, FT_GUID=0x0011, FT_REF=0x0012,
    FT_IID=0x0013, FT_ENTREF=0x0014, FT_VEC2F=0x0015, FT_VEC2I=0x0016,
    FT_UINT8=0x0017, FT_COLOR=0x0018, FT_INT16=0x0019, FT_LOCSTR=0x002B,
    FT_INLINE1=0x0088, FT_INLINE2=0x0089, FT_BLOB=0x0165, FT_FLAGS=0x039C,
    FT_ARRAY=0x898A
};
// Only these three are u16-length-prefixed strings. FT_FLAGS (0x039C) is an
// on-disk ARRY of bools and FT_LOCSTR (0x002B) is a u32 character count followed
// by UTF-16LE — reading either as a u16-prefixed string desynchronises the rest
// of the record. Neither appears in any entity schema, which is why the entity
// path never noticed; the scenario trees use both. See heap.cpp for the general
// container encoding and --heaptest for the check.
static bool stringLike(uint32_t t) {
    return t==FT_STRING||t==FT_GUID||t==FT_REF;
}

// ---- little-endian readers over a raw buffer -------------------------------
struct Data {
    const uint8_t* d = nullptr; size_t n = 0;
    uint8_t  u8 (size_t p) const { return p<n ? d[p] : 0; }
    uint16_t u16(size_t p) const { return p+2<=n ? d[p]|(d[p+1]<<8) : 0; }
    int16_t  i16(size_t p) const { return (int16_t)u16(p); }
    uint32_t u32(size_t p) const { return p+4<=n ? (uint32_t)(d[p]|(d[p+1]<<8)|(d[p+2]<<16)|((uint32_t)d[p+3]<<24)) : 0; }
    int32_t  i32(size_t p) const { return (int32_t)u32(p); }
    float    f32(size_t p) const { uint32_t v=u32(p); float f; std::memcpy(&f,&v,4); return f; }
    double   f64(size_t p) const { uint64_t v=u32(p)|((uint64_t)u32(p+4)<<32); double f; std::memcpy(&f,&v,8); return f; }
    bool tag(size_t p, const char* t) const { return p+4<=n && std::memcmp(d+p,t,4)==0; }
    std::string str(size_t p, size_t& out) const {   // u16 len + bytes
        uint16_t L=u16(p); size_t s=p+2;
        std::string r; if (s+L<=n) r.assign((const char*)d+s, L);
        out = s+L; return r;
    }
};

struct Field { std::string name; uint32_t type, size; };
struct Schema { std::string name; uint16_t type_id=0, version=0; std::vector<Field> fields; };

struct Chunk {
    std::string tag; size_t offset=0, size=0, data_off=0;
    std::vector<Chunk> children;
    long meta_schema_off=-1, meta_width=-1, meta_height=-1;   // used bits of meta
};

// ---- parsed object value (only what the Scene needs is interpreted) --------
struct Value { int kind=0; double f=0, v3[3]={0,0,0}; long i=0; std::string s;
               long off=-1; uint32_t ftype=0; };
enum { V_NONE, V_INT, V_FLOAT, V_VEC3, V_STR };
typedef std::map<std::string, Value> Obj;

struct GTRDLayer { std::string name; float uvScale=1.0f; bool active=false; };

struct Parser {
    Data D;
    std::vector<uint8_t> buf;
    Chunk root;
    std::map<uint16_t, Schema> schemas;

    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        f.seekg(0, std::ios::end); std::streamoff sz=f.tellg(); f.seekg(0);
        std::vector<uint8_t> b((size_t)sz);
        f.read((char*)b.data(), sz);
        return loadBytes(std::move(b));
    }

    // Parse an already-resident buffer (structural edits reparse this way, so a
    // place/delete never round-trips through a temp file).
    bool loadBytes(std::vector<uint8_t> bytes) {
        buf = std::move(bytes);
        D.d = buf.data(); D.n = buf.size();
        root = Chunk{}; schemas.clear();
        if (!D.tag(0,"SCEN")) return false;
        size_t scenSize = D.u32(4);
        root = Chunk{"SCEN",0,scenSize,8};
        size_t end = std::min((size_t)8+scenSize, D.n);
        parseChildren(root, 12, end);
        collectSchemas(root);
        return true;
    }

    void parseChildren(Chunk& parent, size_t pos, size_t end) {
        while (pos+8 <= end) {
            size_t tp=pos;
            bool ascii=true;
            for (int k=0;k<4;k++){ uint8_t b=D.u8(tp+k); if(b<0x20||b>=0x7F){ascii=false;break;} }
            if (!ascii) break;
            std::string tag((const char*)D.d+tp, 4);
            size_t size = D.u32(pos+4);
            size_t chunkEnd = pos+8+size;
            if (chunkEnd > end+4) break;
            Chunk c{tag,pos,size,pos+8};
            if (tag=="PREC"||tag=="SETS"||tag=="OJTS") { parseChildren(c, pos+12, chunkEnd); }
            else if (tag=="OBJS") { c.meta_schema_off=(long)D.u32(pos+8); parseChildren(c, pos+12, chunkEnd); }
            else if (tag=="WRLD") { c.meta_width=(long)D.u32(pos+12); c.meta_height=(long)D.u32(pos+16); parseChildren(c, pos+20, chunkEnd); }
            else if (tag=="GTRN") { parseChildren(c, pos+9, chunkEnd); }
            else if (tag=="UNTS") { parseChildren(c, pos+16, chunkEnd); }
            // PATH/CAMS/WTHR/SCHD/STOR/BLCK/GTRD are leaves. So are GROL/GDCL/
            // GRVL: their content is a slot pool (a 6-dword header then fixed
            // slot records), not a run of chunks — recursing at pos+8 read the
            // u32 record count as a tag, which is why they came out empty.
            // overlays.cpp owns their real structure.
            parent.children.push_back(std::move(c));
            pos = chunkEnd;
        }
    }

    void collectSchemas(const Chunk& c) {
        if (c.tag=="SCHD") parseSchd(c.data_off, c.offset+8+c.size);
        for (auto& ch : c.children) collectSchemas(ch);
    }

    void parseSchd(size_t pos, size_t limit) {
        uint16_t count = D.u16(pos); pos+=4;   // count + unk
        for (int s=0;s<count;s++) {
            if (pos>=limit || !D.tag(pos,"SCHM")) break;
            size_t cs=pos+8, ce=cs+D.u32(pos+4), p=cs;
            Schema sc;
            size_t np; sc.name = D.str(p,np); p=np;
            sc.type_id=D.u16(p); p+=2; sc.version=D.u16(p); p+=2;
            uint16_t fc=D.u16(p); p+=2;
            for (int fi=0; fi<fc; fi++) {
                if (p+2>ce) break;
                Field fl; fl.name=D.str(p,np); p=np;
                if (p+8>ce) break;
                fl.type=D.u32(p); p+=4; fl.size=D.u32(p); p+=4;
                sc.fields.push_back(std::move(fl));
            }
            schemas[sc.type_id]=std::move(sc);
            pos=ce;
        }
    }

    const Chunk* find(const Chunk& c, const char* tag) const {
        if (c.tag==tag) return &c;
        for (auto& ch : c.children) { const Chunk* r=find(ch,tag); if(r) return r; }
        return nullptr;
    }

    // ---- entity (OBJT/VOBJ/ARRY) parser --------------------------------------
    size_t readField(size_t p, uint32_t ft, uint32_t fs, size_t limit, Value& out) {
        out = Value{};
        if (p>=limit) return p;
        if (ft==FT_INT32){ if(p+4>limit)return limit; out={V_INT}; out.i=D.i32(p); return p+4; }
        if (ft==FT_FLOAT){ if(p+4>limit)return limit; out={V_FLOAT}; out.f=D.f32(p); return p+4; }
        if (ft==FT_BOOL){ if(p+1>limit)return limit; out={V_INT}; out.i=D.u8(p)?1:0; return p+1; }
        if (stringLike(ft)){ if(p+2>limit)return limit; size_t np; out={V_STR}; out.s=D.str(p,np); return np; }
        if (ft==FT_UINT8){ if(p+1>limit)return limit; out={V_INT}; out.i=D.u8(p); return p+1; }
        if (ft==FT_INT16){ if(p+2>limit)return limit; out={V_INT}; out.i=D.i16(p); return p+2; }
        if (ft==FT_COLOR){ if(p+4>limit)return limit; out={V_INT}; out.i=(long)D.u32(p); return p+4; }
        if (ft==FT_IID||ft==FT_ENTREF){ if(p+4>limit)return limit; out={V_INT}; out.i=(long)D.u32(p); return p+4; }
        // 0x0005 is a PAIR OF FLOATS, not a double. Both readings are 8 bytes, so
        // no walk can tell them apart — the values do: SLocation.Size reads
        // 0..697 x 0..677 as vec2f (ellipse half-extents in a world that tops out
        // near 512x672) and 7.5e9 / 2.3e20 as a double.
        if (ft==FT_FLOAT64){ if(p+8>limit)return limit; out={V_FLOAT}; out.f=D.f32(p); return p+8; }
        if (ft==FT_VEC3){ if(p+12>limit)return limit; out={V_VEC3}; out.v3[0]=D.f32(p); out.v3[1]=D.f32(p+4); out.v3[2]=D.f32(p+8); return p+12; }
        if (ft==FT_VEC2I||ft==FT_VEC2F){ if(p+8>limit)return limit; return p+8; }
        if (ft==FT_LOCSTR){ if(p+4>limit)return limit; return p+4+2*(size_t)D.u32(p); }
        // A composite type id's LOW byte selects the container kind; the bytes
        // above it are the element (and for HASH the key) type. 0x898A is only
        // the commonest of these, not the only one.
        {   uint32_t kind = ft & 0xFF;
            if (kind==0x8A || kind==0x90 || kind==0x9C) return readArray(p, limit);
            if (kind==0xA5 || kind==0xA6) {          // HEAP / HASH: skip the span
                if (p+8>limit) return limit;
                if (D.tag(p,"HEAP")||D.tag(p,"HASH")) return p+8+D.u32(p+4);
            }
        }
        if (ft==FT_INLINE1||ft==FT_INLINE2) return readInline(p, limit);
        if (ft==FT_BLOB){ size_t nb=(fs>0&&fs<0xFFFF)?fs:0; if(nb>0&&p+nb<=limit) return p+nb; return p; }
        if (fs>0 && fs<0xFFFF){ if(p+fs<=limit) return p+fs; return p; }
        if (fs==0xFFFF || fs==0){
            if (D.tag(p,"ARRY")||D.tag(p,"OBJT")||D.tag(p,"VOBJ")) return readInline(p, limit);
            if (p+2<=limit){ uint16_t L=D.u16(p); if(L<4096 && p+2+L<=limit){ out={V_STR}; size_t np; out.s=D.str(p,np); return np; } }
        }
        return p;
    }
    size_t readArray(size_t p, size_t limit) {
        if (p+12>limit || !D.tag(p,"ARRY")) return p;
        size_t arrEnd=p+8+D.u32(p+4); uint32_t cnt=D.u32(p+8); p+=12;
        Obj tmp;
        for (uint32_t k=0;k<cnt;k++){ if(p>=arrEnd)break; if(D.tag(p,"OBJT")){ size_t e; parseObjt(p,e,tmp); p=e; } else break; }
        return arrEnd;
    }
    size_t readInline(size_t p, size_t limit) {
        Obj tmp; size_t e;
        if (D.tag(p,"OBJT")) { parseObjt(p,e,tmp); return e; }
        if (D.tag(p,"VOBJ")) { parseVobj(p,e,tmp); return e; }
        if (D.tag(p,"ARRY")) return readArray(p, limit);
        if (D.tag(p,"HEAP")||D.tag(p,"HASH")) return p+8+D.u32(p+4);
        return p;
    }
    void parseVobj(size_t pos, size_t& endOut, Obj& obj) {
        if (!D.tag(pos,"VOBJ")) { endOut=pos; return; }
        size_t contentEnd=pos+8+D.u32(pos+4);
        uint16_t typeId=D.u16(pos+8);
        auto it=schemas.find(typeId);
        if (obj.find("_type")==obj.end() && it!=schemas.end()) { Value v{V_STR}; v.s=it->second.name; obj["_type"]=v; }
        size_t p=pos+10;
        endOut=contentEnd;
        if (p+2>contentEnd) return;
        p+=2;  // version
        if (it!=schemas.end()) {
            for (auto& fl : it->second.fields) {
                if (p>=contentEnd) break;
                size_t fstart=p;
                Value val; p = readField(p, fl.type, fl.size, contentEnd, val);
                val.off=(long)fstart; val.ftype=fl.type;
                if (val.kind!=V_NONE) obj[fl.name]=val;
            }
        }
    }
    void parseObjt(size_t pos, size_t& endOut, Obj& obj) {
        if (!D.tag(pos,"OBJT")) { endOut=pos; return; }
        size_t contentEnd=pos+8+D.u32(pos+4);
        size_t ve; parseVobj(pos+10, ve, obj);
        size_t p=ve;
        while (p+10<=contentEnd && D.tag(p,"VOBJ")) {
            Obj tr; size_t te; parseVobj(p, te, tr); p=te;
            for (auto& kv : tr) if (kv.first[0] != '_') obj[kv.first]=kv.second;   // merge, skip _type
        }
        endOut=contentEnd;
    }

    void parseEntities(std::vector<Obj>& out) {
        const Chunk* unts=find(root,"UNTS"); if(!unts) return;
        const Chunk* objs=find(*unts,"OBJS"); if(!objs) return;
        size_t pos=objs->data_off+4;
        size_t objsEnd=objs->offset+8+objs->size;
        long schd=objs->meta_schema_off;
        size_t dataEnd = (schd>0 && (size_t)schd<objsEnd) ? (size_t)schd : objsEnd;
        while (pos < dataEnd-8) {
            if (D.tag(pos,"OBJT")) {
                size_t start=pos; Obj o; size_t e; parseObjt(pos,e,o); pos=e;
                Value vs{V_INT}; vs.i=(long)start; o["_objtStart"]=vs;
                Value ve{V_INT}; ve.i=(long)e;     o["_objtEnd"]=ve;
                out.push_back(std::move(o));
            } else break;
        }
    }

    // ---- GTRD terrain: heightmap locator + splatmap colormap -----------------
    bool parseGTRD(size_t& splatOff, int& lw, int& lh, std::vector<GTRDLayer>& layers, size_t& gtrdEnd) {
        const Chunk* g=find(root,"GTRD"); if(!g) return false;
        size_t p=g->data_off, end=g->offset+8+g->size;
        p+=1;                       // version u8
        lw=(int)D.u32(p); p+=4; lh=(int)D.u32(p); p+=4;
        p+=8;                       // world_x, world_y f32
        uint32_t layerCount=D.u32(p); p+=4;
        for (uint32_t i=0;i<layerCount && p<end;i++){
            size_t np; std::string name=D.str(p,np); p=np;
            p+=4;                   // unk u32
            float uv=D.f32(p); p+=4;             // uv_scale f32
            std::string det=D.str(p,np); p=np;   // detail
            uint8_t flag=D.u8(p); p+=1;
            layers.push_back({name, uv, flag!=0});
        }
        splatOff=p; gtrdEnd=end; return true;
    }

    // locate the elevation grid (port of get_heightmap) using entity Z samples
    bool heightmap(int WW, int WH, const std::vector<Obj>& ents,
                   std::vector<float>& outH, int& outW, int& outH_) {
        size_t gs, gtrdEnd; int lw,lh; std::vector<GTRDLayer> layers;
        if (!parseGTRD(gs,lw,lh,layers,gtrdEnd)) return false;
        int W=WW+1, H=WH+1; size_t need=(size_t)W*H;
        // entity samples: (fx,fy,ez)
        struct S { float fx,fy,ez; };
        std::vector<S> E;
        for (auto& o : ents) {
            auto it=o.find("Pos"); if(it==o.end()||it->second.kind!=V_VEC3) continue;
            float x=(float)it->second.v3[0], y=(float)it->second.v3[1], z=(float)it->second.v3[2];
            float fx=x/WW*(W-1), fy=y/WH*(H-1);
            if (fx>=0&&fx<W-1&&fy>=0&&fy<H-1){ E.push_back({fx,fy,z}); if(E.size()>=200)break; }
        }
        // collect height-like runs across 4 byte phases
        std::vector<std::pair<size_t,size_t>> runs;   // (runlen, offset)
        for (int phase=0;phase<4;phase++){
            size_t start=gs+phase; if(start>=gtrdEnd)continue;
            size_t n=(gtrdEnd-start)/4;
            size_t run=0, rstart=0;
            for (size_t i=0;i<n;i++){
                float v=D.f32(start+i*4);
                bool ok = (v==v) && v>-500.0f && v<500.0f;
                if (ok){ if(run==0)rstart=i; run++; }
                else { if(run>=need/2) runs.push_back({run,start+rstart*4}); run=0; }
            }
            if (run>=need/2) runs.push_back({run,start+rstart*4});
        }
        if (runs.empty()) return false;
        std::sort(runs.rbegin(), runs.rend());
        size_t off;
        auto fit=[&](size_t o)->double{
            double sxx=0,syy=0,sxy=0,mx=0,my=0; int nn=0;
            std::vector<double> xs,ys;
            for (auto& e : E){
                int x0=(int)e.fx, y0=(int)e.fy; size_t b=o+((size_t)y0*W+x0)*4;
                if (b+(size_t)W*4+4>D.n) return -9.0;
                float h00=D.f32(b); if(h00!=h00||std::fabs(h00)>500) return -9.0;
                float h10=D.f32(b+4), h01=D.f32(b+(size_t)W*4), h11=D.f32(b+(size_t)W*4+4);
                double tx=e.fx-x0, ty=e.fy-y0;
                double pv=h00*(1-tx)*(1-ty)+h10*tx*(1-ty)+h01*(1-tx)*ty+h11*tx*ty;
                xs.push_back(pv); ys.push_back(e.ez);
            }
            nn=(int)xs.size(); if(nn<20) return -9.0;
            for(double v:xs)mx+=v; mx/=nn; for(double v:ys)my+=v; my/=nn;
            for(int k=0;k<nn;k++){ sxx+=(xs[k]-mx)*(xs[k]-mx); syy+=(ys[k]-my)*(ys[k]-my); sxy+=(xs[k]-mx)*(ys[k]-my); }
            if(sxx<1e-9)sxx=1e-9; if(syy<1e-9)syy=1e-9; return sxy*sxy/(sxx*syy);
        };
        if (E.size()<20) { off=runs[0].second; }
        else {
            double best=-9.0; size_t bestOff=runs[0].second;
            size_t row=4*(size_t)W;
            for (auto& r : runs){
                if (best>0.9) break;
                size_t r0=r.second, runlen=r.first;
                size_t hi=r0 + (runlen>need? (runlen-need)*4 : 0) + row*16;
                hi=std::min(hi, r0+row*40);
                for (size_t o=r0;o<=hi;o+=4){ double v=fit(o); if(v>best){best=v;bestOff=o;} }
            }
            // editor robustness: if entity calibration is weak, fall back to the
            // longest height-like run rather than showing no terrain at all.
            off = (best<0.4) ? runs[0].second : bestOff;
        }
        outW=W; outH_=H; outH.resize(need);
        for (size_t i=0;i<need;i++){ float v=D.f32(off+i*4); if(!(v==v)||v>1e30f||v<-1e30f)v=0; outH[i]=v; }
        heightOff=off; splatStart=gs+0; // gs used for splat via off; store real
        // splatmap starts right after the located heightmap
        splatBase=off+need*4; gtrdLimit=gtrdEnd; gLayers=layers; gW=W; gH=H;
        return true;
    }

    // baked splat colormap (port of _bake_terrain_colormap + palette)
    size_t heightOff=0, splatStart=0, splatBase=0, gtrdLimit=0; int gW=0,gH=0;
    std::vector<GTRDLayer> gLayers;

    void colormap(std::vector<unsigned char>& out) {
        int W=gW, H=gH; size_t need=(size_t)W*H;
        std::vector<int> active;
        for (int i=0;i<(int)gLayers.size();i++) if(gLayers[i].active) active.push_back(i);
        if (active.empty()) return;
        // per-layer palette colour
        auto colOf=[&](const std::string& nm){
            std::string s; for(char c:nm) s+=(char)tolower((unsigned char)c);
            struct P{ const char* k; float r,g,b; };
            static const P pal[]={
                {"grass",0.27f,0.39f,0.17f},{"foliage",0.27f,0.39f,0.17f},{"meadow",0.27f,0.39f,0.17f},
                {"tillage",0.34f,0.25f,0.16f},{"soil",0.34f,0.25f,0.16f},{"mud",0.34f,0.25f,0.16f},{"dirt",0.34f,0.25f,0.16f},{"field",0.34f,0.25f,0.16f},{"ploughland",0.34f,0.25f,0.16f},
                {"gritty",0.52f,0.44f,0.30f},{"ground",0.52f,0.44f,0.30f},{"sand",0.52f,0.44f,0.30f},{"straw",0.52f,0.44f,0.30f},{"dry",0.52f,0.44f,0.30f},{"default",0.52f,0.44f,0.30f},
                {"cobble",0.44f,0.43f,0.42f},{"road",0.44f,0.43f,0.42f},{"pavement",0.44f,0.43f,0.42f},{"stone",0.44f,0.43f,0.42f},{"rock",0.44f,0.43f,0.42f},{"ruin",0.44f,0.43f,0.42f},{"gravel",0.44f,0.43f,0.42f},{"mine",0.44f,0.43f,0.42f},
                {"water",0.20f,0.29f,0.33f},{"river",0.20f,0.29f,0.33f},{"puddle",0.20f,0.29f,0.33f},{"sea",0.20f,0.29f,0.33f},
                {"snow",0.80f,0.82f,0.85f},{"winter",0.80f,0.82f,0.85f},{"ice",0.80f,0.82f,0.85f},
            };
            for (auto& p:pal) if(s.find(p.k)!=std::string::npos) return V3{p.r,p.g,p.b};
            return V3{0.35f,0.33f,0.28f};
        };
        std::vector<V3> cols(gLayers.size());
        for (size_t i=0;i<gLayers.size();i++) cols[i]=colOf(gLayers[i].name);
        // splat weight grids
        auto weight=[&](int layer, size_t gi)->float{
            size_t a=splatBase+(size_t)layer*need+gi;
            return a<D.n ? D.u8(a)/255.0f : 0.0f;
        };
        int base=active[0];
        out.resize(need*3);
        for (size_t gi=0;gi<need;gi++){
            V3 c=cols[base];
            for (size_t oi=1;oi<active.size();oi++){
                int li=active[oi]; float w=weight(li,gi);
                if (w>0){ V3 lc=cols[li]; c.x=c.x*(1-w)+lc.x*w; c.y=c.y*(1-w)+lc.y*w; c.z=c.z*(1-w)+lc.z*w; }
            }
            out[gi*3]=(unsigned char)(c.x*255); out[gi*3+1]=(unsigned char)(c.y*255); out[gi*3+2]=(unsigned char)(c.z*255);
        }
    }
    // Slice the raw per-layer uint8 opacity grids that follow the heightmap
    // (one gW*gH grid per layer, file order). Must run after heightmap().
    void splatWeights(std::vector<std::vector<unsigned char>>& out) {
        out.clear();
        size_t need=(size_t)gW*gH; if (need==0) return;
        for (size_t i=0;i<gLayers.size();i++){
            size_t a=splatBase+i*need;
            if (a+need>D.n || a+need>gtrdLimit) break;   // ran out of grids
            out.emplace_back(D.d+a, D.d+a+need);
        }
    }
    const std::vector<GTRDLayer>& layers() const { return gLayers; }
    struct V3 { float x,y,z; };
};

// size-field offset (chunk.offset+4) of every chunk whose byte range contains
// [es,ee) -- i.e. all ancestors of the entity OBJS.
static void collect_container_sizes(const Chunk& c, size_t es, size_t ee, std::vector<long>& out) {
    if (c.offset <= es && c.offset + 8 + c.size >= ee) out.push_back((long)c.offset + 4);
    for (const auto& ch : c.children) collect_container_sizes(ch, es, ee, out);
}

// Lift the parsed field maps into the editor's Entity list (the fields the editor
// edits, plus each one's byte offset so a save can overwrite it in place).
static void lift_entities(const std::vector<Obj>& ents, std::vector<Entity>& out) {
    out.clear(); out.reserve(ents.size());
    for (auto& o : ents) {
        Entity e;
        auto it=o.find("_type"); e.type = (it!=o.end()&&it->second.kind==V_STR)?it->second.s:"?";
        auto pr=o.find("Prototype"); if(pr!=o.end()&&pr->second.kind==V_STR) e.proto=pr->second.s;
        auto ps=o.find("Pos"); if(ps!=o.end()&&ps->second.kind==V_VEC3){ e.pos[0]=(float)ps->second.v3[0]; e.pos[1]=(float)ps->second.v3[1]; e.pos[2]=(float)ps->second.v3[2]; e.posOff=ps->second.off; }
        auto dr=o.find("Dir"); if(dr!=o.end()){ if(dr->second.kind==V_FLOAT)e.dir=(float)dr->second.f; else if(dr->second.kind==V_VEC3)e.dir=(float)dr->second.v3[0]; e.dirOff=dr->second.off; }
        auto pl=o.find("Player"); if(pl!=o.end()&&pl->second.kind==V_INT){ e.player=(int)pl->second.i; e.playerOff=pl->second.off; e.playerFtype=pl->second.ftype; }
        auto id=o.find("ID"); if(id!=o.end()&&id->second.kind==V_INT){ e.id=id->second.i; e.idOff=id->second.off; }
        // SEntityDesc.Scale — uniform scale, mostly meaningful on doodads. Absent
        // from some schemas, so default to 1 and only record an offset if present.
        auto sc=o.find("Scale");
        if (sc!=o.end() && sc->second.kind==V_FLOAT && sc->second.ftype==FT_FLOAT) {
            float v=(float)sc->second.f;
            if (v>0.0001f && v<1000.0f) e.scale=v;
            e.scaleOff=sc->second.off;
        }
        if (pr!=o.end() && pr->second.kind==V_STR) e.protoOff = pr->second.off;
        auto os=o.find("_objtStart"); if(os!=o.end()) e.objtStart=os->second.i;
        auto oe=o.find("_objtEnd");   if(oe!=o.end()) e.objtEnd=oe->second.i;
        e.kind = e.type=="SBuildingUnitDesc"?1 : (e.type=="SDoodadDesc"?0:2);
        // keep every schema field so the Properties panel is schema-driven; the four
        // the viewport mirrors (Pos/Dir/Player/Scale) are flagged, not duplicated
        for (auto& kv : o) {
            if (kv.first.empty() || kv.first[0]=='_') continue;   // _type/_objtStart/...
            const Value& v = kv.second;
            if (v.kind == V_NONE || v.off < 0) continue;
            EntityField f;
            f.name = kv.first; f.ftype = v.ftype; f.off = v.off;
            switch (v.kind) {
                case V_INT:   f.kind=FK_INT;   f.i=v.i; break;
                case V_FLOAT: f.kind=FK_FLOAT; f.f=v.f; break;
                case V_VEC3:  f.kind=FK_VEC3;  for(int k=0;k<3;k++) f.v3[k]=(float)v.v3[k]; break;
                case V_STR:   f.kind=FK_STR;   f.s=v.s; break;
                default: continue;
            }
            f.mirrored = (f.name=="Pos"||f.name=="Dir"||f.name=="Player"||f.name=="Scale");
            e.fields.push_back(std::move(f));
        }
        out.push_back(std::move(e));
    }
}

// Record the size-field offset of EVERY container that holds the entity OBJS
// (SCEN, WRLD, ..., UNTS, OBJS) so a delete/insert can shrink/grow all of them,
// plus the OBJS absolute schema_offset and the UNTS entity_count.
static void lift_containers(const Parser& P, Scene& out) {
    out.containerSizeOffs.clear();
    out.objsSchemaOff = -1; out.untsCountOff = -1;
    const Chunk* untsC = P.find(P.root, "UNTS");
    const Chunk* objsC = untsC ? P.find(*untsC, "OBJS") : nullptr;
    if (objsC) {
        out.objsSchemaOff = (long)objsC->data_off;          // schema_offset u32
        size_t es = objsC->offset, ee = objsC->offset + 8 + objsC->size;
        collect_container_sizes(P.root, es, ee, out.containerSizeOffs);
    }
    if (untsC) out.untsCountOff = (long)untsC->offset + 12;  // entity_count u32
}

// Re-walk the entity table over a mutated Scene::raw. Terrain, splat and overlay
// data are untouched by an entity insert/erase, so they are kept as-is (including
// unsaved brush edits) — only their byte offsets shift, and only if they sit at or
// after the edit point. `delta` is the signed byte change applied at `editPos`.
static bool reparse_entities(Scene& s, long editPos, long delta) {
    Parser P;
    if (!P.loadBytes(std::move(s.raw))) { s.raw = std::move(P.buf); return false; }
    std::vector<Obj> ents; P.parseEntities(ents);
    lift_entities(ents, s.entities);
    lift_containers(P, s);
    if (delta != 0) {
        if (s.heightOff >= editPos) s.heightOff += delta;
        if (s.splatOff  >= editPos) s.splatOff  += delta;
        // WTHR sits in WRLD alongside the terrain, so its offsets shift under an
        // entity insert/erase exactly the same way. Unsaved preset edits live in
        // Scene::weather, so they survive; only the write targets move.
        if (s.wthrOff >= editPos) s.wthrOff += delta;
        for (WeatherPreset& wp : s.weather) {
            if (wp.nameOff >= editPos) wp.nameOff += delta;
            if (wp.tailOff >= editPos) wp.tailOff += delta;
        }
        // Overlay pool + decal transform offsets, same rule. In the shipped chunk
        // order UNTS is the LAST child of WRLD, so an entity edit never actually
        // moves any of this — but nothing enforces that order, and a stale
        // xformOff would write a decal transform into the middle of the terrain.
        auto shiftPool = [&](Scene::OverlayPool& p) {
            if (p.chunkOff   >= editPos) p.chunkOff   += delta;
            if (p.hdrOff     >= editPos) p.hdrOff     += delta;
            if (p.contentEnd >= editPos) p.contentEnd += delta;
            for (Scene::OverlaySlotRef& sl : p.live) {
                if (sl.chunkOff >= editPos) sl.chunkOff += delta;
                if (sl.bodyOff  >= editPos) sl.bodyOff  += delta;
            }
        };
        shiftPool(s.roadPool); shiftPool(s.decalPool); shiftPool(s.riverPool);
        for (Scene::DecalRec& dr : s.decalRecs)
            if (dr.xformOff >= editPos) dr.xformOff += delta;
    }
    s.raw = std::move(P.buf);
    return true;
}

} // namespace

bool load_map_native(const std::string& path, Scene& out) {
    Parser P;
    if (!P.load(path)) return false;
    out = Scene{};
    // map name from filename
    size_t sl=path.find_last_of("/\\"); std::string base=(sl==std::string::npos)?path:path.substr(sl+1);
    size_t dot=base.find_last_of('.'); out.name=(dot==std::string::npos)?base:base.substr(0,dot);
    // WRLD
    const Chunk* wrld=P.find(P.root,"WRLD");
    int WW = wrld && wrld->meta_width>0 ? (int)wrld->meta_width : 0;
    int WH = wrld && wrld->meta_height>0 ? (int)wrld->meta_height : 0;
    out.world_w=WW; out.world_h=WH;
    // entities
    std::vector<Obj> ents; P.parseEntities(ents);
    lift_entities(ents, out.entities);
    // heightmap + colormap
    if (WW>0 && WH>0) {
        int W=0,H=0;
        if (P.heightmap(WW,WH,ents,out.heights,W,H)) {
            out.grid_w=W; out.grid_h=H;
            out.heightOff = (long)P.heightOff;   // for native height save
            out.splatOff  = (long)P.splatBase;   // per-layer uint8 weight grids
            P.colormap(out.colors);
            // real terrain paint: layer texture paths + uv scale + per-vertex opacity
            for (const auto& l : P.layers())
                out.terrainLayers.push_back({l.name, l.uvScale, l.active});
            P.splatWeights(out.splatWeights);
        } else {
            // no locatable heightmap -> synthesize a flat ground plane so the
            // map still renders (never just dots in the void).
            out.grid_w=WW+1; out.grid_h=WH+1;
            out.heights.assign((size_t)out.grid_w*out.grid_h, 0.0f);
        }
    }
    // BLCK block grid (read-only). Dims come from BLCK's own header, never WRLD.
    if (const Chunk* bc = P.find(P.root, "BLCK")) {
        size_t p0 = bc->data_off, end = bc->offset + 8 + bc->size;
        if (p0 + 12 <= end) {
            uint32_t ver = P.D.u32(p0);
            int bw = (int)P.D.u32(p0 + 4), bh = (int)P.D.u32(p0 + 8);
            size_t need = (size_t)bw * bh * 3;
            if (ver == 3 && bw > 0 && bh > 0 && p0 + 12 + need <= end) {
                out.blckW = bw; out.blckH = bh;
                size_t fp = p0 + 12, tp = fp + (size_t)bw * bh * 2;
                out.blckFlags.resize((size_t)bw * bh);
                for (size_t k = 0; k < out.blckFlags.size(); k++)
                    out.blckFlags[k] = P.D.u16(fp + k * 2);
                out.blckTypes.assign(P.D.d + tp, P.D.d + tp + (size_t)bw * bh);
            }
        }
    }

    lift_containers(P, out);

    out.raw = std::move(P.buf);   // keep the bytes for native in-place save
    out.srcPath = path;
    // roads (GROL/GROA) + decals (GDCL/GDEC) overlay geometry (needs heights set)
    parse_overlays(out.raw, out);
    parse_weather(out.raw, out);       // WRLD/WTHR lighting presets
    map_scenario_read(out.raw, out.scen);   // locations / objectives / triggers
    out.loaded = true;
    return true;
}

// ---- structural edits (in memory) -------------------------------------------
// All of these mutate Scene::raw directly and then re-walk the entity table.
// Nothing round-trips through a file, so pending field edits already flushed into
// `raw` survive, and a place/delete costs one entity walk instead of a full reload.

namespace {

inline uint32_t rd32(const std::vector<unsigned char>& b, long o) {
    return (uint32_t)(b[o] | (b[o+1]<<8) | (b[o+2]<<16) | ((uint32_t)b[o+3]<<24));
}
inline void wr32(std::vector<unsigned char>& b, long o, uint32_t v) {
    b[o]=v&0xff; b[o+1]=(v>>8)&0xff; b[o+2]=(v>>16)&0xff; b[o+3]=(v>>24)&0xff;
}

// Adjust every ancestor container size + the OBJS schema_offset by `delta` bytes,
// and the UNTS entity_count by `countDelta`. Every one of these offsets lies in a
// chunk header *before* the entity region, so they stay valid across the edit.
void patch_container_sizes(const Scene& s, std::vector<unsigned char>& b,
                           long delta, int countDelta) {
    auto bump = [&](long off) {
        if (off < 0 || off + 4 > (long)b.size()) return;
        wr32(b, off, (uint32_t)((int64_t)rd32(b, off) + delta));
    };
    for (long off : s.containerSizeOffs) bump(off);
    bump(s.objsSchemaOff);   // SCHD sits after the entity region -> it shifts too
    if (countDelta && s.untsCountOff >= 0 && s.untsCountOff + 4 <= (long)b.size()) {
        int64_t v = (int64_t)rd32(b, s.untsCountOff) + countDelta;
        if (v < 0) v = 0;
        wr32(b, s.untsCountOff, (uint32_t)v);
    }
}

// Absolute offset of the SCHD that terminates the entity region — i.e. where a new
// OBJT is appended.
long schd_pos(const Scene& s) {
    if (s.objsSchemaOff < 0 || s.objsSchemaOff + 4 > (long)s.raw.size()) return -1;
    long p = (long)rd32(s.raw, s.objsSchemaOff);
    return (p >= 0 && p <= (long)s.raw.size()) ? p : -1;
}

} // namespace

bool erase_objt_bytes(Scene& s, long pos, long len) {
    if (s.raw.empty() || pos < 0 || len <= 0 || pos + len > (long)s.raw.size()) return false;
    s.raw.erase(s.raw.begin() + pos, s.raw.begin() + pos + len);
    patch_container_sizes(s, s.raw, -len, -1);
    return reparse_entities(s, pos, -len);
}

bool insert_objt_at_index(Scene& s, int entIndex, const std::vector<unsigned char>& objt) {
    if (s.raw.empty() || objt.empty()) return false;
    long pos;
    if (entIndex >= 0 && entIndex < (int)s.entities.size())
        pos = s.entities[entIndex].objtStart;   // restore the original slot
    else
        pos = schd_pos(s);                      // append at the end of the list
    if (pos < 0 || pos > (long)s.raw.size()) return false;
    s.raw.insert(s.raw.begin() + pos, objt.begin(), objt.end());
    patch_container_sizes(s, s.raw, (long)objt.size(), +1);
    return reparse_entities(s, pos, (long)objt.size());
}

bool delete_entity_bytes(Scene& s, long id, std::vector<unsigned char>* outBytes,
                         int* outIndex) {
    int idx = -1;
    for (size_t i = 0; i < s.entities.size(); i++) if (s.entities[i].id == id) { idx=(int)i; break; }
    if (idx < 0) return false;
    const Entity& e = s.entities[idx];
    if (e.objtStart < 0 || e.objtEnd <= e.objtStart || e.objtEnd > (long)s.raw.size()) return false;
    if (outBytes) outBytes->assign(s.raw.begin() + e.objtStart, s.raw.begin() + e.objtEnd);
    if (outIndex) *outIndex = idx;
    return erase_objt_bytes(s, e.objtStart, e.objtEnd - e.objtStart);
}

// Build the OBJT blob for a clone of `srcId` with a new ID and position, and
// optionally a different Prototype GUID.
//
// Swapping the prototype is what lets the editor place a model the map has never
// used, without constructing an OBJT from the schema: clone a record of the right
// entity schema and point it at another prototype. Every CPCW prototype GUID is
// exactly 36 characters (verified: 2017/2017 in ProtoDB, 3427/3427 on M_01), so
// the swap is size-preserving and the record never moves. If a GUID of a different
// length ever turns up, refuse rather than corrupt the record.
bool build_entity_clone(const Scene& s, long srcId, const float pos[3], long newId,
                        std::vector<unsigned char>& out, const std::string& protoGuid) {
    const Entity* src = nullptr;
    for (const auto& en : s.entities) if (en.id == srcId) { src = &en; break; }
    if (!src || src->objtStart < 0 || src->objtEnd <= src->objtStart ||
        src->objtEnd > (long)s.raw.size()) return false;
    long ss = src->objtStart;
    out.assign(s.raw.begin() + ss, s.raw.begin() + src->objtEnd);
    if (src->posOff >= ss && src->posOff + 12 <= src->objtEnd) {
        long r = src->posOff - ss;
        for (int k = 0; k < 3; k++) { uint32_t v; std::memcpy(&v, &pos[k], 4); wr32(out, r + k*4, v); }
    }
    if (src->idOff >= ss && src->idOff + 4 <= src->objtEnd)
        wr32(out, src->idOff - ss, (uint32_t)newId);
    if (!protoGuid.empty()) {
        if (src->protoOff < ss) return false;
        long r = src->protoOff - ss;
        if (r + 2 > (long)out.size()) return false;
        size_t len = (size_t)(out[r] | (out[r+1] << 8));
        if (len != protoGuid.size() || r + 2 + (long)len > (long)out.size()) return false;
        std::memcpy(&out[r + 2], protoGuid.data(), len);
    }
    return true;
}

bool add_entity_bytes(Scene& s, long srcId, const float pos[3], long newId,
                      std::vector<unsigned char>* outBytes, const std::string& protoGuid) {
    std::vector<unsigned char> blob;
    if (!build_entity_clone(s, srcId, pos, newId, blob, protoGuid)) return false;
    if (outBytes) *outBytes = blob;
    return insert_objt_at_index(s, -1, blob);   // append before SCHD
}

// ---- chunk outline ----------------------------------------------------------
static void flatten_chunks(const Chunk& c, int depth, std::vector<ChunkNode>& out) {
    out.push_back({ depth, c.tag, (long)c.offset, (long)c.size });
    for (const auto& ch : c.children) flatten_chunks(ch, depth + 1, out);
}
bool map_chunk_outline(const std::vector<unsigned char>& raw, std::vector<ChunkNode>& out) {
    out.clear();
    if (raw.empty()) return false;
    Parser P;
    if (!P.loadBytes(raw)) return false;   // copies; the inspector is not a hot path
    flatten_chunks(P.root, 0, out);
    return true;
}

// ---- chunk tiling check -----------------------------------------------------
// The detector `cpcw_map.py roundtrip` was believed to be and is not. That
// command re-emits every chunk's ORIGINAL byte span and never recomputes a size,
// so a structural edit that forgets to bump an ancestor container still round-
// trips IDENTICAL. This walk actually checks the two invariants a structural
// edit can break:
//   (1) each container's children TILE its content exactly — no gap, no overlap,
//       no trailing slack — after its own fixed sub-header;
//   (2) every OBJS chunk's absolute `schema_offset` still points at its own SCHD
//       child (true on 225/225 OBJS across the 45 shipped maps).
// Bytes after the root SCEN chunk are reported separately rather than treated as
// an error: the format allows a trailer and the Python writer preserves it.

// Content bytes a container reserves for itself before its first child. Must
// stay in step with Parser::parseChildren.
static long chunk_subheader(const std::string& tag) {
    if (tag=="SCEN") return 4;
    if (tag=="PREC"||tag=="SETS"||tag=="OJTS"||tag=="OBJS") return 4;
    if (tag=="WRLD") return 12;
    if (tag=="GTRN") return 1;
    if (tag=="UNTS") return 8;
    return 0;
}

static void tile_walk(const Chunk& c, ChunkTileReport& r) {
    r.chunks++;
    if (c.children.empty()) return;
    r.containers++;
    long contentStart = (long)c.offset + 8 + chunk_subheader(c.tag);
    long contentEnd   = (long)c.offset + 8 + (long)c.size;
    long cursor = contentStart;
    char msg[256];
    for (const Chunk& ch : c.children) {
        long off = (long)ch.offset;
        if (off != cursor) {
            long d = off - cursor;
            if (d > 0) r.gapBytes += d; else r.overlapBytes += -d;
            snprintf(msg, sizeof(msg), "%s@%ld: child %s@%ld %s %ld bytes",
                     c.tag.c_str(), (long)c.offset, ch.tag.c_str(), off,
                     d > 0 ? "leaves a gap of" : "overlaps by", d > 0 ? d : -d);
            r.issues.push_back(msg);
        }
        cursor = off + 8 + (long)ch.size;
    }
    if (cursor != contentEnd) {
        long d = contentEnd - cursor;
        if (d > 0) r.gapBytes += d; else r.overlapBytes += -d;
        snprintf(msg, sizeof(msg), "%s@%ld: children end at %ld but the chunk ends at %ld (%+ld)",
                 c.tag.c_str(), (long)c.offset, cursor, contentEnd, d);
        r.issues.push_back(msg);
    }
    for (const Chunk& ch : c.children) tile_walk(ch, r);
}

static void tile_check_objs(const Chunk& c, ChunkTileReport& r) {
    if (c.tag=="OBJS") {
        r.objs++;
        long schd = -1;
        for (const Chunk& ch : c.children) if (ch.tag=="SCHD") { schd = (long)ch.offset; break; }
        if (schd >= 0 && schd == c.meta_schema_off) r.objsSchemaOk++;
        else {
            char msg[256];
            snprintf(msg, sizeof(msg), "OBJS@%ld: schema_offset %ld but its SCHD child is at %ld",
                     (long)c.offset, c.meta_schema_off, schd);
            r.issues.push_back(msg);
        }
    }
    for (const Chunk& ch : c.children) tile_check_objs(ch, r);
}

bool map_chunk_tile(const std::vector<unsigned char>& raw, ChunkTileReport& out) {
    out = ChunkTileReport{};
    if (raw.empty()) return false;
    Parser P;
    if (!P.loadBytes(raw)) return false;
    out.fileSize = (long)raw.size();
    tile_walk(P.root, out);
    tile_check_objs(P.root, out);
    out.trailerBytes = out.fileSize - (long)(P.root.offset + 8 + P.root.size);
    out.ok = out.gapBytes == 0 && out.overlapBytes == 0 &&
             out.objs > 0 && out.objsSchemaOk == out.objs;
    return true;
}

// ---- file wrappers (dev harnesses --addtest / --deltest keep working) --------
static bool write_all(const std::string& path, const std::vector<unsigned char>& b) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)b.data(), (std::streamsize)b.size());
    return (bool)f;
}

bool delete_entity_native(const Scene& s, long id, const std::string& outPath) {
    Scene tmp = s;
    if (!delete_entity_bytes(tmp, id, nullptr, nullptr)) return false;
    return write_all(outPath, tmp.raw);
}

bool add_entity_native(const Scene& s, long srcId, const float pos[3], long newId,
                       const std::string& outPath) {
    Scene tmp = s;
    if (!add_entity_bytes(tmp, srcId, pos, newId, nullptr)) return false;
    return write_all(outPath, tmp.raw);
}

// FT ids needed by the saver (must match the enum above)
enum { SAVE_FT_INT32=0x0001, SAVE_FT_FLOAT=0x0002, SAVE_FT_BOOL=0x0003,
       SAVE_FT_FLOAT64=0x0005, SAVE_FT_VEC3=0x0006, SAVE_FT_IID=0x0013,
       SAVE_FT_ENTREF=0x0014, SAVE_FT_UINT8=0x0017, SAVE_FT_COLOR=0x0018,
       SAVE_FT_INT16=0x0019 };

// Is this field type fixed-width, so the Properties panel can edit it in place
// without changing the record size? Strings are not — they would resize the OBJT.
bool field_is_writable(unsigned ftype) {
    switch (ftype) {
        case SAVE_FT_INT32: case SAVE_FT_FLOAT: case SAVE_FT_BOOL:
        case SAVE_FT_FLOAT64: case SAVE_FT_VEC3: case SAVE_FT_IID:
        case SAVE_FT_ENTREF: case SAVE_FT_UINT8: case SAVE_FT_COLOR:
        case SAVE_FT_INT16:
            return true;
        default: return false;
    }
}

// Overwrite one schema field in place. Size-preserving by construction — the write
// width comes from the schema's own field type, so the record never moves.
static void write_field(std::vector<unsigned char>& b, const EntityField& f) {
    long o = f.off;
    if (o < 0) return;
    auto put32=[&](long p, uint32_t v){ if(p<0||p+4>(long)b.size())return; b[p]=v&0xff; b[p+1]=(v>>8)&0xff; b[p+2]=(v>>16)&0xff; b[p+3]=(v>>24)&0xff; };
    auto putf =[&](long p, float x){ uint32_t v; std::memcpy(&v,&x,4); put32(p,v); };
    switch (f.ftype) {
        case SAVE_FT_FLOAT:   putf(o, (float)f.f); break;
        case SAVE_FT_VEC3:    putf(o, f.v3[0]); putf(o+4, f.v3[1]); putf(o+8, f.v3[2]); break;
        case SAVE_FT_FLOAT64: { double d=f.f; uint64_t v; std::memcpy(&v,&d,8);
                                put32(o,(uint32_t)(v&0xFFFFFFFFu)); put32(o+4,(uint32_t)(v>>32)); } break;
        case SAVE_FT_BOOL:
        case SAVE_FT_UINT8:   if (o < (long)b.size()) b[o] = (unsigned char)(f.i & 0xff); break;
        case SAVE_FT_INT16:   if (o+2 <= (long)b.size()) { b[o]=f.i&0xff; b[o+1]=(f.i>>8)&0xff; } break;
        case SAVE_FT_INT32: case SAVE_FT_IID: case SAVE_FT_ENTREF: case SAVE_FT_COLOR:
                              put32(o, (uint32_t)f.i); break;
        default: break;       // strings and containers are not edited in place
    }
}

void apply_edits_inplace(const Scene& s, const std::vector<long>& editedIds,
                         std::vector<unsigned char>& b) {
    auto put32=[&](long off, uint32_t v){ if(off<0||off+4>(long)b.size())return; b[off]=v&0xff; b[off+1]=(v>>8)&0xff; b[off+2]=(v>>16)&0xff; b[off+3]=(v>>24)&0xff; };
    auto putf =[&](long off, float f){ uint32_t v; std::memcpy(&v,&f,4); put32(off,v); };
    // edited terrain heights: write ONLY brush-touched cells (keeps NaN sentinels
    // and untouched cells byte-identical)
    if (s.terrainEdited && s.heightOff >= 0 && !s.heights.empty()) {
        bool haveMask = s.heightDirty.size() == s.heights.size();
        for (size_t i = 0; i < s.heights.size(); i++)
            if (!haveMask || s.heightDirty[i]) putf(s.heightOff + (long)i*4, s.heights[i]);
    }
    // painted splat layers: one uint8 opacity grid per layer, laid out back to back
    // from splatOff. Fixed-size, so writing them keeps the file byte-faithful; only
    // cells the brush touched are written.
    if (s.splatEdited && s.splatOff >= 0 && !s.splatWeights.empty()) {
        size_t need = (size_t)s.grid_w * s.grid_h;
        bool haveMask = s.splatDirty.size() == s.splatWeights.size();
        for (size_t L = 0; L < s.splatWeights.size(); L++) {
            const auto& g = s.splatWeights[L];
            if (g.size() != need) continue;
            long base = s.splatOff + (long)(L * need);
            for (size_t i = 0; i < need; i++) {
                if (haveMask && !s.splatDirty[L][i]) continue;
                long o = base + (long)i;
                if (o >= 0 && o < (long)b.size()) b[o] = g[i];
            }
        }
    }
    // WTHR lighting presets: every field is fixed-width, so this is the same
    // size-preserving in-place write, gated on its own edited flag.
    apply_weather_inplace(s, b);
    for (long id : editedIds) {
        const Entity* e=nullptr;
        for (const auto& en : s.entities) if (en.id==id) { e=&en; break; }
        if (!e) continue;
        if (e->posOff>=0) { putf(e->posOff,e->pos[0]); putf(e->posOff+4,e->pos[1]); putf(e->posOff+8,e->pos[2]); }
        if (e->dirOff>=0) putf(e->dirOff, e->dir);   // Dir yaw = first float
        if (e->scaleOff>=0) putf(e->scaleOff, e->scale);
        // schema fields edited in the Properties panel (everything but the four the
        // viewport mirrors). Only dirty ones are written, so a read->write round trip
        // can never perturb a field the user did not touch.
        for (const EntityField& f : e->fields)
            if (f.dirty && !f.mirrored) write_field(b, f);
        if (e->playerOff>=0) {
            long o=e->playerOff; uint32_t v=(uint32_t)e->player;
            switch (e->playerFtype) {
                case SAVE_FT_UINT8: if(o<(long)b.size()) b[o]=v&0xff; break;
                case SAVE_FT_INT16: if(o+2<=(long)b.size()){ b[o]=v&0xff; b[o+1]=(v>>8)&0xff; } break;
                default: put32(o,v); break;   // INT32 / IID / ENTREF
            }
        }
    }
}

bool save_map_native(const Scene& s, const std::vector<long>& editedIds,
                     const std::string& outPath) {
    if (s.raw.empty()) return false;
    std::vector<unsigned char> b = s.raw;   // copy; overwrite only edited fields
    apply_edits_inplace(s, editedIds, b);
    return write_all(outPath, b);
}
