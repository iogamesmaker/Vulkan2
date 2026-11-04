## Learning Vulkan: Attempt 2
My previous attempt at learning Vulkan was a pretty lazy attempt to say the least. I only managed to get a proper build system working after 2 days of throwing random AI generated nonsense in a CMakeLists file, and I used some obscure Vulkan wrapper called "Diligent Engine" with slightly outdated documentation which made it a pain to work with, not knowing all the features and stuff.
I poured very little effort in trying to understand how the graphics pipeline shit actually worked, and let AI deal with the weird Vulkan backend shit. No final goal in mind, just wanted some pretty pictures I guess, so I never bothered to look further than a shadertoy-esque engine.

### Building (Windows, MSVC)
Needs the Visual Studio Build Tools (C++ workload), CMake, Ninja, the Vulkan SDK and vcpkg in `C:\code\clone\vcpkg` (or set `VCPKG_ROOT`). Then:
```
build.bat              # RelWithDebInfo
build.bat Debug
build.bat Release clean
```
`Vulkan2.exe` lands in the project root. The MinGW/clang setup (`recmake.sh`) still works too.

This time around, instead of no plan at all I got a not-so-concrete but still existant concept for a final project. I want to create a photorealistic-ish terrain visualizer. Anything considered photorealistic around 2010 will do I'm not gonna make red dead 3.