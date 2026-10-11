/******************************************************************************/
// KeeperFX VR: the meshes the Keeper's hand is built from: smooth unit
// shapes for the fingers, knuckles and claws, and a lofted, hand-shaped palm.
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/vr/VRHandMesh.h"

#include <cmath>
#include "post_inc.h"

namespace VR {

namespace {

constexpr float k_pi = 3.14159265f;

struct V3 { float x, y, z; };

V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 normalize(V3 a)
{
    const float l = std::sqrt(dot(a, a));
    return (l > 1e-9f) ? V3{ a.x / l, a.y / l, a.z / l } : V3{ 0, 0, 1 };
}

void push(std::vector<float>& v, V3 p, V3 n)
{
    v.insert(v.end(), { p.x, p.y, p.z, n.x, n.y, n.z });
}

/** Two triangles over a grid cell, from four corners with their normals. */
void push_quad(std::vector<float>& v, V3 p00, V3 n00, V3 p10, V3 n10, V3 p11, V3 n11, V3 p01, V3 n01)
{
    push(v, p00, n00); push(v, p10, n10); push(v, p11, n11);
    push(v, p11, n11); push(v, p01, n01); push(v, p00, n00);
}

MeshRange range_begin(const std::vector<float>& v) { MeshRange r; r.first = (int)(v.size() / 6); return r; }
void range_end(const std::vector<float>& v, MeshRange& r) { r.count = (int)(v.size() / 6) - r.first; }

void build_sphere(std::vector<float>& v, int rings, int sides)
{
    auto at = [&](int i, int j) {
        const float th = k_pi * (float)i / (float)rings, ph = 2.0f * k_pi * (float)j / (float)sides;
        return V3{ std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph) };
    };
    for (int i = 0; i < rings; i++)
        for (int j = 0; j < sides; j++)
        {
            const V3 a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            push_quad(v, a, a, b, b, c, c, d, d);
        }
}

void build_tube(std::vector<float>& v, int sides)
{
    // r(z) = 1 + (1 - taper) z on z in [0, -1]: the normal leans back by the slope.
    const float slope = 1.0f - k_tube_taper;
    for (int j = 0; j < sides; j++)
    {
        const float a0 = 2.0f * k_pi * (float)j / (float)sides, a1 = 2.0f * k_pi * (float)(j + 1) / (float)sides;
        const V3 n0 = normalize({ std::cos(a0), std::sin(a0), -slope }), n1 = normalize({ std::cos(a1), std::sin(a1), -slope });
        const V3 p00 = { std::cos(a0), std::sin(a0), 0.0f }, p10 = { std::cos(a1), std::sin(a1), 0.0f };
        const V3 p01 = { std::cos(a0) * k_tube_taper, std::sin(a0) * k_tube_taper, -1.0f };
        const V3 p11 = { std::cos(a1) * k_tube_taper, std::sin(a1) * k_tube_taper, -1.0f };
        push_quad(v, p00, n0, p10, n1, p11, n1, p01, n0);
    }
}

void build_cone(std::vector<float>& v, int sides)
{
    const V3 tip = { 0.0f, 0.0f, -1.0f };
    for (int j = 0; j < sides; j++)
    {
        const float a0 = 2.0f * k_pi * (float)j / (float)sides, a1 = 2.0f * k_pi * (float)(j + 1) / (float)sides;
        const V3 p0 = { std::cos(a0), std::sin(a0), 0.0f }, p1 = { std::cos(a1), std::sin(a1), 0.0f };
        const V3 n0 = normalize({ std::cos(a0), std::sin(a0), -1.0f }), n1 = normalize({ std::cos(a1), std::sin(a1), -1.0f });
        const float am = 0.5f * (a0 + a1);
        push(v, p0, n0); push(v, p1, n1); push(v, tip, normalize({ std::cos(am), std::sin(am), -1.0f }));
        // Base cap, facing back towards the fingertip.
        push(v, p1, { 0, 0, 1 }); push(v, p0, { 0, 0, 1 }); push(v, { 0, 0, 0 }, { 0, 0, 1 });
    }
}

/** The palm: rounded-rectangle sections (superellipses) from the wrist to the
 *  knuckles, narrow at the wrist and widest just before the knuckles, flat on
 *  the back with a fuller palm side. */
void build_palm(std::vector<float>& v, int sides)
{
    struct Section { float z, width, thick, power, belly, shift; };
    const Section sec[] = {
        { 0.046f, 0.050f, 0.024f, 3.0f, 0.000f, 0.000f },  // wrist
        { 0.025f, 0.064f, 0.026f, 3.4f, 0.004f, 0.000f },
        { -0.005f, 0.075f, 0.024f, 3.8f, 0.005f, 0.002f },
        { -0.030f, 0.080f, 0.020f, 4.0f, 0.003f, 0.000f },
        { -0.044f, 0.074f, 0.016f, 4.0f, 0.000f, 0.000f },  // knuckles
    };
    const int n_sec = (int)(sizeof(sec) / sizeof(sec[0]));
    std::vector<V3> pos((size_t)(n_sec * sides));
    for (int k = 0; k < n_sec; k++)
        for (int i = 0; i < sides; i++)
        {
            const float t = 2.0f * k_pi * (float)i / (float)sides;
            const float c = std::cos(t), s = std::sin(t), e = 2.0f / sec[k].power;
            const float y = std::copysign(std::pow(std::fabs(c), e), c) * sec[k].width * 0.5f + sec[k].shift;
            float x = std::copysign(std::pow(std::fabs(s), e), s) * sec[k].thick * 0.5f;
            if (s < 0.0f)
                x -= sec[k].belly * std::fabs(s);  // the palm side bulges a little
            pos[(size_t)(k * sides + i)] = { x, y, sec[k].z };
        }
    // Smooth normals from the neighbours around and along, pointing outwards.
    auto P = [&](int k, int i) { return pos[(size_t)(k * sides + ((i + sides) % sides))]; };
    std::vector<V3> nrm(pos.size());
    for (int k = 0; k < n_sec; k++)
        for (int i = 0; i < sides; i++)
        {
            const V3 around = P(k, i + 1) - P(k, i - 1);
            const V3 along = P(k < n_sec - 1 ? k + 1 : k, i) - P(k > 0 ? k - 1 : k, i);
            V3 n = normalize(cross(around, along));
            const V3 p = P(k, i);
            if (dot(n, { p.x, p.y - sec[k].shift, 0.0f }) < 0.0f)
                n = { -n.x, -n.y, -n.z };
            nrm[(size_t)(k * sides + i)] = n;
        }
    auto N = [&](int k, int i) { return nrm[(size_t)(k * sides + ((i + sides) % sides))]; };
    for (int k = 0; k < n_sec - 1; k++)
        for (int i = 0; i < sides; i++)
            push_quad(v, P(k, i), N(k, i), P(k, i + 1), N(k, i + 1), P(k + 1, i + 1), N(k + 1, i + 1), P(k + 1, i), N(k + 1, i));
    // End caps (mostly hidden by the cuff and the knuckles).
    for (int end_k : { 0, n_sec - 1 })
    {
        const V3 c = { 0.0f, sec[end_k].shift, sec[end_k].z };
        const V3 n = { 0.0f, 0.0f, end_k == 0 ? 1.0f : -1.0f };
        for (int i = 0; i < sides; i++)
        {
            push(v, P(end_k, i), n); push(v, P(end_k, i + 1), n); push(v, c, n);
        }
    }
}

} // namespace

void BuildHandMesh(std::vector<float>& verts, HandMesh& mesh)
{
    verts.clear();
    mesh.sphere = range_begin(verts); build_sphere(verts, 10, 16); range_end(verts, mesh.sphere);
    mesh.tube = range_begin(verts);   build_tube(verts, 16);       range_end(verts, mesh.tube);
    mesh.cone = range_begin(verts);   build_cone(verts, 12);       range_end(verts, mesh.cone);
    mesh.palm = range_begin(verts);   build_palm(verts, 28);       range_end(verts, mesh.palm);
}

} // namespace VR
