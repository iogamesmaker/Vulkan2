#version 450
// builds the blade cards of one clump procedurally (no vertex buffer): BLADES_PER_CLUMP quads per instance
// grass: blades from the ambientCG atlas. wheat fields: 3 whole oat plants from their atlas (the other 3 cards unused)

#include "foliagecommon.glsl"
#define INTERACTION_SET 0
#define INTERACTION_BINDING 5
#include "interaction.glsl"

layout(std430, set = 0, binding = 0) readonly buffer Instances { FoliageInstance instances[]; };

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec2 outUV;		 // atlas uv
layout(location = 2) out vec3 outNormal;
layout(location = 3) out float outHeight;	 // 0 at the root, 1 at the tip
layout(location = 4) flat out int outKind;	 // 0 grass blade, 1 wheat (oat plant), 3 flower head
layout(location = 6) flat out float outMoisture;
layout(location = 5) flat out float outSeed;
layout(location = 7) out vec3 outPrevPosition; // with last frame's wind (motion vectors)
layout(location = 8) flat out vec3 outGroundNormal; // the terrain's normal under the clump (foliage.comp)
layout(location = 9) flat out vec4 outGroundAlbedo; // the terrain's colour under the clump, w = 1 when known (foliage.comp)

invariant gl_Position; // the prepass and the shading pass must produce the exact same depth

// bounding boxes of the blades in grass_blades.png (Foliage001), x0 y0 x1 y1, roots at y1.
// the last one is a curly dry blade, only used now and then
const vec4 BLADES[9] = vec4[](
	vec4(0.0366, 0.0918, 0.0913, 0.9077), vec4(0.1499, 0.0571, 0.1826, 0.9077), vec4(0.2061, 0.0728, 0.2915, 0.9067),
	vec4(0.3394, 0.0942, 0.3760, 0.9033), vec4(0.4136, 0.0981, 0.4897, 0.9067), vec4(0.5400, 0.1123, 0.5747, 0.9116),
	vec4(0.6313, 0.1411, 0.6992, 0.9102), vec4(0.7295, 0.1089, 0.7808, 0.9087), vec4(0.8154, 0.1226, 0.9551, 0.9077)
);

// the six daisies in flowers.png (FlowerSet001)
const vec4 FLOWERS[6] = vec4[](
	vec4(0.0547, 0.1621, 0.3281, 0.4297), vec4(0.3594, 0.1562, 0.6328, 0.4512), vec4(0.6914, 0.1641, 0.9492, 0.4238),
	vec4(0.0312, 0.5078, 0.3555, 0.8359), vec4(0.3809, 0.5527, 0.6504, 0.8184), vec4(0.6973, 0.5625, 0.9727, 0.8438)
);

// the three oat plants in oat.png (repacked from ShareTextures AvenaSativa_1), roots at y1
const vec4 OATS[3] = vec4[](
	vec4(0.0117, 0.1606, 0.3389, 0.9883), vec4(0.3506, 0.1484, 0.7559, 0.9883), vec4(0.7676, 0.4795, 0.9644, 0.9883)
);

const vec2 CORNERS[6] = vec2[](vec2(-0.5, 0.0), vec2(0.5, 0.0), vec2(0.5, 1.0), vec2(-0.5, 0.0), vec2(0.5, 1.0), vec2(-0.5, 1.0));

void main() {
	FoliageInstance inst = instances[gl_InstanceIndex];
	vec2 groundXZ = unpackHalf2x16(floatBitsToUint(inst.params.x));
	outGroundNormal = vec3(groundXZ.x, sqrt(max(1.0 - dot(groundXZ, groundXZ), 0.0)), groundXZ.y);
	float moisture;
	outGroundAlbedo = unpackGround(inst.params.w, moisture);
	// (the near ring and the shadow casters are drawn with BLADE_SEGMENTS quads per blade, the rest with one)
	bool segmentLayout = uint(gl_InstanceIndex) >= FOLIAGE_SEGMENTED_FROM;
	int blade = gl_VertexIndex / (segmentLayout ? 6 * BLADE_SEGMENTS : 6);
	int segment = segmentLayout ? (gl_VertexIndex / 6) % BLADE_SEGMENTS : 0;
	vec2 corner = CORNERS[gl_VertexIndex % 6];

	int type = int(inst.params.y) % 8;
	float ring = float(int(inst.params.y) / 8);
	float seed = inst.params.z;
	float size = inst.posScale.w;

	// wheat fields only use cards 0..2 (a whole plant each, grain included): the rest goes outside the clip volume
	// and gets clipped away before rasterisation
	// (the two far rings: 3 plants per clump, they're wider)
	if (type == FOLIAGE_WHEAT && blade >= (ring > 1.5 ? 3 : 6)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		outPosition = vec3(0.0); outPrevPosition = vec3(0.0); outUV = vec2(0.0); outNormal = vec3(0.0, 1.0, 0.0); outHeight = 0.0;
		outKind = 1; outSeed = 0.0; outMoisture = 0.0;
		return;
	}

	float bladeLOD = 1.0;
	// blade LOD: a grass clump has all its blades up close, fewer further out (the far ring's wider cards fill in).
	// the cards past the count go outside the clip volume like the unused wheat cards
	if (type == FOLIAGE_GRASS || (type == FOLIAGE_FLOWERS && blade >= 6)) {
		float clumpDistance = distance(inst.posScale.xyz, pc.campos);
		// continuous: the last blade shrinks away instead of popping out as you walk (bladeLOD scales its height)
		float blades = ring > 0.5 ? 3.0 : mix(float(BLADES_PER_CLUMP), 3.0, smoothstep(1.0, 4.5, clumpDistance));
		bladeLOD = clamp(blades - float(blade), 0.0, 1.0);
		if (bladeLOD <= 0.0) {
			gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
			outPosition = vec3(0.0); outPrevPosition = vec3(0.0); outUV = vec2(0.0); outNormal = vec3(0.0, 1.0, 0.0); outHeight = 0.0;
			outKind = 0; outSeed = 0.0; outMoisture = 0.0;
			return;
		}
	}

	// flowers: cards 3..5 are the flower heads sitting on top of stems 0..2, the rest of the clump is grass
	bool flowerHead = type == FOLIAGE_FLOWERS && blade >= 3 && blade < 6;
	if (type == FOLIAGE_FLOWERS && blade >= 6) type = FOLIAGE_GRASS;

	// only the near ring's blades (and flower stems) bend through several segments: a single quad can't curve, every
	// blade was a straight rigid spike. further away, and for the flat cards (heads, oats, leaves), one segment
	bool segmented = segmentLayout && !flowerHead && (type == FOLIAGE_GRASS || type == FOLIAGE_FLOWERS);
	if (!segmented && segment > 0) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		outPosition = vec3(0.0); outPrevPosition = vec3(0.0); outUV = vec2(0.0); outNormal = vec3(0.0, 1.0, 0.0); outHeight = 0.0;
		outKind = 0; outSeed = 0.0; outMoisture = 0.0;
		return;
	}

	int stalk = flowerHead ? blade - 3 : blade;
	vec4 r = hash42(vec2(seed, float(stalk) * 7.31));

	// spread the blades around the clump centre, the far ring has fewer, bigger clumps so they spread more
	float clumpRadius = (ring > 2.5 ? 0.4 : (ring > 1.5 ? 0.2 : (ring > 0.5 ? 0.1 : 0.05))) * size; // 10 blades spread a bit wider to cover the ground
	float angle = r.y * 6.2831853;
	vec3 root = inst.posScale.xyz + vec3(cos(angle), 0.0, sin(angle)) * sqrt(r.z) * clumpRadius;

	vec4 rect;
	float height, width;
	if (type == FOLIAGE_WHEAT) {
		rect = OATS[min(int(r.x * 3.0), 2)];
		// ~1m, the short plant (a top without its lower stalk) is drawn shorter so all the grain sits about as high
		height = 0.17 * (0.85 + 0.3 * r.z) * size * (rect.w - rect.y) / 0.84;
		width = height * (rect.z - rect.x) / (rect.w - rect.y);
	} else if (type == FOLIAGE_FLOWERS) {
		height = 0.06 * (0.7 + 0.6 * r.z) * size; // stems ~30-50cm, a bit above the grass
		rect = BLADES[int(r.x * 8.0)];
		width = height * (rect.z - rect.x) / (rect.w - rect.y) * 0.6;
	} else {
		height = 0.09 * (0.55 + 0.9 * r.z) * size; // ~30-80cm, the same in every ring (taller far grass showed as a step)
		rect = BLADES[r.x > 0.96 ? 8 : int(r.x * 8.0)];
		width = height * (rect.z - rect.x) / (rect.w - rect.y) * mix(0.8, 1.1, r.w); // the photographed blade's own proportions (~1-2cm)
		height *= bladeLOD;
	}
	// thin blades disappear at a distance, fatter cards keep the coverage (whole plant cards are wide already)
	if (ring > 0.5 && type != FOLIAGE_WHEAT) width *= ring > 1.5 ? 5.0 : 2.5;
	// wheat: the outer rings' plants are much further apart (cells 2.8x, 5.5x, 11x the near ring's), wider cards keep
	// the field closed (it went bare a few metres out). only 3 of the 6 plants in the two far rings, wider still
	if (type == FOLIAGE_WHEAT) width *= ring > 2.5 ? 9.0 : (ring > 1.5 ? 5.0 : (ring > 0.5 ? 2.4 : 1.0));

	// keep blades at least a couple of pixels wide so they don't shimmer away
	float dist = distance(root, pc.campos);
	width = max(width, dist * 0.003);

	// wind: a slow gust field moving over the world plus a little per blade flutter, bends the tips most
	vec2 windDir = normalize(vec2(0.8, 0.35));
	vec2 worldXZ = root.xz + pc.offsetWorld;
	float gustPhase = dot(worldXZ, windDir) * 2.5, flutterPhase = r.w * 40.0;
	// grass arcs over, it doesn't stand like spikes. the weather's wind: barely moving on a calm day, flattened in a gale
	float leanScale = (type == FOLIAGE_WHEAT ? 0.25 : 0.5) * mix(0.55, 1.9, interaction.trailInfo.z);
	float lean = ((sin(pc.time * 1.3 + gustPhase) * 0.5 + 0.5) * 0.6 + sin(pc.time * 4.7 + flutterPhase) * 0.15 + 0.3) * leanScale;
	float leanBefore = ((sin(pc.sundir.x * 1.3 + gustPhase) * 0.5 + 0.5) * 0.6 + sin(pc.sundir.x * 4.7 + flutterPhase) * 0.15 + 0.3) * leanScale; // last frame
	vec3 bendDir = normalize(vec3(windDir.x, 0.0, windDir.y) + vec3(r.w - 0.5, 0.0, r.z - 0.5) * 0.8);

	vec3 up = vec3(0.0, 1.0, 0.0);
	vec3 tipOffset = bendDir * height * lean;
	vec3 tipBefore = bendDir * height * leanBefore;

	// walking through it pushes the blades aside and down, along the whole trail behind you: they spring back up
	// over a second or so after you've passed, swinging a little past upright first (interaction.glsl)
	{
		vec4 touch = interactionPush(root, 0.13, 0.3); // ~75cm around the feet
		if (touch.y > 0.0 || dot(touch.xz, touch.xz) > 0.0) {
			vec3 pushed = vec3(touch.x, 0.0, touch.z) * height * 0.95 - up * height * 0.55 * touch.y;
			tipOffset = tipOffset * (1.0 - touch.y) + pushed;
			tipBefore = tipBefore * (1.0 - touch.y) + pushed;
		}
	}

	float t = segmented ? (float(segment) + corner.y) / float(BLADE_SEGMENTS) : corner.y; // 0 root, 1 tip
	// at rest a blade arcs outwards from its clump (longer ones further), its tip a little lower for it
	vec3 outward = normalize(vec3(cos(angle), 0.0, sin(angle)));
	float arc = type == FOLIAGE_WHEAT ? 0.05 : mix(0.12, 0.6, r.z);
	vec3 restOffset = outward * height * arc;
	vec3 stemPoint;
	vec3 tangent = up;
	vec3 headUp = up;
	if (flowerHead) {
		// a daisy facing mostly up, tilted towards the camera so it doesn't vanish edge on
		vec3 stalkTip = root + up * height * (1.0 - 0.3 * arc) + tipOffset + restOffset; // where its (arcing) stem ends
		vec3 toCamera = normalize(pc.campos - stalkTip);
		vec3 facing = normalize(up * 1.2 + toCamera);
		vec3 side = normalize(cross(facing, up + vec3(1e-3, 0.0, 0.0)));
		headUp = normalize(cross(side, facing));
		float headSize = height * (0.28 + 0.12 * r.x);
		stemPoint = stalkTip + headUp * headSize * (t - 0.5);
		width = headSize;
		int flower = int(r.w * 6.0);
		rect = FLOWERS[flower];
	} else {
		stemPoint = root + up * height * t * (1.0 - 0.3 * arc * t) + (tipOffset + restOffset) * t * t; // quadratic bend
		tangent = normalize(up * height * (1.0 - 0.6 * arc * t) + (tipOffset + restOffset) * 2.0 * t);
	}

	// face the card partly towards the camera so it's never seen edge on
	float yaw = fract(seed * 0.1234) * 6.2831853 * 7.0 + r.w * 6.2831853;
	vec3 randomRight = vec3(cos(yaw), 0.0, sin(yaw));
	vec3 toCam = pc.campos - root;
	toCam.y = 0.0;
	vec3 cameraRight = normalize(cross(up, toCam + vec3(1e-4, 0.0, 0.0)));
	// (mostly their own way: turned towards the camera they all lined up flat, like paper cut outs. thin enough now
	// that edge on blades just thin out)
	vec3 right = normalize(mix(randomRight, cameraRight, segmented ? 0.35 : 0.65));
	if (flowerHead) right = normalize(cross(headUp, normalize(pc.campos - stemPoint)));

	// blades taper, flower heads and whole oat plants are drawn as they are in their atlas
	vec3 pos = stemPoint + right * corner.x * width; // the blade shape narrows itself (foliagealpha.glsl)

	// rounded normals across the blade so it shades like a curved leaf instead of a flat card
	// (along the bend: the tip of an arcing blade faces the sky, its base the side)
	vec3 cardNormal = normalize(cross(right, tangent));
	// (gently: a strongly bent normal lights one half of every blade fully and leaves the other half dark)
	vec3 normal = normalize(cardNormal + right * corner.x * 0.5 + up * 0.3);
	if (flowerHead) normal = normalize(cross(right, headUp) + up);

	outPosition = pos;
	// the same point with last frame's bend (the tip moves the most, the root not at all)
	outPrevPosition = pos - (tipOffset - tipBefore) * (flowerHead ? 1.0 : t * t);
	// every card shows its rect of the atlas (the blade, the oat plant, the flower head), root at the rect's bottom
	outUV = vec2(mix(rect.x, rect.z, corner.x + 0.5), mix(rect.w, rect.y, t));
	outNormal = normal;
	outHeight = flowerHead ? 1.0 : t;
	outKind = flowerHead ? 3 : (type == FOLIAGE_WHEAT ? 1 : 0);
	outSeed = r.w;
	outMoisture = moisture;

	gl_Position = pc.viewproj * vec4(pos, 1.0);
}
