// 3D viewport: renders the map terrain (from the heightmap grid) and entity
// markers with an orbit camera, in OpenGL 3.3 core. Drawn into the editor's
// dockspace central-node region (see main.cpp).
#pragma once
#include "glcore.h"
#include "scene.h"
#include "protodb.h"
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
    }

    // Resolve each entity's Prototype (ProtoDB at dataRoot/ProtoDB.bin) to a
    // model .srm under dataRoot, load unique models, and place an instance at the
    // entity's world position + yaw. Models are assembled with the exact engine
    // skin rule (SKIN_FULL). Missing files are skipped.
    void buildModels(const Scene& s, const std::string& dataRoot) {
        clearModels(); modelsBuilt = true;
        if (dataRoot.empty()) return;
        buildTexIndex(dataRoot);
        auto index = protodb_model_index(dataRoot + "/ProtoDB.bin");
        if (index.empty()) return;
        for (const Entity& e : s.entities) {
            if (e.proto.empty()) continue;
            std::string g; for (char c : e.proto) g += (char)tolower((unsigned char)c);
            auto it = index.find(g); if (it == index.end()) continue;
            GLModel* gm = loadModel(dataRoot + "/" + it->second);
            if (!gm || gm->parts.empty() || gm->vao == 0) continue;
            V3 wp{ e.pos[0], e.pos[2], e.pos[1] };
            float yaw = e.dir * 3.14159265f / 180.0f;
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
        for (auto it = std::filesystem::recursive_directory_iterator(dataRoot, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = (char)tolower((unsigned char)c);
            if (ext != ".dds") continue;
            std::string key = it->path().stem().string();
            for (char& c : key) c = (char)tolower((unsigned char)c);
            texIndex.emplace(key, it->path().string());
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
            DdsImage img = dds_load(pi->second);
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
            glUseProgram(terrainProg);
            glUniformMatrix4fv(uTerrMVP,1,GL_FALSE,mvp.m);
            float light[3]={0.4f,0.8f,0.35f}; glUniform3fv(uTerrLight,1,light);
            glUniform1i(uTerrUseColor, terrainHasColor ? 1 : 0);
            glBindVertexArray(terrainVAO);
            glDrawElements(GL_TRIANGLES, terrainCount, GL_UNSIGNED_INT, 0);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
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
};
