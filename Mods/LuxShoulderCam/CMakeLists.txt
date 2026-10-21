# Lux Shoulder Cam for WrathModern: the shoulder camera of Stormhand's LuxShoulderCam.dll as a mod.
# Works with the unmodified LuxShoulderCam addon (Interface/AddOns/LuxShoulderCam), which calls LSC().
#   cmake -S Mods/LuxShoulderCam -B build/mods/LuxShoulderCam -G Ninja && ninja -C build/mods/LuxShoulderCam
# With -DWRATHMODERN_GAME_DIR=<game directory> the build copies the mod into its Mods folder.
cmake_minimum_required(VERSION 3.20)
project(LuxShoulderCam LANGUAGES C CXX)

set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_CXX_STANDARD 17)

add_library(LuxShoulderCam MODULE LuxShoulderCam.cpp)
set_target_properties(LuxShoulderCam PROPERTIES PREFIX "" SUFFIX ".dylib")
target_include_directories(LuxShoulderCam PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../WoW/Source/WorldClient/Mods
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Source/lua-5.1/src)
target_compile_options(LuxShoulderCam PRIVATE -fvisibility=hidden -Wall)
# The Lua C API comes from the client executable
target_link_options(LuxShoulderCam PRIVATE "-undefined" "dynamic_lookup")

set(WRATHMODERN_GAME_DIR "" CACHE PATH "Game directory to copy the mod into")
if (WRATHMODERN_GAME_DIR)
    add_custom_command(TARGET LuxShoulderCam POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:LuxShoulderCam>" "${WRATHMODERN_GAME_DIR}/Mods/"
        VERBATIM)
endif ()
