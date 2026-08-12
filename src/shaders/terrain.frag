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
	mat4 viewproj;
	vec3 campos;
	float tessellationFactor;
	vec3 sundir;
	float factor;
	ivec2 offset;
	ivec2 screen;
} pc;

void main() {
	if(fragUV.x > 0.984 || fragUV.y > 0.984 || fragUV.x < 0.016 || fragUV.y < 0.016) discard;

    vec2 size = vec2(textureSize(heightmap, 0));
    vec2 readCoord = fragUV + (vec2(pc.offset) / size);

    float hR = textureOffset(heightmap, readCoord, ivec2(1, 0)).a;
    float hL = textureOffset(heightmap, readCoord, ivec2(-1, 0)).a;
    float hT = textureOffset(heightmap, readCoord, ivec2(0, 1)).a;
    float hB = textureOffset(heightmap, readCoord, ivec2(0, -1)).a;
	
	float dzdx = (hR - hL) * 256.0f;
    float dzdy = (hT - hB) * 256.0f;
	float height1 = texture(pom[0], readCoord * 200.0).r;
	float height2 = texture(pom[1], readCoord * 200.0).r;
	float blend = smoothstep(0.9 * 0.9, 0.7 * 0.7, (dzdx * dzdx) + (dzdy * dzdy) + (height1 - height2) * (pc.tessellationFactor == 10.0f ? 0.6f : 0.0f));
	
	vec3 va = normalize(vec3(2.0, (hR - hL) * 256.0f, 0.0));
    vec3 vb = normalize(vec3(0.0, (hT - hB) * 256.0f, 2.0));
    vec3 normal = normalize(cross(vb, va));
	
	
	vec3 normalTexture1 = normalize((texture(normalmap[0], readCoord * 200.0).rgb) * 2.0 - 1.0);
	vec3 normalTexture2 = normalize((texture(normalmap[1], readCoord * 200.0).rgb) * 2.0 - 1.0);
	vec3 normalTexture = mix(normalTexture1, normalTexture2, blend); 
	
	mat3 TBN = mat3(va, vb, normal);
	normal = normalize(TBN * normalTexture);
	
	vec3 lightDir = pc.sundir;
	
	vec3 albedo1 = texture(albedo[0], readCoord * 200.0).rgb;
	vec3 albedo2 = texture(albedo[1], readCoord * 200.0).rgb;
	vec3 albedo = mix(albedo1, albedo2, blend);
	
	float diff = max(dot(normal, lightDir), 0.0);
    outColor = vec4((0.4 + 0.6 * diff) * albedo, 1.0);
    //outColor = vec4(normal * 0.5 + 0.5, 1.0);
	//outColor = vec4(LOD / 64.f, 0.0, 0.0, 1.0);
	
	//outColor = vec4(0.1, diff * 0.7 + 0.3, 0.1, 1.0);
}