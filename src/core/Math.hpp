#pragma once

#include <cmath>
#include <cstring>
#include <iostream>

namespace core {
namespace math {

constexpr float PI = 3.14159265359f;

inline float toRadians(float degrees) {
    return degrees * (PI / 180.0f);
}

struct Vec2 {
    float x, y;
};

struct Vec3 {
    float x, y, z;

    Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vec3 operator*(float scalar) const { return {x * scalar, y * scalar, z * scalar}; }
    
    Vec3& operator+=(const Vec3& other) { x += other.x; y += other.y; z += other.z; return *this; }
    
    static float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    
    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }
    
    static Vec3 normalize(const Vec3& v) {
        float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (len > 0.0f) return {v.x / len, v.y / len, v.z / len};
        return {0.0f, 0.0f, 0.0f};
    }
};

struct Vec4 {
    float x, y, z, w;
};

struct Mat4 {
    float data[4][4];

    static Mat4 identity() {
        Mat4 m;
        std::memset(m.data, 0, sizeof(m.data));
        m.data[0][0] = 1.0f;
        m.data[1][1] = 1.0f;
        m.data[2][2] = 1.0f;
        m.data[3][3] = 1.0f;
        return m;
    }

    static Mat4 translate(const Vec3& v) {
        Mat4 m = identity();
        m.data[3][0] = v.x;
        m.data[3][1] = v.y;
        m.data[3][2] = v.z;
        return m;
    }

    static Mat4 rotate(float angle, const Vec3& axis) {
        Mat4 m = identity();
        float c = std::cos(angle);
        float s = std::sin(angle);
        float t = 1.0f - c;
        
        Vec3 a = Vec3::normalize(axis);
        float x = a.x, y = a.y, z = a.z;

        m.data[0][0] = t * x * x + c;
        m.data[0][1] = t * x * y + z * s; // GLM/OpenGL layout usually column-major?
        // Vulkan/GLSL mat4 is column-major.
        // My implementation of operator* (row-major logic or column-major logic?)
        // In operator*: res.data[c][r] += data[k][r] * other.data[c][k]; -> looks like standard matrix mult if data[col][row].
        // Let's stick to standard formula. If funny rotations happen, transpose.
        // Formula for column-major (OpenGL compatible):
        m.data[0][1] = t * x * y + z * s;
        m.data[0][2] = t * x * z - y * s;
        
        m.data[1][0] = t * x * y - z * s;
        m.data[1][1] = t * y * y + c;
        m.data[1][2] = t * y * z + x * s;
        
        m.data[2][0] = t * x * z + y * s;
        m.data[2][1] = t * y * z - x * s;
        m.data[2][2] = t * z * z + c;

        return m;
    }

    static Mat4 perspective(float fov, float aspect, float zNear, float zFar) {
        Mat4 m;
        std::memset(m.data, 0, sizeof(m.data));
        float tanHalfFov = std::tan(fov / 2.0f);

        m.data[0][0] = 1.0f / (aspect * tanHalfFov);
        m.data[1][1] = -1.0f / tanHalfFov; // Vulkan flips Y
        
        m.data[2][2] = zFar / (zNear - zFar);
        m.data[2][3] = -1.0f;
        m.data[3][2] = -(zFar * zNear) / (zFar - zNear);
        
        return m;
    }

    static Mat4 ortho(float left, float right, float bottom, float top, float zNear, float zFar) {
        Mat4 m = identity();
        m.data[0][0] = 2.0f / (right - left);
        m.data[1][1] = 2.0f / (bottom - top);
        m.data[2][2] = -1.0f / (zFar - zNear); // Vulkan Z [0,1], Input Z is negative in View Space
        m.data[3][0] = -(right + left) / (right - left);
        m.data[3][1] = -(bottom + top) / (bottom - top);
        m.data[3][2] = -zNear / (zFar - zNear);
        return m;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = Vec3::normalize(center - eye);
        Vec3 s = Vec3::normalize(Vec3::cross(f, up));
        Vec3 u = Vec3::cross(s, f);

        Mat4 m = identity();
        m.data[0][0] = s.x; m.data[1][0] = s.y; m.data[2][0] = s.z;
        m.data[0][1] = u.x; m.data[1][1] = u.y; m.data[2][1] = u.z;
        m.data[0][2] = -f.x; m.data[1][2] = -f.y; m.data[2][2] = -f.z;
        m.data[3][0] = -Vec3::dot(s, eye);
        m.data[3][1] = -Vec3::dot(u, eye);
        m.data[3][2] = Vec3::dot(f, eye);
        return m;
    }
    
    Mat4 operator*(const Mat4& other) const {
        Mat4 res;
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                res.data[c][r] = 0.0f;
                for (int k = 0; k < 4; k++) {
                    res.data[c][r] += data[k][r] * other.data[c][k];
                }
            }
        }
        return res;
    }

    // General 4×4 matrix inverse (Gauss-Jordan elimination).
    // Returns identity if matrix is singular (det ≈ 0).
    // data layout: data[col][row] (column-major, matches GLSL mat4).
    static Mat4 inverse(const Mat4& src) {
        // Work on a flat copy for clarity: a[row][col]
        float a[4][4];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                a[r][c] = src.data[c][r]; // transpose to row-major for elimination

        float inv[4][4];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                inv[r][c] = (r == c) ? 1.0f : 0.0f;

        for (int col = 0; col < 4; ++col) {
            // Find pivot
            int pivot = col;
            float maxVal = std::abs(a[col][col]);
            for (int row = col + 1; row < 4; ++row) {
                if (std::abs(a[row][col]) > maxVal) {
                    maxVal = std::abs(a[row][col]);
                    pivot = row;
                }
            }
            // Swap rows
            if (pivot != col) {
                for (int k = 0; k < 4; ++k) {
                    std::swap(a[col][k],   a[pivot][k]);
                    std::swap(inv[col][k], inv[pivot][k]);
                }
            }
            float diag = a[col][col];
            if (std::abs(diag) < 1e-8f) return identity(); // singular
            float invDiag = 1.0f / diag;
            for (int k = 0; k < 4; ++k) {
                a[col][k]   *= invDiag;
                inv[col][k] *= invDiag;
            }
            for (int row = 0; row < 4; ++row) {
                if (row == col) continue;
                float factor = a[row][col];
                for (int k = 0; k < 4; ++k) {
                    a[row][k]   -= factor * a[col][k];
                    inv[row][k] -= factor * inv[col][k];
                }
            }
        }

        // Convert back to column-major
        Mat4 result;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                result.data[c][r] = inv[r][c];
        return result;
    }
};

} // namespace math
} // namespace core
