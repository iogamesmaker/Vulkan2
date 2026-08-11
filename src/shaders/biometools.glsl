#ifndef BIOME_TOOLS_GLSL
#define BIOME_TOOLS_GLSL

#define N_BIOMES    3

#define MOUNTAIN	0
#define COLDPEAKS   1
#define PLAINS		2
#define WHEATFIELD	3

struct Biome {
	vec3 min; // x = humidity, y = temperature, z = altitude
	vec3 max; // 
	
	float blend;
	
	ivec4 textures; // 1: normal, 2: steep, 34 unused
};

Biome biomes[N_BIOMES];

void initBiomes() {
	// Mountain biome
	biomes[MOUNTAIN].textures = ivec4(1, 0, -1, -1);
	biomes[MOUNTAIN].blend = 0.1f;
	biomes[MOUNTAIN].min = vec3(0.0, 0.0, 0.5);
	biomes[MOUNTAIN].max = vec3(1.0, 1.0, 0.8);

	// Cold peaks biome
	biomes[COLDPEAKS].textures = ivec4(1, 0, -1, -1);
	biomes[COLDPEAKS].blend = 0.05f;
	biomes[COLDPEAKS].min = vec3(0.0, 0.0, 0.8);
	biomes[COLDPEAKS].max = vec3(1.0, 1.0, 1.0);
}

#endif