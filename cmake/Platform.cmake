# Platform selection for source globs.
#
# The source tree keeps platform code in per-platform directories, the way the original client did:
# Win32/ and Win64/ for Windows, Mac/ for macOS. Shared code lives next to them. Modules glob their
# sources recursively and pass the list through wowdecomp_platform_sources() to drop the directories
# that belong to other platforms.

if (APPLE)
    set(WOWDECOMP_PLATFORM_MAC 1)
    set(WOWDECOMP_BIN_DIR ${CMAKE_SOURCE_DIR}/bin/Mac)
elseif (WIN32)
    set(WOWDECOMP_PLATFORM_WIN 1)
    set(WOWDECOMP_BIN_DIR ${CMAKE_SOURCE_DIR}/bin/Win)
endif ()

# Win64/ files that are portable logic over Win32 kernel objects and Winsock. The macOS build compiles them
# unchanged against the curated <windows.h> and <winsock2.h> in Platform/Mac/Include instead of keeping a second copy;
# everything else in Win32/ and Win64/ has a macOS counterpart in a Mac/ directory.
set(WOWDECOMP_MAC_SHARED_WIN64
    "/Packages/Streaming/Win64/[^/]+\\.cpp$"
    "/System/File/Win64/Filestack_Streaming\\.cpp$"
    "/Storm/Source/Win64/SCritSect\\.cpp$"
    "/Storm/Source/Win64/SLock\\.cpp$"
    "/Os/Win64/OsFile\\.cpp$"
    "/WowServices/Win64/WowConnection\\.cpp$"
    "/WowServices/Win64/BattlenetLogin\\.cpp$"
    "/GameClient/Win64/WardenClient\\.cpp$"
)

function(wowdecomp_platform_sources var)
    set(sources ${${var}})

    if (WOWDECOMP_PLATFORM_MAC)
        set(shared "")

        foreach (pattern ${WOWDECOMP_MAC_SHARED_WIN64})
            set(matches ${sources})
            list(FILTER matches INCLUDE REGEX "${pattern}")
            list(APPEND shared ${matches})
        endforeach ()

        list(FILTER sources EXCLUDE REGEX "/Win(32|64)/")
        list(APPEND sources ${shared})

        # Objective-C++ sources of the Mac/ directories and the Metal graphics device; modules glob
        # only *.cpp.
        file(GLOB_RECURSE objcxx CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.mm")
        list(FILTER objcxx INCLUDE REGEX "/(Mac|CGxDeviceMetal)/")
        list(APPEND sources ${objcxx})

        # Shared Win64 files get <windows.h> the way the Windows build gives it to them, through
        # headers that include it only on Windows.
        if (shared)
            set_source_files_properties(${shared} PROPERTIES COMPILE_OPTIONS "-include;windows.h")
        endif ()
        # Logitech G-series keyboard LCD support exists only on Windows.
        list(FILTER sources EXCLUDE REGEX "/UILCD/(Windows/|LCD\\.cpp$)")
        list(FILTER sources EXCLUDE REGEX "/CGxDevice(D3d|D3d9Ex|OpenGL)/")
    else ()
        list(FILTER sources EXCLUDE REGEX "/Mac/")
        list(FILTER sources EXCLUDE REGEX "/CGxDeviceMetal/")
    endif ()

    set(${var} ${sources} PARENT_SCOPE)
endfunction()
