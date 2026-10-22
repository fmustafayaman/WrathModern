cmake_minimum_required(VERSION 3.10)

if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR
        "In-source builds not allowed.
        Please make a new directory (called a build directory) and run CMake from there.
        You may need to remove CMakeCache.txt."
    )
endif ()

project(wowdecomp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_SYSTEM_NAME MATCHES "Windows|Darwin")
    message(FATAL_ERROR "wowdecomp targets Windows and macOS.")
endif ()

include(${CMAKE_SOURCE_DIR}/cmake/Platform.cmake)

# x64 is the target. Win32 stays buildable: it is the configuration whose struct layouts can be
# checked against Wow.exe 3.3.5a (12340).
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WOWDECOMP_ARCH_64 1)
    add_compile_definitions(WOWDECOMP_ARCH_64=1)
elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(WOWDECOMP_ARCH_32 1)
    add_compile_definitions(WOWDECOMP_ARCH_32=1)
    # x87 floating point like the VS2005 original; keeps Win32 code comparable with the binary (re/tools/verify.py).
    if (MSVC)
        add_compile_options(/arch:IA32 /fp:fast)
    endif ()
    # 32-bit time_t like the original build: time/localtime/mktime resolve to _time32/_localtime32/_mktime32.
    add_compile_definitions(_USE_32BIT_TIME_T)
else ()
    message(FATAL_ERROR "Unsupported pointer size.")
endif ()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if (WOWDECOMP_PLATFORM_MAC)
    enable_language(OBJC OBJCXX)

    if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The macOS build is 64-bit only.")
    endif ()

    add_compile_definitions(
        WOWDECOMP_PLATFORM_MAC=1
        NOMINMAX
    )

    # The decompiled sources are written against MSVC. These keep clang close to it without
    # turning the whole tree into MSVC-compatibility mode.
    add_compile_options(
        -fms-extensions
        -fdeclspec
        -fno-strict-aliasing
        -fwrapv
        # MSVC truncates out-of-range enumerators to the underlying type; so does clang with this off.
        -Wno-c++11-narrowing
        -Wno-register
    )

    # Every arm64 Mac has an Apple M1 or later: code is scheduled and vectorised for those cores, and the
    # optimised configurations use -O3
    if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
        add_compile_options(-mcpu=apple-m1)
    endif ()
    foreach (lang C CXX OBJC OBJCXX)
        string(REPLACE "-O2" "-O3" CMAKE_${lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${lang}_FLAGS_RELWITHDEBINFO}")
    endforeach ()

    include_directories(BEFORE SYSTEM ${CMAKE_SOURCE_DIR}/Platform/Mac/Include)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:SHELL:-include ${CMAKE_SOURCE_DIR}/Platform/Mac/Prefix.h>")
    add_compile_options("$<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>")
else ()
    add_compile_definitions(
        WOWDECOMP_PLATFORM_WIN=1
        WIN32
        _WINDOWS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif ()

# Off by default: the client checks MPQ file MD5s (signaturefile) and GlueXML/FrameXML/AddOn .toc.sig files like the
# original. ON accepts unsigned or modified game data and interface files.
option(WOWDECOMP_DISABLE_SIGNATURE_CHECKS "Skip MPQ file signature and .toc signature checks" OFF)
if (WOWDECOMP_DISABLE_SIGNATURE_CHECKS)
    add_compile_definitions(DISABLE_SIGNATURE_CHECKS=1)
endif ()

# Client-side improvements in the style of the common 3.3.5a executable patches: raised graphics limits, client bug
# fixes, interface conveniences and privacy defaults. Nothing that changes what the server sees or checks.
option(WRATHMODERN_CLIENT_PATCHES "Build with the WrathModern client improvements" ON)
if (WRATHMODERN_CLIENT_PATCHES)
    add_compile_definitions(WRATHMODERN_CLIENT_PATCHES=1)
endif ()

# Mods: dynamic libraries in the game's Mods folder that draw with Metal at hooks of the world render
# (WoW/Source/WorldClient/Mods/WMModAPI.h). Without a Mods folder nothing is loaded.
option(WRATHMODERN_MODS "Load mods from the game's Mods folder (macOS)" ON)
if (WRATHMODERN_MODS AND WOWDECOMP_PLATFORM_MAC)
    add_compile_definitions(WRATHMODERN_MODS=1)
endif ()

# macOS platform support, linked into every module so Mac/ implementations can use it.
if (WOWDECOMP_PLATFORM_MAC)
    add_subdirectory(Platform/Mac)
    link_libraries(MacPlatform)
endif ()

# Third-party code is linked, not decompiled, for now.
if (EXISTS ${CMAKE_SOURCE_DIR}/ThirdParty/CMakeLists.txt)
    add_subdirectory(ThirdParty)
endif ()

add_subdirectory(Engine/Source/BLPFile)
add_subdirectory(Engine/Source/Base)
add_subdirectory(Engine/Source/BlizzardCore)
add_subdirectory(Engine/Source/ComSat)
add_subdirectory(Engine/Source/ConsoleBase)
add_subdirectory(Engine/Source/Event)
add_subdirectory(Engine/Source/FFX)
add_subdirectory(Engine/Source/Frame)
add_subdirectory(Engine/Source/FrameScript)
add_subdirectory(Engine/Source/FrameXML)
add_subdirectory(Engine/Source/Gx)
add_subdirectory(Engine/Source/Gxu)
add_subdirectory(Engine/Source/Images)
add_subdirectory(Engine/Source/jpeg)
add_subdirectory(Engine/Source/lua-5.1)
add_subdirectory(Engine/Source/Model2)
add_subdirectory(Engine/Source/Os)
add_subdirectory(Engine/Source/pcre)
add_subdirectory(Engine/Source/Scrn)
add_subdirectory(Engine/Source/Services)
add_subdirectory(Engine/Source/ShaderEffect)
add_subdirectory(Engine/Source/SoundEngine)
add_subdirectory(Engine/Source/SteelseriesMouse)
add_subdirectory(Engine/Source/Storm)
add_subdirectory(Engine/Source/Tempest)
add_subdirectory(WoW/Common)
add_subdirectory(WoW/Source/Battlenet)
add_subdirectory(WoW/Source/Console)
add_subdirectory(WoW/Source/ConsoleDevice)
add_subdirectory(WoW/Source/DB)
add_subdirectory(WoW/Source/Game)
add_subdirectory(WoW/Source/Glue)
add_subdirectory(WoW/Source/Magic)
add_subdirectory(WoW/Source/LiquidSystem)
add_subdirectory(WoW/Source/Minigame)
add_subdirectory(WoW/Source/Net)
add_subdirectory(WoW/Source/Object)
add_subdirectory(WoW/Source/ObjectAlloc)
add_subdirectory(WoW/Source/ObjectMgrClient)
add_subdirectory(WoW/Source/SoundInterface)
add_subdirectory(WoW/Source/UIUtil)
add_subdirectory(WoW/Source/Ui)
add_subdirectory(WoW/Source/WorldClient)
add_subdirectory(WoW/Source/WowServices)
add_subdirectory(WoW/Source/WowSvcs)
add_subdirectory(WoW/Source)

if (WRATHMODERN_MODS AND WOWDECOMP_PLATFORM_MAC)
    add_subdirectory(Mods)
endif ()
if (WIN32)
    add_subdirectory(Tools/WowClient)
endif ()
if (WOWDECOMP_ARCH_64)
    add_subdirectory(Tools/XvidDecoder)
endif ()

if (WIN32)
foreach (target Wow WowClient MapViewer M2Viewer GxTest MpqTest BlpTest DbcTest AssetCheck)
    if (TARGET ${target})
        target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/waystone.rc)
    endif ()
endforeach ()
endif ()
