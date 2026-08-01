// 3D viewport: renders the map terrain (from the heightmap grid) and entity
// markers with an orbit camera, in OpenGL 3.3 core. Drawn into the editor's
// dockspace central-node region (see main.cpp).
#pragma once
#include "glcore.h"
#include "scene.h"
#include "protodb.h"
#include "vfs.h"             // resolve logical paths to disk or extracted-from-pak temp
#include "srm_model.h"        // viewer's .srm geometry loader (added to the build)
#include "dds.h"             // viewer's DDS decoder (added to the build)
#include <cmath>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

// ------------------------------------------------------------------ math ----
struct V3 { float x = 0, y = 0, z = 0; };
static inline V3 operator+(V3 a, V3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline V3 operator-(V3 a, V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline V3 operator*(V3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
static inline float dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3 cross(V3 a, V3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static inline V3 norm(V3 a) { float l = std::sqrt(dot(a,a)); return l>1e-8f ? a*(1.0f/l) : a; }

struct M4 { float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; };
static inline M4 mul(const M4& a, const M4& b) {
    M4 r; for (int c=0;c<4;c++) for (int row=0;row<4;row++) {
        float s=0; for (int k=0;k<4;k++) s += a.m[k*4+row]*b.m[c*4+k];
        r.m[c*4+row]=s; } return r;
}
static inline M4 perspective(float fovy, float aspect, float zn, float zf) {
    M4 r{}; for (int i=0;i<16;i++) r.m[i]=0;
    float t = std::tan(fovy*0.5f);
    r.m[0]=1.0f/(aspect*t); r.m[5]=1.0f/t;
    r.m[10]=(zf+zn)/(zn-zf); r.m[11]=-1.0f; r.m[14]=(2*zf*zn)/(zn-zf);
    return r;
}
static inline M4 translate(V3 t){ M4 m; m.m[12]=t.x; m.m[13]=t.y; m.m[14]=t.z; return m; }
static inline M4 scaleM(float x,float y,float z){ M4 m; m.m[0]=x; m.m[5]=y; m.m[10]=z; return m; }
static inline M4 rotY(float a){ M4 m; float c=std::cos(a),s=std::sin(a); m.m[0]=c; m.m[2]=-s; m.m[8]=s; m.m[10]=c; return m; }

static inline M4 lookAt(V3 eye, V3 c, V3 up) {
    V3 f=norm(c-eye), s=norm(cross(f,up)), u=cross(s,f);
    M4 r; r.m[0]=s.x; r.m[4]=s.y; r.m[8]=s.z;
    r.m[1]=u.x; r.m[5]=u.y; r.m[9]=u.z;
    r.m[2]=-f.x; r.m[6]=-f.y; r.m[10]=-f.z;
    r.m[3]=0; r.m[7]=0; r.m[11]=0;
    r.m[12]=-dot(s,eye); r.m[13]=-dot(u,eye); r.m[14]=dot(f,eye); r.m[15]=1;
    return r;
}

struct Camera {
    V3 target{256,0,256};
    float yaw = 0.8f, pitch = 0.9f, dist = 500.0f;
    V3 eye() const {
        return target + V3{std::cos(pitch)*std::sin(yaw), std::sin(pitch),
                           std::cos(pitch)*std::cos(yaw)} * dist;
    }
    // Kept separate as well as combined: ImGuizmo wants view and projection as two
    // column-major float[16]s (the same layout glUniformMatrix4fv takes).
    M4 view() const { return lookAt(eye(), target, {0,1,0}); }
    M4 proj(float aspect) const { return perspective(0.9f, aspect, 0.5f, 8000.0f); }
    M4 viewProj(float aspect) const { return mul(proj(aspect), view()); }
};

// --------------------------------------------------------------- renderer ----
static inline GLuint glCompile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log);
               fprintf(stderr, "shader compile: %s\n", log); }
    return s;
}
static inline GLuint glProgram(const char* vs, const char* fs) {
    GLuint v=glCompile(GL_VERTEX_SHADER,vs), f=glCompile(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p,1024,nullptr,log);
               fprintf(stderr, "link: %s\n", log); }
    glDeleteShader(v); glDeleteShader(f); return p;
}
static inline GLuint glProgram(const char* vs, const char* gs, const char* fs) {
    GLuint v=glCompile(GL_VERTEX_SHADER,vs), g=glCompile(GL_GEOMETRY_SHADER,gs),
           f=glCompile(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,g); glAttachShader(p,f);
    glLinkProgram(p);
    GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p,1024,nullptr,log);
               fprintf(stderr, "link: %s\n", log); }
    glDeleteShader(v); glDeleteShader(g); glDeleteShader(f); return p;
}

class Viewport3D {
    struct Part { int off=0, count=0; GLuint tex=0; bool alphaTest=false; };   // index range + diffuse tex
    struct GLModel { GLuint vao=0, vbo=0, ebo=0; std::vector<Part> parts;
                     V3 bmin{1e9f,1e9f,1e9f}, bmax{-1e9f,-1e9f,-1e9f}; };  // local AABB
    struct ModelInst { GLModel* model; M4 xf; int entIdx=-1; };
public:
    void init() {
        terrainProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; layout(location=1) in vec3 aN;\n"
            "layout(location=2) in vec3 aCol;\n"
            "uniform mat4 uMVP; out vec3 vN; out float vH; out vec3 vCol;\n"
            "void main(){ gl_Position=uMVP*vec4(aPos,1.0); vN=aN; vH=aPos.y; vCol=aCol; }\n",
            "#version 330 core\n"
            "in vec3 vN; in float vH; in vec3 vCol; out vec4 F;\n"
            "uniform vec3 uLight; uniform int uUseColor;\n"
            "void main(){ vec3 n=normalize(vN);\n"
            " float d=max(dot(n,normalize(uLight)),0.0)*0.75+0.25;\n"
            " vec3 col;\n"
            " if(uUseColor==1){ col=vCol; }\n"
            " else if(vH<0.0){ col=mix(vec3(0.20,0.30,0.50),vec3(0.34,0.44,0.32),clamp(vH/-20.0+1.0,0.0,1.0)); }\n"
            " else { col=mix(vec3(0.34,0.44,0.30),vec3(0.55,0.50,0.40),clamp(vH/25.0,0.0,1.0));\n"
            "        col=mix(col,vec3(0.9),clamp((vH-18.0)/12.0,0.0,1.0)); }\n"
            " F=vec4(col*d,1.0); }\n");
        uTerrMVP = glGetUniformLocation(terrainProg, "uMVP");
        uTerrLight = glGetUniformLocation(terrainProg, "uLight");
        uTerrUseColor = glGetUniformLocation(terrainProg, "uUseColor");

        entProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; layout(location=1) in vec3 aCol;\n"
            "layout(location=2) in float aSize;\n"
            "uniform mat4 uMVP; uniform float uSize; out vec3 vCol;\n"
            "void main(){ gl_Position=uMVP*vec4(aPos,1.0);\n"
            " gl_PointSize = uSize > 0.0 ? uSize : aSize; vCol=aCol; }\n",
            "#version 330 core\n"
            "in vec3 vCol; out vec4 F; uniform int uWhite;\n"
            "void main(){ vec2 c=gl_PointCoord*2.0-1.0; if(dot(c,c)>1.0) discard;\n"
            " F=vec4(uWhite==1?vec3(1.0):vCol,1.0); }\n");
        uEntMVP = glGetUniformLocation(entProg, "uMVP");
        uEntSize = glGetUniformLocation(entProg, "uSize");
        uEntWhite = glGetUniformLocation(entProg, "uWhite");

        modelProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; layout(location=1) in vec3 aN;\n"
            "layout(location=2) in vec2 aUV;\n"
            "uniform mat4 uMVP; uniform mat4 uModel; out vec3 vN; out vec2 vUV;\n"
            "void main(){ gl_Position=uMVP*vec4(aPos,1.0); vN=mat3(uModel)*aN; vUV=aUV; }\n",
            "#version 330 core\n"
            "in vec3 vN; in vec2 vUV; out vec4 F;\n"
            "uniform vec3 uLight, uColor; uniform sampler2D uTex; uniform int uHasTex, uAlphaTest;\n"
            "void main(){\n"
            " vec4 tx = uHasTex==1 ? texture(uTex, vec2(vUV.x, 1.0-vUV.y)) : vec4(uColor,1.0);\n"
            " if (uAlphaTest==1 && tx.a < 0.5) discard;\n"                    // foliage/fence cutout
            " vec3 n=normalize(vN); if(!gl_FrontFacing) n=-n;\n"             // two-sided
            " vec3 kdir=normalize(uLight);\n"                                // key
            " vec3 fdir=normalize(vec3(-0.5, 0.35, -0.6));\n"               // fill (opposite-ish)
            " float key=max(dot(n,kdir),0.0);\n"
            " float fill=max(dot(n,fdir),0.0);\n"
            " float amb=mix(0.32, 0.52, n.y*0.5+0.5);\n"                    // hemispheric ambient
            " float lit=amb + 0.60*key + 0.16*fill;\n"
            " vec3 c=pow(tx.rgb*lit, vec3(1.0/2.2));\n"                      // gamma (fixes muddy)
            " F=vec4(c,1.0); }\n");
        uMdlMVP=glGetUniformLocation(modelProg,"uMVP");
        uMdlModel=glGetUniformLocation(modelProg,"uModel");
        uMdlLight=glGetUniformLocation(modelProg,"uLight");
        uMdlColor=glGetUniformLocation(modelProg,"uColor");
        uMdlHasTex=glGetUniformLocation(modelProg,"uHasTex");
        uMdlAlphaTest=glGetUniformLocation(modelProg,"uAlphaTest");
        glUseProgram(modelProg); glUniform1i(glGetUniformLocation(modelProg,"uTex"),0);

        // Splat-textured terrain: blend up to MAXL real layer .dds by per-vertex
        // opacity (2 RGBA weight textures) tiled by uv_scale. Base layer opaque.
        terrainTexProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; layout(location=1) in vec3 aN;\n"
            "uniform mat4 uMVP; uniform vec2 uGridInv;\n"
            "out vec3 vN; out vec2 vTerrUV; out vec2 vWorldXZ;\n"
            "void main(){ gl_Position=uMVP*vec4(aPos,1.0); vN=aN;\n"
            " vTerrUV=vec2((aPos.x+0.5)*uGridInv.x,(aPos.z+0.5)*uGridInv.y);\n"
            " vWorldXZ=vec2(aPos.x,aPos.z); }\n",
            "#version 330 core\n"
            "in vec3 vN; in vec2 vTerrUV; in vec2 vWorldXZ; out vec4 F;\n"
            "uniform vec3 uLight; uniform int uLayerCount; uniform float uTile;\n"
            "uniform sampler2D uLayerTex[8]; uniform float uUvScale[8];\n"
            "uniform int uHasTex[8]; uniform vec3 uFallback[8];\n"
            "uniform sampler2D uW0; uniform sampler2D uW1;\n"
            "void main(){ float w[8];\n"
            " vec4 a=texture(uW0,vTerrUV), b=texture(uW1,vTerrUV);\n"
            " w[0]=a.r;w[1]=a.g;w[2]=a.b;w[3]=a.a;w[4]=b.r;w[5]=b.g;w[6]=b.b;w[7]=b.a;\n"
            " vec3 col=vec3(0.0);\n"
            " for(int i=0;i<uLayerCount;i++){\n"
            "   vec3 s = (uHasTex[i]==1) ? texture(uLayerTex[i], vWorldXZ*uTile*uUvScale[i]).rgb : uFallback[i];\n"
            "   float al = (i==0)?1.0:clamp(w[i],0.0,1.0);\n"
            "   col = mix(col, s, al); }\n"
            " float d=max(dot(normalize(vN),normalize(uLight)),0.0)*0.75+0.25;\n"
            " F=vec4(col*d,1.0); }\n");
        uTTMVP=glGetUniformLocation(terrainTexProg,"uMVP");
        uTTGridInv=glGetUniformLocation(terrainTexProg,"uGridInv");
        uTTLight=glGetUniformLocation(terrainTexProg,"uLight");
        uTTLayerCount=glGetUniformLocation(terrainTexProg,"uLayerCount");
        uTTTile=glGetUniformLocation(terrainTexProg,"uTile");
        uTTUvScale=glGetUniformLocation(terrainTexProg,"uUvScale");
        uTTHasTex=glGetUniformLocation(terrainTexProg,"uHasTex");
        uTTFallback=glGetUniformLocation(terrainTexProg,"uFallback");
        glUseProgram(terrainTexProg);
        int units[8]={0,1,2,3,4,5,6,7};
        glUniform1iv(glGetUniformLocation(terrainTexProg,"uLayerTex"),8,units);
        glUniform1i(glGetUniformLocation(terrainTexProg,"uW0"),8);
        glUniform1i(glGetUniformLocation(terrainTexProg,"uW1"),9);
        glUseProgram(0);

        // colour-code pick pass: flat per-entity id, alpha-cut respected so foliage
        // is picked by its leaves and not by its quad. Shares the model VAO layout
        // (loc0 pos / loc1 normal / loc2 uv) and the terrain VAO's loc0.
        pickProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; layout(location=2) in vec2 aUV;\n"
            "uniform mat4 uMVP; out vec2 vUV;\n"
            "void main(){ gl_Position=uMVP*vec4(aPos,1.0); vUV=aUV; }\n",
            "#version 330 core\n"
            "uniform vec3 uCode; uniform sampler2D uTex;\n"
            "uniform int uAlphaTest; uniform int uHasTex;\n"
            "in vec2 vUV; out vec4 o;\n"
            "void main(){\n"
            "   if(uAlphaTest==1 && uHasTex==1 && texture(uTex,vUV).a < 0.5) discard;\n"
            "   o = vec4(uCode, 1.0);\n"
            "}\n");
        uPkMVP=glGetUniformLocation(pickProg,"uMVP");
        uPkCode=glGetUniformLocation(pickProg,"uCode");
        uPkAlphaTest=glGetUniformLocation(pickProg,"uAlphaTest");
        uPkHasTex=glGetUniformLocation(pickProg,"uHasTex");
        uPkTex=glGetUniformLocation(pickProg,"uTex");
        // entity dots in the pick pass: gl_VertexID IS the entity index, so the
        // code needs no extra vertex attribute. Hidden entities have size 0.
        pickPointProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; layout(location=2) in float aSize;\n"
            "uniform mat4 uMVP; flat out vec3 vCode;\n"
            "void main(){\n"
            "   gl_Position=uMVP*vec4(aPos,1.0);\n"
            "   gl_PointSize = aSize>0.0 ? aSize+3.0 : 0.0;\n"
            "   int id = gl_VertexID + 1;\n"
            "   vCode = vec3(float(id & 255), float((id>>8) & 255), float((id>>16) & 255))/255.0;\n"
            "}\n",
            "#version 330 core\n"
            "flat in vec3 vCode; out vec4 o;\n"
            "void main(){ o = vec4(vCode, 1.0); }\n");
        uPpMVP=glGetUniformLocation(pickPointProg,"uMVP");

        // road/decal overlays: textured, alpha-blended, terrain-projected
        overlayProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUV;\n"
            "uniform mat4 uMVP; out vec2 vUV;\n"
            "void main(){ gl_Position=uMVP*vec4(aPos,1.0); vUV=aUV; }\n",
            "#version 330 core\n"
            "in vec2 vUV; out vec4 F; uniform sampler2D uTex;\n"
            "void main(){ vec4 t=texture(uTex, vec2(vUV.x,1.0-vUV.y));\n"
            " if(t.a<0.04) discard; F=t; }\n");
        uOvMVP=glGetUniformLocation(overlayProg,"uMVP");
        glUseProgram(overlayProg); glUniform1i(glGetUniformLocation(overlayProg,"uTex"),0);
        glUseProgram(0);

        // flat-colour line shader (terrain brush cursor ring)
        lineProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; uniform mat4 uMVP;\n"
            "void main(){ gl_Position=uMVP*vec4(aPos,1.0); }\n",
            "#version 330 core\n"
            "out vec4 F; uniform vec3 uColor;\n"
            "void main(){ F=vec4(uColor,1.0); }\n");
        uLineMVP=glGetUniformLocation(lineProg,"uMVP");
        uLineColor=glGetUniformLocation(lineProg,"uColor");
        // Thick lines: a geometry shader expands each GL_LINES segment into a
        // screen-space quad of uThick pixels — GL 3.3 core clamps glLineWidth to 1.
        thickProg = glProgram(
            "#version 330 core\n"
            "layout(location=0) in vec3 aPos; uniform mat4 uMVP;\n"
            "void main(){ gl_Position=uMVP*vec4(aPos,1.0); }\n",
            "#version 330 core\n"
            "layout(lines) in; layout(triangle_strip, max_vertices=4) out;\n"
            "uniform vec2 uViewport; uniform float uThick;\n"
            "void main(){\n"
            " vec4 p0=gl_in[0].gl_Position, p1=gl_in[1].gl_Position;\n"
            " if(p0.w<=0.0||p1.w<=0.0) return;\n"          // skip segments behind camera
            " vec2 n0=p0.xy/p0.w, n1=p1.xy/p1.w;\n"
            " vec2 d=(n1-n0)*uViewport; if(dot(d,d)<1e-12) return; d=normalize(d);\n"
            " vec2 nm=vec2(-d.y,d.x); vec2 off=(nm/uViewport)*uThick;\n"
            " gl_Position=vec4((n0+off)*p0.w,p0.z,p0.w); EmitVertex();\n"
            " gl_Position=vec4((n0-off)*p0.w,p0.z,p0.w); EmitVertex();\n"
            " gl_Position=vec4((n1+off)*p1.w,p1.z,p1.w); EmitVertex();\n"
            " gl_Position=vec4((n1-off)*p1.w,p1.z,p1.w); EmitVertex();\n"
            " EndPrimitive(); }\n",
            "#version 330 core\n"
            "out vec4 F; uniform vec3 uColor;\n"
            "void main(){ F=vec4(uColor,1.0); }\n");
        uThMVP=glGetUniformLocation(thickProg,"uMVP");
        uThColor=glGetUniformLocation(thickProg,"uColor");
        uThViewport=glGetUniformLocation(thickProg,"uViewport");
        uThThick=glGetUniformLocation(thickProg,"uThick");
    }

    // Set the terrain brush cursor ring (world-space line-loop vertices, xyz*).
    // Pass an empty vector to hide it.
    void setBrushRing(std::vector<float> ringXYZ) { brushRing = std::move(ringXYZ); }

    // Resolve each entity's Prototype (ProtoDB at dataRoot/ProtoDB.bin) to a
    // model .srm under dataRoot, load unique models, and place an instance at the
    // entity's world position + yaw. Models are assembled with the exact engine
    // skin rule (SKIN_FULL). Missing files are skipped.
    void buildModels(const Scene& s, const std::string& dataRoot) {
        instances.clear(); modelsBuilt = true;   // reuse cached models/textures
        if (dataRoot.empty() && !vfs_any_mounted()) return;
        buildTexIndex(dataRoot);
        std::string protoPath = vfs_resolve("ProtoDB.bin", dataRoot);
        auto index = protodb_model_index(protoPath);
        if (index.empty()) return;
        for (int ei = 0; ei < (int)s.entities.size(); ei++) {
            const Entity& e = s.entities[ei];
            if (e.proto.empty()) continue;
            std::string g; for (char c : e.proto) g += (char)tolower((unsigned char)c);
            auto it = index.find(g); if (it == index.end()) continue;
            std::string mp = vfs_resolve(it->second, dataRoot);
            if (mp.empty()) continue;
            GLModel* gm = loadModel(mp);
            if (!gm || gm->parts.empty() || gm->vao == 0) continue;
            V3 wp{ e.pos[0], e.pos[2], e.pos[1] };
            instances.push_back({ gm, entityXform(wp, e.dir, e.scale), ei });
        }
    }
    // World transform of an entity's model. The model is RH at load (negate X and
    // Z), so +180 deg of yaw aligns the facing with the game; SEntityDesc.Scale is
    // a uniform scale applied in model space (doodads use it heavily).
    static M4 entityXform(const V3& wp, float yawDeg, float scale) {
        float yaw = (yawDeg + 180.0f) * 3.14159265f / 180.0f;
        M4 xf = mul(translate(wp), rotY(yaw));
        if (scale > 0.0f && scale != 1.0f) xf = mul(xf, scaleM(scale, scale, scale));
        return xf;
    }
    // Live-update one entity's model transform (fast: just its matrix, no rebuild)
    // so drag-move is smooth.
    void moveInstance(int entIdx, const V3& wp, float yawDeg, float scale = 1.0f) {
        M4 xf = entityXform(wp, yawDeg, scale);
        for (auto& inst : instances) if (inst.entIdx == entIdx) { inst.xf = xf; break; }
    }
    int modelInstanceCount() const { return (int)instances.size(); }
    bool modelsAreBuilt() const { return modelsBuilt; }

    void buildTerrain(const Scene& s) {
        terrainCount = 0;
        if (s.heights.empty() || s.grid_w < 2 || s.grid_h < 2) return;
        int W=s.grid_w, H=s.grid_h;
        const std::vector<float>& h = s.heights;
        terrainHasColor = (s.colors.size() == (size_t)W * H * 3);
        std::vector<float> verts; verts.reserve((size_t)W*H*9);
        auto at=[&](int i,int j){ i=i<0?0:(i>=W?W-1:i); j=j<0?0:(j>=H?H-1:j); return h[(size_t)j*W+i]; };
        for (int j=0;j<H;j++) for (int i=0;i<W;i++) {
            size_t gi=(size_t)j*W+i;
            float y=h[gi];
            V3 n=norm(V3{-(at(i+1,j)-at(i-1,j)), 2.0f, -(at(i,j+1)-at(i,j-1))});
            verts.push_back((float)i); verts.push_back(y); verts.push_back((float)j);
            verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
            if (terrainHasColor)
                verts.insert(verts.end(), {s.colors[gi*3]/255.0f,
                             s.colors[gi*3+1]/255.0f, s.colors[gi*3+2]/255.0f});
            else
                verts.insert(verts.end(), {0.5f, 0.5f, 0.5f});
        }
        std::vector<unsigned> idx; idx.reserve((size_t)(W-1)*(H-1)*6);
        for (int j=0;j<H-1;j++) for (int i=0;i<W-1;i++) {
            unsigned a=j*W+i, b=j*W+i+1, c=(j+1)*W+i, d=(j+1)*W+i+1;
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
        terrainCount = (int)idx.size();
        if (!terrainVAO) glGenVertexArrays(1,&terrainVAO);
        if (!terrainVBO) glGenBuffers(1,&terrainVBO);
        if (!terrainEBO) glGenBuffers(1,&terrainEBO);
        glBindVertexArray(terrainVAO);
        glBindBuffer(GL_ARRAY_BUFFER, terrainVBO);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, terrainEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(unsigned), idx.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(3*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(6*sizeof(float)));
        glBindVertexArray(0);
    }

    // Rebuild the entity point cloud. ``show[kind]`` toggles visibility per
    // category (0 doodad, 1 building, 2 effect); hidden ones get size 0 (so the
    // VBO index still equals the entity index for selection). Marker size scales
    // with category so gameplay buildings stand out over doodad clutter.
    void buildEntities(const Scene& s, const bool show[3] = nullptr) {
        entCount = (int)s.entities.size();
        if (entCount == 0) return;
        static const float pal[8][3] = {
            {0.8f,0.8f,0.8f},{0.86f,0.27f,0.27f},{0.27f,0.47f,0.86f},{0.31f,0.78f,0.35f},
            {0.86f,0.78f,0.27f},{0.78f,0.35f,0.82f},{0.31f,0.82f,0.82f},{0.90f,0.55f,0.24f}};
        static const float kindSize[3] = {4.0f, 10.0f, 6.0f};  // doodad / building / effect
        std::vector<float> v; v.reserve((size_t)entCount*7);
        for (const Entity& e : s.entities) {
            const float* c = pal[((e.player%8)+8)%8];
            int k = (e.kind >= 0 && e.kind < 3) ? e.kind : 2;
            float sz = (show && !show[k]) ? 0.0f : kindSize[k];
            v.push_back(e.pos[0]); v.push_back(e.pos[2]); v.push_back(e.pos[1]); // X, elev, Y
            v.push_back(c[0]); v.push_back(c[1]); v.push_back(c[2]);
            v.push_back(sz);
        }
        if (!entVAO) glGenVertexArrays(1,&entVAO);
        if (!entVBO) glGenBuffers(1,&entVBO);
        glBindVertexArray(entVAO);
        glBindBuffer(GL_ARRAY_BUFFER, entVBO);
        glBufferData(GL_ARRAY_BUFFER, v.size()*sizeof(float), v.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,7*sizeof(float),(void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,7*sizeof(float),(void*)(3*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,7*sizeof(float),(void*)(6*sizeof(float)));
        glBindVertexArray(0);
    }

    GLModel* loadModel(const std::string& path) {
        auto it = modelCache.find(path);
        if (it != modelCache.end()) return &it->second;
        GLModel gm{};
        SrmModel m;
        if (srm_parse(path, m, nullptr)) {
            std::vector<RenderMesh> rms;
            srm_build_render(m, SKIN_FULL, VAR_ALL, rms);
            std::vector<float> verts; std::vector<unsigned> idx; unsigned base = 0;
            for (auto& rm : rms) {                      // one Part per RenderMesh (its diffuse tex)
                Part part; part.off = (int)idx.size();
                // .srm is DirectX LH; render in OpenGL RH. Negating a SINGLE axis is a
                // reflection -> mirrors chiral detail (fuselage text read backwards).
                // Negate BOTH X and Z = a 180-deg Y rotation (det +1, NO mirror), so
                // text reads correctly (user-confirmed). Two data reflections preserve
                // winding orientation, so keep normal index order; exterior stays CCW
                // (Back-cull = solid). Model yaw keeps its +180 offset to match facing.
                for (auto& v : rm.verts) {
                    verts.push_back(-v.x); verts.push_back(v.y); verts.push_back(-v.z);
                    verts.push_back(-v.nx); verts.push_back(v.ny); verts.push_back(-v.nz);
                    verts.push_back(v.u); verts.push_back(v.v);
                    gm.bmin.x=std::min(gm.bmin.x,-v.x); gm.bmin.y=std::min(gm.bmin.y,v.y); gm.bmin.z=std::min(gm.bmin.z,-v.z);
                    gm.bmax.x=std::max(gm.bmax.x,-v.x); gm.bmax.y=std::max(gm.bmax.y,v.y); gm.bmax.z=std::max(gm.bmax.z,-v.z);
                }
                for (size_t t = 0; t + 2 < rm.indices.size(); t += 3) {
                    idx.push_back(base + rm.indices[t]);
                    idx.push_back(base + rm.indices[t+1]);
                    idx.push_back(base + rm.indices[t+2]);
                }
                base += (unsigned)rm.verts.size();
                part.count = (int)rm.indices.size();
                part.tex = loadTexture(rm.diffuseTex);
                part.alphaTest = rm.alphaTest;
                gm.parts.push_back(part);
            }
            if (!idx.empty()) {
                glGenVertexArrays(1,&gm.vao); glGenBuffers(1,&gm.vbo); glGenBuffers(1,&gm.ebo);
                glBindVertexArray(gm.vao);
                glBindBuffer(GL_ARRAY_BUFFER,gm.vbo);
                glBufferData(GL_ARRAY_BUFFER,verts.size()*sizeof(float),verts.data(),GL_STATIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,gm.ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,idx.size()*sizeof(unsigned),idx.data(),GL_STATIC_DRAW);
                glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);
                glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(3*sizeof(float)));
                glEnableVertexAttribArray(2); glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(6*sizeof(float)));
                glBindVertexArray(0);
            }
        }
        modelCache[path] = gm; return &modelCache[path];
    }

    // basename(lower,no ext) -> full .dds path, built once per data root
    void buildTexIndex(const std::string& dataRoot) {
        if (!texIndex.empty() && texRoot == dataRoot) return;
        texIndex.clear(); texRoot = dataRoot;
        std::error_code ec;
        if (!dataRoot.empty()) {                     // disk textures (extracted content)
            for (auto it = std::filesystem::recursive_directory_iterator(dataRoot, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                std::string ext = it->path().extension().string();
                for (char& c : ext) c = (char)tolower((unsigned char)c);
                if (ext != ".dds") continue;
                std::string key = it->path().stem().string();
                for (char& c : key) c = (char)tolower((unsigned char)c);
                texIndex.emplace(key, it->path().string());     // absolute disk path
            }
        }
        for (const std::string& logical : vfs_list_suffix(".dds")) {  // pak textures
            size_t sl = logical.find_last_of('/');
            std::string base = (sl == std::string::npos) ? logical : logical.substr(sl + 1);
            size_t d = base.find_last_of('.'); if (d != std::string::npos) base = base.substr(0, d);
            texIndex.emplace(base, logical);         // disk added first -> disk wins on dup
        }
    }
    GLuint loadTexture(const std::string& diffuse) {
        if (diffuse.empty()) return 0;
        std::string key = diffuse;
        size_t dot = key.find_last_of('.'); if (dot != std::string::npos) key = key.substr(0, dot);
        for (char& c : key) c = (char)tolower((unsigned char)c);
        auto ci = texCache.find(key); if (ci != texCache.end()) return ci->second;
        GLuint tex = 0;
        auto pi = texIndex.find(key);
        if (pi != texIndex.end()) {
            std::error_code ec;
            std::string real = std::filesystem::exists(pi->second, ec)
                               ? pi->second : vfs_resolve(pi->second, "");
            DdsImage img = real.empty() ? DdsImage{} : dds_load(real);
            if (img.ok && img.width > 0) {
                texDim[key] = { img.width, img.height };   // for road width-from-texture
                glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
                glGenerateMipmap(GL_TEXTURE_2D);   // trilinear -> no shimmer on distant/oblique surfaces
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                float maxAniso = 1.0f;             // clamp to hw max; no-op if unsupported
                glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
                if (maxAniso > 1.0f)
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                                    maxAniso < 8.0f ? maxAniso : 8.0f);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }
        texCache[key] = tex; return tex;
    }
    // keyword fallback colour for a layer whose .dds can't be resolved
    static void layerFallbackColor(const std::string& nm, float out[3]) {
        std::string s; for (char c : nm) s += (char)tolower((unsigned char)c);
        struct P { const char* k; float r,g,b; };
        static const P pal[] = {
            {"grass",0.27f,0.39f,0.17f},{"foliage",0.27f,0.39f,0.17f},{"meadow",0.27f,0.39f,0.17f},
            {"tillage",0.34f,0.25f,0.16f},{"soil",0.34f,0.25f,0.16f},{"mud",0.34f,0.25f,0.16f},
            {"dirt",0.34f,0.25f,0.16f},{"field",0.34f,0.25f,0.16f},{"ploughland",0.34f,0.25f,0.16f},
            {"gritty",0.52f,0.44f,0.30f},{"ground",0.52f,0.44f,0.30f},{"sand",0.52f,0.44f,0.30f},
            {"straw",0.52f,0.44f,0.30f},{"dry",0.52f,0.44f,0.30f},{"default",0.52f,0.44f,0.30f},
            {"cobble",0.44f,0.43f,0.42f},{"road",0.44f,0.43f,0.42f},{"pavement",0.44f,0.43f,0.42f},
            {"stone",0.44f,0.43f,0.42f},{"rock",0.44f,0.43f,0.42f},{"ruin",0.44f,0.43f,0.42f},
            {"gravel",0.44f,0.43f,0.42f},{"mine",0.44f,0.43f,0.42f},
            {"water",0.20f,0.29f,0.33f},{"river",0.20f,0.29f,0.33f},{"puddle",0.20f,0.29f,0.33f},
            {"snow",0.80f,0.82f,0.85f},{"winter",0.80f,0.82f,0.85f},{"ice",0.80f,0.82f,0.85f},
        };
        for (auto& p : pal) if (s.find(p.k) != std::string::npos) { out[0]=p.r;out[1]=p.g;out[2]=p.b; return; }
        out[0]=0.35f; out[1]=0.33f; out[2]=0.28f;
    }
    // strip a leading map prefix (M1_, M12_, M_01_, Tutor_1_) from a lowercased stem
    static std::string stripMapPrefix(const std::string& stem) {
        size_t i = 0;
        if (stem.compare(0,1,"m")==0) {
            size_t j=1; if (j<stem.size()&&stem[j]=='_') j++;
            size_t k=j; while (k<stem.size() && isdigit((unsigned char)stem[k])) k++;
            if (k>j && k<stem.size() && stem[k]=='_') i=k+1;
        } else if (stem.compare(0,6,"tutor_")==0) {
            size_t k=6; while (k<stem.size() && (isdigit((unsigned char)stem[k])||stem[k]=='_')) k++;
            i=k;
        }
        return stem.substr(i);
    }
    // Resolve a GTRD layer path (e.g. "Terrain/Layer/M_01/Cobblestone_02_c_n") to a
    // GL texture: try the basename, then map-prefix-stripped, then a longest-prefix
    // scan over the .dds stem index. Returns 0 if nothing resolves.
    // Resolve a material path to the texIndex KEY (lowercased stem) that best matches,
    // or "" if none. Skips normal/aux maps in the fuzzy fallback (they are blue and
    // rendered decals purple when picked as the diffuse).
    std::string resolveTexKey(const std::string& logical) {
        size_t sl = logical.find_last_of('/');
        std::string base = (sl==std::string::npos) ? logical : logical.substr(sl+1);
        std::string low; for (char c : base) low += (char)tolower((unsigned char)c);
        if (texIndex.count(low)) return low;
        std::string np = stripMapPrefix(low);
        if (np != low && texIndex.count(np)) return np;
        auto isAuxMap = [](const std::string& k){
            auto ends = [&](const std::string& s){
                return k.size()>=s.size() && k.compare(k.size()-s.size(),s.size(),s)==0; };
            return ends("_n") || ends("_nm") || ends("_bump") || ends("_spec") ||
                   k.find("normal")!=std::string::npos || k.find("bump")!=std::string::npos;
        };
        const std::string& q = np;
        // A road surface's material (e.g. sw_wide_05) must resolve to the tiling STRIP
        // texture, not a corner/junction decal piece that shares the prefix. Skip piece
        // textures unless the query itself is one (decals name their own piece).
        auto hasTok = [](const std::string& s, const char* t){ return s.find(t)!=std::string::npos; };
        bool qPiece = hasTok(q,"corner")||hasTok(q,"cross")||hasTok(q,"junc")||hasTok(q,"end");
        std::string bestKey; size_t bestLen = 0;
        for (const auto& kv : texIndex) {
            const std::string& k = kv.first;
            if (isAuxMap(k)) continue;
            if (!qPiece && (hasTok(k,"corner")||hasTok(k,"cross")||hasTok(k,"junc"))) continue;
            size_t m = (q.size()<k.size()) ? q.size() : k.size();
            if (m < 4) continue;
            // longest prefix wins; ties -> the key closest in length to q (the base strip)
            if (q.compare(0,m,k,0,m)==0 &&
                (m > bestLen || (m == bestLen && !bestKey.empty() && k.size() < bestKey.size())))
                { bestLen=m; bestKey=k; }
        }
        return bestKey;
    }
    GLuint resolveLayerTex(const std::string& logical) {
        std::string k = resolveTexKey(logical);
        return k.empty() ? 0 : loadTexture(k);
    }
    // Pixel dims of the resolved texture (loads it if needed). false if unresolved.
    bool resolveTexDims(const std::string& logical, int& w, int& h) {
        std::string k = resolveTexKey(logical);
        if (k.empty()) return false;
        loadTexture(k);                       // ensures texDim[k] is populated
        auto it = texDim.find(k); if (it == texDim.end()) return false;
        w = it->second.first; h = it->second.second; return true;
    }
    // Build the per-map splat textures + active-layer arrays for terrainTexProg.
    void buildSplatTextures(const Scene& s, const std::string& dataRoot) {
        splatReady = false; splatLayerCount = 0;
        if (s.terrainLayers.empty() || s.splatWeights.empty()) return;
        if (dataRoot.empty() && !vfs_any_mounted()) return;
        buildTexIndex(dataRoot);
        int W=s.grid_w, H=s.grid_h; if (W<2||H<2) return;
        // active layers in file order (base first); cap at MAXL=8
        std::vector<int> act;
        for (int i=0;i<(int)s.terrainLayers.size();i++)
            if (s.terrainLayers[i].active && i<(int)s.splatWeights.size()) act.push_back(i);
        if (act.empty()) return;
        if (act.size()>8) { fprintf(stderr,"terrain: %zu active layers, capping at 8\n", act.size()); act.resize(8); }
        splatLayerCount = (int)act.size();
        int resolved = 0;
        for (int k=0;k<splatLayerCount;k++){
            const auto& L = s.terrainLayers[act[k]];
            GLuint t = resolveLayerTex(L.path);
            splatLayerTex[k] = t; splatHasTex[k] = t?1:0; splatUv[k] = L.uvScale>0?L.uvScale:1.0f;
            if (t) resolved++;
            layerFallbackColor(L.path, &splatFallback[k*3]);
        }
        // pack per-layer weights (active order) into 2 RGBA8 weight textures
        size_t need=(size_t)W*H;
        std::vector<unsigned char> w0(need*4,0), w1(need*4,0);
        for (int k=0;k<splatLayerCount;k++){
            const std::vector<unsigned char>& g = s.splatWeights[act[k]];
            if (g.size()<need) continue;
            std::vector<unsigned char>& dst = (k<4)?w0:w1; int ch=k%4;
            for (size_t gi=0;gi<need;gi++) dst[gi*4+ch]=g[gi];
        }
        auto up=[&](GLuint& tex, const std::vector<unsigned char>& d){
            if(!tex) glGenTextures(1,&tex);
            glBindTexture(GL_TEXTURE_2D,tex);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,d.data());
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D,0);
        };
        up(splatW0,w0); up(splatW1,w1);
        splatActive = act;
        splatGridInv[0]=1.0f/W; splatGridInv[1]=1.0f/H;
        splatReady = (resolved>0);   // only use textured path if at least one layer resolved
    }
    // Re-upload just the two weight textures after painting — the layer .dds files
    // and the resolve work are unchanged, so a paint stroke costs one upload, not a
    // full rebuild.
    void refreshSplatWeights(const Scene& s) {
        if (!splatReady || splatActive.empty()) return;
        int W=s.grid_w, H=s.grid_h; if (W<2||H<2) return;
        size_t need=(size_t)W*H;
        std::vector<unsigned char> w0(need*4,0), w1(need*4,0);
        for (int k=0;k<(int)splatActive.size() && k<8;k++){
            const std::vector<unsigned char>& g = s.splatWeights[splatActive[k]];
            if (g.size()<need) continue;
            std::vector<unsigned char>& dst = (k<4)?w0:w1; int ch=k%4;
            for (size_t gi=0;gi<need;gi++) dst[gi*4+ch]=g[gi];
        }
        auto up=[&](GLuint tex, const std::vector<unsigned char>& d){
            if(!tex) return;
            glBindTexture(GL_TEXTURE_2D,tex);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,d.data());
            glBindTexture(GL_TEXTURE_2D,0);
        };
        up(splatW0,w0); up(splatW1,w1);
    }
    bool splatIsReady() const { return splatReady; }

    // Build GL batches for road/decal overlays, grouped by resolved texture so the
    // whole overlay set draws in a handful of calls. Meshes whose texture won't
    // resolve are skipped.
    void buildOverlays(const Scene& s, const std::string& dataRoot) {
        clearOverlays();
        if (s.roads.empty() && s.decals.empty() && s.roadSplines.empty()) return;
        if (dataRoot.empty() && !vfs_any_mounted()) return;
        buildTexIndex(dataRoot);
        // group vertices/indices by (GL texture, isDecal)
        std::map<std::pair<GLuint,int>, std::pair<std::vector<float>, std::vector<unsigned>>> groups;
        auto add=[&](const std::vector<Scene::OverlayMesh>& list, int isDecal){
            for (const auto& m : list) {
                GLuint t = resolveLayerTex(m.tex);
                if (!t) continue;
                auto& g = groups[{t,isDecal}];
                unsigned base = (unsigned)(g.first.size()/5);
                g.first.insert(g.first.end(), m.verts.begin(), m.verts.end());
                for (unsigned ix : m.idx) g.second.push_back(base + ix);
            }
        };
        add(s.roads, 0); add(s.decals, 1);

        // --- centreline roads: extrude with width from the road TEXTURE ----------
        // Engine (FUN_004d7a10) derives width from the road tex's short (across) dim,
        // tiling the long (along) dim down the spline (see [[cpcw-road-groa]]). Strip
        // textures are 1024x128 (narrow) / 1024x256 (wide); width = shortDim * WPT.
        const float WPT = 0.02f, BIAS = 0.25f;   // world units per texel; overlay lift
        auto Hgt = [&](float x, float z)->float {
            if (s.heights.empty() || s.grid_w<2 || s.grid_h<2) return 0.0f;
            if (x<0)x=0; if (x>s.grid_w-1)x=(float)(s.grid_w-1);
            if (z<0)z=0; if (z>s.grid_h-1)z=(float)(s.grid_h-1);
            int x0=(int)x,z0=(int)z,x1=x0+1<s.grid_w?x0+1:x0,z1=z0+1<s.grid_h?z0+1:z0;
            float tx=x-x0,tz=z-z0; auto gg=[&](int i,int j){return s.heights[(size_t)j*s.grid_w+i];};
            float aa=gg(x0,z0)*(1-tx)+gg(x1,z0)*tx, bb=gg(x0,z1)*(1-tx)+gg(x1,z1)*tx;
            return aa*(1-tz)+bb*tz;
        };
        auto nameHW = [](const std::string& mat)->float {   // fallback for square/unresolved
            std::string m; for(char c:mat) m+=(char)tolower((unsigned char)c);
            auto h=[&](const char*t){return m.find(t)!=std::string::npos;};
            if (h("runway")) return 9.0f; if (h("dwide")) return 4.5f;
            if (h("asfalt_wide")||h("asphalt_wide")) return 3.8f; if (h("wide")) return 3.0f;
            if (h("road")||h("high")) return 2.8f; if (h("cobblestone")) return 2.0f;
            if (h("sidewalk")) return 1.4f; if (h("narrow")) return 1.2f; return 1.8f;
        };
        for (const auto& rs : s.roadSplines) {
            GLuint t = resolveLayerTex(rs.tex);
            if (!t || rs.cx.size() < 2) continue;
            int tw=0, th=0; float hw; float tileLen = 12.0f;
            if (resolveTexDims(rs.tex, tw, th) && tw>0 && th>0) {
                int shortd = tw<th?tw:th, longd = tw<th?th:tw;
                if ((float)longd/(float)shortd >= 1.5f) {    // elongated strip -> tex width
                    hw = shortd * WPT * 0.5f;
                    tileLen = longd * WPT;
                } else hw = nameHW(rs.tex);                  // ~square: width not encoded
            } else hw = nameHW(rs.tex);
            auto& g = groups[{t,0}];
            unsigned base = (unsigned)(g.first.size()/5);
            const auto& px = rs.cx; const auto& pz = rs.cz;
            float vrun = 0.0f;
            for (size_t i=0;i<px.size();i++){
                size_t a=i>0?i-1:i, c=i+1<px.size()?i+1:i;
                float dx=px[c]-px[a], dz=pz[c]-pz[a];
                float len=std::sqrt(dx*dx+dz*dz); if(len<1e-4f)len=1e-4f;
                float nx=-dz/len, nz=dx/len;
                if(i>0){float sx=px[i]-px[i-1],sz=pz[i]-pz[i-1];vrun+=std::sqrt(sx*sx+sz*sz);}
                float lx=px[i]-nx*hw,lz=pz[i]-nz*hw,rx=px[i]+nx*hw,rz=pz[i]+nz*hw;
                float u=vrun/tileLen;                        // along road (tiles)
                g.first.insert(g.first.end(), { lx, Hgt(lx,lz)+BIAS, lz, u, 0.0f });
                g.first.insert(g.first.end(), { rx, Hgt(rx,rz)+BIAS, rz, u, 1.0f });
            }
            for (size_t i=0;i+1<px.size();i++){
                unsigned aa=base+(unsigned)(i*2), b2=aa+1, cc=aa+2, dd=aa+3;
                g.second.insert(g.second.end(), { aa,cc,b2, b2,cc,dd });
            }
        }
        for (auto& kv : groups) {
            OverlayBatch ob; ob.tex = kv.first.first; ob.isDecal = kv.first.second; ob.count = (int)kv.second.second.size();
            glGenVertexArrays(1,&ob.vao); glGenBuffers(1,&ob.vbo); glGenBuffers(1,&ob.ebo);
            glBindVertexArray(ob.vao);
            glBindBuffer(GL_ARRAY_BUFFER, ob.vbo);
            glBufferData(GL_ARRAY_BUFFER, kv.second.first.size()*sizeof(float), kv.second.first.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ob.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, kv.second.second.size()*sizeof(unsigned), kv.second.second.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,5*sizeof(float),(void*)0);
            glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,5*sizeof(float),(void*)(3*sizeof(float)));
            glBindVertexArray(0);
            overlayBatches.push_back(ob);
        }
    }
    void clearOverlays() {
        for (auto& b : overlayBatches) {
            if (b.vao) glDeleteVertexArrays(1,&b.vao);
            if (b.vbo) glDeleteBuffers(1,&b.vbo);
            if (b.ebo) glDeleteBuffers(1,&b.ebo);
        }
        overlayBatches.clear();
    }
    int overlayBatchCount() const { return (int)overlayBatches.size(); }

    void clearModels() {
        for (auto& kv : modelCache) {
            if (kv.second.vao) glDeleteVertexArrays(1,&kv.second.vao);
            if (kv.second.vbo) glDeleteBuffers(1,&kv.second.vbo);
            if (kv.second.ebo) glDeleteBuffers(1,&kv.second.ebo);
        }
        for (auto& kv : texCache) if (kv.second) glDeleteTextures(1, &kv.second);
        modelCache.clear(); instances.clear(); texCache.clear(); modelsBuilt = false;
    }

    // wireframe AABB of `inst` (transformed by its xf), drawn depth-off so the
    // highlight is always visible. Used for selected/hovered entities.
    void drawHiliteBox(const M4& mvp, const ModelInst& inst, float r, float g, float b) {
        V3 lo = inst.model->bmin, hi = inst.model->bmax;
        if (lo.x > hi.x) return;
        V3 c[8] = {{lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,lo.y,hi.z},{lo.x,lo.y,hi.z},
                   {lo.x,hi.y,lo.z},{hi.x,hi.y,lo.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z}};
        V3 w[8]; const float* m = inst.xf.m;
        for (int i=0;i<8;i++){
            w[i].x=m[0]*c[i].x+m[4]*c[i].y+m[8]*c[i].z+m[12];
            w[i].y=m[1]*c[i].x+m[5]*c[i].y+m[9]*c[i].z+m[13];
            w[i].z=m[2]*c[i].x+m[6]*c[i].y+m[10]*c[i].z+m[14];
        }
        static const int E[24]={0,1,1,2,2,3,3,0, 4,5,5,6,6,7,7,4, 0,4,1,5,2,6,3,7};
        std::vector<float> ln; ln.reserve(24*3);
        for (int i=0;i<24;i++){ ln.push_back(w[E[i]].x); ln.push_back(w[E[i]].y); ln.push_back(w[E[i]].z); }
        glDisable(GL_DEPTH_TEST);
        drawThickLines(mvp, ln, r, g, b, 2.5f);   // real thickness (glLineWidth is clamped)
        glEnable(GL_DEPTH_TEST);
    }

    void render(const Camera& cam, float aspect, bool wireframe, int selected,
                bool showModels = true, bool showDots = true, int hovered = -1) {
        M4 mvp = cam.viewProj(aspect);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
        if (showModels && !instances.empty()) {
            glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
            // Optional backface cull on models only (terrain/overlays stay two-sided).
            // Culls the single-sided text-decal backfaces that render text mirrored.
            if (cullMode) { glEnable(GL_CULL_FACE); glCullFace(cullMode==1?GL_BACK:GL_FRONT); }
            glUseProgram(modelProg);
            float light[3]={0.4f,0.8f,0.35f}; glUniform3fv(uMdlLight,1,light);
            float col[3]={0.72f,0.72f,0.75f}; glUniform3fv(uMdlColor,1,col);
            // flipModelX reflects each model in its LOCAL X. Combined with loadModel's
            // negate-Z that is a 180-deg Y rotation (two reflections = no mirror), so
            // it un-mirrors fuselage/decal text. Experiment toggle (key X) to confirm.
            for (auto& inst : instances) {
                M4 xf = flipModelX ? mul(inst.xf, scaleM(-1,1,1)) : inst.xf;
                M4 mvpM = mul(mvp, xf);
                glUniformMatrix4fv(uMdlMVP,1,GL_FALSE,mvpM.m);
                glUniformMatrix4fv(uMdlModel,1,GL_FALSE,xf.m);
                glBindVertexArray(inst.model->vao);
                for (auto& part : inst.model->parts) {
                    // Alpha-tested foliage/fence cards are single-sided quads -> draw
                    // them two-sided (cull off) so leaves show from both sides.
                    if (cullMode) { if (part.alphaTest) glDisable(GL_CULL_FACE);
                                    else { glEnable(GL_CULL_FACE); glCullFace(cullMode==1?GL_BACK:GL_FRONT); } }
                    glUniform1i(uMdlHasTex, part.tex ? 1 : 0);
                    glUniform1i(uMdlAlphaTest, part.alphaTest ? 1 : 0);
                    if (part.tex) glBindTexture(GL_TEXTURE_2D, part.tex);
                    glDrawElements(GL_TRIANGLES, part.count, GL_UNSIGNED_INT,
                                   (void*)(size_t)(part.off * sizeof(unsigned)));
                }
            }
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            if (cullMode) glDisable(GL_CULL_FACE);
            // hover (cyan) + selection highlight boxes. The primary selection is
            // yellow (it is what the Properties panel and the gizmo act on); the
            // rest of a multi-selection is a dimmer orange.
            if (hovered >= 0 || selected >= 0 || !selectionSet.empty())
                for (auto& inst : instances) {
                    if (inst.entIdx == selected)                 drawHiliteBox(mvp, inst, 1.0f, 0.85f, 0.15f);
                    else if (selectionSet.count(inst.entIdx))    drawHiliteBox(mvp, inst, 0.95f, 0.55f, 0.12f);
                    else if (inst.entIdx == hovered)             drawHiliteBox(mvp, inst, 0.30f, 0.90f, 1.0f);
                }
        }
        if (terrainCount) {
            glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
            float light[3]={0.4f,0.8f,0.35f};
            bool useTex = (terrainMode==0) && splatReady && terrainTexProg;
            if (useTex) {
                glUseProgram(terrainTexProg);
                glUniformMatrix4fv(uTTMVP,1,GL_FALSE,mvp.m);
                glUniform2f(uTTGridInv, splatGridInv[0], splatGridInv[1]);
                glUniform3fv(uTTLight,1,light);
                glUniform1i(uTTLayerCount, splatLayerCount);
                glUniform1f(uTTTile, terrainTile);
                glUniform1fv(uTTUvScale, splatLayerCount, splatUv);
                glUniform1iv(uTTHasTex, splatLayerCount, splatHasTex);
                glUniform3fv(uTTFallback, splatLayerCount, splatFallback);
                for (int k=0;k<splatLayerCount;k++){ glActiveTexture(GL_TEXTURE0+k); glBindTexture(GL_TEXTURE_2D, splatLayerTex[k]); }
                glActiveTexture(GL_TEXTURE0+8); glBindTexture(GL_TEXTURE_2D, splatW0);
                glActiveTexture(GL_TEXTURE0+9); glBindTexture(GL_TEXTURE_2D, splatW1);
                glActiveTexture(GL_TEXTURE0);
            } else {
                glUseProgram(terrainProg);
                glUniformMatrix4fv(uTerrMVP,1,GL_FALSE,mvp.m);
                glUniform3fv(uTerrLight,1,light);
                glUniform1i(uTerrUseColor, (terrainMode!=2 && terrainHasColor) ? 1 : 0);
            }
            glBindVertexArray(terrainVAO);
            glDrawElements(GL_TRIANGLES, terrainCount, GL_UNSIGNED_INT, 0);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
        // road/decal overlays: textured, alpha-blended, lifted above the terrain.
        // Depth WRITES off so the many coplanar overlays don't z-fight each other;
        // two passes (roads first, then decals) so markings paint on top of roads.
        if (!overlayBatches.empty() && !wireframe) {
            glUseProgram(overlayProg);
            glUniformMatrix4fv(uOvMVP,1,GL_FALSE,mvp.m);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-2.0f,-2.0f);
            glDepthMask(GL_FALSE);
            glActiveTexture(GL_TEXTURE0);
            for (int pass = 0; pass < 2; pass++)         // 0 = roads, 1 = decals
                for (auto& b : overlayBatches) {
                    if (b.isDecal != pass) continue;
                    if (b.isDecal ? !showDecals : !showRoads) continue;
                    glBindTexture(GL_TEXTURE_2D, b.tex);
                    glBindVertexArray(b.vao);
                    glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, 0);
                }
            glDepthMask(GL_TRUE);
            glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_BLEND);
        }
        if (showDots && entCount) {
            glUseProgram(entProg);
            glUniformMatrix4fv(uEntMVP,1,GL_FALSE,mvp.m);
            glUniform1i(uEntWhite,0); glUniform1f(uEntSize,0.0f);  // 0 => per-vertex size
            glBindVertexArray(entVAO);
            glDrawArrays(GL_POINTS, 0, entCount);
            if (!selectionSet.empty()) {          // whole selection, incl. model-less
                glUniform1i(uEntWhite,1); glUniform1f(uEntSize,10.0f);
                for (int si : selectionSet)
                    if (si >= 0 && si < entCount && si != selected)
                        glDrawArrays(GL_POINTS, si, 1);
            }
            if (selected >= 0 && selected < entCount) {
                glUniform1i(uEntWhite,1); glUniform1f(uEntSize,13.0f);
                glDrawArrays(GL_POINTS, selected, 1);
            }
        }
        // terrain brush cursor ring — drawn last, depth-test off so it's always
        // visible as a cursor showing the exact area the brush will modify.
        if (brushRing.size() >= 9) {
            // expand the closed loop of points into GL_LINES segment pairs
            size_t n = brushRing.size() / 3;
            std::vector<float> segs; segs.reserve(n * 6);
            for (size_t i = 0; i < n; i++) {
                size_t j = (i + 1) % n;
                segs.insert(segs.end(), { brushRing[i*3], brushRing[i*3+1], brushRing[i*3+2],
                                          brushRing[j*3], brushRing[j*3+1], brushRing[j*3+2] });
            }
            glDisable(GL_DEPTH_TEST);
            drawThickLines(mvp, segs, 1.0f, 0.85f, 0.2f, 2.0f);
            glEnable(GL_DEPTH_TEST);
        }
        glBindVertexArray(0);
        glUseProgram(0);
    }

    int terrainTris() const { return terrainCount; }
    int entityCount() const { return entCount; }
    V3 debugEye(const Camera& c) const { return c.eye(); }

    // ---- GPU colour-code picking ---------------------------------------------
    // Every pickable entity is redrawn into an offscreen buffer flat-shaded with
    // its own index packed into RGB; reading a pixel back names the entity under
    // the cursor exactly — occlusion, alpha-cut foliage and overlapping models all
    // resolve for free, which a projected-AABB test cannot do. Reading a whole
    // rectangle back and collecting the unique codes gives rubber-band select.
    // The terrain draws as code 0, so it occludes anything behind it.
    //
    // This runs on demand (click / marquee), not every frame: doubling the model
    // pass on a 3400-entity map is not worth it for the hover highlight, which
    // keeps using the cheap CPU AABB test in pickModel().
    bool pickPassReady() const { return pickProg != 0; }

    // World-space AABB centre + radius of every model instance. Used by the
    // --picktest harness to verify the colour-code buffer without a mouse.
    struct InstProbe { int entIdx; V3 center; float radius; };
    void instanceProbes(std::vector<InstProbe>& out) const {
        out.clear(); out.reserve(instances.size());
        for (const auto& inst : instances) {
            V3 lo = inst.model->bmin, hi = inst.model->bmax;
            if (lo.x > hi.x) continue;
            V3 c{ (lo.x+hi.x)*0.5f, (lo.y+hi.y)*0.5f, (lo.z+hi.z)*0.5f };
            const float* m = inst.xf.m;
            V3 w{ m[0]*c.x + m[4]*c.y + m[8]*c.z + m[12],
                  m[1]*c.x + m[5]*c.y + m[9]*c.z + m[13],
                  m[2]*c.x + m[6]*c.y + m[10]*c.z + m[14] };
            float ex = (hi.x-lo.x)*0.5f, ey = (hi.y-lo.y)*0.5f, ez = (hi.z-lo.z)*0.5f;
            float sc = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);   // uniform scale
            out.push_back({ inst.entIdx, w, std::sqrt(ex*ex+ey*ey+ez*ez) * (sc>0?sc:1.0f) });
        }
    }

    void renderPickBuffer(const Camera& cam, int w, int h, bool showModels, bool showDots) {
        if (w < 1 || h < 1 || !pickProg) return;
        ensurePickTarget(w, h);
        if (!pickFBO) return;
        GLint prevFBO = 0; glGetIntegerv(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &prevFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, pickFBO);
        glViewport(0, 0, w, h);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 1);                     // 0 = nothing / terrain
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        M4 mvp = cam.viewProj((float)w / (float)h);

        // terrain first, as code 0 — it must occlude entities behind hills
        if (terrainCount) {
            glUseProgram(pickProg);
            glUniformMatrix4fv(uPkMVP, 1, GL_FALSE, mvp.m);
            float zero[3] = {0, 0, 0};
            glUniform3fv(uPkCode, 1, zero);
            glUniform1i(uPkAlphaTest, 0); glUniform1i(uPkHasTex, 0);
            glBindVertexArray(terrainVAO);
            glDrawElements(GL_TRIANGLES, terrainCount, GL_UNSIGNED_INT, 0);
        }
        if (showModels) {
            glUseProgram(pickProg);
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(uPkTex, 0);
            for (auto& inst : instances) {
                if (inst.entIdx < 0) continue;
                M4 xf = flipModelX ? mul(inst.xf, scaleM(-1,1,1)) : inst.xf;
                M4 mvpM = mul(mvp, xf);
                glUniformMatrix4fv(uPkMVP, 1, GL_FALSE, mvpM.m);
                float code[3]; codeOf(inst.entIdx, code);
                glUniform3fv(uPkCode, 1, code);
                glBindVertexArray(inst.model->vao);
                for (auto& part : inst.model->parts) {
                    glUniform1i(uPkAlphaTest, part.alphaTest ? 1 : 0);
                    glUniform1i(uPkHasTex, part.tex ? 1 : 0);
                    if (part.tex) glBindTexture(GL_TEXTURE_2D, part.tex);
                    glDrawElements(GL_TRIANGLES, part.count, GL_UNSIGNED_INT,
                                   (void*)(size_t)(part.off * sizeof(unsigned)));
                }
            }
        }
        // entity dots: entities with no model are only pickable this way. gl_VertexID
        // is the entity index, so the code needs no per-point attribute.
        if (showDots && entCount && pickPointProg) {
            glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
            glUseProgram(pickPointProg);
            glUniformMatrix4fv(uPpMVP, 1, GL_FALSE, mvp.m);
            glBindVertexArray(entVAO);
            glDrawArrays(GL_POINTS, 0, entCount);
        }
        glBindVertexArray(0); glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
    }

    // Entity index under buffer pixel (px, py) — origin bottom-left, i.e. already
    // flipped from ImGui's top-left screen space. -1 = terrain / empty.
    int pickBufferAt(int px, int py) const {
        if (!pickFBO || px < 0 || py < 0 || px >= pickW || py >= pickH) return -1;
        GLint prevFBO = 0; glGetIntegerv(0x8CA6, &prevFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, pickFBO);
        unsigned char px4[4] = {0,0,0,0};
        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px4);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
        return decodeCode(px4);
    }

    // Raw RGB copy of the pick buffer (debug: dump it and look at it).
    bool readPickBufferRGB(std::vector<unsigned char>& rgb, int& w, int& h) const {
        if (!pickFBO) return false;
        w = pickW; h = pickH;
        rgb.assign((size_t)w * h * 3, 0);
        GLint prevFBO = 0; glGetIntegerv(0x8CA6, &prevFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, pickFBO);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
        return true;
    }

    // Unique entity indices covered by a buffer-space rectangle.
    void pickBufferRect(int x, int y, int w, int h, std::vector<int>& out) const {
        out.clear();
        if (!pickFBO || w < 1 || h < 1) return;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > pickW) w = pickW - x;
        if (y + h > pickH) h = pickH - y;
        if (w < 1 || h < 1) return;
        GLint prevFBO = 0; glGetIntegerv(0x8CA6, &prevFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, pickFBO);
        std::vector<unsigned char> buf((size_t)w * h * 4);
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
        std::set<int> uniq;
        for (size_t i = 0; i + 3 < buf.size(); i += 4) {
            int id = decodeCode(&buf[i]);
            if (id >= 0) uniq.insert(id);
        }
        out.assign(uniq.begin(), uniq.end());
    }

    // Pick the model instance under screen point mp: the one nearest the camera
    // whose projected AABB contains the point. Returns entIdx (-1 if none). Cheap
    // and approximate — used for the per-frame hover highlight. Click selection
    // goes through the exact colour-code buffer above.
    int pickModel(const ImVec2& mp, const ImVec2& cmin, const ImVec2& cmax,
                  const Camera& cam) const {
        float W = cmax.x-cmin.x, H = cmax.y-cmin.y;
        if (W <= 1 || H <= 1) return -1;
        M4 vp = cam.viewProj(W/H);
        V3 eye = cam.eye();
        int best = -1; float bestDepth = 1e30f;
        for (const auto& inst : instances) {
            V3 lo = inst.model->bmin, hi = inst.model->bmax;
            if (lo.x > hi.x) continue;
            const float* m = inst.xf.m;
            float minx=1e30f,miny=1e30f,maxx=-1e30f,maxy=-1e30f; bool any=false;
            V3 ctr{0,0,0};
            for (int c=0;c<8;c++){
                V3 p{ (c&1)?hi.x:lo.x, (c&2)?hi.y:lo.y, (c&4)?hi.z:lo.z };
                V3 w{ m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],
                      m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],
                      m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14] };
                ctr = ctr + w;
                float cx=vp.m[0]*w.x+vp.m[4]*w.y+vp.m[8]*w.z+vp.m[12];
                float cy=vp.m[1]*w.x+vp.m[5]*w.y+vp.m[9]*w.z+vp.m[13];
                float cw=vp.m[3]*w.x+vp.m[7]*w.y+vp.m[11]*w.z+vp.m[15];
                if (cw <= 0.001f) continue;
                float sx=cmin.x+(cx/cw*0.5f+0.5f)*W;
                float sy=cmin.y+(1.0f-(cy/cw*0.5f+0.5f))*H;
                minx=std::min(minx,sx); maxx=std::max(maxx,sx);
                miny=std::min(miny,sy); maxy=std::max(maxy,sy); any=true;
            }
            if (!any) continue;
            if (mp.x>=minx && mp.x<=maxx && mp.y>=miny && mp.y<=maxy) {
                ctr = ctr * 0.125f;
                float dx=ctr.x-eye.x, dy=ctr.y-eye.y, dz=ctr.z-eye.z;
                float depth = dx*dx+dy*dy+dz*dz;
                if (depth < bestDepth) { bestDepth = depth; best = inst.entIdx; }
            }
        }
        return best;
    }

private:
    // ---- colour-code pick target --------------------------------------------
    static void codeOf(int entIdx, float out[3]) {
        unsigned id = (unsigned)entIdx + 1;             // 0 is reserved for "nothing"
        out[0] = ( id        & 0xFF) / 255.0f;
        out[1] = ((id >> 8)  & 0xFF) / 255.0f;
        out[2] = ((id >> 16) & 0xFF) / 255.0f;
    }
    static int decodeCode(const unsigned char* rgba) {
        unsigned id = (unsigned)rgba[0] | ((unsigned)rgba[1] << 8) | ((unsigned)rgba[2] << 16);
        return id ? (int)id - 1 : -1;
    }
    void ensurePickTarget(int w, int h) {
        if (pickFBO && w == pickW && h == pickH) return;
        if (pickTex)   { glDeleteTextures(1, &pickTex); pickTex = 0; }
        if (pickDepth) { glDeleteRenderbuffers(1, &pickDepth); pickDepth = 0; }
        if (pickFBO)   { glDeleteFramebuffers(1, &pickFBO); pickFBO = 0; }
        pickW = w; pickH = h;
        glGenFramebuffers(1, &pickFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, pickFBO);
        glGenTextures(1, &pickTex);
        glBindTexture(GL_TEXTURE_2D, pickTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // NEAREST: the code must never be filtered/interpolated into a wrong id
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pickTex, 0);
        glGenRenderbuffers(1, &pickDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, pickDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, pickDepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "pick FBO incomplete (%dx%d) — click select falls back to AABB\n", w, h);
            glDeleteFramebuffers(1, &pickFBO); pickFBO = 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    GLuint pickProg=0, pickPointProg=0, pickFBO=0, pickTex=0, pickDepth=0;
    GLint uPkMVP=-1, uPkCode=-1, uPkAlphaTest=-1, uPkHasTex=-1, uPkTex=-1, uPpMVP=-1;
    int pickW=0, pickH=0;

    std::map<std::string, GLModel> modelCache;
    std::vector<ModelInst> instances;
    GLuint modelProg=0;
    GLint uMdlMVP=-1, uMdlModel=-1, uMdlLight=-1, uMdlColor=-1, uMdlHasTex=-1, uMdlAlphaTest=-1;
    bool modelsBuilt=false;
    std::map<std::string, GLuint> texCache;    // basename -> GL texture
    std::map<std::string, std::pair<int,int>> texDim;  // basename -> (w,h) pixels
    std::map<std::string, std::string> texIndex; // basename -> .dds path
    std::string texRoot;

    GLuint terrainProg=0, entProg=0;
    GLuint terrainVAO=0, terrainVBO=0, terrainEBO=0, entVAO=0, entVBO=0;
    GLint uTerrMVP=-1, uTerrLight=-1, uTerrUseColor=-1, uEntMVP=-1, uEntSize=-1, uEntWhite=-1;
    int terrainCount=0, entCount=0;
    bool terrainHasColor=false;

    // splat-textured terrain
    GLuint terrainTexProg=0;
    GLint uTTMVP=-1,uTTGridInv=-1,uTTLight=-1,uTTLayerCount=-1,uTTTile=-1,uTTUvScale=-1,uTTHasTex=-1,uTTFallback=-1;
    int   splatLayerCount=0;
    GLuint splatLayerTex[8]={0}; float splatUv[8]={0}; int splatHasTex[8]={0};
    float splatFallback[24]={0}; GLuint splatW0=0, splatW1=0; float splatGridInv[2]={0,0};
    std::vector<int> splatActive;   // scene layer index of each packed weight channel
    bool  splatReady=false;

    // road/decal overlays
    GLuint overlayProg=0; GLint uOvMVP=-1;
    struct OverlayBatch { GLuint tex=0, vao=0, vbo=0, ebo=0; int count=0, isDecal=0; };
    std::vector<OverlayBatch> overlayBatches;

    // terrain brush cursor ring
    GLuint lineProg=0, ringVAO=0, ringVBO=0; GLint uLineMVP=-1, uLineColor=-1;
    std::vector<float> brushRing;
    // thick screen-space lines (geometry-shader quad expansion)
    GLuint thickProg=0, thickVAO=0, thickVBO=0;
    GLint  uThMVP=-1, uThColor=-1, uThViewport=-1, uThThick=-1;

    // Draw a set of GL_LINES segments (segs = xyz pairs) as quads `thick` px wide.
    void drawThickLines(const M4& mvp, const std::vector<float>& segs,
                        float r, float g, float b, float thick) {
        if (segs.size() < 6) return;
        GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
        if (!thickVAO) { glGenVertexArrays(1,&thickVAO); glGenBuffers(1,&thickVBO); }
        glBindVertexArray(thickVAO); glBindBuffer(GL_ARRAY_BUFFER, thickVBO);
        glBufferData(GL_ARRAY_BUFFER, segs.size()*sizeof(float), segs.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);
        glUseProgram(thickProg);
        glUniformMatrix4fv(uThMVP,1,GL_FALSE,mvp.m);
        float col[3]={r,g,b}; glUniform3fv(uThColor,1,col);
        glUniform2f(uThViewport, (float)vp[2], (float)vp[3]);
        glUniform1f(uThThick, thick);
        glDrawArrays(GL_LINES, 0, (int)(segs.size()/3));
    }
public:
    int   terrainMode=0;          // 0 Textured, 1 Palette, 2 Height ramp
    float terrainTile=0.125f;     // texture repeats every 1/tile world units (uvScale=1)
    bool  showRoads=true, showDecals=true;
    // Model backface cull: 0 Off, 1 Back, 2 Front. Exterior faces are CCW (verified
    // live: Front-cull shows the interior), so Back-cull is correct/standard and
    // hides the hull interior. Toggle: View>Model cull, or key C.
    int   cullMode=1;
    bool  flipModelX=false;       // reflect models in local X (mirror-text experiment, key X)
    std::set<int> selectionSet;   // every selected entity index (primary passed to render)
};
