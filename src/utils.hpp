// utils.hpp
#ifndef UTILS_HPP
#define UTILS_HPP
#include <string>
#include <SDL3/SDL.h>

namespace util {
	inline std::string getpath(const std::string& subDir) { // inline, this header is included in multiple .cpp files
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