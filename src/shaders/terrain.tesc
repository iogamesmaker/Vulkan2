#version 450
// tessellation control thing
// culls patches outside the view frustum / outside the circular world, and picks tessellation levels
// so triangle edges are roughly the same size on screen everywhere

layout(vertices = 4) out;

layout(location = 0) in vec3 inPosition[];
layout(location = 1) in vec2 inUV[];

layout(location = 0) out vec3 outPosition[];
layout(location = 1) out vec2 outUV[];
layout(location = 5) flat out int outLOD[];

layout(set = 0, binding = 0) uniform sampler2D heightmap;

layout(push_constant) uniform constants {
	mat4 viewproj;
	vec3 campos;
	float tessellationFactor;
	vec3 sundir;
	float factor;
	ivec2 offset;
	float projScale;   // screen height / 2 * projection[1][1] of the main camera, the same for the shadow pass
	float worldRadius; // nothing gets drawn further away than this (horizontally)
	float fixedTessLevel; // > 0: shadow pass, ignore the camera based LOD
	float time;
	vec2 pad1;
} pc;

float heightAt(vec2 uv) {
	vec2 readCoord = uv + vec2(pc.offset) / vec2(textureSize(heightmap, 0));
	return texture(heightmap, readCoord).a * pc.factor - pc.factor * 0.5;
}

// LOD only depends on the edge itself so neighbouring patches always agree (no cracks)
float tessFactor(vec3 p0, vec3 p1, vec2 uv0, vec2 uv1) {
	vec3 mid = 0.5 * (p0 + p1);
	mid.y = heightAt(0.5 * (uv0 + uv1));

	float dist = max(distance(mid, pc.campos), 1.0);
	float edgePixels = distance(p0.xz, p1.xz) * pc.projScale / dist;
	float targetPixels = 40.0 / max(pc.tessellationFactor, 0.1); // factor 10 -> 4px triangles
	float level = edgePixels / targetPixels;
	// no finer than it's worth: the patches' finest (0.375 units, ~2m) only around your feet where you walk over
	// it, half a heightmap texel (0.75) from ~40 units on: the bicubic heightmap and the bumps' coarse octaves are
	// all there is to show, and the pixel target alone kept triangles at the finest level out to ~1km
	float minSpacing = mix(0.375, 0.75, smoothstep(8.0, 40.0, dist));
	level = min(level, distance(p0.xz, p1.xz) / minSpacing);
	return clamp(level, 1.0, 64.0);
}

// shadow pass: the cascade's level within its radius of where it was rendered around (pc.sundir = centre.x, radius,
// centre.z, the shadow pass has no lighting), coarser further out where the casters are mountains towards a low
// sun. only depends on the edge and the cascade, so it doesn't change until the cascade is re-rendered
float shadowTessFactor(vec3 p0, vec3 p1) {
	vec2 mid = 0.5 * (p0.xz + p1.xz);
	float radius = pc.sundir.y;
	float d = distance(mid, pc.sundir.xz);
	return clamp(pc.fixedTessLevel * radius / max(d, radius), 1.0, 64.0);
}

bool patchVisible() {
	vec3 p0 = gl_in[0].gl_Position.xyz;
	vec3 p3 = gl_in[3].gl_Position.xyz;
	vec2 minXZ = min(p0.xz, p3.xz);
	vec2 maxXZ = max(p0.xz, p3.xz);

	// circular world: closest point of the patch to the camera
	vec2 closest = clamp(pc.campos.xz, minXZ, maxXZ);
	if (distance(closest, pc.campos.xz) > pc.worldRadius) return false;

	// frustum: the patch box with the full possible terrain height
	vec3 boxMin = vec3(minXZ.x, -pc.factor * 0.5, minXZ.y);
	vec3 boxMax = vec3(maxXZ.x,  pc.factor * 0.5, maxXZ.y);

	// count corners outside each plane, the patch is gone if all 8 are outside the same one
	ivec4 outsideA = ivec4(0); // -x, +x, -y, +y
	ivec2 outsideB = ivec2(0); // near, far
	for (int i = 0; i < 8; i++) {
		vec3 corner = vec3((i & 1) != 0 ? boxMax.x : boxMin.x, (i & 2) != 0 ? boxMax.y : boxMin.y, (i & 4) != 0 ? boxMax.z : boxMin.z);
		vec4 clip = pc.viewproj * vec4(corner, 1.0);
		outsideA += ivec4(clip.x < -clip.w, clip.x > clip.w, clip.y < -clip.w, clip.y > clip.w);
		outsideB += ivec2(clip.z < 0.0, clip.z > clip.w);
	}
	return !(any(equal(outsideA, ivec4(8))) || any(equal(outsideB, ivec2(8))));
}

void main() {
	if(gl_InvocationID == 0) {
		if(!patchVisible()) {
			gl_TessLevelOuter[0] = 0.0; // a 0 outer level discards the whole patch
			gl_TessLevelOuter[1] = 0.0;
			gl_TessLevelOuter[2] = 0.0;
			gl_TessLevelOuter[3] = 0.0;
			gl_TessLevelInner[0] = 0.0;
			gl_TessLevelInner[1] = 0.0;
		} else {
			vec3 p0 = gl_in[0].gl_Position.xyz;
			vec3 p1 = gl_in[1].gl_Position.xyz;
			vec3 p2 = gl_in[2].gl_Position.xyz;
			vec3 p3 = gl_in[3].gl_Position.xyz;

			if (pc.fixedTessLevel > 0.0) {
				// shadow casters: levels that don't change as the camera moves so the shadows don't wobble
				gl_TessLevelOuter[0] = shadowTessFactor(p0, p2);
				gl_TessLevelOuter[1] = shadowTessFactor(p0, p1);
				gl_TessLevelOuter[2] = shadowTessFactor(p1, p3);
				gl_TessLevelOuter[3] = shadowTessFactor(p2, p3);
			} else {
				gl_TessLevelOuter[0] = tessFactor(p0, p2, inUV[0], inUV[2]);
				gl_TessLevelOuter[1] = tessFactor(p0, p1, inUV[0], inUV[1]);
				gl_TessLevelOuter[2] = tessFactor(p1, p3, inUV[1], inUV[3]);
				gl_TessLevelOuter[3] = tessFactor(p2, p3, inUV[2], inUV[3]);
			}

			gl_TessLevelInner[0] = max(gl_TessLevelOuter[1], gl_TessLevelOuter[3]);
			gl_TessLevelInner[1] = max(gl_TessLevelOuter[0], gl_TessLevelOuter[2]);
		}
	}
	outLOD[gl_InvocationID] = int(gl_TessLevelOuter[gl_InvocationID]);
	outPosition[gl_InvocationID] = inPosition[gl_InvocationID];
	outUV[gl_InvocationID] = inUV[gl_InvocationID];
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
}
