# Sample mod, built on its own: it depends only on the mod interface header.
#   cmake -S Mods/Samples/DepthFog -B build/mods/DepthFog -G Ninja && ninja -C build/mods/DepthFog
# Copy DepthFog.dylib into the game's Mods folder to load it.
cmake_minimum_required(VERSION 3.20)
project(DepthFog LANGUAGES CXX OBJCXX)

set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_CXX_STANDARD 17)

add_library(DepthFog MODULE DepthFog.mm)
set_target_properties(DepthFog PROPERTIES PREFIX "" SUFFIX ".dylib")
target_include_directories(DepthFog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../WoW/Source/WorldClient/Mods)
target_compile_options(DepthFog PRIVATE -fobjc-arc -fvisibility=hidden -Wall)
target_link_libraries(DepthFog PRIVATE "-framework Metal" "-framework Foundation")
