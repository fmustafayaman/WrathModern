if (WOWDECOMP_PLATFORM_MAC)
    # Linked into the client: the movie decoder has no separate module on macOS. xvidcore comes from
    # Homebrew (brew install xvid) and is linked statically, so the client does not depend on it at
    # runtime.
    find_path(XVID_INCLUDE_DIR xvid.h HINTS /opt/homebrew/opt/xvid/include /usr/local/opt/xvid/include)
    find_library(XVID_STATIC_LIBRARY libxvidcore.a HINTS /opt/homebrew/opt/xvid/lib /usr/local/opt/xvid/lib)

    if (NOT XVID_INCLUDE_DIR OR NOT XVID_STATIC_LIBRARY)
        message(FATAL_ERROR "xvidcore not found. Install it with: brew install xvid")
    endif ()

    add_library(XvidDecoder STATIC
        XvidDecoder.cpp
    )

    target_include_directories(XvidDecoder
        PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${XVID_INCLUDE_DIR}
    )

    target_link_libraries(XvidDecoder
        PRIVATE
            ${XVID_STATIC_LIBRARY}
    )

    return ()
endif ()

add_library(XvidDecoder SHARED
    XvidDecoder.cpp
    XvidDecoder.def
)

set_target_properties(XvidDecoder PROPERTIES
    OUTPUT_NAME XvidDecoder
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/Win
)

foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set_target_properties(XvidDecoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_SOURCE_DIR}/bin/Win)
endforeach ()

target_include_directories(XvidDecoder
    PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/ThirdParty/xvidcore/include
)

target_link_libraries(XvidDecoder
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ThirdParty/xvidcore/lib/x64/libxvidcore.lib
)

target_compile_options(XvidDecoder
    PRIVATE
        $<$<CONFIG:Debug>:/MD>
)
