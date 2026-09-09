// Minimal math types shared across Ghost-Ripper components.
// Column-major matrices (OpenGL convention), right-handed, clip z in [-1, 1].
#pragma once

#include <cmath>
#include <cstdint>

namespace ghost {

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }

    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const {
        const float len = length();
        return len > 1e-12f ? *this / len : Vec3{};
    }
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

struct Mat4 {
    // Column-major: m[col * 4 + row], matches OpenGL uniform layout.
    float m[16];

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    static Mat4 multiply(const Mat4& a, const Mat4& b) {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k)
                    s += a.m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = s;
            }
        return r;
    }

    // Right-handed perspective, OpenGL NDC (z in [-1, 1]). fov_y in radians.
    static Mat4 perspective(float fov_y, float aspect, float z_near, float z_far) {
        Mat4 r{};
        const float f = 1.0f / std::tan(fov_y * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (z_far + z_near) / (z_near - z_far);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * z_far * z_near) / (z_near - z_far);
        return r;
    }

    static Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
        const Vec3 f = (center - eye).normalized();
        const Vec3 s = f.cross(up).normalized();
        const Vec3 u = s.cross(f);
        Mat4 r = identity();
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -s.dot(eye);
        r.m[13] = -u.dot(eye);
        r.m[14] = f.dot(eye);
        return r;
    }

    static Mat4 translate(const Vec3& t) {
        Mat4 r = identity();
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(float s) {
        Mat4 r = identity();
        r.m[0] = r.m[5] = r.m[10] = s;
        return r;
    }

    // Rotation around an arbitrary (normalized) axis, angle in radians.
    static Mat4 rotate(const Vec3& axis, float angle) {
        const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
        const float x = axis.x, y = axis.y, z = axis.z;
        Mat4 r = identity();
        r.m[0]  = t * x * x + c;     r.m[4]  = t * x * y - s * z; r.m[8]  = t * x * z + s * y;
        r.m[1]  = t * x * y + s * z; r.m[5]  = t * y * y + c;     r.m[9]  = t * y * z - s * x;
        r.m[2]  = t * x * z - s * y; r.m[6]  = t * y * z + s * x; r.m[10] = t * z * z + c;
        return r;
    }

    Vec3 transform_point(const Vec3& p) const {
        const float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        const float iw = (w != 0.0f) ? 1.0f / w : 1.0f;
        return {(m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12]) * iw,
                (m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13]) * iw,
                (m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]) * iw};
    }

    // Upper-left 3x3 applied to a direction vector (no translation).
    Vec3 transform_dir(const Vec3& v) const {
        return {m[0] * v.x + m[4] * v.y + m[8]  * v.z,
                m[1] * v.x + m[5] * v.y + m[9]  * v.z,
                m[2] * v.x + m[6] * v.y + m[10] * v.z};
    }
};

} // namespace ghost
