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

function(wowdecomp_platform_sources var)
    set(sources ${${var}})

    if (WOWDECOMP_PLATFORM_MAC)
        list(FILTER sources EXCLUDE REGEX "/Win(32|64)/")
        # Logitech G-series keyboard LCD support exists only on Windows.
        list(FILTER sources EXCLUDE REGEX "/UILCD/(Windows/|LCD\\.cpp$)")
        list(FILTER sources EXCLUDE REGEX "/CGxDevice(D3d|D3d9Ex|OpenGL)/")
    else ()
        list(FILTER sources EXCLUDE REGEX "/Mac/")
        list(FILTER sources EXCLUDE REGEX "/CGxDeviceMetal/")
    endif ()

    set(${var} ${sources} PARENT_SCOPE)
endfunction()
