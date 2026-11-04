// volumetric fog: a 3D grid of cells ("froxels") fitted to the view, x/y across the screen, z along the view ray
// out to ubo.fogDistance. volfog_inject.comp fills every froxel with how dense the fog is and how much light it
// scatters, volfog_integrate.comp sums them front to back, lighting.comp looks the result up per pixel.
// the slices are spread quadratically: thin near the camera where the detail shows, thick far away
#ifndef VOLFOG_GLSL
#define VOLFOG_GLSL

// distance along the view ray (world units) at slice coordinate s (0 = camera, 1 = far)
float fogSliceDistance(float s, float far) {
	return far * s * s;
}

// and back
float fogSliceCoord(float dist, float far) {
	return sqrt(clamp(dist / far, 0.0, 1.0));
}

// the fog between the camera and dist along the ray through uv: rgb = light scattered towards the camera,
// a = transmittance. every texel of the integrated volume holds the sum up to the END of its slice
vec4 sampleVolumetricFog(sampler3D fog, vec2 uv, float dist, float far) {
	float slices = float(textureSize(fog, 0).z);
	float s = fogSliceCoord(dist, far) * slices;
	vec4 integrated = texture(fog, vec3(uv, clamp(s - 0.5, 0.5, slices - 0.5) / slices));
	return s < 1.0 ? mix(vec4(0.0, 0.0, 0.0, 1.0), integrated, s) : integrated; // fades in over the first slice
}

#endif
