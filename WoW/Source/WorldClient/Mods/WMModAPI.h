/*
 * WrathModern mod interface.
 *
 * A mod is a dynamic library placed in the Mods folder of the game directory. The client loads every
 * *.dylib there once the graphics device exists and calls its WMModLoad entry point. Mods draw with Metal
 * into the frame the client is rendering at fixed points of the world render, add Lua functions, events
 * and console variables to the interface, and are told when the interface scripts are (re)created.
 *
 * This header describes only the interface; it holds no client code, so a mod under any licence can
 * include it. Every pointer the host hands out stays owned by the host.
 *
 * Lua: the client's Lua 5.1 C API (lua_*, luaL_*) is exported by the client executable. A mod that adds
 * Lua functions includes Lua 5.1's lua.h/lauxlib.h and links with "-undefined dynamic_lookup", so its calls
 * reach the client's interpreter. Never link a second copy of Lua into a mod.
 *
 * Compatibility: WMModHost.size is the size of the structure the client filled. A member exists when
 * WM_HOST_HAS(host, member) is true, so a mod built against a newer header can run on an older client.
 */

#ifndef WRATHMODERN_MOD_API_H
#define WRATHMODERN_MOD_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WM_MOD_API_VERSION 2
#define WM_MOD_ENTRY_POINT "WMModLoad"

#define WM_HOST_HAS(host, member) ((host)->size >= offsetof(WMModHost, member) + sizeof((host)->member))

struct lua_State;

/* Points of the world render a mod is called at, in the order they occur within a frame */
typedef enum WMHook {
    /* Terrain, map objects and opaque models are drawn: the depth buffer holds the opaque scene */
    WM_HOOK_AFTER_OPAQUE = 0,
    /* Detail doodads are drawn; barriers, weather, liquids and transparent models follow */
    WM_HOOK_BEFORE_LIQUIDS = 1,
    /* Transparent geometry, spell effects, particles and weather are drawn; sun and moon glare follow */
    WM_HOOK_BEFORE_GLARES = 2,
    /* The world is complete; the client's screen effects (glow) follow, then the interface */
    WM_HOOK_BEFORE_SCREEN_EFFECTS = 3,
    /*
     * Around the whole world render (CGWorldFrame::OnWorldRender), after the day/night state of the frame is
     * set: a mod may change it for the render (the stock fog range) and put it back at the end. No frame
     * targets are acquired at these two; GetWorldState is not captured yet at the first.
     */
    WM_HOOK_WORLD_BEGIN = 4,
    WM_HOOK_WORLD_END = 5,
    /*
     * Around the liquid surfaces of the world (CWorldScene::RenderLiquidPass, Liquid::CRenderer::Flush): every
     * liquid draw between them is announced through OnLiquidDraw. Frame targets may be acquired at both.
     */
    WM_HOOK_LIQUIDS_BEGIN = 6,
    WM_HOOK_LIQUIDS_END = 7,
    WM_HOOK_COUNT
} WMHook;

/* The interface script environments. The client destroys and recreates its Lua state when it moves
 * between the login screens (glue) and the game, and on /reload. */
typedef enum WMScriptContext {
    WM_SCRIPT_GLUE = 1,
    WM_SCRIPT_GAME = 2,
    WM_SCRIPT_ALL = 3
} WMScriptContext;

/*
 * The world as the client renders it this frame. Matrices are the client's: row-major, transforming row
 * vectors (v * M), with a Direct3D projection mapping view depth to [0, 1]. The view matrix carries the
 * camera's translation; world coordinates are the client's (yards, Z up). Colours are the client's 8-bit
 * values scaled to 0-1, without gamma conversion.
 */
typedef struct WMWorldState {
    float view[16];
    float projection[16];
    float cameraPosition[3];
    float cameraTarget[3];
    float nearClip;
    float farClip;
    int32_t mapId;
    int32_t cameraInLiquid;

    /* Day and night: 0 at midnight, 0.5 at noon */
    float dayFraction;
    /* Unit vectors from the sky centre toward the sun and the moon */
    float sunDirection[3];
    float moonDirection[3];
    /* Whether the moon is the light that is up (the client switches at its dawn and dusk thresholds) */
    int32_t moonIsLight;

    float ambientColor[3];
    float directColor[3];
    float sunColor[3];
    float fogColor[3];
    /* The zone's fog distance and the blended fog range in effect */
    float zoneFogEnd;
    float fogStart;
    float fogEnd;
    /* 0 in clear weather, toward 1 in storms */
    float stormBlend;
    /* 0 outdoors, toward 1 inside a map object interior */
    float interiorFogBlend;

    /* Seconds since the client started, and since the previous world frame */
    double time;
    float elapsed;

    /*
     * The world viewport's depth range as the opaque pass leaves it: the depth buffer holds
     * viewportMinZ + d (viewportMaxZ - viewportMinZ) for the projection's depth d. The sky and the distant
     * terrain draw beyond viewportMaxZ. Added after version 2's first release; read it only from a mod
     * built with the client.
     */
    float viewportMinZ;
    float viewportMaxZ;
} WMWorldState;

/* The frame a hook draws into. Every object is an unretained Objective-C pointer of the type noted. */
typedef struct WMFrameTargets {
    void* commandBuffer; /* id<MTLCommandBuffer>: encode passes on it, never commit it */
    void* color;         /* id<MTLTexture>: the world's colour target */
    void* depth;         /* id<MTLTexture>: its depth target, or NULL */
    uint32_t width;
    uint32_t height;
    uint32_t sampleCount; /* above 1 when the targets are multisampled */
} WMFrameTargets;

/*
 * The orbit target of the third-person camera, as CGCamera::CalcTargetCamera computes it each frame from
 * the followed object's safe world position, before the flight bounds and the camera eye are derived from
 * it. Shifting targetPosition shifts the whole orbit (camera collision follows the shifted target).
 */
typedef struct WMCameraTarget {
    /* The camera eye of the previous frame */
    float cameraPosition[3];
    /* The orbit target; a mod may change it */
    float targetPosition[3];
    /* The active player's position and facing (radians), when playerValid */
    float playerPosition[3];
    float playerFacing;
    int32_t playerValid;
} WMCameraTarget;

/* A Lua function a mod adds to the interface; the client's lua_CFunction */
typedef int (*WMLuaFunction)(struct lua_State* L);

/* Called when a console variable a mod registered changes. Return non-zero to accept the value. */
typedef int32_t (*WMCVarChanged)(const char* name, const char* oldValue, const char* newValue, void* user);

typedef struct WMModHost {
    uint32_t version;
    /* id<MTLDevice> the client renders with */
    void* device;
    /* The game directory, and the mod's own folder (Mods/<library name without .dylib>), UTF-8 */
    const char* gameDirectory;
    const char* modDirectory;

    /* Writes a line to Logs/Mods.log */
    void (*Log)(const char* message);
    /* The world state of the frame being rendered; valid during a hook */
    const WMWorldState* (*GetWorldState)(void);
    /*
     * Ends the client's open encoders and fills targets with the frame's command buffer and render
     * targets, for the mod to encode its own passes. Valid until the hook returns; the client resumes
     * drawing into the same targets afterwards. Returns 0 when no frame is being rendered.
     */
    int32_t (*AcquireFrameTargets)(WMFrameTargets* targets);

    /* Version 2 -------------------------------------------------------------------------------------- */

    /* Size of this structure as filled by the client; see WM_HOST_HAS */
    uint32_t size;

    /*
     * Adds a global Lua function to the given contexts (WMScriptContext bits). The client registers it
     * again every time it creates a Lua state, after its own functions, so a mod registers once, usually
     * from WMModLoad. A name the client already uses is replaced in the contexts given.
     */
    void (*RegisterLuaFunction)(const char* name, WMLuaFunction function, uint32_t contexts);
    /*
     * Adds an interface event (as with Frame:RegisterEvent) to the given contexts and returns its id for
     * SignalEvent, or -1. Register events from WMModLoad; ids stay valid across Lua state changes.
     */
    int32_t (*RegisterEvent)(const char* name, uint32_t contexts);
    /*
     * Raises an event in the current context; format describes the arguments as FrameScript_SignalEvent
     * does (for example %s string, %d integer, %f number). Does nothing when the event is not part of the
     * current context.
     */
    void (*SignalEvent)(int32_t event, const char* format, ...);
    /* Runs Lua source in the current context; chunkName appears in errors */
    void (*ExecuteLua)(const char* source, const char* chunkName);
    /* The current Lua state, or NULL between contexts */
    struct lua_State* (*GetLuaState)(void);
    /* The context the current Lua state belongs to (WM_SCRIPT_GLUE or WM_SCRIPT_GAME), or 0 */
    uint32_t (*GetScriptContext)(void);

    /*
     * Registers a console variable (saved to Config.wtf like the client's own) and returns non-zero on
     * success. changed may be NULL. A variable registered earlier keeps its saved value.
     */
    int32_t (*RegisterCVar)(const char* name, const char* help, const char* defaultValue, WMCVarChanged changed, void* user);
    /* The value of any console variable, or NULL when it does not exist; valid until it changes */
    const char* (*GetCVar)(const char* name);
    /* Sets any console variable as the player would; returns non-zero when it exists */
    int32_t (*SetCVar)(const char* name, const char* value);
} WMModHost;

typedef struct WMModInfo {
    /* Set to WM_MOD_API_VERSION */
    uint32_t version;
    const char* name;
    void* user;
    /* Called at every hook while the world is rendered; may be NULL */
    void (*OnHook)(WMHook hook, void* user);
    /* Called once before the client exits; may be NULL */
    void (*Shutdown)(void* user);

    /* Version 2 -------------------------------------------------------------------------------------- */

    /*
     * Called after the client created a Lua state for context and registered its functions and the mods'
     * (before any interface file runs), and again with loaded set once the interface files and addons
     * have loaded. May be NULL.
     */
    void (*OnScriptContext)(uint32_t context, int32_t loaded, void* user);
    /* Called each time the camera computes its orbit target (see WMCameraTarget); may be NULL */
    void (*OnCameraTarget)(WMCameraTarget* camera, void* user);
    /*
     * Called as each camera (CSimpleCamera: the world camera and those of model frames) is created, with its
     * vertical field of view in radians, which the mod may change. May be NULL.
     */
    void (*OnCameraCreated)(float* fieldOfView, void* user);
    /* Called as the game enters (entered = 1) and leaves (0) the world, before the client's own handling */
    void (*OnWorld)(int32_t entered, void* user);
    /* Called once per frame while the world is rendered; elapsed is the frame time in seconds */
    void (*OnFrame)(float elapsed, void* user);
    /*
     * Called before (begin = 1) and after (begin = 0) each liquid surface draw, with its Liquid::CMaterialSettings;
     * a mod may tag the draw's pixels (GxMetalSetStencilTag) between the two. Added after version 2's first
     * release; read only by clients that know it. May be NULL.
     */
    void (*OnLiquidDraw)(const void* settings, int32_t begin, void* user);
} WMModInfo;

/*
 * The entry point a mod exports under WM_MOD_ENTRY_POINT. It fills info and returns non-zero to stay
 * loaded. host stays valid for the life of the process. A version 1 mod (WMModInfo without
 * OnScriptContext) still loads: the client reads only the members of the version the mod declares.
 */
typedef int32_t (*WMModLoadFunction)(const WMModHost* host, WMModInfo* info);

#ifdef __cplusplus
}
#endif

#endif
