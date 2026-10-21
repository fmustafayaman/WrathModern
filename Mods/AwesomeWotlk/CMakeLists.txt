# Awesome WotLK for WrathModern: noname08662/awesome_wotlk (GPL-3.0, a fork of FrostAtom/awesome_wotlk)
# ported as a mod. This directory is GPL-3.0 like the original; see Docs/Mods/AwesomeWotlk.md.
wrathmodern_add_mod(AwesomeWotlk
    Entry.cpp
    CVars.cpp
    UnitAPI.cpp
    Camera.cpp
    Window.mm
)
target_link_libraries(AwesomeWotlk PRIVATE "-framework AppKit")
target_compile_options(AwesomeWotlk PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
