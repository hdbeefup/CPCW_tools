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
    M4 viewProj(float aspect) const {
        return mul(perspective(0.9f, aspect, 1.0f, 8000.0f),
                   lookAt(eye(), target, {0,1,0}));
    }
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

class Viewport3D {
    struct Part { int off=0, count=0; GLuint tex=0; };   // index range + diffuse tex
    struct GLModel { GLuint vao=0, vbo=0, ebo=0; std::vector<Part> parts; };
    struct ModelInst { GLModel* model; M4 xf; };
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
            "uniform vec3 uLight, uColor; uniform sampler2D uTex; uniform int uHasTex;\n"
            "void main(){ float d=max(dot(normalize(vN),normalize(uLight)),0.0)*0.7+0.35;\n"
            " vec3 base = uHasTex==1 ? texture(uTex, vec2(vUV.x, 1.0-vUV.y)).rgb : uColor;\n"
            " F=vec4(base*d,1.0); }\n");
        uMdlMVP=glGetUniformLocation(modelProg,"uMVP");
        uMdlModel=glGetUniformLocation(modelProg,"uModel");
        uMdlLight=glGetUniformLocation(modelProg,"uLight");
        uMdlColor=glGetUniformLocation(modelProg,"uColor");
        uMdlHasTex=glGetUniformLocation(modelProg,"uHasTex");
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
        for (const Entity& e : s.entities) {
            if (e.proto.empty()) continue;
            std::string g; for (char c : e.proto) g += (char)tolower((unsigned char)c);
            auto it = index.find(g); if (it == index.end()) continue;
            std::string mp = vfs_resolve(it->second, dataRoot);
            if (mp.empty()) continue;
            GLModel* gm = loadModel(mp);
            if (!gm || gm->parts.empty() || gm->vao == 0) continue;
            V3 wp{ e.pos[0], e.pos[2], e.pos[1] };
            // The X mirror (LH->RH handedness) reverses the yaw sense, so negate
            // the entity yaw to match the in-game facing.
            float yaw = -e.dir * 3.14159265f / 180.0f;
            M4 xf = mul(mul(translate(wp), rotY(yaw)), scaleM(-1.0f, 1.0f, 1.0f));
            instances.push_back({ gm, xf });
        }
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
                for (auto& v : rm.verts) {
                    verts.push_back(v.x); verts.push_back(v.y); verts.push_back(v.z);
                    verts.push_back(v.nx); verts.push_back(v.ny); verts.push_back(v.nz);
                    verts.push_back(v.u); verts.push_back(v.v);
                }
                for (auto i : rm.indices) idx.push_back(base + i);
                base += (unsigned)rm.verts.size();
                part.count = (int)rm.indices.size();
                part.tex = loadTexture(rm.diffuseTex);
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
                glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
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
    GLuint resolveLayerTex(const std::string& logical) {
        size_t sl = logical.find_last_of('/');
        std::string base = (sl==std::string::npos) ? logical : logical.substr(sl+1);
        std::string low; for (char c : base) low += (char)tolower((unsigned char)c);
        GLuint t = loadTexture(low); if (t) return t;
        std::string np = stripMapPrefix(low);
        if (np != low) { t = loadTexture(np); if (t) return t; }
        // longest-prefix / suffix-tolerant scan (Cobblestone_02_c_n -> cobblestone_02)
        const std::string& q = np;
        std::string bestKey; size_t bestLen = 0;
        for (const auto& kv : texIndex) {
            const std::string& k = kv.first;
            size_t m = (q.size()<k.size()) ? q.size() : k.size();
            if (m < 4) continue;
            if (q.compare(0,m,k,0,m)==0 && m > bestLen) { bestLen=m; bestKey=k; }
        }
        if (!bestKey.empty()) return loadTexture(bestKey);
        return 0;
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
        splatGridInv[0]=1.0f/W; splatGridInv[1]=1.0f/H;
        splatReady = (resolved>0);   // only use textured path if at least one layer resolved
    }
    bool splatIsReady() const { return splatReady; }

    // Build GL batches for road/decal overlays, grouped by resolved texture so the
    // whole overlay set draws in a handful of calls. Meshes whose texture won't
    // resolve are skipped.
    void buildOverlays(const Scene& s, const std::string& dataRoot) {
        clearOverlays();
        if ((s.roads.empty() && s.decals.empty())) return;
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

    void render(const Camera& cam, float aspect, bool wireframe, int selected,
                bool showModels = true, bool showDots = true) {
        M4 mvp = cam.viewProj(aspect);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
        if (showModels && !instances.empty()) {
            glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
            glUseProgram(modelProg);
            float light[3]={0.4f,0.8f,0.35f}; glUniform3fv(uMdlLight,1,light);
            float col[3]={0.72f,0.72f,0.75f}; glUniform3fv(uMdlColor,1,col);
            for (auto& inst : instances) {
                M4 mvpM = mul(mvp, inst.xf);
                glUniformMatrix4fv(uMdlMVP,1,GL_FALSE,mvpM.m);
                glUniformMatrix4fv(uMdlModel,1,GL_FALSE,inst.xf.m);
                glBindVertexArray(inst.model->vao);
                for (auto& part : inst.model->parts) {
                    glUniform1i(uMdlHasTex, part.tex ? 1 : 0);
                    if (part.tex) glBindTexture(GL_TEXTURE_2D, part.tex);
                    glDrawElements(GL_TRIANGLES, part.count, GL_UNSIGNED_INT,
                                   (void*)(size_t)(part.off * sizeof(unsigned)));
                }
            }
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
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
        // road/decal overlays: textured, alpha-blended, lifted above the terrain
        if (!overlayBatches.empty() && !wireframe) {
            glUseProgram(overlayProg);
            glUniformMatrix4fv(uOvMVP,1,GL_FALSE,mvp.m);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-2.0f,-2.0f);
            glActiveTexture(GL_TEXTURE0);
            for (auto& b : overlayBatches) {
                if (b.isDecal ? !showDecals : !showRoads) continue;
                glBindTexture(GL_TEXTURE_2D, b.tex);
                glBindVertexArray(b.vao);
                glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, 0);
            }
            glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_BLEND);
        }
        if (showDots && entCount) {
            glUseProgram(entProg);
            glUniformMatrix4fv(uEntMVP,1,GL_FALSE,mvp.m);
            glUniform1i(uEntWhite,0); glUniform1f(uEntSize,0.0f);  // 0 => per-vertex size
            glBindVertexArray(entVAO);
            glDrawArrays(GL_POINTS, 0, entCount);
            if (selected >= 0 && selected < entCount) {
                glUniform1i(uEntWhite,1); glUniform1f(uEntSize,13.0f);
                glDrawArrays(GL_POINTS, selected, 1);
            }
        }
        // terrain brush cursor ring — drawn last, depth-test off so it's always
        // visible as a cursor showing the exact area the brush will modify.
        if (brushRing.size() >= 9) {
            if (!ringVAO) { glGenVertexArrays(1,&ringVAO); glGenBuffers(1,&ringVBO); }
            glBindVertexArray(ringVAO);
            glBindBuffer(GL_ARRAY_BUFFER, ringVBO);
            glBufferData(GL_ARRAY_BUFFER, brushRing.size()*sizeof(float), brushRing.data(), GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);
            glDisable(GL_DEPTH_TEST);
            glUseProgram(lineProg);
            glUniformMatrix4fv(uLineMVP,1,GL_FALSE,mvp.m);
            float col[3]={1.0f,0.85f,0.2f}; glUniform3fv(uLineColor,1,col);
            glDrawArrays(GL_LINE_LOOP, 0, (int)(brushRing.size()/3));
            glEnable(GL_DEPTH_TEST);
        }
        glBindVertexArray(0);
        glUseProgram(0);
    }

    int terrainTris() const { return terrainCount; }
    int entityCount() const { return entCount; }
    V3 debugEye(const Camera& c) const { return c.eye(); }

private:
    std::map<std::string, GLModel> modelCache;
    std::vector<ModelInst> instances;
    GLuint modelProg=0;
    GLint uMdlMVP=-1, uMdlModel=-1, uMdlLight=-1, uMdlColor=-1, uMdlHasTex=-1;
    bool modelsBuilt=false;
    std::map<std::string, GLuint> texCache;    // basename -> GL texture
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
    bool  splatReady=false;

    // road/decal overlays
    GLuint overlayProg=0; GLint uOvMVP=-1;
    struct OverlayBatch { GLuint tex=0, vao=0, vbo=0, ebo=0; int count=0, isDecal=0; };
    std::vector<OverlayBatch> overlayBatches;

    // terrain brush cursor ring
    GLuint lineProg=0, ringVAO=0, ringVBO=0; GLint uLineMVP=-1, uLineColor=-1;
    std::vector<float> brushRing;
public:
    int   terrainMode=0;          // 0 Textured, 1 Palette, 2 Height ramp
    float terrainTile=0.125f;     // texture repeats every 1/tile world units (uvScale=1)
    bool  showRoads=true, showDecals=true;
};
