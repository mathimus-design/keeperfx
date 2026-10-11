/******************************************************************************/
// KeeperFX VR: the meshes the Keeper's hand is built from.
/******************************************************************************/
#ifndef KFX_VR_HANDMESH_H
#define KFX_VR_HANDMESH_H

#include <vector>

namespace VR {

/** A run of triangles in the hand's vertex buffer (6 floats per vertex:
 *  position, then smooth normal). */
struct MeshRange {
    int first = 0;
    int count = 0;
};

/** The hand's parts. The unit shapes are posed and scaled per joint:
 *  - sphere: radius 1 at the origin (joints, knuckles, pads, fingertips)
 *  - tube: from z = 0 (radius 1) to z = -1 (radius k_tube_taper), no caps (finger bones)
 *  - cone: base radius 1 at z = 0, tip at z = -1, capped at the base (claws)
 *  - palm: the palm itself, in the palm's own frame and in metres (fingers
 *    towards -Z, back of the hand +X, thumb side +Y, wrist at +Z). */
struct HandMesh {
    MeshRange sphere, tube, cone, palm;
};

constexpr float k_tube_taper = 0.82f;

/** Fills @p verts with every part and @p mesh with where each one is. */
void BuildHandMesh(std::vector<float>& verts, HandMesh& mesh);

} // namespace VR

#endif // KFX_VR_HANDMESH_H
