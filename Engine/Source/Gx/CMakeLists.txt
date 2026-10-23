file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
wowdecomp_platform_sources(PRIVATE_SOURCES)

add_library(Gx STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Gx
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if (WOWDECOMP_PLATFORM_MAC)
    target_link_libraries(Gx
        PRIVATE
            "-framework Metal"
            "-framework MetalFX"
            "-framework QuartzCore"
            "-framework AppKit"
            "-framework CoreGraphics"
    )
endif ()

if (WIN32)
    target_link_libraries(Gx
        PRIVATE
            d3d9.lib
            nvapi
            opengl32.lib
    )
endif ()
