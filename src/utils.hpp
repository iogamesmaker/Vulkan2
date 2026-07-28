// utils.hpp
#ifndef UTILS_HPP
#define UTILS_HPP
#include <string>
#include <SDL3/SDL.h>

namespace util {
	std::string getpath(const std::string& subDir) {
		const char* base = SDL_GetBasePath();
		std::string path;

		if (base) {
			path = base;
		} else {
			std::cerr << "SDL_GetBasePath: " << SDL_GetError() << std::endl;
			return "./";
		}

		return path + subDir;
	}
}
#endif