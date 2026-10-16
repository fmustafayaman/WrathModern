// Exercises the Metal-backed Direct3D 9 device offscreen and checks the rendered pixels.
//   devicetest
#import <Foundation/Foundation.h>
#include "../../Engine/Source/Gx/CGxDeviceMetal/D3D9/Device.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace D3D9Metal;

static int s_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { s_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Vertex { float x, y, z; uint32_t color; float u, v; };

static uint32_t ReadPixel(Device* device, uint32_t x, uint32_t y) {
    Surface* back = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DLOCKED_RECT locked;
    uint32_t value = 0xDEADBEEF;
    if (Succeeded(back->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        value = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch + x * 4);
        back->UnlockRect();
    }
    back->Release();
    return value;
}

int main() {
    @autoreleasepool {
        Direct3D* d3d = Direct3D::Create();
        PresentParameters pp = {};
        pp.BackBufferWidth = 64;
        pp.BackBufferHeight = 64;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.Windowed = 1;
        pp.EnableAutoDepthStencil = 1;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        Device* device = nullptr;
        HRESULT hr = d3d->CreateDevice(pp, &device);
        CHECK(Succeeded(hr) && device, "CreateDevice %x", hr);
        if (!device) return 1;

        D3DVERTEXELEMENT9 elements[] = {
            { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 },
            { 0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 },
            D3DDECL_END(),
        };
        VertexDeclaration* decl = nullptr;
        device->CreateVertexDeclaration(elements, &decl);
        device->SetVertexDeclaration(decl);

        VertexBuffer* vb = nullptr;
        device->CreateVertexBuffer(sizeof(Vertex) * 64, D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &vb);

        auto upload = [&](const Vertex* vertices, uint32_t count) {
            void* data = nullptr;
            vb->Lock(0, sizeof(Vertex) * count, &data, D3DLOCK_DISCARD);
            memcpy(data, vertices, sizeof(Vertex) * count);
            vb->Unlock();
            device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        };

        // 1. Clear, then a fixed-function triangle covering the left half
        device->SetRenderState(D3DRS_LIGHTING, 0);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF102030, 1.0f, 0);
        Vertex quad[4] = {
            { -1, -1, 0.5f, 0xFFFF0000, 0, 1 }, { -1, 1, 0.5f, 0xFFFF0000, 0, 0 },
            { 0, -1, 0.5f, 0xFFFF0000, 1, 1 }, { 0, 1, 0.5f, 0xFFFF0000, 1, 0 },
        };
        upload(quad, 4);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        hr = device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        CHECK(Succeeded(hr), "draw %x", hr);
        uint32_t left = ReadPixel(device, 10, 32), right = ReadPixel(device, 50, 32);
        CHECK((left & 0xFFFFFF) == 0xFF0000, "ffp left pixel %08x", left);
        CHECK((right & 0xFFFFFF) == 0x102030, "clear right pixel %08x", right);
        // Half-pixel rule: x = 0 in clip space is pixel boundary 32 in Metal terms; with Direct3D
        // pixel centers at integers, pixel 32's center lies on the edge -> top-left rule; 31 is in
        CHECK((ReadPixel(device, 31, 32) & 0xFFFFFF) == 0xFF0000, "pixel 31 %08x", ReadPixel(device, 31, 32));

        // 2. Programmable vs_1_1 + ps_1_1 passing the color through
        const uint32_t vs[] = { 0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000, 0x0000001F, 0x8000000A, 0x900F0001,
            0x00000001, 0xC00F0000, 0x90E40000, 0x00000001, 0xD00F0000, 0x90E40001, 0x0000FFFF };
        const uint32_t ps[] = { 0xFFFF0101, 0x00000001, 0x800F0000, 0x90E40000, 0x0000FFFF };
        VertexShader* vshader = nullptr;
        PixelShader* pshader = nullptr;
        CHECK(Succeeded(device->CreateVertexShader(vs, &vshader, sizeof(vs))), "create vs");
        CHECK(Succeeded(device->CreatePixelShader(ps, &pshader, sizeof(ps))), "create ps");
        device->SetVertexShader(vshader);
        device->SetPixelShader(pshader);
        Vertex quad2[4] = {
            { 0, -1, 0.5f, 0xFF00FF00, 0, 1 }, { 0, 1, 0.5f, 0xFF00FF00, 0, 0 },
            { 1, -1, 0.5f, 0xFF00FF00, 1, 1 }, { 1, 1, 0.5f, 0xFF00FF00, 1, 0 },
        };
        upload(quad2, 4);
        hr = device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        CHECK(Succeeded(hr), "draw shaders %x", hr);
        right = ReadPixel(device, 50, 32);
        CHECK((right & 0xFFFFFF) == 0x00FF00, "shader right pixel %08x", right);
        CHECK((ReadPixel(device, 10, 32) & 0xFFFFFF) == 0xFF0000, "left kept %08x", ReadPixel(device, 10, 32));

        // 3. Texture upload and sampling through the fixed-function stage
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        Texture* texture = nullptr;
        CHECK(Succeeded(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture)), "create texture");
        Surface* level = nullptr;
        texture->GetSurfaceLevel(0, &level);
        D3DLOCKED_RECT locked;
        level->LockRect(&locked, nullptr, 0);
        uint32_t* texels = static_cast<uint32_t*>(locked.pBits);
        texels[0] = 0xFF0000FF; texels[1] = 0xFFFFFF00;
        reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + locked.Pitch)[0] = 0xFFFF00FF;
        reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + locked.Pitch)[1] = 0xFF00FFFF;
        level->UnlockRect();
        level->Release();
        device->SetTexture(0, texture);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        Vertex full[4] = {
            { -1, -1, 0.5f, 0xFFFFFFFF, 0, 1 }, { -1, 1, 0.5f, 0xFFFFFFFF, 0, 0 },
            { 1, -1, 0.5f, 0xFFFFFFFF, 1, 1 }, { 1, 1, 0.5f, 0xFFFFFFFF, 1, 0 },
        };
        upload(full, 4);
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        CHECK((ReadPixel(device, 8, 8) & 0xFFFFFF) == 0x0000FF, "texel 0,0 %08x", ReadPixel(device, 8, 8));
        CHECK((ReadPixel(device, 56, 8) & 0xFFFFFF) == 0xFFFF00, "texel 1,0 %08x", ReadPixel(device, 56, 8));
        CHECK((ReadPixel(device, 8, 56) & 0xFFFFFF) == 0xFF00FF, "texel 0,1 %08x", ReadPixel(device, 8, 56));
        CHECK((ReadPixel(device, 56, 56) & 0xFFFFFF) == 0x00FFFF, "texel 1,1 %08x", ReadPixel(device, 56, 56));

        // 4. Alpha test discards: diffuse alpha 0x40 against ref 0x80
        device->SetTexture(0, nullptr);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, 1);
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        device->SetRenderState(D3DRS_ALPHAREF, 0x80);
        Vertex faint[4];
        memcpy(faint, full, sizeof(full));
        for (Vertex& v : faint) v.color = 0x40FFFFFF;
        upload(faint, 4);
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        CHECK((ReadPixel(device, 32, 32) & 0xFFFFFF) == 0x000000, "alpha test kept %08x", ReadPixel(device, 32, 32));
        device->SetRenderState(D3DRS_ALPHAREF, 0x40);
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        CHECK((ReadPixel(device, 32, 32) & 0xFFFFFF) == 0xFFFFFF, "alpha test equal discarded %08x", ReadPixel(device, 32, 32));
        device->SetRenderState(D3DRS_ALPHATESTENABLE, 0);

        // 5. Occlusion query counts the samples of the left half
        Query* query = nullptr;
        device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
        upload(quad, 4);
        query->Issue(D3DISSUE_BEGIN);
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        query->Issue(D3DISSUE_END);
        uint32_t samples = 0;
        while (query->GetData(&samples, 4, D3DGETDATA_FLUSH) == S_FALSE) {}
        CHECK(samples == 32 * 64, "occlusion samples %u", samples);

        // 6. Depth test: nearer quad wins, farther one is rejected
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
        Vertex nearQuad[4]; memcpy(nearQuad, full, sizeof(full));
        for (Vertex& v : nearQuad) { v.z = 0.2f; v.color = 0xFF0000FF; }
        Vertex farQuad[4]; memcpy(farQuad, full, sizeof(full));
        for (Vertex& v : farQuad) { v.z = 0.8f; v.color = 0xFFFF0000; }
        upload(nearQuad, 4);
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        upload(farQuad, 4);
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        CHECK((ReadPixel(device, 32, 32) & 0xFFFFFF) == 0x0000FF, "depth test %08x", ReadPixel(device, 32, 32));

        // 7. Triangle fan
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        Vertex fan[4] = { { -1, -1, 0.5f, 0xFF00FF00, 0, 0 }, { -1, 1, 0.5f, 0xFF00FF00, 0, 0 }, { 1, 1, 0.5f, 0xFF00FF00, 0, 0 }, { 1, -1, 0.5f, 0xFF00FF00, 0, 0 } };
        upload(fan, 4);
        device->DrawPrimitive(D3DPT_TRIANGLEFAN, 0, 2);
        CHECK((ReadPixel(device, 5, 5) & 0xFFFFFF) == 0x00FF00 && (ReadPixel(device, 58, 58) & 0xFFFFFF) == 0x00FF00, "fan %08x %08x", ReadPixel(device, 5, 5), ReadPixel(device, 58, 58));

        query->Release();
        texture->Release();
        vshader->Release();
        pshader->Release();
        vb->Release();
        decl->Release();
        device->Release();
        d3d->Release();

        printf("%s (%d failures)\n", s_failures ? "FAILED" : "all checks passed", s_failures);
        return s_failures != 0;
    }
}
