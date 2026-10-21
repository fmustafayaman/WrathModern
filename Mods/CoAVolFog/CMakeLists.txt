# CoAVolFog for WrathModern: Raz0r1337/coa-vfog (GPL-3.0, a fork of jealous-sound/coa-vfog) ported to the
# native Metal device as a mod. This directory is GPL-3.0 like the original (LICENSE); fog_data, water_data,
# water_types, fog_model, engine_lights.h, engine_actors.h and log.h are the original files unchanged.
# Plan and slices: WOWRE.md, "Faz 5". The mod reads CoAVolFog.ini, fogdata.bin and waterdata.bin from the
# game's Mods folder, where the Windows install keeps them.

# The Metal shaders are compiled at run time (per variant, with preprocessor macros) from source embedded
# in the library; editing a .metal file configures again
set(COAVOLFOG_SHADER ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/vf_fog.metal)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${COAVOLFOG_SHADER})
file(READ ${COAVOLFOG_SHADER} COAVOLFOG_SHADER_TEXT)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/vf_fog_source.h
    "#pragma once\n// Generated from Shaders/vf_fog.metal\nstatic const char kVfFogSource[] = R\"MSL(${COAVOLFOG_SHADER_TEXT})MSL\";\n")

set(COAVOLFOG_PANEL ${CMAKE_CURRENT_SOURCE_DIR}/Source/panel.lua)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${COAVOLFOG_PANEL})
file(READ ${COAVOLFOG_PANEL} COAVOLFOG_PANEL_TEXT)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/panel_source.h
    "#pragma once\n// Generated from Source/panel.lua\nstatic const char kPanelSource[] = R\"LUA(${COAVOLFOG_PANEL_TEXT})LUA\";\n")

wrathmodern_add_mod(CoAVolFog
    Source/CoAVolFog.mm
    Source/config.cpp
    Source/log.cpp
    Source/fog_data.cpp
    Source/water_data.cpp
    Source/fog_model.cpp
    Source/noise_volume.cpp
    Source/engine_lights.cpp
    Source/local_light_stabiliser.cpp
    Source/engine_mac.mm
    Source/renderer_metal.mm
)
target_include_directories(CoAVolFog PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(CoAVolFog PRIVATE "-framework Metal" "-framework Foundation")
target_compile_options(CoAVolFog PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
