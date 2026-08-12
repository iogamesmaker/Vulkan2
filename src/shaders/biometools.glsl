#ifndef BIOME_TOOLS_GLSL
#define BIOME_TOOLS_GLSL

#define N_BIOMES    3

#define MOUNTAIN	0
#define COLDPEAKS   1
#define PLAINS		2
//#define WHEATFIELD	3

vec3 linnormalize(vec3 val) { // makes val.x + val.y + val.z equal to 1
	return val / (val.x + val.y + val.z);
}

struct Biome {
	vec3 value; // r = temperature, g = humidity, b = altitude
	vec3 importance;
	vec3 debugcolor;
	
	float blend;
	
	ivec4 textures; // 1: normal, 2: steep, 34 unused
};

Biome biomes[N_BIOMES];

void initBiomes() {
	// Mountain biome
	biomes[MOUNTAIN].textures = ivec4(1, 0, -1, -1);
	biomes[MOUNTAIN].blend = 10.f / 3.5f;
	biomes[MOUNTAIN].value = vec3(0.5, 0.5, 0.7);
	biomes[MOUNTAIN].importance = linnormalize(vec3(0.1, 0.1, 0.6));
	biomes[MOUNTAIN].debugcolor = vec3(0.5);

	// Cold peaks biome
	biomes[COLDPEAKS].textures = ivec4(1, 0, -1, -1);
	biomes[COLDPEAKS].blend = 10.f / 3.5f;
	biomes[COLDPEAKS].value = vec3(0.1, 0.5, 1.0);
	biomes[COLDPEAKS].importance = linnormalize(vec3(0.3, 0.0, 0.6));
	biomes[COLDPEAKS].debugcolor = vec3(1.0);
	
	// Plains
	biomes[PLAINS].textures = ivec4(1, 0, -1, -1);
	biomes[PLAINS].blend = 10.f / 8.0f;
	biomes[PLAINS].value = vec3(0.5, 0.5, 0.5);
	biomes[PLAINS].importance = linnormalize(vec3(0.6, 0.5, 0.5));
	biomes[PLAINS].debugcolor = vec3(0.8, 0.8, 0.0);
}

int whatBiome(vec3 value) {
	float weight = -1.0;
	int biome = -1;
	
	for(int i = 0; i < N_BIOMES; i++) {
		float humidity    = 1.0 - abs(biomes[i].value.x - value.x);
		float temperature = 1.0 - abs(biomes[i].value.y - value.y);
		float altitude    = 1.0 - abs(biomes[i].value.z - value.z);
		
		float finalweight = humidity * biomes[i].importance.x +  temperature * biomes[i].importance.y +  altitude * biomes[i].importance.z;
		
		if(finalweight > weight) {
			biome = i;
			weight= finalweight;
		}
	}
	
	return biome;
}

void whatBiomes(vec3 value, out vec3 weights, out ivec3 ids) {
	weights = vec3(-1.0);
	ids = ivec3(-1);
	
	for(int i = 0; i < N_BIOMES; i++) {
		float humidity    = 1.0 - abs(biomes[i].value.x - value.x);
		float temperature = 1.0 - abs(biomes[i].value.y - value.y);
		float altitude    = 1.0 - abs(biomes[i].value.z - value.z);
		
		float finalweight = humidity * biomes[i].importance.x +  temperature * biomes[i].importance.y +  altitude * biomes[i].importance.z;
		
		if(finalweight > weights.x) {
			if(finalweight > weights.y) {
				if(finalweight > weights.z) {
					ids.x = ids.y; weights.x = weights.y;
					ids.y = ids.z; weights.y = weights.z;
					ids.z = i; weights.z = finalweight;
				} else {
					ids.x = ids.y; weights.x = weights.y;
					ids.y = i; weights.y = finalweight;
				}
			} else {
				ids.x = i; weights.x = finalweight;
			}
		}
	}
	
	if(weights.x <= 0.0) ids.x = -1;
	if(weights.y <= 0.0) ids.y = -1;
	if(weights.z <= 0.0) ids.z = -1;
	if(ids.x == -1) {weights.x = 0.0;}
	else {
		weights.x = max(0.0, 1.0 - (biomes[ids.x].blend * (weights.z - weights.x)));
		if(weights.x == 0.0) ids.x = -1;
	}
	if(ids.y == -1) {weights.y = 0.0;}
	else {
		weights.y = max(0.0, 1.0 - (biomes[ids.y].blend * (weights.z - weights.y)));
		if(weights.y == 0.0) ids.y = -1;
	}
	if(ids.z == -1) {weights.z = 0.0;}
	else {weights.z = 1.0;}
	
	weights = linnormalize(weights);
}

#endif