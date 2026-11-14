# Smooth-motion correspondence validation

Smooth motion is an optional rendering enhancement. Simulation, network inputs,
save states and the native frame cadence are unchanged. Only newer geometry is
repositioned; topology, textures, materials and lighting remain the newer frame's.

The matcher has two kinds of evidence. Unchanged mesh/pose cohorts associate
roots when viewport slots are recycled. Within a paired root, stationary poses,
bounded unique node/mesh pairs and separated reciprocal motion associate groups.
Uncertain groups keep their newer local pose. They may follow a common root only
when that root has decisive anchors, shared-mesh correspondence or bounded
local motion. Cross-mesh exact poses and followers alone cannot establish
continuity across a scene replacement.

## Source-audit regressions

Fable 5.2's review of `1054cee` identified local-animation coverage losses. The
checked-in tests now verify these cases against analytic midpoint positions:

- Two meshes on different nodes, sharing one pose and moving together, retain
  their local interpolation. Their old/new pose cohorts must form a complete
  bijection; an unrelated merge or split is still ambiguous.
- Close parts with unchanged unique node/mesh pairs retain bounded motion even
  when spatial nearest-neighbour separation is insufficient.
- A part reaching another mesh's old pose cannot reserve that other part before
  its bounded identity is considered. Same-mesh stationary reservations retain
  priority to protect against recycled paths between stationary instances.
- If a plausible unique identity was consumed by that stationary reservation,
  its conflicted leftover follows only the supported root; it cannot invent
  local motion from a different nearest predecessor.
- Entirely replaced content under a surviving root slot remains native when no
  correspondence supports that root. A matched HUD cannot support a different
  world root.

The subsequent audit of `a2e30ef` identified a moving-parent attachment loss,
weak root-continuity evidence and an order-dependent path conflict check. Tests
now cover mesh-swapped small children under matched local parents (including
simultaneous root motion), coherent sibling subtrees, conflicting subtrees and
independent root-level siblings. A directly matched parent takes priority over
its independently animating descendants. Followers do not supply attachment
evidence to each other. Replaced scenes with coincident identity-local models
remain native, and reserved path owners are measured before conflict checks.

Root-anchor votes use a sparse map rather than allocating the Cartesian product
of roots. Geometry bounds are computed lazily for unresolved matches and key or
path conflicts. Tests also cover 128 roots, root stretch/shear ground truth,
handedness flips, nested local transforms and descendants of billboards. Mixed
rotation and nonuniform stretch/shear preserve endpoints; their approximate
intermediate path is bounded against two analytic trajectories (measured maximum
error 0.183 units in those fixtures, not a universal bound).

## Private replay evidence and limits

On a locally preserved widescreen multiplayer replay, independent diagnostics
compare newer vertices with frozen prior interpolation math using independently
selected predecessors. There are no game assets, dumps or replay inputs here.

- Bird interval: 7,652 separated correspondence observations. The earlier
  installed build had 819 errors above 0.2 units; the repaired candidate has zero
  reference vertex error. Third-frame trajectories corroborate 643 of 647 errors
  in an earlier narrow-view sample; this is evidence, not persistent entity IDs.
- Car/body neighborhood: 31,839 unchanged-local observations. The earlier build
  had 3,179 common-camera discontinuities on viewport recycling; the repaired
  candidate has zero reference vertex error.
- The stable-part correction reduces root-only followers in the full bird
  interval from 65,707 to 52,863 of 193,766 model observations, recovering local
  animation. These are full-scene counts, not just selected birds.

Two frames cannot uniquely identify every crossing, birth, disappearance or
recycled node. Identical meshes and poses remain especially ambiguous; bounded
keys and pose continuity are heuristics. Uncertain animation may stay at native
cadence. There is no universal flicker-free claim, and the reported entire-rim
disappearance/color change is not conclusively reproduced as that exact symptom.

Newer-frame endpoints can retain small floating-point round-trip differences on
animated models (up to 1.53e-5 in the inspected pairs); full-scene bitwise endpoint
equality is not claimed. The complete Windows test suite also has two existing
local UDP host-handoff timeout checks at `tests/test_net.cpp:286,291`. Rendering
tests and independent geometry checks do not establish live FPS or network health.
