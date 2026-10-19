# Diagnostic tools for the Warden emulation and the login version proof. macOS only: the rest of
# the repository builds these by hand (see the compile lines in the source headers).

if (WOWDECOMP_PLATFORM_MAC)
    foreach (probe WardenVersionProbe WardenSmoke BuckSmoke)
        if (probe STREQUAL WardenVersionProbe)
            set(sources version_probe.cpp)
        elseif (probe STREQUAL BuckSmoke)
            set(sources buck_smoke.cpp)
        else ()
            set(sources warden_smoke.cpp)
        endif ()

        add_executable(${probe} ${sources})

        set_target_properties(${probe} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Tools/Wardendump
        )

        target_include_directories(${probe}
            PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/Platform/Mac/Include /opt/homebrew/include
        )

        target_compile_definitions(${probe} PRIVATE
            WOWDECOMP_ARCH_64=1 WOWDECOMP_PLATFORM_MAC=1 WRATHMODERN_WARDEN_EMULATION=1
        )

        target_link_libraries(${probe}
            PRIVATE
                WowSvcs
                WowServices
                Glue
                Game
                WoWCommon
                Battlenet
                DB
                Console
                ConsoleDevice
                Net
                Ui
                UIUtil
                WorldClient
                ObjectMgrClient
                Object
                SoundInterface
                Minigame
                Magic
                LiquidSystem
                FrameXML
                FrameScript
                lua
                Frame
                Scrn
                Services
                ShaderEffect
                Tempest
                SoundEngine
                SteelseriesMouse
                Storm
                Os
                Gx
                Gxu
                Images
                Model2
                FFX
                Event
                ConsoleBase
                ComSat
                BlizzardCore
                Base
                BLPFile
                lzma
                blast
                zlib
                z
                /opt/homebrew/lib/libunicorn.dylib
                "-framework Foundation"
                "-framework CoreFoundation"
                "-framework IOKit"
                "-framework AppKit"
                "-framework Metal"
                "-framework CoreVideo"
        )
    endforeach ()
endif ()
