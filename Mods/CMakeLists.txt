# Mods built with the client. They include the client's headers (the engine SDK) and call client
# functions by name: the executable exports its symbols, and a mod links with -undefined dynamic_lookup
# so every call reaches the running client. A mod therefore has to be rebuilt with the client it runs in.
# With WRATHMODERN_GAME_DIR set, each mod is copied into the game directory's Mods folder after linking.

function(wrathmodern_add_mod name)
    add_library(${name} MODULE ${ARGN})
    set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".dylib")
    # Never link client libraries into a mod: their code would be a second copy with its own globals
    set_property(TARGET ${name} PROPERTY LINK_LIBRARIES "")
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/WoW/Source/WorldClient/Mods
        ${CMAKE_SOURCE_DIR}/Engine/Source/lua-5.1/src)
    target_compile_options(${name} PRIVATE -fvisibility=hidden)
    target_link_options(${name} PRIVATE "-undefined" "dynamic_lookup")

    if (WRATHMODERN_GAME_DIR)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${name}>" "${WRATHMODERN_GAME_DIR}/Mods/"
            COMMENT "Copying ${name} into ${WRATHMODERN_GAME_DIR}/Mods"
            VERBATIM)
    endif ()
endfunction()

option(WRATHMODERN_BUILD_MODS "Build the mods in Mods/ with the client" ON)

if (WRATHMODERN_BUILD_MODS)
    # Mods/LuxShoulderCam also builds on its own; it needs only the mod interface and Lua headers
    wrathmodern_add_mod(LuxShoulderCam LuxShoulderCam/LuxShoulderCam.cpp)
    add_subdirectory(AwesomeWotlk)
endif ()
