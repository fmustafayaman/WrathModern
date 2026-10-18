#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDeviceMetal/D3D9/Device.h"

// The adapter queries of CGxDevice for macOS: the Metal device and the main display stand for the
// Direct3D adapter and the Windows display device.

static const D3D9Metal::D3DFORMAT s_adapterDepthFormats[4] = { D3D9Metal::D3DFMT_D16, D3D9Metal::D3DFMT_D24X8, D3D9Metal::D3DFMT_D24S8, D3D9Metal::D3DFMT_D32 };
static const CGxFormat::Format s_gxDepthFormats[4] = { CGxFormat::Fmt_Ds160, CGxFormat::Fmt_Ds24X, CGxFormat::Fmt_Ds248, CGxFormat::Fmt_Ds320 };
static const uint32_t s_multisampleCounts[4] = { 1, 2, 4, 8 };

int32_t CGxDevice::AdapterFormats(EGxApi api, TSGrowableArray<CGxFormat>& formats) {
    formats.SetCount(0);
    formats.m_chunk = 64;
    formats.Reserve(256, 0);

    if (api != GxApi_Metal) {
        return 0;
    }

    D3D9Metal::Direct3D* d3d = D3D9Metal::Direct3D::Create();
    if (!d3d) {
        return 0;
    }

    static const CGxFormat::Format s_colorFormats[2] = { CGxFormat::Fmt_ArgbX888, CGxFormat::Fmt_Argb8888 };

    for (CGxFormat::Format colorFormat : s_colorFormats) {
        for (uint32_t depth = 0; depth < 4; depth++) {
            for (uint32_t samples : s_multisampleCounts) {
                D3D9Metal::D3DMULTISAMPLE_TYPE type = samples > 1 ? static_cast<D3D9Metal::D3DMULTISAMPLE_TYPE>(samples) : D3D9Metal::D3DMULTISAMPLE_NONE;

                if (D3D9Metal::Succeeded(d3d->CheckDeviceMultiSampleType(0, D3D9Metal::D3DFMT_X8R8G8B8, 0, type, nullptr)) && D3D9Metal::Succeeded(d3d->CheckDeviceMultiSampleType(0, s_adapterDepthFormats[depth], 0, type, nullptr))) {
                    CGxFormat format;
                    memset(&format, 0, sizeof(format));
                    format.m_colorFormat = colorFormat;
                    format.m_depthFormat = s_gxDepthFormats[depth];
                    format.m_multisampleCount = samples;

                    if (!CGxDevice::FormatExists(formats, format)) {
                        *formats.New() = format;
                    }
                }
            }
        }
    }

    d3d->Release();
    return formats.Count() != 0;
}

int32_t CGxDevice::DeviceAdapterID(uint16_t& vendorID, uint16_t& deviceID, uint32_t& driverVersionHi, uint32_t& driverVersionLo) {
    vendorID = 0xFFFF;
    deviceID = 0xFFFF;
    driverVersionHi = 0;
    driverVersionLo = 0;

    int32_t result = 0;
    D3D9Metal::Direct3D* d3d = D3D9Metal::Direct3D::Create();

    if (d3d) {
        D3D9Metal::AdapterIdentifier identifier;
        if (D3D9Metal::Succeeded(d3d->GetAdapterIdentifier(0, 0, &identifier))) {
            vendorID = static_cast<uint16_t>(identifier.VendorId);
            deviceID = static_cast<uint16_t>(identifier.DeviceId);
            driverVersionHi = identifier.DriverVersionHigh;
            driverVersionLo = identifier.DriverVersionLow;
            result = 1;
        }
        d3d->Release();
    }

    CGxDevice::Log("CGxDevice::DeviceAdapterID(): RET: %d, VID: %x, DID: %x, DVER: %x.%x", result, vendorID, deviceID, driverVersionHi, driverVersionLo);

    return result;
}

int32_t CGxDevice::DeviceAdapterInfer(uint16_t& deviceID) {
    deviceID = 0;

    D3D9Metal::Direct3D* d3d = D3D9Metal::Direct3D::Create();
    if (!d3d) {
        return 0;
    }

    // The same classification as the Windows version: hardware transform and lighting, more than
    // two texture units and pixel shader 2.0
    D3D9Metal::D3DCAPS9 caps;
    int32_t result = 0;

    if (D3D9Metal::Succeeded(d3d->GetDeviceCaps(0, &caps))) {
        if ((caps.DevCaps & D3D9Metal::D3DDEVCAPS_HWTRANSFORMANDLIGHT) && caps.MaxSimultaneousTextures > 2 && static_cast<uint16_t>(caps.PixelShaderVersion) >= 0x200) {
            deviceID = 3;
        } else if ((caps.DevCaps & D3D9Metal::D3DDEVCAPS_HWTRANSFORMANDLIGHT) && caps.MaxSimultaneousTextures > 2 && static_cast<uint16_t>(caps.PixelShaderVersion) >= 0x101) {
            deviceID = 2;
        } else if ((caps.DevCaps & D3D9Metal::D3DDEVCAPS_HWTRANSFORMANDLIGHT) && caps.MaxSimultaneousTextures >= 2) {
            deviceID = 1;
        }

        result = 1;
    }

    d3d->Release();
    CGxDevice::Log("CGxDevice::DeviceAdapterInfer(): RET: %d, DID: %x", result, deviceID);

    return result;
}

int32_t CGxDevice::AdapterDesktopMode(CGxMonitorMode& mode) {
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(CGMainDisplayID());
    if (!current) {
        return 0;
    }

    // Pixels, which on Retina displays are twice the points of the desktop resolution
    mode.m_size.x = static_cast<int32_t>(CGDisplayModeGetPixelWidth(current));
    mode.m_size.y = static_cast<int32_t>(CGDisplayModeGetPixelHeight(current));
    double refresh = CGDisplayModeGetRefreshRate(current);
    mode.m_refreshRate = refresh > 0.0 ? static_cast<uint32_t>(refresh + 0.5) : 60;
    mode.m_bpp = 32;

    CGDisplayModeRelease(current);
    return 1;
}

static int CGxMonitorModeSort(const void* a, const void* b) {
    const CGxMonitorMode* modeA = static_cast<const CGxMonitorMode*>(a);
    const CGxMonitorMode* modeB = static_cast<const CGxMonitorMode*>(b);

    return modeA->m_size.x * modeA->m_size.y - modeB->m_size.x * modeB->m_size.y;
}

int32_t CGxDevice::AdapterMonitorModes(TSGrowableArray<CGxMonitorMode>& modes) {
    modes.SetCount(0);

    CGDirectDisplayID display = CGMainDisplayID();
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    uint32_t refreshRate = 60;
    if (current) {
        double refresh = CGDisplayModeGetRefreshRate(current);
        refreshRate = refresh > 0.0 ? static_cast<uint32_t>(refresh + 0.5) : 60;
    }

    // Fullscreen scales the rendered image to the display without changing its mode, so every
    // resolution the display offers, at its pixel size, is available
    NSDictionary* options = @{ (__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes: @YES };
    CFArrayRef list = CGDisplayCopyAllDisplayModes(display, (__bridge CFDictionaryRef)options);

    auto add = [&](int32_t width, int32_t height) {
        if (width < 640 || height < 480) {
            return;
        }
        for (uint32_t i = 0; i < modes.Count(); i++) {
            if (modes[i].m_size.x == width && modes[i].m_size.y == height) {
                return;
            }
        }
        CGxMonitorMode* mode = modes.New();
        mode->m_size.x = width;
        mode->m_size.y = height;
        mode->m_bpp = 32;
        mode->m_refreshRate = refreshRate;
    };

    if (list) {
        for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
            CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(list, i);
            if (CGDisplayModeIsUsableForDesktopGUI(mode)) {
                add(static_cast<int32_t>(CGDisplayModeGetPixelWidth(mode)), static_cast<int32_t>(CGDisplayModeGetPixelHeight(mode)));
                add(static_cast<int32_t>(CGDisplayModeGetWidth(mode)), static_cast<int32_t>(CGDisplayModeGetHeight(mode)));
            }
        }
        CFRelease(list);
    }

    if (current) {
        add(static_cast<int32_t>(CGDisplayModeGetPixelWidth(current)), static_cast<int32_t>(CGDisplayModeGetPixelHeight(current)));
        CGDisplayModeRelease(current);
    }

    // The standard resolutions Windows drivers list, as far as the display fits them. The client's
    // default formats are 4:3 sizes such as 1024x768 that a Mac display does not report, and the
    // gxResolution callback rejects a default missing from this list, leaving the CVar without a
    // value on a fresh install.
    static const int32_t s_standard[][2] = {
        { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 },
        { 1280, 960 }, { 1280, 1024 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 },
        { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
    };
    int32_t maxWidth = 0;
    int32_t maxHeight = 0;
    for (uint32_t i = 0; i < modes.Count(); i++) {
        maxWidth = std::max(maxWidth, modes[i].m_size.x);
        maxHeight = std::max(maxHeight, modes[i].m_size.y);
    }
    for (const auto& size : s_standard) {
        if (size[0] <= maxWidth && size[1] <= maxHeight) {
            add(size[0], size[1]);
        }
    }

    qsort(modes.Ptr(), modes.Count(), sizeof(CGxMonitorMode), CGxMonitorModeSort);

    return modes.Count() != 0;
}
