// Smooth motion: the 3D between two frames' scenes, after vf3-recomp's
// (src/video/r3d_scene.h, interpolate; MIT, Copyright (c) 2026 mr-finster),
// for pictures between the arcade's 57.5 frames a second.
//
// Models sharing a node and full local transform form one group. Viewport
// indices and node paths can be recycled, so roots first match by at least
// three distinct unchanged mesh/pose anchors, with reciprocal unique majorities
// and compatible projections. Without decisive anchors, an uncontested root
// key may continue. Group matches reserve unique unchanged same-mesh poses,
// then accept bounded, unique unchanged node/mesh pairs. Coincident parts must keep
// a complete bijection between their shared old and new poses; splits and
// merges remain ambiguous. Remaining groups may match exact poses across mesh
// changes, then reciprocal nearest local origins. A plausible identity already
// reserved by another group cannot be replaced by a different nearest instance.
// Spatial matches are
// across meshes (animation can replace a mesh), within both groups' radii, at
// least four times closer than alternatives in both directions, and with no
// plausible contradictory node-path match. This is bounded geometric evidence,
// not persistent game identity; two-frame ambiguity cannot always be resolved.
// Uncertain groups retain their newer local animation under a root supported
// by decisive anchors or an actual matched group; followers alone cannot
// establish continuity across a scene replacement. Otherwise they stay native.
// Models without valid root metadata retain the path/mesh fallback, reserving each predecessor.
// Models below the same root
// share that root's move, with their local animation blended separately,
// keeping scenery and animated parts together as the camera turns. The root's
// relative eye-space move
// follows its screw motion, with stretch/shear blended continuously, so its
// choice of model origin does not shift the scenery during camera turns.
// A root turning further than kMaxTurn keeps its newer rotation while its
// position still blends. Other models
// use their own matrix: position and signed stretch/shear straight, rotation
// along the shortest turn. Their polygons move by the blended matrix times
// the inverse of the newer one, which put them where they are.
// Legacy models without root metadata and not found move as their nearest
// matched relative in the culling-node tree does; with none (below the
// viewport's node), and models that moved
// too far in the frame (more than kMaxMove of their distance from the eye),
// they're drawn as in the newer scene. One that turned
// further than kMaxTurn degrees (a wheel spinning: which way can't be told)
// is moved with the rest but turned as in the newer scene, so it stays on
// its car. If fewer than half the newer scene's models were blended, it's a
// cut (the camera somewhere else): the newer scene as it is.
//
// Only the 3D's geometry moves: lighting, fog, colours, textures and level
// of detail are the newer frame's, as are the 2D layers.
#pragma once

#include "runtime/real3d_scene.hpp"

#include <cstddef>

namespace m3 {

struct InterpStats {
    std::size_t models = 0;    // the newer scene's
    std::size_t blended = 0;   // moved, including supported root-followers
    std::size_t unmatched = 0; // no reliable local match (may still follow a root)
    std::size_t followed = 0;  // uncertain local: common root, or legacy nearest relative
    std::size_t moved = 0;     // moved too far: drawn as in the newer
    std::size_t spun = 0;      // turned too far: blended in place, the newer turn
    std::size_t nearest = 0;   // found as the nearest of the same model (not by its nodes)
    std::size_t ambiguous = 0; // of those, another nearly as near (within twice the distance)
    std::size_t compared = 0;  // models compared in those searches
    bool cut = false;          // the newer scene drawn as it is
};

// `out` becomes the scene at `t` from `a` (0) to `b` (1). Returns false (and
// `out` is `b`) at a cut, or when either is empty or block culled.
bool interpolate(const Real3dScene& a, const Real3dScene& b, float t, Real3dScene& out, InterpStats* stats = nullptr);

constexpr float kMaxMove = 0.75f;  // of the distance from the eye, in a frame
constexpr float kMaxTurn = 90.0f;  // degrees in a frame

} // namespace m3
