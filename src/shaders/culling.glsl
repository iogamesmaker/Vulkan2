// the frustum test the scatter passes share (trees, rocks, ground cover, grass)
#ifndef CULLING_GLSL
#define CULLING_GLSL

// a sphere (world centre, world radius) entirely outside the view. the radius goes into clip space with the
// projection's own scale: a fixed guess of it (~1.4 at the normal 70 degrees) was 8-50x too small while zoomed in
// (C), so things still half on screen got culled right where you were looking
bool sphereOutsideView(mat4 viewProj, vec3 centre, float radius) {
	vec4 clip = viewProj * vec4(centre, 1.0);
	// the rows of the matrix: x, y and w per world unit (the view is rigid, so w's row is the unit forward vector)
	float sx = length(vec3(viewProj[0][0], viewProj[1][0], viewProj[2][0])) + 1.0;
	float sy = length(vec3(viewProj[0][1], viewProj[1][1], viewProj[2][1])) + 1.0;
	return clip.w < -radius || abs(clip.x) > clip.w + radius * sx || abs(clip.y) > clip.w + radius * sy;
}

#endif
