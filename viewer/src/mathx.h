// Minimal column-vector 3D math for the CPCW viewer.
//
// Matrices use the SAME convention as the proven Blender importer
// (mathutils, column vectors: v' = M * v).  Node local = T * R * S with
// R = Rx * Ry * Rz; world = parent * local.  This is what assembles CPCW
// models correctly (see blendertools/SRM_Blender/import_srm.py).
#pragma once
#include <cmath>

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 normalize(const Vec3& v) {
    float l = std::sqrt(dot(v, v));
    if (l < 1e-8f) return Vec3(0, 0, 1);
    return Vec3(v.x / l, v.y / l, v.z / l);
}

// Row-major storage, indexed m[row][col]. Column-vector transform.
struct Mat4 {
    float m[4][4];
    Mat4() { identity(); }
    void identity() {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) m[r][c] = (r == c) ? 1.0f : 0.0f;
    }
    static Mat4 translation(float x, float y, float z) {
        Mat4 t; t.m[0][3] = x; t.m[1][3] = y; t.m[2][3] = z; return t;
    }
    static Mat4 scale(float x, float y, float z) {
        Mat4 t; t.m[0][0] = x; t.m[1][1] = y; t.m[2][2] = z; return t;
    }
    static Mat4 rotX(float a) {
        Mat4 t; float c = std::cos(a), s = std::sin(a);
        t.m[1][1] = c; t.m[1][2] = -s; t.m[2][1] = s; t.m[2][2] = c; return t;
    }
    static Mat4 rotY(float a) {
        Mat4 t; float c = std::cos(a), s = std::sin(a);
        t.m[0][0] = c; t.m[0][2] = s; t.m[2][0] = -s; t.m[2][2] = c; return t;
    }
    static Mat4 rotZ(float a) {
        Mat4 t; float c = std::cos(a), s = std::sin(a);
        t.m[0][0] = c; t.m[0][1] = -s; t.m[1][0] = s; t.m[1][1] = c; return t;
    }
    Mat4 operator*(const Mat4& b) const {
        Mat4 o;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[r][k] * b.m[k][c];
                o.m[r][c] = s;
            }
        return o;
    }
    // v' = M * (v, 1)
    Vec3 point(const Vec3& v) const {
        return Vec3(
            m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3],
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3],
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3]);
    }
    // v' = M(3x3) * v  (no translation) — for normals / directions
    Vec3 dir(const Vec3& v) const {
        return Vec3(
            m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z);
    }
};
