/******************************************************************************/
// Dungeon Keeper - VR mode
/******************************************************************************/
/** @file VRMath.h
 *     Minimal vector / quaternion / matrix helpers for the VR pass.
 *     Matrices are column-major (m[col * 4 + row]), as OpenGL expects.
 */
/******************************************************************************/
#pragma once

#include <cmath>

namespace VR {

struct Vec3 { float x, y, z; };

inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

/** Rotates @p v about the vertical (Y) axis by @p angle radians. */
inline Vec3 RotateY(Vec3 v, float angle)
{
    const float c = std::cos(angle), s = std::sin(angle);
    return { c * v.x + s * v.z, v.y, -s * v.x + c * v.z };
}

/** Heading of a horizontal vector, matching RotateY(): RotateY((0,0,1), a) has heading a. */
inline float Heading(Vec3 v) { return std::atan2(v.x, v.z); }

struct Quat { float x, y, z, w; };

inline Vec3 Rotate(Quat q, Vec3 v)
{
    // v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const Vec3 u = { q.x, q.y, q.z };
    const Vec3 t = { u.y * v.z - u.z * v.y + q.w * v.x,
                     u.z * v.x - u.x * v.z + q.w * v.y,
                     u.x * v.y - u.y * v.x + q.w * v.z };
    return { v.x + 2.0f * (u.y * t.z - u.z * t.y),
             v.y + 2.0f * (u.z * t.x - u.x * t.z),
             v.z + 2.0f * (u.x * t.y - u.y * t.x) };
}

struct Mat4 {
    float m[16];

    static Mat4 Identity()
    {
        Mat4 r = {};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    static Mat4 Translation(Vec3 t)
    {
        Mat4 r = Identity();
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    static Mat4 Scale(Vec3 s)
    {
        Mat4 r = Identity();
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }
    static Mat4 RotationY(float angle)
    {
        // Same rotation as VR::RotateY().
        Mat4 r = Identity();
        const float c = std::cos(angle), s = std::sin(angle);
        r.m[0] = c;  r.m[8]  = s;
        r.m[2] = -s; r.m[10] = c;
        return r;
    }
    /** Rotation about X; positive turns -Z towards +Y. */
    static Mat4 RotationX(float angle)
    {
        Mat4 r = Identity();
        const float c = std::cos(angle), s = std::sin(angle);
        r.m[5] = c;  r.m[9]  = -s;
        r.m[6] = s;  r.m[10] = c;
        return r;
    }
    /** Rigid transform from a rotation and a position. */
    static Mat4 FromPose(Quat q, Vec3 p)
    {
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 r = Identity();
        r.m[0] = 1.0f - 2.0f * (yy + zz); r.m[1] = 2.0f * (xy + wz);        r.m[2]  = 2.0f * (xz - wy);
        r.m[4] = 2.0f * (xy - wz);        r.m[5] = 1.0f - 2.0f * (xx + zz); r.m[6]  = 2.0f * (yz + wx);
        r.m[8] = 2.0f * (xz + wy);        r.m[9] = 2.0f * (yz - wx);        r.m[10] = 1.0f - 2.0f * (xx + yy);
        r.m[12] = p.x; r.m[13] = p.y; r.m[14] = p.z;
        return r;
    }
    /** Inverse of a rigid transform (rotation + translation only). */
    Mat4 InverseRigid() const
    {
        Mat4 r = Identity();
        for (int c = 0; c < 3; c++)
            for (int rw = 0; rw < 3; rw++)
                r.m[c * 4 + rw] = m[rw * 4 + c];
        for (int rw = 0; rw < 3; rw++)
            r.m[12 + rw] = -(r.m[rw] * m[12] + r.m[4 + rw] * m[13] + r.m[8 + rw] * m[14]);
        return r;
    }
    /** OpenGL projection from the four half-angles of an OpenXR field of view. */
    static Mat4 Projection(float angle_left, float angle_right, float angle_up, float angle_down,
                           float near_z, float far_z)
    {
        const float tan_l = std::tan(angle_left), tan_r = std::tan(angle_right);
        const float tan_u = std::tan(angle_up),   tan_d = std::tan(angle_down);
        const float w = tan_r - tan_l, h = tan_u - tan_d;
        Mat4 r = {};
        r.m[0]  = 2.0f / w;
        r.m[5]  = 2.0f / h;
        r.m[8]  = (tan_r + tan_l) / w;
        r.m[9]  = (tan_u + tan_d) / h;
        r.m[10] = -(far_z + near_z) / (far_z - near_z);
        r.m[11] = -1.0f;
        r.m[14] = -2.0f * far_z * near_z / (far_z - near_z);
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            r.m[c * 4 + rw] = a.m[rw] * b.m[c * 4] + a.m[4 + rw] * b.m[c * 4 + 1]
                            + a.m[8 + rw] * b.m[c * 4 + 2] + a.m[12 + rw] * b.m[c * 4 + 3];
    return r;
}

} // namespace VR
