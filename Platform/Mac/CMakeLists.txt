# macOS platform support shared by the Mac/ implementations of the engine modules.

file(GLOB MACPLATFORM_SOURCES CONFIGURE_DEPENDS "*.cpp" "*.mm")

add_library(MacPlatform STATIC
    ${MACPLATFORM_SOURCES}
)

target_include_directories(MacPlatform
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(MacPlatform
    PUBLIC
        "-framework CoreFoundation"
        "-framework Foundation"
)
