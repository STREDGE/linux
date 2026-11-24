#include "imgui_rw.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <imgui/imgui.h>

#include <stdint.h>
#include <string.h>

namespace imgui_rw
{
namespace
{
    // Вершина RwIm2D на Android (28 байт) — см. Im2DRenderQuad в libGTASA.
    struct RwOpenGLVertex
    {
        float    x, y, z, rhw;
        uint32_t color;          // RGBA в памяти — как ImU32 в ImGui
        float    u, v;
    };
    static_assert(sizeof(RwOpenGLVertex) == 28, "размер вершины RW");

    enum
    {
        rwRENDERSTATETEXTURERASTER     = 1,
        rwRENDERSTATETEXTUREADDRESS    = 2,
        rwRENDERSTATEZTESTENABLE       = 6,
        rwRENDERSTATEZWRITEENABLE      = 8,
        rwRENDERSTATETEXTUREFILTER     = 9,
        rwRENDERSTATESRCBLEND          = 10,
        rwRENDERSTATEDESTBLEND         = 11,
        rwRENDERSTATEVERTEXALPHAENABLE = 12,
        rwRENDERSTATEFOGENABLE         = 14,
        rwRENDERSTATECULLMODE          = 20,
    };
    enum { rwPRIMTYPETRILIST = 3 };
    enum { rwFILTERLINEAR = 2, rwTEXTUREADDRESSCLAMP = 3, rwCULLMODECULLNONE = 1 };
    enum { rwBLENDSRCALPHA = 5, rwBLENDINVSRCALPHA = 6 };
    enum { rwRASTERTYPETEXTURE = 0x04, rwRASTERFORMAT8888 = 0x0500, rwRASTERLOCKWRITE = 0x02 };

    void*    (*RwRasterCreate)(int w, int h, int depth, int flags) = nullptr;
    uint8_t* (*RwRasterLock)(void* raster, unsigned char level, int lockMode) = nullptr;
    void*    (*RwRasterUnlock)(void* raster) = nullptr;
    void*    (*RwTextureCreate)(void* raster) = nullptr;
    int      (*RwRenderStateSet)(int state, void* value) = nullptr;
    int      (*RwRenderStateGet)(int state, void* value) = nullptr;
    int      (*RwIm2DRenderIndexedPrimitive)(int prim, RwOpenGLVertex* v, int nv, uint16_t* idx, int ni) = nullptr;
    float*   NearScreenZ = nullptr;
    float*   RecipNearClip = nullptr;
    uint8_t* RsGlobal = nullptr;   // +8 ширина, +12 высота экрана

    void*    g_fontRaster = nullptr;
    bool     g_ready = false;

    RwOpenGLVertex* g_verts = nullptr;
    int             g_vertsCap = 0;

    bool Sym(void* h, void** out, const char* name)
    {
        *out = (void*)aml->GetSym(h, name);
        if(!*out) logger->Error("ImGui/RW: нет символа %s", name);
        return *out != nullptr;
    }
}

bool Init(void* h)
{
    bool ok = true;
    ok &= Sym(h, (void**)&RwRasterCreate,   "_Z14RwRasterCreateiiii");
    ok &= Sym(h, (void**)&RwRasterLock,     "_Z12RwRasterLockP8RwRasterhi");
    ok &= Sym(h, (void**)&RwRasterUnlock,   "_Z14RwRasterUnlockP8RwRaster");
    ok &= Sym(h, (void**)&RwTextureCreate,  "_Z15RwTextureCreateP8RwRaster");
    ok &= Sym(h, (void**)&RwRenderStateSet, "_Z16RwRenderStateSet13RwRenderStatePv");
    ok &= Sym(h, (void**)&RwRenderStateGet, "_Z16RwRenderStateGet13RwRenderStatePv");
    ok &= Sym(h, (void**)&RwIm2DRenderIndexedPrimitive,
              "_Z28RwIm2DRenderIndexedPrimitive15RwPrimitiveTypeP14RwOpenGLVertexiPti");
    ok &= Sym(h, (void**)&NearScreenZ,      "_ZN9CSprite2d11NearScreenZE");
    ok &= Sym(h, (void**)&RecipNearClip,    "_ZN9CSprite2d13RecipNearClipE");
    ok &= Sym(h, (void**)&RsGlobal,         "RsGlobal");
    return ok;
}

bool Ready() { return g_ready; }

bool EnsureDeviceObjects()
{
    if(g_ready) return true;
    if(!RwRasterCreate) return false;

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    g_fontRaster = RwRasterCreate(w, h, 32, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
    if(!g_fontRaster) { logger->Error("ImGui/RW: не создан растр %dx%d", w, h); return false; }
    uint8_t* dst = RwRasterLock(g_fontRaster, 0, rwRASTERLOCKWRITE);
    if(!dst) { logger->Error("ImGui/RW: растр не заблокирован"); return false; }
    memcpy(dst, pixels, (size_t)w * h * 4);
    RwRasterUnlock(g_fontRaster);
    RwTextureCreate(g_fontRaster); // текстура держит растр; рисуем через растр напрямую

    io.Fonts->SetTexID((ImTextureID)g_fontRaster);
    io.Fonts->ClearTexData();
    g_ready = true;
    logger->Info("ImGui/RW: атлас шрифтов %dx%d готов", w, h);
    return true;
}

void NewFrame(float dt)
{
    ImGuiIO& io = ImGui::GetIO();
    int sw = *(int*)(RsGlobal + 8), sh = *(int*)(RsGlobal + 12);
    if(sw > 0 && sh > 0) io.DisplaySize = ImVec2((float)sw, (float)sh);
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
}

void Render()
{
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if(!dd || dd->TotalVtxCount == 0 || !g_ready) return;

    // Сохраняем то, что меняем, — игра дальше рисует своё
    const int saveIds[] = { rwRENDERSTATETEXTURERASTER, rwRENDERSTATEZTESTENABLE, rwRENDERSTATEZWRITEENABLE,
                            rwRENDERSTATETEXTUREFILTER, rwRENDERSTATESRCBLEND, rwRENDERSTATEDESTBLEND,
                            rwRENDERSTATEVERTEXALPHAENABLE, rwRENDERSTATEFOGENABLE, rwRENDERSTATECULLMODE,
                            rwRENDERSTATETEXTUREADDRESS };
    void* saved[sizeof(saveIds) / sizeof(saveIds[0])] = {};
    for(size_t i = 0; i < sizeof(saveIds) / sizeof(saveIds[0]); ++i) RwRenderStateGet(saveIds[i], &saved[i]);

    RwRenderStateSet(rwRENDERSTATEZTESTENABLE,       (void*)0);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE,      (void*)0);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)1);
    RwRenderStateSet(rwRENDERSTATESRCBLEND,          (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND,         (void*)rwBLENDINVSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEFOGENABLE,         (void*)0);
    RwRenderStateSet(rwRENDERSTATECULLMODE,          (void*)rwCULLMODECULLNONE);
    RwRenderStateSet(rwRENDERSTATETEXTUREFILTER,     (void*)rwFILTERLINEAR);
    RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS,    (void*)rwTEXTUREADDRESSCLAMP);

    const float z = *NearScreenZ, rhw = *RecipNearClip;
    for(int n = 0; n < dd->CmdListsCount; ++n)
    {
        const ImDrawList* cl = dd->CmdLists[n];
        const int nv = cl->VtxBuffer.Size;
        if(nv > g_vertsCap)
        {
            delete[] g_verts;
            g_vertsCap = nv + 1024;
            g_verts = new RwOpenGLVertex[g_vertsCap];
        }
        for(int i = 0; i < nv; ++i)
        {
            const ImDrawVert& s = cl->VtxBuffer[i];
            g_verts[i] = { s.pos.x, s.pos.y, z, rhw, s.col, s.uv.x, s.uv.y };
        }
        for(const ImDrawCmd& cmd : cl->CmdBuffer)
        {
            if(cmd.UserCallback || cmd.ElemCount == 0) continue;
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER, cmd.GetTexID());
            // Индексы 16-битные; VtxOffset сдвигаем указателем на вершины
            RwIm2DRenderIndexedPrimitive(rwPRIMTYPETRILIST, g_verts + cmd.VtxOffset, nv - (int)cmd.VtxOffset,
                                         (uint16_t*)(cl->IdxBuffer.Data + cmd.IdxOffset), (int)cmd.ElemCount);
        }
    }

    for(size_t i = 0; i < sizeof(saveIds) / sizeof(saveIds[0]); ++i) RwRenderStateSet(saveIds[i], saved[i]);
}

} // namespace imgui_rw
