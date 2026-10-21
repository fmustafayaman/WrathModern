# Exercises the version 2 mod interface: a Lua function, an event, a console variable and the script
# context callbacks. Results go to Logs/Mods.log.
#   cmake -S Mods/Samples/ApiSmoke -B build/mods/ApiSmoke -G Ninja && ninja -C build/mods/ApiSmoke
cmake_minimum_required(VERSION 3.20)
project(ApiSmoke LANGUAGES C CXX)

set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_CXX_STANDARD 17)

add_library(ApiSmoke MODULE ApiSmoke.cpp)
set_target_properties(ApiSmoke PROPERTIES PREFIX "" SUFFIX ".dylib")
target_include_directories(ApiSmoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../WoW/Source/WorldClient/Mods
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../Engine/Source/lua-5.1/src)
target_compile_options(ApiSmoke PRIVATE -fvisibility=hidden -Wall)
# The Lua C API comes from the client executable
target_link_options(ApiSmoke PRIVATE "-undefined" "dynamic_lookup")
