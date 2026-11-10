// the prop meshes' layout (Props::loadMesh, the photoscans): rocks, then logs, then stumps. shared by the placement
// (propplacement.glsl) and props.frag
#ifndef PROP_MESHES_GLSL
#define PROP_MESHES_GLSL

#define PROP_ROCKS 10
#define PROP_FIRST_LOG 10u
#define PROP_FIRST_STUMP 13u
#define PROP_LOGS 3
#define PROP_STUMPS 2
// the meshes textured with the tiling ambientCG stone (projected from three sides) instead of their own scan texture:
// the rock set's rocks, which share one 1k texture between seven of them
// (both moss rock sets: rock_moss_set_02's three, rock_moss_set_01's three)
bool propStone(uint mesh) { return mesh >= 3u && mesh <= 8u; }

#endif
