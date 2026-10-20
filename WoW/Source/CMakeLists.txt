file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (WOWDECOMP_PLATFORM_MAC)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Mac/*.cpp" "Mac/*.mm")
elseif (WOWDECOMP_ARCH_64)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Win64/*.cpp")
else ()
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Win32/*.cpp")
endif ()

if (WOWDECOMP_PLATFORM_MAC)
    add_executable(Wow MACOSX_BUNDLE ${PRIVATE_SOURCES} ${PLATFORM_SOURCES})
else ()
    add_executable(Wow WIN32 ${PRIVATE_SOURCES} ${PLATFORM_SOURCES})
endif ()

if (WOWDECOMP_PLATFORM_MAC)
    set(WOW_OUTPUT_NAME WrathModern)
elseif (WOWDECOMP_ARCH_64)
    set(WOW_OUTPUT_NAME Wow64)
else ()
    set(WOW_OUTPUT_NAME Wow32)
endif ()

set_target_properties(Wow PROPERTIES
    OUTPUT_NAME ${WOW_OUTPUT_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${WOWDECOMP_BIN_DIR}
)

if (WOWDECOMP_PLATFORM_MAC)
    set_target_properties(Wow PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Mac/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME WrathModern
        MACOSX_BUNDLE_GUI_IDENTIFIER org.wrathmodern.client
        MACOSX_BUNDLE_SHORT_VERSION_STRING 3.3.5
        MACOSX_BUNDLE_BUNDLE_VERSION 12340
    )

    # The client reads its data from the directory holding the application, so development builds
    # can be copied into a game directory: -DWRATHMODERN_GAME_DIR=/path/to/World of Warcraft
    set(WRATHMODERN_GAME_DIR "" CACHE PATH "Game directory to copy the application into after each build")
    # Name of the copy in the game directory, so builds of different branches can sit side by side
    set(WRATHMODERN_APP_NAME "WrathModern" CACHE STRING "Name of the application copied into the game directory, without .app")

    # A stable signature keeps macOS's grants (access to the game volume) across rebuilds
    add_custom_command(TARGET Wow POST_BUILD
        COMMAND ${CMAKE_SOURCE_DIR}/Tools/sign-app.sh "$<TARGET_BUNDLE_DIR:Wow>"
        VERBATIM
    )

    if (WRATHMODERN_GAME_DIR)
        add_custom_command(TARGET Wow POST_BUILD
            COMMAND rsync -a --delete "$<TARGET_BUNDLE_DIR:Wow>/" "${WRATHMODERN_GAME_DIR}/${WRATHMODERN_APP_NAME}.app/"
            COMMAND ${CMAKE_SOURCE_DIR}/Tools/sign-app.sh "${WRATHMODERN_GAME_DIR}/${WRATHMODERN_APP_NAME}.app"
            COMMENT "Copying the application into ${WRATHMODERN_GAME_DIR}/${WRATHMODERN_APP_NAME}.app"
            VERBATIM
        )
    endif ()
endif ()

foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set_target_properties(Wow PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${WOWDECOMP_BIN_DIR})
endforeach ()

target_include_directories(Wow
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

if (MSVC)
    target_link_options(Wow PRIVATE "/INCREMENTAL:NO" "/ENTRY:start")
endif ()

target_link_libraries(Wow
    PRIVATE
        BLPFile
        Base
        BlizzardCore
        ComSat
        ConsoleBase
        Event
        FFX
        Frame
        FrameScript
        lua
        FrameXML
        Gx
        Gxu
        Images
        Model2
        Os
        Scrn
        Services
        ShaderEffect
        Tempest
        SoundEngine
        SteelseriesMouse
        Storm
        WoWCommon
        Battlenet
        Console
        ConsoleDevice
        DB
        Game
        Glue
        LiquidSystem
        Magic
        Minigame
        Net
        Object
        ObjectAlloc
        ObjectMgrClient
        SoundInterface
        UIUtil
        Ui
        WorldClient
        WowServices
        WowSvcs
)
