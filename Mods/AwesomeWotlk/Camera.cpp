// Camera options of awesome_wotlk (Camera.cpp): cameraFov and showPlayer. Indirect visibility (fading the
// models between the camera and the player) follows with the camera collision hooks.

#include <cmath>
#include <cstring>
#include "Mods/AwesomeWotlk/AwesomeWotlk.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"

namespace awesome::camera {

namespace {

// cameraFov is in the original's units: 100 is the client's default of pi / 2
float FieldOfView(float cameraFov) {
    return static_cast<float>(M_PI / 200.0 * cameraFov);
}

void ApplyShowPlayer(int show) {
    if (CGPlayer_C* player = ClntObjMgrGetActivePlayerObj()) {
        player->m_scale = show == 0 ? 0.0f : 1.0f;
    }
}

}

void OnCameraCreated(float* fieldOfView, void* user) {
    *fieldOfView = FieldOfView(cvars::Float("cameraFov"));
}

void OnWorld(int32_t entered, void* user) {
    ApplyShowPlayer(cvars::Int("showPlayer"));
}

void OnCVarChanged(const char* name, float value) {
    if (!strcmp(name, "cameraFov")) {
        if (CGCamera* camera = CGWorldFrame::GetActiveCamera()) {
            camera->m_fov = FieldOfView(value);
        }
    } else if (!strcmp(name, "showPlayer")) {
        ApplyShowPlayer(static_cast<int>(value));
    }
}

}
