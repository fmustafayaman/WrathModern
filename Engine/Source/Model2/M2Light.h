#ifndef ENGINE_SOURCE_MODEL2_M2LIGHT_H
#define ENGINE_SOURCE_MODEL2_M2LIGHT_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C4Plane.h"
#include "Engine/Source/Tempest/CAaSphere.h"
#include "Engine/Source/Model2/Types.h"

class CM2Light;
class CM2Scene;

class CM2Light {
    public:
    /* 0000 */ CM2Scene* m_scene;
    /* 0004 */ uint32_t m_stamp;
    /* 0008 */ M2LIGHTTYPE m_type;
    /* 000C */ C3Vector m_pos;
    /* 0018 */ C3Vector m_viewPos;
    /* 0024 */ C3Vector m_dir;
    /* 0030 */ C3Vector m_ambColor;
    /* 003C */ C3Vector m_dirColor;
    /* 0048 */ C3Vector m_specColor;
    /* 0054 */ float m_constantAttenuation;
    /* 0058 */ float m_linearAttenuation;
    /* 005C */ float m_quadraticAttenuation;
    /* 0060 */ uint32_t m_visible;
    /* 0064 */ CM2Light** m_lightPrev;
    /* 0068 */ CM2Light* m_lightNext;

    CM2Light();
    ~CM2Light();
    void GxLightSet(uint32_t index);
    void Initialize(CM2Scene* scene);
    void Link();
    void SetDirection(const C3Vector& dir);
    void SetLightType(M2LIGHTTYPE type);
    void SetPosition(const C3Vector& pos);
    void SetVisible(int32_t visible);
    void Unlink();
};

class CM2Lighting {
    public:
    /* 0000 */ CM2Scene* m_scene;
    /* 0004 */ CAaSphere m_sphere4;
    /* 0014 */ uint32_t m_flags;
    /* 0018 */ C3Vector m_vector18;
    /* 0024 */ C3Vector m_vector24;
    /* 0030 */ C3Vector m_vector30;
    /* 003C */ C3Vector m_vector3C;
    /* 0048 */ C3Vector m_vector48;
    /* 0054 */ C3Vector m_sunAmbient;
    /* 0060 */ C3Vector m_sunDiffuse;
    /* 006C */ C3Vector m_sunSpecular;
    /* 0078 */ C3Vector m_sunDir;
    /* 0084 */ CM2Light* m_lights [4];
    /* 0094 */ float m_lightDist [4];
    /* 00A4 */ uint32_t m_lightCount;
    /* 00A8 */ float m_fogStart;
    /* 00AC */ float m_fogEnd;
    /* 00B0 */ float m_fogScale;
    /* 00B4 */ float m_fogDensity;
    /* 00B8 */ C3Vector m_fogColor;
    /* 00C4 */ C4Plane m_liquidPlane;
#if defined(WRATHMODERN_CLIENT_PATCHES)
    // St0ny #67 "lights": the fifth to eighth nearest point lights, nearest first. The four nearest stay in m_lights,
    // where the shader permutations (m_lightCount) and the original vertex constants c17-c27 expect them; these go to
    // c236-c246 for models and buildings and to c239-c250 for the ground.
    /* 00D4 */ CM2Light* m_extraLights [4];
    /* 00E4 */ float m_extraLightDist [4];
    /* 00F4 */ uint32_t m_extraLightCount = 0;
#endif

    CM2Lighting();
    CM2Lighting(const CAaSphere& sphere);
    CM2Lighting(const C3Vector& position);
    void Reset();
    void AddAmbient(const C3Vector& color);
    void AddDiffuse(const C3Vector& color, const C3Vector& dir);
    void AddLight(CM2Light* light);
#if defined(WRATHMODERN_CLIENT_PATCHES)
    void AddExtraLight(CM2Light* light, float dist);
#endif
    void CameraSpace();
    int32_t GetPointLight(uint32_t index, C3Vector& pos, C3Vector& color, C3Vector& attenuation);
    int32_t GetSunLight(C3Vector& dir, C3Vector& ambient, C3Vector& diffuse, C3Vector& specular);
    void Initialize(CM2Scene* scene, const CAaSphere& sphere);
    void SetFog(const C3Vector& color, float start, float end);
    void SetFog(const C3Vector& color, float start, float end, float density);
    void SetupGxFog();
    void SetupGxLights(const C3Vector* origin);
    void SetupSunlight();
};

#endif
