// Native ProtoDB.bin -> {guid: model .srm path}. Same OBJT/VOBJ/SCHD field
// format as .map (see mapfile.cpp); here we walk EVERY object (incl. nested
// arrays) and collect GUID + ModelName. Port of cpcw_protodb.build_model_index.
#include "protodb.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

enum {
    FT_INT32=0x0001, FT_FLOAT=0x0002, FT_BOOL=0x0003, FT_STRING=0x0004,
    FT_FLOAT64=0x0005, FT_VEC3=0x0006, FT_GUID=0x0011, FT_REF=0x0012,
    FT_IID=0x0013, FT_ENTREF=0x0014, FT_VEC2F=0x0015, FT_VEC2I=0x0016,
    FT_UINT8=0x0017, FT_COLOR=0x0018, FT_INT16=0x0019, FT_LOCSTR=0x002B,
    FT_INLINE1=0x0088, FT_INLINE2=0x0089, FT_BLOB=0x0165, FT_FLAGS=0x039C,
    FT_ARRAY=0x898A
};
static bool stringLike(uint32_t t){ return t==FT_STRING||t==FT_GUID||t==FT_REF||t==FT_FLAGS||t==FT_LOCSTR; }

struct Field { std::string name; uint32_t type, size; };
struct Schema { std::string name; std::vector<Field> fields; };

struct DB {
    std::vector<uint8_t> b; size_t n=0;
    std::map<uint16_t, Schema> schemas;
    std::map<std::string,std::string>* out=nullptr;       // guid -> model (legacy)
    std::map<std::string,ProtoInfo>* full=nullptr;        // guid -> everything

    uint8_t  u8 (size_t p){ return p<n?b[p]:0; }
    uint16_t u16(size_t p){ return p+2<=n?(b[p]|(b[p+1]<<8)):0; }
    uint32_t u32(size_t p){ return p+4<=n?(uint32_t)(b[p]|(b[p+1]<<8)|(b[p+2]<<16)|((uint32_t)b[p+3]<<24)):0; }
    bool tag(size_t p,const char* t){ return p+4<=n && std::memcmp(b.data()+p,t,4)==0; }
    std::string str(size_t p,size_t& out){ uint16_t L=u16(p); size_t s=p+2; std::string r; if(s+L<=n)r.assign((const char*)b.data()+s,L); out=s+L; return r; }

    bool load(const std::string& path){
        std::ifstream f(path,std::ios::binary); if(!f) return false;
        f.seekg(0,std::ios::end); std::streamoff sz=f.tellg(); f.seekg(0);
        b.resize((size_t)sz); f.read((char*)b.data(),sz); n=b.size();
        if(!tag(0,"OBJS")) return false;
        parseSchd(u32(8));       // schema_offset @ +8
        return true;
    }
    void parseSchd(size_t pos){
        if(!tag(pos,"SCHD")) return;
        size_t limit=pos+8+u32(pos+4); pos+=8;
        uint16_t count=u16(pos); pos+=4;
        for(int s=0;s<count;s++){
            if(pos>=limit||!tag(pos,"SCHM")) break;
            size_t cs=pos+8, ce=cs+u32(pos+4), p=cs, np;
            Schema sc; sc.name=str(p,np); p=np;
            uint16_t typeId=u16(p); p+=2; p+=2; uint16_t fc=u16(p); p+=2; // type_id, version, field_count
            for(int fi=0;fi<fc;fi++){
                if(p+2>ce) break;
                Field fl; fl.name=str(p,np); p=np;
                if(p+8>ce) break;
                fl.type=u32(p); p+=4; fl.size=u32(p); p+=4;
                sc.fields.push_back(std::move(fl));
            }
            schemas[typeId]=std::move(sc);
            pos=ce;
        }
    }

    // field reader (advances correctly for every type; returns string for
    // string-like into `sv`, else sv stays empty)
    size_t readField(size_t p, uint32_t ft, uint32_t fs, size_t limit, std::string& sv){
        sv.clear();
        if(p>=limit) return p;
        if(ft==FT_INT32||ft==FT_FLOAT||ft==FT_COLOR||ft==FT_IID||ft==FT_ENTREF){ return p+4<=limit?p+4:limit; }
        if(ft==FT_BOOL||ft==FT_UINT8){ return p+1<=limit?p+1:limit; }
        if(ft==FT_INT16){ return p+2<=limit?p+2:limit; }
        if(ft==FT_FLOAT64){ return p+8<=limit?p+8:limit; }
        if(ft==FT_VEC3){ return p+12<=limit?p+12:limit; }
        if(ft==FT_VEC2F||ft==FT_VEC2I){ return p+8<=limit?p+8:limit; }
        if(stringLike(ft)){ if(p+2>limit)return limit; size_t np; sv=str(p,np); return np; }
        if(ft==FT_ARRAY) return readArray(p,limit);
        if(ft==FT_INLINE1||ft==FT_INLINE2) return readInline(p,limit);
        if(ft==FT_BLOB){ size_t nb=(fs>0&&fs<0xFFFF)?fs:0; return (nb>0&&p+nb<=limit)?p+nb:p; }
        if(fs>0&&fs<0xFFFF){ return p+fs<=limit?p+fs:p; }
        if(fs==0xFFFF||fs==0){
            if(tag(p,"ARRY")||tag(p,"OBJT")||tag(p,"VOBJ")) return readInline(p,limit);
            if(p+2<=limit){ uint16_t L=u16(p); if(L<4096&&p+2+L<=limit){ size_t np; sv=str(p,np); return np; } }
        }
        return p;
    }
    size_t readArray(size_t p, size_t limit){
        if(p+12>limit||!tag(p,"ARRY")) return p;
        size_t arrEnd=p+8+u32(p+4); uint32_t cnt=u32(p+8); p+=12;
        for(uint32_t k=0;k<cnt;k++){ if(p>=arrEnd)break; if(tag(p,"OBJT")){ size_t e; parseObjt(p,e); p=e; } else break; }
        return arrEnd;
    }
    struct Lift;
    size_t readInline(size_t p, size_t limit){
        if(tag(p,"OBJT")){ size_t e; parseObjt(p,e); return e; }
        if(tag(p,"VOBJ")){ size_t e; skipVobj(p,e); return e; }
        if(tag(p,"ARRY")) return readArray(p,limit);
        return p;
    }
    void skipVobj(size_t pos, size_t& endOut){
        endOut = tag(pos,"VOBJ") ? pos+8+u32(pos+4) : pos;
    }
    // Fields lifted out of one prototype object.
    struct Lift { std::string guid, model, name, schema; };

    void parseVobj(size_t pos, size_t& endOut, Lift& L){
        if(!tag(pos,"VOBJ")){ endOut=pos; return; }
        size_t contentEnd=pos+8+u32(pos+4); uint16_t typeId=u16(pos+8);
        auto it=schemas.find(typeId);
        size_t p=pos+10; endOut=contentEnd;
        if(p+2>contentEnd) return; p+=2;   // version
        if(it!=schemas.end()){
            if(L.schema.empty()) L.schema=it->second.name;
            for(auto& fl:it->second.fields){
                if(p>=contentEnd) break;
                std::string sv; p=readField(p,fl.type,fl.size,contentEnd,sv);
                if(!sv.empty()){
                    if(fl.name=="GUID") L.guid=sv;
                    else if(fl.name=="Name") { if(L.name.empty()) L.name=sv; }
                    else if(fl.name=="ModelName" || (L.model.empty()&&fl.name=="MarketModelName")) L.model=sv;
                }
            }
        }
    }
    void parseObjt(size_t pos, size_t& endOut){
        if(!tag(pos,"OBJT")){ endOut=pos; return; }
        size_t contentEnd=pos+8+u32(pos+4);
        Lift L; size_t ve; parseVobj(pos+10, ve, L);
        size_t p=ve;
        // trailing VOBJs extend the same object (derived schemas); merge them
        while(p+10<=contentEnd && tag(p,"VOBJ")){
            Lift x; size_t te; parseVobj(p,te,x); p=te;
            if(!x.guid.empty()) L.guid=x.guid;
            if(!x.name.empty() && L.name.empty()) L.name=x.name;
            if(!x.model.empty() && L.model.empty()) L.model=x.model;
        }
        if(!L.guid.empty()){
            std::string gl; for(char c:L.guid) gl+=(char)tolower((unsigned char)c);
            std::string mp; for(char c:L.model) mp+=(c=='\\'?'/':c);
            if(out && !mp.empty()) (*out)[gl]=mp;
            if(full){
                ProtoInfo& pi=(*full)[gl];
                if(pi.model.empty()) pi.model=mp;
                if(pi.name.empty())  pi.name=L.name;
                if(pi.schema.empty())pi.schema=L.schema;
            }
        }
        endOut=contentEnd;
    }
};

} // namespace

std::map<std::string,std::string> protodb_model_index(const std::string& path){
    std::map<std::string,std::string> idx;
    DB db; db.out=&idx;
    if(!db.load(path)) return idx;
    size_t e; db.parseObjt(12, e);   // root OBJT at offset 12
    return idx;
}

std::map<std::string,ProtoInfo> protodb_full_index(const std::string& path){
    std::map<std::string,ProtoInfo> idx;
    DB db; db.full=&idx;
    if(!db.load(path)) return idx;
    size_t e; db.parseObjt(12, e);
    return idx;
}

// ProtoDB prototype schemas and .map entity schemas are the same names in two
// dialects: "SP<X>" there, "S<X>Desc" here. SPDoodad -> SDoodadDesc,
// SPVehicleUnit -> SVehicleUnitDesc, SPBuildingUnit -> SBuildingUnitDesc.
std::string protodb_map_schema(const std::string& schema){
    if(schema.size()<3 || schema[0]!='S' || schema[1]!='P') return "";
    return "S" + schema.substr(2) + "Desc";
}
