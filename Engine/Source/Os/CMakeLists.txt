file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")
wowdecomp_platform_sources(PRIVATE_SOURCES)

add_library(Os STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Os
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if(WIN32)
    target_link_libraries(Os
        PUBLIC
            imm32
            wininet
            ws2_32
            dinput8
            dxguid
            version
    )
endif()

if (WOWDECOMP_PLATFORM_MAC)
    target_link_libraries(Os
        PUBLIC
            "-framework AppKit"
            "-framework Carbon"
            "-framework GameController"
            "-framework QuartzCore"
            "-framework Security"
    )
endif ()

# The Warden emulation (mac only) runs inside the Os module and links Unicorn.
if (WOWDECOMP_PLATFORM_MAC AND WRATHMODERN_WARDEN_EMULATION)
    target_include_directories(Os PUBLIC ${UNICORN_INCLUDE_DIR})
    target_link_libraries(Os PUBLIC ${UNICORN_LIBRARY})
endif ()
