# CoAVolFog for WrathModern: Raz0r1337/coa-vfog (GPL-3.0, a fork of jealous-sound/coa-vfog) ported to the
# native Metal device as a mod. This directory is GPL-3.0 like the original (LICENSE); fog_data, water_data,
# water_types and log.h are the original files unchanged. Plan and slices: WOWRE.md, "Faz 5".
# The mod reads CoAVolFog.ini, fogdata.bin and waterdata.bin from the game's Mods folder, where the
# Windows install keeps them.
wrathmodern_add_mod(CoAVolFog
    Source/CoAVolFog.mm
    Source/config.cpp
    Source/log.cpp
    Source/fog_data.cpp
    Source/water_data.cpp
)
target_link_libraries(CoAVolFog PRIVATE "-framework Metal" "-framework Foundation")
target_compile_options(CoAVolFog PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
