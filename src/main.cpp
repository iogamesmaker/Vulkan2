#if defined(_WIN32) || defined(_WIN64)
#define PLATFORM_WIN32 1
#elif defined(__APPLE__)
#define PLATFORM_MACOS 1
#elif defined(__linux__)
#define PLATFORM_LINUX 1
#endif

#include <stdexcept>
#include <iostream>
#include "application.hpp"

int main() {
	try {
		Application app(800, 600);
		app.run();
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return -1;
	}
	return 0;
}