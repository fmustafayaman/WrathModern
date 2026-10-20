/*
 * WrathModern mod interface.
 *
 * A mod is a dynamic library placed in the Mods folder next to the application. The client loads every
 * *.dylib there once the graphics device exists and calls its WMModLoad entry point. Mods draw with Metal
 * into the frame the client is rendering, at fixed points of the world render.
 *
 * This header describes only the interface; it holds no client code, so a mod under any licence can
 * include it. Every pointer the host hands out stays owned by the host.
 */

#ifndef WRATHMODERN_MOD_API_H
#define WRATHMODERN_MOD_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WM_MOD_API_VERSION 1
#define WM_MOD_ENTRY_POINT "WMModLoad"

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
    WM_HOOK_COUNT
} WMHook;

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
} WMWorldState;

/* The frame a hook draws into. Every object is an unretained Objective-C pointer of the type noted. */
typedef struct WMFrameTargets {
    void* commandBuffer; /* id<MTLCommandBuffer>: encode passes on it, never commit it */
    void* color;         /* id<MTLTexture>: the world's colour target */
    void* depth;         /* id<MTLTexture>: its depth target (Depth32Float), or NULL */
    uint32_t width;
    uint32_t height;
    uint32_t sampleCount; /* above 1 when the targets are multisampled */
} WMFrameTargets;

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
} WMModInfo;

/*
 * The entry point a mod exports under WM_MOD_ENTRY_POINT. It fills info and returns non-zero to stay
 * loaded. host stays valid for the life of the process.
 */
typedef int32_t (*WMModLoadFunction)(const WMModHost* host, WMModInfo* info);

#ifdef __cplusplus
}
#endif

#endif
