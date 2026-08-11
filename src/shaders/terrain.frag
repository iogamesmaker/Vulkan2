#version 450

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec2 fragUV;
layout(location = 5) flat in int LOD;

layout(set = 0, binding = 0) uniform sampler2D heightmap;

layout(set = 0, binding = 1) uniform sampler2D albedo[];
layout(set = 0, binding = 2) uniform sampler2D normalmap[];
layout(set = 0, binding = 3) uniform sampler2D pom[];

layout(push_constant) uniform constants {
	mat4 proj;
	mat4 view;
	ivec2 offset;
	ivec2 screen;
	float tessellationFactor;
	float factor;
} pc;

void main() {
	if(fragUV.x > 0.984 || fragUV.y > 0.984 || fragUV.x < 0.016 || fragUV.y < 0.016) discard;

    vec2 size = vec2(textureSize(heightmap, 0));
    vec2 readCoord = fragUV + (pc.offset * (1.0 / size));

    float hR = textureOffset(heightmap, readCoord, ivec2(1, 0)).r;
    float hL = textureOffset(heightmap, readCoord, ivec2(-1, 0)).r;
    float hT = textureOffset(heightmap, readCoord, ivec2(0, 1)).r;
    float hB = textureOffset(heightmap, readCoord, ivec2(0, -1)).r;

    vec3 va = normalize(vec3(2.0, 0.0, (hR - hL) * 256.0f));
    vec3 vb = normalize(vec3(0.0, 2.0, (hT - hB) * 256.0f));
    vec3 normal = normalize(cross(va, vb));
	vec3 normalTexture = normalize((texture(normalmap[0], readCoord * 100.0).rgb) * 2.0 - 1.0);
	
	mat3 TBN = mat3(va, vb, normal);
	normal = normalize(TBN * normalTexture);
	
	vec3 lightDir = normalize(vec3(-2.0, 10.0, 2.0));
	
	float diff = max(dot(normal, lightDir), 0.0);
    outColor = (0.25 + 0.75 * diff) * texture(albedo[0], readCoord * 100.0);
    //outColor = vec4(normal * 0.5 + 0.5, 1.0);
	//outColor = vec4(LOD / 64.f, 0.0, 0.0, 1.0);
	
	//outColor = vec4(0.1, diff * 0.7 + 0.3, 0.1, 1.0);
}