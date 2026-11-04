// deferred shading: the opaque materials (terrain, trees, impostors, grass) only write what the surface is, the
// lighting (shadows, sun / moon, sky, aerial perspective, fog) happens once per pixel in lighting.comp
//   target 0 (RGBA8 sRGB): albedo (linear in the shader, the format encodes), a = ambient occlusion
//   target 1 (RGBA16F):    world space normal
//   target 2 (RGBA8):      r = material id / 255, g = roughness, b = direct light scale (POM self shadow, bark
//                          cavities, leaf AO), a = unused
//   target 3 (RG16F):      screen motion (uv) of the surface since last frame on top of the camera's, only the
//                          wind animated foliage / trees write it (writeVelocity), TAA reprojects with it
// must match gbuffer.hpp
#ifndef GBUFFER_GLSL
#define GBUFFER_GLSL

#define MAT_TERRAIN	 1 // lambert
#define MAT_BARK	 2 // GGX + lambert
#define MAT_LEAF	 3 // wrapped diffuse + light through the leaves
#define MAT_GRASS	 4 // wrapped diffuse + light through the blades + sheen, shadows looked up with an up normal
#define MAT_IMPOSTOR 5 // wrapped diffuse (far trees)

#ifdef GBUFFER_WRITE
layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gMaterial;
layout(location = 3) out vec2 gVelocity;

void writeGBuffer(vec3 albedo, float ambientOcclusion, vec3 normal, int material, float roughness, float directScale) {
	gAlbedo = vec4(albedo, ambientOcclusion);
	gNormal = vec4(normal, 0.0);
	gMaterial = vec4(float(material) / 255.0, roughness, directScale, 0.0);
	gVelocity = vec2(0.0); // static: the camera reprojection (depth) covers it
}

// after writeGBuffer: where this point was last frame (the wind) seen from this frame's camera, in uv
void writeVelocity(mat4 viewProj, vec3 position, vec3 previousPosition) {
	vec4 now = viewProj * vec4(position, 1.0), before = viewProj * vec4(previousPosition, 1.0);
	gVelocity = (now.xy / now.w - before.xy / before.w) * 0.5;
}
#endif

#endif
