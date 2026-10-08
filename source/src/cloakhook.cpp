// cloakhook - proxy dsetup.dll that adds ONLY the cloak rendering path to an Interlude client.
//
// The stock Interlude Engine.dll already contains everything needed to draw a cloak
// (APawn::GetCloakMesh, UMesh::CloakMeshGetInstance, USubSkeletalMeshInstance::Render, ...) but nothing
// ever calls it. We hook USkeletalMeshInstance::Render / DrawSection and perform that call ourselves,
// right before the character body's first section is drawn, so the cloak uses the body's bone pose.
//
// Layout (all derived from engine.dll build sha256 bae9ac30... and 508974c7..., see README.md):
//   FDynamicActor        +0x00 AActor*
//   AActor               +0x104 Mesh (body)
//   APawn                +0x3bc UMesh* SubMeshes[14]
//   USkeletalMeshInstance+0x60 Mesh, +0x64 Actor
//   USkeletalMesh        +0x208 RefSkeleton.Data (stride 0x40, first dword = FName index), +0x20c RefSkeleton.Num
//
// Build: 32-bit MSVC (see CMakeLists.txt).  Thiscall functions are declared __fastcall with a dummy EDX.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <d3d9.h>
#include "MinHook.h"
#include "essence.h"
#include "clothpack.h"
#include "clothguide.h"
#include "clothwiden.h"
#include "fx.h"

// L2.exe imports DSETUP.dll by ordinal 11 (DirectXSetupGetVersion). Forward it to the real Microsoft DLL,
// which must sit next to us renamed to dsetup_ms.dll.
#pragma comment(linker, "/export:DirectXSetupGetVersion=dsetup_ms.DirectXSetupGetVersion,@11")

namespace {

// ----------------------------------------------------------------------------------------------
// config / log
// ----------------------------------------------------------------------------------------------
struct Config {
    wchar_t package[64] = L"LineShieldCloaks";  // package that holds the cloak SkeletalMeshes
    int  idMin = 9400, idMax = 9466;            // accepted <id> range in <Body>_Cloak_<id>
    int  slot = 13;                             // sub-mesh index used while drawing the cloak
    int  forceId = 0;                           // >0: test mode, load <pkg>.<Body>_Cloak_<forceId> for every player
    bool log = true;
    bool debug = false;                         // verbose, rate-limited tracing of every stage
    bool scanIds = false;                       // debug: find where the client keeps the equipped item ids
    bool capeTest = false;                      // experiment: oscillate the pawn's Cape_Bone with SetBoneRotation (sine)
    int  capeAxis = 0;                          //   0 pitch, 1 yaw, 2 roll
    int  capeSpace = 0;                         //   `Space` argument of SetBoneRotation
    float capeAmp = 25.0f;                      //   degrees
    float capeFreq = 0.5f;                      //   Hz
    bool vertTest = false;                      // experiment: deform the cloak's skinned vertices (sine sway of the hem)
    int  vertAxis = 0;                          //   coordinate to displace (0 x, 1 y, 2 z)
    int  vertUp = 2;                            //   coordinate that is "height" in the skinned vertex space
    float vertAmp = 6.0f;                       //   units
    float vertFreq = 0.5f;                      //   Hz
    bool constProbe = false;                    // debug: log the vertex-shader constants set while the cloak is drawn (GPU skinning)
    bool matSway = false;                       // swing the whole cloak (rigid sections: one matrix) around its top edge
    int  swayAxis = 0;                          //   local axis the pendulum rotates around (0 x, 1 y, 2 z)
    int  swayUp = 2;                            //   local axis that points "up" (the pivot sits at the max of this axis)
    float swayAmp = 12.0f;                      //   degrees
    float swayFreq = 0.6f;                      //   Hz
    // physics mode (default): a damped spring per character, driven by its velocity and turning measured from the mesh matrix
    bool  swayPhysics = true;
    int   swayFwd = 1;                          //   local axis that points forward (the other horizontal axis is lateral)
    float swayFwdSign = 1.0f, swaySideSign = 1.0f, swayTurnSign = 1.0f;
    float swayKv = 0.20f;                       //   degrees of backward tilt per unit/s of forward speed
    float swayKs = 0.12f;                       //   degrees of sideways tilt per unit/s of lateral speed
    float swayKw = 6.0f;                        //   degrees per rad/s of turning
    float swayMax = 40.0f;                      //   clamp (degrees)
    float swayStiff = 9.0f;                     //   spring natural frequency (rad/s)
    float swayDamp = 0.35f;                     //   damping ratio (<1 = it overshoots and settles)
    float swayIdle = 1.5f;                      //   degrees of idle breathing
    // D3D9 vertex-buffer deformation: the top edge stays glued, the hem bends (this is the cloth-like motion)
    bool  vbDeform = false;
    int   vbMode = 1;                           //   0 = calibration sine (VBAmp/VBFreq), 1 = physics (the spring above)
    float vbAmp = 25.0f;                        //   degrees of hem swing in mode 0
    float vbFreq = 0.5f;                        //   Hz in mode 0
    float clothDelay = 0.18f;                   //   seconds the hem lags behind the top (the pose travels down the cloth)
    float clothPow = 1.5f;                      //   how fast the bend grows with the distance from the top edge
    float rippleAmp = 1.2f;                     //   units of ripple across the width
    float rippleLen = 26.0f;                    //   units between ripple crests
    // VBMode 2: cloth simulation (displacement field with neighbour coupling), all in mesh units / seconds
    float clothKHem = 14.0f;                    //   spring back to the rest shape at the hem (rad/s^2: 14 -> ~0.6 Hz)
    float clothKTop = 500.0f;                   //   ... and at the top edge
    float clothKN = 60.0f;                      //   coupling between neighbouring points (waves travel through this; low = floppy, high = plank)
    float clothSmooth = 20.0f;                  //   1/s: how quickly neighbours equalise their velocities (damps ripples)
    float clothZeta = 0.15f;                    //   damping ratio of the spring to rest
    float clothDrag = 1.0f;                     //   air push per unit of body speed (forward); lateral/vertical are scaled from it
    float clothInertia = 0.4f;                  //   how much of the body's acceleration the cloth feels
    float clothGust = 35.0f;                    //   fluttering acceleration (grows with speed)
    float clothGravity = 90.0f;                 //   pull towards "down" when the torso tilts (kneeling, leaning); zero while it is upright
    float clothTurn = 0.5f;                     //   how strongly turning (yaw acceleration, centrifugal) throws the cloth
    float clothTether = 0.03f;                  //   cloth does not stretch: a point may be at most this much farther from the top edge than at rest
    float clothZMax = 8.0f;                     //   vertical displacement limit (units)
    float clothMax = 14.0f;                     //   limit of the displacement (units)
    // Essence mantles: baked animated cloaks played from the pawn's own animation (tools/build_capes.py -> essence_capes.bin)
    bool  essence = false;
    wchar_t essencePack[260] = L"essence_capes.bin";
    int   essenceFirstId = 9400;                //   item mesh id essenceFirstId + N plays design NewMantleNN
    int   essenceWingBase = 61;                 //   (item id - FirstId) from here on = the wing-like cloaks listed in EssenceWingMap (cycled)
    int   essenceWingMap[8] = { 11, 12, 13, 14, 10, 15, 0, 0 };   //   pack design ids: 11 Valakas wings, 12/13 Death Knight bat wings, 14 DK ranker winged mantle, 10 Eigis blades, 15 MG ranker angel ribbons
    int   essenceWingCount = 6;
    int   essenceComboLook = 11;                //   design 16 (Ranker wings) is worn over this cloth look: 11 = Cloak of Darkness for the Chosen
    int   essenceDesigns = 10;                  //   with ForceDesign < 0: the list below is indexed by (item id - FirstId) % count
    int   essenceDesignMap[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };   //   pack design ids in item order (8 = Rus2024, 9 = design 0 in red; 10 = aegis exists in the pack but is not listed)
    int   essenceForceDesign = -1;              //   >= 0: every cloak id plays this design (handy to test with whatever item is equipped)
    float essenceOffset[3] = { 0.f, 0.f, 0.f }; //   shifts the mantle in actor space (units)
    float essenceScale = 1.0f;
    float essenceSnug = 0.f;                    //   units the upper part of the mantle is pulled towards the chest (the lower part hangs free)
    float essenceSnugBody[14] = { -999.f, -999.f, -999.f, -999.f, -999.f, -999.f, -999.f, -999.f, -999.f, -999.f, -999.f, -999.f, -999.f, -999.f };   //   per body override, -999 = use essenceSnug
    float essenceBright = 0.85f;                //   unlit: texture x this factor
    int   essenceLight = 2;                     //   0 unlit, 1 always lit by the engine's own D3D lights, 2 lit when the engine had lighting on for the carrier
    float essenceLitBright = 1.0f;              //   lit: material colour (diffuse and ambient)
    // standard Essence cloaks: cloth simulation (essence_cloth.bin, src/cloth.h)
    bool  essenceCloth = true;
    wchar_t essenceClothPack[260] = L"essence_cloth.bin";
    int   essenceClothBase = 10;                //   (item id - FirstId) below this = animated mantle designs, from here on = look (rel - base) of the cloth pack
    float essenceClothGravity = 70.f;           //   units/s^2 (the Essence notifies carry (0, -10..-45, -50))
    float essenceClothWind[5] = { -9.f, -20.f, -36.f, 0.f, -26.f };   //   backwards acceleration while idle, walking, running, sitting, attacking
    float essenceClothStiff = 0.85f;            //   spring stiffness per iteration (Essence 0.6 .. 0.85)
    float essenceClothDamp = 2.8f;              //   velocity damping per second
    int   essenceClothIter = 8;
    float essenceClothFollow = 0.6f;
    float essenceClothWidth = 0.08f;            //   pull that keeps the cloak as wide as its cut (0 = off)
    bool  essenceFxParticles = true;            //   particle effects of the cloaks (wings, rays, glows; essence_fx.bin)
    wchar_t essenceFxPack[260] = L"essence_fx.bin";
    wchar_t essenceFxMap[1024] = L"9400:h_new_clothB_deco,9407:b_anta_cloth_deco,9420:d_cloth_deco_a+d_cloth_deco_c,9421:d_cloth_deco_b+d_cloth_deco_d+d_cloth_deco_c,9423:h_worldSiege_cloth_deco,9460:v_disron_cloth_deco,9466:h_new_clothB_deco";   // item id : effect class
    float essenceFxOffset[3] = { 0.f, -11.f, 6.f };   //   where the effect sits on the torso (animated mantles): left, front, up from Spine2 (units)
    float essenceFxClothOffset[3] = { 0.f, -3.f, 1.5f };   //   same for the cloth cloaks: from the middle of the top edge of the cloak
    float essenceFxScale = 1.f;                 //   size of the effects
    bool  essenceFxWingFlip = true;             //   the animated wings (wing_high) sweep backwards (0 = forwards)
    int   essenceFxAxis = 0;                    //   how the effect's own axes sit on the torso: 0 x = front, z = up | 1 x = back, z = up | 2 x = up, z = back | 3 x = down, z = front
    wchar_t essenceFxForce[64] = L"";             //   test: show this effect class on EVERY Essence cloak (e.g. d_cloth_deco_d), changed live
    float essenceFxPGain = 1.f;                 //   brightness of the particles
    float essenceClothTopRamp = 9.f;            //   height over which the torso collider grows from the thickness of the pinned rows to its full radius (0 = plain capsule)
    float essenceClothWiden = 1.f;              //   how far the top of the cloth is widened towards the width of its collar (0 = as cut, 1 = to the collar), drawn only
    bool  essenceClothTorso = true;             //   orient the cloak by the real torso (shoulder line + spine) instead of assuming the idle pose faces straight ahead
    float essenceClothGuide[5] = { 0.f, 0.10f, 0.16f, 0.f, 0.10f };   //   pull of the cloth towards the baked cape deformation (per 1/90 s step) while idle, walking, running, sitting, attacking
    bool  essenceClothInelastic = true;         //   contacts keep the pre-contact velocity (a moving collider cannot launch the cloth)
    float essenceClothMaxStep = 1.0f;           //   cap on a particle's displacement per 1/90 s step (units): 1.0 = 90 units/s
    int   essenceClothDump = 1;                 //   debug: dump every particle and collider of the first running / sitting samples (torso angle > 55 deg) to the log
    bool  essenceCollar = true;
    bool  essenceFx = true;                     //   material effects of the cloaks: environment-map shine, animated fire / glow layers
    float essenceFxGain = 1.f;                  //   brightness of the additive effect layers
    float essenceSway = 1.f;                    //   secondary motion of the animated mantles (hem lags the torso, idle flutter, trailing in walk/run): 0 = off, 1 = default, 2 = double
    bool  essenceCrest = true;                  //   draw the clan's own crest in the window of the clan cloaks (UNetworkHandler::GetPledgeCrestTex)
    bool  essenceClothPelvisSit = true;          //   a pelvis capsule between the thighs while sitting (the cloak rests on the seat behind, never through it)
    float essenceClothSkin = 0.35f;             //   extra radius around the body capsules
    float essenceClothSnug = 0.f;               //   moves the pinned top towards the chest (+Y), units
    bool  essenceClothRef = true;               //   pin relative to the pawn's own idle Spine2 orientation (cancels any axis-mapping error) instead of the Essence bind
    float essenceBlend = 0.20f;                 //   seconds to blend between two animation sequences (only when the engine does not tween itself)
    bool  essenceTween = true;                  //   follow the engine's own tween (negative AnimFrame) so the mantle blends exactly like the body
    bool  essenceAttach = true;                 //   the mantle root follows the pawn's real Spine2 bone (read from the engine) instead of the baked Essence body
    bool  essenceShowCarrier = false;           //   draw the old (rigid/cloth) carrier cloak too: it sits correctly on the back, a reference for EssenceOffset
    int   essenceFrameMode = 0;                 //   0 auto (fraction if <= 1), 1 AnimFrame is a 0..1 fraction, 2 AnimFrame is a frame number
    float clothPin = 0.04f;                     //   top fraction of the height that is glued to the body
} g_cfg;

HMODULE g_self = nullptr;
wchar_t g_dir[MAX_PATH] = L"";

void Log(const char* fmt, ...) {
    if (!g_cfg.log) return;
    char line[1024];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = _snprintf_s(line, sizeof line, _TRUNCATE, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    n += _vsnprintf_s(line + n, sizeof line - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    line[n++] = '\r'; line[n++] = '\n';
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%scloakhook.log", g_dir);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, line, (DWORD)n, &w, nullptr); CloseHandle(h); }
}

void ReadEssenceCfg();

void LoadConfig() {
    wchar_t ini[MAX_PATH]; _snwprintf_s(ini, _TRUNCATE, L"%scloakhook.ini", g_dir);
    GetPrivateProfileStringW(L"CloakHook", L"Package", g_cfg.package, g_cfg.package, 64, ini);
    g_cfg.idMin   = GetPrivateProfileIntW(L"CloakHook", L"IdMin", g_cfg.idMin, ini);
    g_cfg.idMax   = GetPrivateProfileIntW(L"CloakHook", L"IdMax", g_cfg.idMax, ini);
    g_cfg.slot    = GetPrivateProfileIntW(L"CloakHook", L"Slot", g_cfg.slot, ini);
    g_cfg.forceId = GetPrivateProfileIntW(L"CloakHook", L"ForceId", g_cfg.forceId, ini);
    g_cfg.log     = GetPrivateProfileIntW(L"CloakHook", L"Log", 1, ini) != 0;
    g_cfg.debug   = GetPrivateProfileIntW(L"CloakHook", L"Debug", 0, ini) != 0;
    g_cfg.scanIds = GetPrivateProfileIntW(L"CloakHook", L"ScanIds", 0, ini) != 0;
    g_cfg.capeTest  = GetPrivateProfileIntW(L"CloakHook", L"CapeTest", 0, ini) != 0;
    g_cfg.capeAxis  = GetPrivateProfileIntW(L"CloakHook", L"CapeAxis", 0, ini);
    g_cfg.capeSpace = GetPrivateProfileIntW(L"CloakHook", L"CapeSpace", 0, ini);
    wchar_t tmp[32];
    GetPrivateProfileStringW(L"CloakHook", L"CapeAmp", L"25", tmp, 32, ini);  g_cfg.capeAmp  = (float)_wtof(tmp);
    GetPrivateProfileStringW(L"CloakHook", L"CapeFreq", L"0.5", tmp, 32, ini); g_cfg.capeFreq = (float)_wtof(tmp);
    g_cfg.vertTest = GetPrivateProfileIntW(L"CloakHook", L"VertTest", 0, ini) != 0;
    g_cfg.constProbe = GetPrivateProfileIntW(L"CloakHook", L"ConstProbe", 0, ini) != 0;
    g_cfg.matSway  = GetPrivateProfileIntW(L"CloakHook", L"MatSway", 0, ini) != 0;
    g_cfg.swayAxis = GetPrivateProfileIntW(L"CloakHook", L"SwayAxis", 0, ini);
    g_cfg.swayUp   = GetPrivateProfileIntW(L"CloakHook", L"SwayUp", 2, ini);
    GetPrivateProfileStringW(L"CloakHook", L"SwayAmp", L"12", tmp, 32, ini);   g_cfg.swayAmp  = (float)_wtof(tmp);
    GetPrivateProfileStringW(L"CloakHook", L"SwayFreq", L"0.6", tmp, 32, ini); g_cfg.swayFreq = (float)_wtof(tmp);
    g_cfg.swayPhysics = GetPrivateProfileIntW(L"CloakHook", L"SwayPhysics", 1, ini) != 0;
    g_cfg.swayFwd = GetPrivateProfileIntW(L"CloakHook", L"SwayFwd", 1, ini) == 0 ? 0 : 1;
    auto fl = [&](const wchar_t* key, const wchar_t* def, float& dst) { GetPrivateProfileStringW(L"CloakHook", key, def, tmp, 32, ini); dst = (float)_wtof(tmp); };
    fl(L"SwayFwdSign", L"1", g_cfg.swayFwdSign);  fl(L"SwaySideSign", L"1", g_cfg.swaySideSign);  fl(L"SwayTurnSign", L"1", g_cfg.swayTurnSign);
    fl(L"SwayKv", L"0.2", g_cfg.swayKv);          fl(L"SwayKs", L"0.12", g_cfg.swayKs);           fl(L"SwayKw", L"6", g_cfg.swayKw);
    fl(L"SwayMax", L"40", g_cfg.swayMax);         fl(L"SwayStiff", L"9", g_cfg.swayStiff);        fl(L"SwayDamp", L"0.35", g_cfg.swayDamp);
    fl(L"SwayIdle", L"1.5", g_cfg.swayIdle);
    g_cfg.vbDeform = GetPrivateProfileIntW(L"CloakHook", L"VBDeform", 0, ini) != 0;
    g_cfg.vbMode   = GetPrivateProfileIntW(L"CloakHook", L"VBMode", 1, ini);
    fl(L"VBAmp", L"25", g_cfg.vbAmp);             fl(L"VBFreq", L"0.5", g_cfg.vbFreq);
    fl(L"ClothDelay", L"0.18", g_cfg.clothDelay); fl(L"ClothPow", L"1.5", g_cfg.clothPow);
    fl(L"RippleAmp", L"1.2", g_cfg.rippleAmp);    fl(L"RippleLen", L"26", g_cfg.rippleLen);
    if (g_cfg.rippleLen < 4.0f) g_cfg.rippleLen = 4.0f;
    ReadEssenceCfg();
    fl(L"ClothKHem", L"14", g_cfg.clothKHem);     fl(L"ClothKTop", L"500", g_cfg.clothKTop);   fl(L"ClothKN", L"60", g_cfg.clothKN);
    fl(L"ClothSmooth", L"20", g_cfg.clothSmooth); fl(L"ClothZeta", L"0.15", g_cfg.clothZeta);  fl(L"ClothDrag", L"1.0", g_cfg.clothDrag);   fl(L"ClothTurn", L"0.5", g_cfg.clothTurn);
    fl(L"ClothInertia", L"0.4", g_cfg.clothInertia); fl(L"ClothGust", L"35", g_cfg.clothGust); fl(L"ClothMax", L"14", g_cfg.clothMax);
    fl(L"ClothTether", L"0.03", g_cfg.clothTether); fl(L"ClothZMax", L"8", g_cfg.clothZMax); fl(L"ClothGravity", L"90", g_cfg.clothGravity);
    fl(L"ClothPin", L"0.04", g_cfg.clothPin);
    if (g_cfg.clothKN > 900.f) g_cfg.clothKN = 900.f;                  // keeps the explicit integration stable
    if (g_cfg.clothKTop > 3000.f) g_cfg.clothKTop = 3000.f;
    if (g_cfg.clothTether < 0.0f) g_cfg.clothTether = 0.0f;
    g_cfg.vertAxis = GetPrivateProfileIntW(L"CloakHook", L"VertAxis", 0, ini);
    g_cfg.vertUp   = GetPrivateProfileIntW(L"CloakHook", L"VertUp", 2, ini);
    GetPrivateProfileStringW(L"CloakHook", L"VertAmp", L"6", tmp, 32, ini);   g_cfg.vertAmp  = (float)_wtof(tmp);
    GetPrivateProfileStringW(L"CloakHook", L"VertFreq", L"0.5", tmp, 32, ini); g_cfg.vertFreq = (float)_wtof(tmp);
}

// Essence keys can be re-read while the game runs (the ini is polled once a second), so offsets / scale / brightness can be tuned live
void ReadEssenceCfg() {
    wchar_t ini[MAX_PATH]; _snwprintf_s(ini, _TRUNCATE, L"%scloakhook.ini", g_dir);
    wchar_t tmp[128];
    g_cfg.essence = GetPrivateProfileIntW(L"CloakHook", L"Essence", 0, ini) != 0;
    GetPrivateProfileStringW(L"CloakHook", L"EssencePack", L"essence_capes.bin", g_cfg.essencePack, 260, ini);
    g_cfg.essenceFirstId = GetPrivateProfileIntW(L"CloakHook", L"EssenceFirstId", 9400, ini);
    g_cfg.essenceForceDesign = GetPrivateProfileIntW(L"CloakHook", L"EssenceForceDesign", -1, ini);
    {
        GetPrivateProfileStringW(L"CloakHook", L"EssenceDesignMap", L"0,1,2,3,4,5,6,7,8,9", tmp, 128, ini);
        int n = 0; wchar_t* p = tmp;
        while (*p && n < 32) { wchar_t* e = nullptr; const long v = wcstol(p, &e, 10); if (e == p) break; g_cfg.essenceDesignMap[n++] = (int)v; p = e; while (*p == L',' || *p == L' ') ++p; }
        g_cfg.essenceDesigns = n > 0 ? n : 1;
    }
    g_cfg.essenceShowCarrier = GetPrivateProfileIntW(L"CloakHook", L"EssenceShowCarrier", 0, ini) != 0;
    g_cfg.essenceAttach = GetPrivateProfileIntW(L"CloakHook", L"EssenceAttach", 1, ini) != 0;
    g_cfg.essenceFrameMode = GetPrivateProfileIntW(L"CloakHook", L"EssenceFrameMode", 0, ini);
    auto fl = [&](const wchar_t* key, const wchar_t* def, float& dst) { GetPrivateProfileStringW(L"CloakHook", key, def, tmp, 128, ini); dst = (float)_wtof(tmp); };
    fl(L"EssenceScale", L"1", g_cfg.essenceScale); fl(L"EssenceBright", L"0.85", g_cfg.essenceBright); fl(L"EssenceBlend", L"0.2", g_cfg.essenceBlend);
    GetPrivateProfileStringW(L"CloakHook", L"EssenceOffset", L"0,0,0", tmp, 128, ini);
    float o[3] = { 0.f, 0.f, 0.f };
    { wchar_t* p = tmp; for (int i = 0; i < 3 && *p; ++i) { o[i] = (float)wcstod(p, &p); while (*p == L',' || *p == L' ') ++p; } }
    memcpy(g_cfg.essenceOffset, o, sizeof o);
    g_cfg.essenceTween = GetPrivateProfileIntW(L"CloakHook", L"EssenceTween", 1, ini) != 0;
    fl(L"EssenceSnug", L"0", g_cfg.essenceSnug);
    g_cfg.essenceCloth = GetPrivateProfileIntW(L"CloakHook", L"EssenceCloth", 1, ini) != 0;
    GetPrivateProfileStringW(L"CloakHook", L"EssenceClothPack", L"essence_cloth.bin", g_cfg.essenceClothPack, 260, ini);
    g_cfg.essenceClothBase = GetPrivateProfileIntW(L"CloakHook", L"EssenceClothBase", 10, ini);
    g_cfg.essenceClothIter = GetPrivateProfileIntW(L"CloakHook", L"EssenceClothIter", 8, ini);
    if (g_cfg.essenceClothIter < 1) g_cfg.essenceClothIter = 1; if (g_cfg.essenceClothIter > 24) g_cfg.essenceClothIter = 24;
    fl(L"EssenceClothGravity", L"70", g_cfg.essenceClothGravity); fl(L"EssenceClothStiff", L"0.85", g_cfg.essenceClothStiff);
    g_cfg.essenceClothRef = GetPrivateProfileIntW(L"CloakHook", L"EssenceClothRef", 1, ini) != 0;
    g_cfg.essenceCollar = GetPrivateProfileIntW(L"CloakHook", L"EssenceCollar", 1, ini) != 0;
    {   wchar_t tg[128]; GetPrivateProfileStringW(L"CloakHook", L"EssenceClothGuide", L"0,0.10,0.16,0,0.10", tg, 128, ini);
        float gv[5]; int gn = 0; const wchar_t* gp = tg;
        while (*gp && gn < 5) { wchar_t* ge = nullptr; const double v = wcstod(gp, &ge); if (ge == gp) break; gv[gn++] = (float)v; gp = ge; while (*gp == L',' || *gp == L' ') ++gp; }
        if (gn == 5) memcpy(g_cfg.essenceClothGuide, gv, sizeof gv);
    }
    g_cfg.essenceClothInelastic = GetPrivateProfileIntW(L"CloakHook", L"EssenceClothInelastic", 1, ini) != 0;
    g_cfg.essenceClothDump = GetPrivateProfileIntW(L"CloakHook", L"EssenceClothDump", 1, ini);
    fl(L"EssenceClothMaxStep", L"1.0", g_cfg.essenceClothMaxStep);
    g_cfg.essenceWingBase = GetPrivateProfileIntW(L"CloakHook", L"EssenceWingBase", 61, ini);
    g_cfg.essenceComboLook = GetPrivateProfileIntW(L"CloakHook", L"EssenceComboLook", 11, ini);
    {   wchar_t tmpw[128];
        GetPrivateProfileStringW(L"CloakHook", L"EssenceWingMap", L"11,12,13,14,10,15", tmpw, 128, ini);
        int n = 0; const wchar_t* p = tmpw;
        while (*p && n < 8) { wchar_t* e = nullptr; const long v = wcstol(p, &e, 10); if (e == p) break; g_cfg.essenceWingMap[n++] = (int)v; p = e; while (*p == L',' || *p == L' ') ++p; }
        g_cfg.essenceWingCount = n > 0 ? n : 1;
    }
    fl(L"EssenceSway", L"1", g_cfg.essenceSway);
    g_cfg.essenceFx = GetPrivateProfileIntW(L"CloakHook", L"EssenceFx", 1, ini) != 0;
    fl(L"EssenceFxGain", L"1", g_cfg.essenceFxGain);
    g_cfg.essenceCrest = GetPrivateProfileIntW(L"CloakHook", L"EssenceCrest", 1, ini) != 0;
    g_cfg.essenceClothPelvisSit = GetPrivateProfileIntW(L"CloakHook", L"EssenceClothPelvisSit", 1, ini) != 0;
    fl(L"EssenceClothFollow", L"0.6", g_cfg.essenceClothFollow);
    fl(L"EssenceClothWidth", L"0.08", g_cfg.essenceClothWidth);
    g_cfg.essenceClothTorso = GetPrivateProfileIntW(L"CloakHook", L"EssenceClothTorso", 1, ini) != 0;
    fl(L"EssenceClothWiden", L"1", g_cfg.essenceClothWiden);
    fl(L"EssenceClothTopRamp", L"9", g_cfg.essenceClothTopRamp);
    g_cfg.essenceFxParticles = GetPrivateProfileIntW(L"CloakHook", L"EssenceFxParticles", 1, ini) != 0;
    GetPrivateProfileStringW(L"CloakHook", L"EssenceFxPack", L"essence_fx.bin", g_cfg.essenceFxPack, 260, ini);
    {   wchar_t tm[1024]; GetPrivateProfileStringW(L"CloakHook", L"EssenceFxMap", L"", tm, 1024, ini); if (tm[0]) wcsncpy_s(g_cfg.essenceFxMap, tm, _TRUNCATE); }
    {   wchar_t to[96]; GetPrivateProfileStringW(L"CloakHook", L"EssenceFxOffset", L"0,-11,6", to, 96, ini);
        float ov[3] = { 0.f, -11.f, 6.f }; wchar_t* op = to;
        for (int i = 0; i < 3 && *op; ++i) { ov[i] = (float)wcstod(op, &op); while (*op == L',' || *op == L' ') ++op; }
        memcpy(g_cfg.essenceFxOffset, ov, sizeof ov);
        GetPrivateProfileStringW(L"CloakHook", L"EssenceFxClothOffset", L"0,-3,1.5", to, 96, ini);
        float cv[3] = { 0.f, -3.f, 1.5f }; op = to;
        for (int i = 0; i < 3 && *op; ++i) { cv[i] = (float)wcstod(op, &op); while (*op == L',' || *op == L' ') ++op; }
        memcpy(g_cfg.essenceFxClothOffset, cv, sizeof cv); }
    fl(L"EssenceFxScale", L"1", g_cfg.essenceFxScale);
    g_cfg.essenceFxAxis = GetPrivateProfileIntW(L"CloakHook", L"EssenceFxAxis", 0, ini);
    g_cfg.essenceFxWingFlip = GetPrivateProfileIntW(L"CloakHook", L"EssenceFxWingFlip", 1, ini) != 0;
    GetPrivateProfileStringW(L"CloakHook", L"EssenceFxForce", L"", g_cfg.essenceFxForce, 64, ini);
    fl(L"EssenceFxParticleGain", L"1", g_cfg.essenceFxPGain);
    fl(L"EssenceClothDamp", L"2.8", g_cfg.essenceClothDamp); fl(L"EssenceClothSkin", L"0.35", g_cfg.essenceClothSkin); fl(L"EssenceClothSnug", L"0", g_cfg.essenceClothSnug);
    {
        GetPrivateProfileStringW(L"CloakHook", L"EssenceClothWind", L"-9,-20,-36,0,-26", tmp, 128, ini);
        float w[5] = { -10.f, -25.f, -45.f, 0.f, -30.f }; wchar_t* p = tmp;
        for (int i = 0; i < 5 && *p; ++i) { w[i] = (float)wcstod(p, &p); while (*p == L',' || *p == L' ') ++p; }
        memcpy(g_cfg.essenceClothWind, w, sizeof w);
    }
    g_cfg.essenceLight = GetPrivateProfileIntW(L"CloakHook", L"EssenceLight", 2, ini);
    fl(L"EssenceLitBright", L"1", g_cfg.essenceLitBright);
    {
        static const wchar_t* const snugKeys[14] = { L"EssenceSnugMFighter", L"EssenceSnugFFighter", L"EssenceSnugMMagic", L"EssenceSnugFMagic", L"EssenceSnugMElf", L"EssenceSnugFElf", L"EssenceSnugMDarkElf",
                                                     L"EssenceSnugFDarkElf", L"EssenceSnugMDwarf", L"EssenceSnugFDwarf", L"EssenceSnugMOrc", L"EssenceSnugFOrc", L"EssenceSnugMShaman", L"EssenceSnugFShaman" };
        for (int b = 0; b < 14; ++b) {
            GetPrivateProfileStringW(L"CloakHook", snugKeys[b], L"", tmp, 128, ini);
            g_cfg.essenceSnugBody[b] = tmp[0] ? (float)_wtof(tmp) : -999.f;
        }
    }
    if (g_cfg.essenceScale < 0.05f) g_cfg.essenceScale = 0.05f;
    if (g_cfg.essenceBright < 0.f) g_cfg.essenceBright = 0.f;
    if (g_cfg.essenceBright > 1.5f) g_cfg.essenceBright = 1.5f;
}

// rate-limited debug line: at most `limit` lines per call site
#define DLOG(limit, ...) do { static volatile LONG c_; if (g_cfg.debug && InterlockedIncrement(&c_) <= (limit)) Log(__VA_ARGS__); } while (0)

// ----------------------------------------------------------------------------------------------
// engine layout
// ----------------------------------------------------------------------------------------------
constexpr size_t kActorMesh     = 0x104;
constexpr size_t kPawnSubMeshes = 0x3bc;
constexpr int    kNumSubMeshes  = 14;
constexpr size_t kInstMesh      = 0x60;
constexpr size_t kInstActor     = 0x64;
constexpr size_t kBoneData      = 0x208;
constexpr size_t kBoneNum       = 0x20c;
constexpr size_t kBoneStride    = 0x40;
constexpr size_t kPawnCloakSkins = 0x4a0;   // APawn::CloakSkins[2]  (what APawn::GetCloakSkin(i) returns)

const wchar_t* const kBodies[14] = { L"MFighter", L"FFighter", L"MMagic", L"FMagic", L"MElf", L"FElf", L"MDarkElf",
                                     L"FDarkElf", L"MDwarf", L"FDwarf", L"MOrc", L"FOrc", L"MShaman", L"FShaman" };

inline std::string kBodiesA(int b) { std::string s; if (b >= 0 && b < 14) for (const wchar_t* w = kBodies[b]; *w; ++w) s.push_back((char)*w); return s; }

// function pointer types (thiscall == fastcall with dummy edx)
using Render_t      = void (__fastcall*)(void* self, void* edx, void* actor, void* scene, void* lights, void* proj, void* ri);
using DrawSection_t = void (__fastcall*)(void* self, void* edx, void* actor, void* mesh, void* scene, const void* mat, void* proj, void* ri, int a, int b, int c);
using GetCloakMesh_t = void* (__fastcall*)(void* pawn, void* edx);
using CloakInst_t    = void* (__fastcall*)(void* mesh, void* edx, void* actor);
using SetSubIdx_t    = void (__fastcall*)(void* inst, void* edx, int idx);
using GetFullName_t  = const wchar_t* (__fastcall*)(void* obj, void* edx, wchar_t* buf);
using IsA_t          = int (__fastcall*)(void* obj, void* edx, void* cls);
using AddToRoot_t    = void (__fastcall*)(void* obj, void* edx);
using LoadObject_t   = void* (__cdecl*)(void* cls, void* outer, const wchar_t* name, const wchar_t* file, uint32_t flags, void* sandbox);
using RIFunc_t       = void (__fastcall*)(void* ri, void* edx);
// int USkeletalMeshInstance::SetBoneRotation(FName bone, FRotator rot /*3 ints by value*/, int space, float alpha)
using SetBoneRot_t   = int (__fastcall*)(void* inst, void* edx, uint32_t bone, int pitch, int yaw, int roll, int space, float alpha);

struct Api {
    Render_t       Render = nullptr;        // USkeletalMeshInstance::Render      (hooked)
    DrawSection_t  DrawSection = nullptr;   // USkeletalMeshInstance::DrawSection (hooked)
    Render_t       SubRender = nullptr;     // USubSkeletalMeshInstance::Render
    GetCloakMesh_t GetCloakMesh = nullptr;
    CloakInst_t    CloakMeshGetInstance = nullptr;
    SetSubIdx_t    SetSubMeshIndex = nullptr;
    GetFullName_t  GetFullName = nullptr;
    IsA_t          IsA = nullptr;
    AddToRoot_t    AddToRoot = nullptr;
    LoadObject_t   LoadObject = nullptr;
    SetBoneRot_t   SetBoneRotation = nullptr;
    void*          pawnClass = nullptr;
    void*          skelMeshClass = nullptr;
    void*          subVtbl = nullptr;
    void*          fnameCtor = nullptr;     // Core: FName::FName(const TCHAR*, EFindName)
    void*          fnameEntry = nullptr;    // Core: FName::GetEntry(int)
} g_api;

Render_t      g_origRender = nullptr;
DrawSection_t g_origDrawSection = nullptr;
// FSkinVertexStream::GetStreamData(void* dest): memcpy(dest, this+0x20, (*(int*)(this+0x24)) << 5)  -> 32-byte vertices
using GetStream_t = void (__fastcall*)(void* self, void* edx, void* dest);
GetStream_t   g_origGetStream = nullptr;
thread_local bool t_inCloak = false;                // true only while USubSkeletalMeshInstance::Render draws our cloak
volatile LONG g_disabled = 0;

// ----------------------------------------------------------------------------------------------
// per-render context (the Render hook publishes it, the DrawSection hook consumes it)
// ----------------------------------------------------------------------------------------------
struct RenderCtx {
    void* self; void* actor; void* scene; void* lights; void* proj; void* ri;
    void* cloak; int idx; bool drawn;
};
thread_local RenderCtx* t_ctx = nullptr;
thread_local bool t_busy = false;

// ----------------------------------------------------------------------------------------------
// helpers (POD only: they contain __try)
// ----------------------------------------------------------------------------------------------
inline char* P(void* p, size_t off) { return static_cast<char*>(p) + off; }

// "SkeletalMesh <pkg>[_NN].<Body>_Cloak_<id>"  ->  true and *body (index into kBodies)
bool ParseCloakName(const wchar_t* full, int* bodyOut, int* idOut = nullptr) {
    if (!full || wcsncmp(full, L"SkeletalMesh ", 13) != 0) return false;
    const wchar_t* p = full + 13;
    size_t pl = wcslen(g_cfg.package);
    if (wcsncmp(p, g_cfg.package, pl) != 0) return false;
    p += pl;
    if (p[0] == L'_' && iswdigit(p[1]) && iswdigit(p[2])) p += 3;   // LineShieldCloaks_01
    if (*p != L'.') return false;
    ++p;
    for (int b = 0; b < 14; ++b) {
        size_t bl = wcslen(kBodies[b]);
        if (wcsncmp(p, kBodies[b], bl) != 0 || wcsncmp(p + bl, L"_Cloak_", 7) != 0) continue;
        const wchar_t* d = p + bl + 7;
        int n = 0; long id = 0;
        while (iswdigit(d[n]) && n < 9) { id = id * 10 + (d[n] - L'0'); ++n; }
        if (n == 0 || d[n] != 0) return false;
        if (id < g_cfg.idMin || id > g_cfg.idMax) return false;
        if (bodyOut) *bodyOut = b;
        if (idOut) *idOut = (int)id;
        return true;
    }
    return false;
}

bool IsCloakMesh(void* mesh) {
    wchar_t buf[4096]; buf[0] = 0;
    const wchar_t* n = g_api.GetFullName(mesh, nullptr, buf);
    return ParseCloakName(n, nullptr);
}

// ---- materials: the cloak is drawn with the pawn's CloakSkins[2]; a real item fills them (armorgrp texture list),
// ---- the test mode has to take them from the mesh's own Materials array (found by scanning the object for a TArray)
uintptr_t g_modLo[2] = {}, g_modHi[2] = {};
bool InEngineModules(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return (v >= g_modLo[0] && v < g_modHi[0]) || (v >= g_modLo[1] && v < g_modHi[1]);
}
bool IsMaterialOfPackage(void* obj) {
    if (!obj || (reinterpret_cast<uintptr_t>(obj) & 3)) return false;
    if (!InEngineModules(*reinterpret_cast<void**>(obj))) return false;          // vtable must live in Engine.dll/Core.dll
    wchar_t buf[2048]; buf[0] = 0;
    const wchar_t* n = g_api.GetFullName(obj, nullptr, buf);
    return n && wcsstr(n, g_cfg.package) != nullptr;
}
bool FindMeshMaterials(void* mesh, void** out, size_t* foundOff = nullptr) {
    for (size_t o = 0x28; o < 0x300; o += 4) {
        __try {
            void** data = *reinterpret_cast<void***>(P(mesh, o));
            int num = *reinterpret_cast<int*>(P(mesh, o + 4));
            int max = *reinterpret_cast<int*>(P(mesh, o + 8));
            if (!data || num < 1 || num > 8 || max < num || max > 64 || (reinterpret_cast<uintptr_t>(data) & 3)) continue;
            bool ok = true;
            for (int i = 0; i < num && ok; ++i) ok = IsMaterialOfPackage(data[i]);
            if (!ok) continue;
            out[0] = data[0]; out[1] = num > 1 ? data[1] : data[0];
            if (foundOff) *foundOff = o;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return false;
}

// cheap per-frame revalidation of a cached result (the engine may free/reuse the mesh object)
bool MaterialsStillThere(void* mesh, size_t off, void* const* mats) {
    __try {
        void** data = *reinterpret_cast<void***>(P(mesh, off));
        int num = *reinterpret_cast<int*>(P(mesh, off + 4));
        if (!data || num < 1) return false;
        return data[0] == mats[0] && (num > 1 ? data[1] : data[0]) == mats[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// returns the 14-slot body index of `body` ("MFighter_m001_u" -> 0) or -1
int BodyIndexOf(void* body) {
    wchar_t buf[4096]; buf[0] = 0;
    const wchar_t* n = g_api.GetFullName(body, nullptr, buf);
    if (!n) return -1;
    const wchar_t* dot = wcsrchr(n, L'.');
    const wchar_t* obj = dot ? dot + 1 : n;
    for (int b = 0; b < 14; ++b) {
        size_t bl = wcslen(kBodies[b]);
        if (wcsncmp(obj, kBodies[b], bl) == 0 && obj[bl] == L'_') return b;
    }
    return -1;
}

// cloak skeleton must be a prefix (same names, same order) of the body skeleton
bool SkeletonCompatible(void* body, void* cloak) {
    uint32_t bn = *reinterpret_cast<uint32_t*>(P(body, kBoneNum));
    uint32_t cn = *reinterpret_cast<uint32_t*>(P(cloak, kBoneNum));
    if (bn < 0x14 || bn > 0xFF || cn < 0x14 || cn > bn) return false;
    const char* bd = *reinterpret_cast<const char**>(P(body, kBoneData));
    const char* cd = *reinterpret_cast<const char**>(P(cloak, kBoneData));
    if (!bd || !cd) return false;
    for (uint32_t i = 0; i < cn; ++i)
        if (*reinterpret_cast<const uint32_t*>(bd + i * kBoneStride) != *reinterpret_cast<const uint32_t*>(cd + i * kBoneStride))
            return false;
    return true;
}

void* PawnOf(void* fdynamicActor) {
    if (!fdynamicActor) return nullptr;
    void* a = *reinterpret_cast<void**>(fdynamicActor);
    if (!a) return nullptr;
    return g_api.IsA(a, nullptr, g_api.pawnClass) ? a : nullptr;
}

// test mode: load <pkg>.<Body>_Cloak_<forceId> for this body (cached)
void* ForcedCloak(void* body) {
    static void* cache[14]; static bool tried[14];
    if (g_cfg.forceId <= 0 || !g_api.LoadObject || !g_api.skelMeshClass || !body) return nullptr;
    int b = BodyIndexOf(body);
    if (b < 0) return nullptr;
    if (!tried[b]) {
        tried[b] = true;
        wchar_t nm[160];
        _snwprintf_s(nm, _TRUNCATE, L"%s.%s_Cloak_%d", g_cfg.package, kBodies[b], g_cfg.forceId);
        cache[b] = g_api.LoadObject(g_api.skelMeshClass, nullptr, nm, nullptr, 0, nullptr);
        if (cache[b] && g_api.AddToRoot) g_api.AddToRoot(cache[b], nullptr);
        Log("force-load %ls -> %p", nm, cache[b]);
    }
    return cache[b];
}

// ---- debug census: log what sits in a pawn's 14 sub-mesh slots / cloak skins whenever it changes (equip / unequip)
bool SafeName(void* obj, wchar_t* out, size_t n) {
    out[0] = 0;
    __try {
        if (!obj || (reinterpret_cast<uintptr_t>(obj) & 3) || !InEngineModules(*reinterpret_cast<void**>(obj))) return false;
        wchar_t buf[2048]; buf[0] = 0;
        const wchar_t* s = g_api.GetFullName(obj, nullptr, buf);
        if (!s) return false;
        wcsncpy_s(out, n, s, _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Census(void* pawn) {
    struct Seen { void* pawn; void* v[19]; };
    static Seen seen[48]; static int nseen = 0; static int lines = 0;
    if (lines > 400) return;
    void* cur[19] = {};
    __try {
        for (int i = 0; i < 14; ++i) cur[i] = reinterpret_cast<void**>(P(pawn, kPawnSubMeshes))[i];
        cur[14] = reinterpret_cast<void**>(P(pawn, kPawnCloakSkins))[0];
        cur[15] = reinterpret_cast<void**>(P(pawn, kPawnCloakSkins))[1];
        cur[16] = *reinterpret_cast<void**>(P(pawn, 0x42c));          // pawn's cached CloakMeshInstance
        cur[17] = *reinterpret_cast<void**>(P(pawn, kActorMesh));     // body mesh
        cur[18] = *reinterpret_cast<void**>(P(pawn, 0x3f4));          // CloakCoverMesh
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    Seen* s = nullptr;
    for (int i = 0; i < nseen; ++i) if (seen[i].pawn == pawn) { s = &seen[i]; break; }
    if (!s) { if (nseen >= 48) return; s = &seen[nseen++]; s->pawn = pawn; memset(s->v, 0xFF, sizeof s->v); }
    if (memcmp(s->v, cur, sizeof cur) == 0) return;
    memcpy(s->v, cur, sizeof cur);
    wchar_t nm[300];
    SafeName(cur[17], nm, 300);
    Log("census pawn=%p body=%ls CloakSkins=%p %p cloakInst@42c=%p cloakCover@3f4=%p", pawn, nm, cur[14], cur[15], cur[16], cur[18]); ++lines;
    for (int i = 0; i < 14; ++i) {
        if (!cur[i]) continue;
        SafeName(cur[i], nm, 300);
        Log("    slot[%2d]=%p %ls", i, cur[i], nm); ++lines;
    }
}

// ---- debug: look for IdMin..IdMax anywhere in a pawn and in the objects it points to (where does the client keep equipped item ids?)
bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const char* end = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const char*>(p) + n <= end;
}

void ScanIds(void* pawn) {
    static void* pawns[64]; static ULONGLONG last[64]; static int np = 0;
    static int lines = 0; static unsigned long long reported[400]; static int nrep = 0;
    int k = -1;
    for (int i = 0; i < np; ++i) if (pawns[i] == pawn) { k = i; break; }
    if (k < 0) { if (np >= 64) return; k = np++; pawns[k] = pawn; last[k] = 0; }
    ULONGLONG now = GetTickCount64();
    if (now - last[k] < 1500 || lines > 300) return;
    last[k] = now;
    const size_t kPawnBytes = 0x1800, kInner = 0x600;
    if (!Readable(pawn, kPawnBytes)) return;
    const uint32_t* pw = static_cast<const uint32_t*>(pawn);
    unsigned nPtr = 0, nRead = 0;
    for (size_t i = 0; i < kPawnBytes / 4; ++i) {
        uint32_t v = pw[i];
        if ((v & 3) == 0 && v >= 0x10000 && v < 0x7ff00000) { ++nPtr; if (Readable(reinterpret_cast<const void*>(v), kInner)) ++nRead; }
        if (v >= (uint32_t)g_cfg.idMin && v <= (uint32_t)g_cfg.idMax) {
            unsigned long long key = ((unsigned long long)(uintptr_t)pawn << 24) ^ (i * 4 + 1) ^ ((unsigned long long)v << 40);
            bool seen = false; for (int r = 0; r < nrep; ++r) if (reported[r] == key) { seen = true; break; }
            if (!seen && nrep < 400) { reported[nrep++] = key; Log("idscan pawn=%p +0x%x = %u", pawn, (unsigned)(i * 4), v); ++lines; }
        }
        if ((v & 3) == 0 && v >= 0x10000 && v < 0x7ff00000 && Readable(reinterpret_cast<const void*>(v), kInner)) {
            const uint32_t* q = reinterpret_cast<const uint32_t*>(v);
            for (size_t j = 0; j < kInner / 4; ++j) {
                uint32_t w = q[j];
                if (w >= (uint32_t)g_cfg.idMin && w <= (uint32_t)g_cfg.idMax) {
                    unsigned long long key = ((unsigned long long)(uintptr_t)pawn << 24) ^ ((i * 4) << 12) ^ (j * 4 + 2) ^ ((unsigned long long)w << 40);
                    bool seen = false; for (int r = 0; r < nrep; ++r) if (reported[r] == key) { seen = true; break; }
                    if (!seen && nrep < 400) { reported[nrep++] = key; Log("idscan pawn=%p *(+0x%x)=%p +0x%x = %u", pawn, (unsigned)(i * 4), reinterpret_cast<void*>(v), (unsigned)(j * 4), w); ++lines; }
                }
            }
        }
    }
    static int summaries = 0;
    if (summaries < 6) { ++summaries; Log("idscan summary pawn=%p: scanned %u bytes, %u pointers (%u readable) for ids %d..%d", pawn, (unsigned)kPawnBytes, nPtr, nRead, g_cfg.idMin, g_cfg.idMax); }
}

// ---- experiment: drive the pawn's Cape_Bone procedurally (the game itself does this for the spine in APawn::SpineRotation)
constexpr size_t kPawnCapeBoneName = 0x44c;      // APawn::CapeBoneName (FName index), see APawn::GetCapeBoneName
void ApplyCapeTest(void* bodyInst, void* pawn) {
    uint32_t cape = *reinterpret_cast<uint32_t*>(P(pawn, kPawnCapeBoneName));
    float t = (float)(GetTickCount64() % 3600000ULL) / 1000.0f;
    float deg = sinf(t * 6.2831853f * g_cfg.capeFreq) * g_cfg.capeAmp;
    int units = (int)(deg * (65536.0f / 360.0f));
    int r[3] = { 0, 0, 0 };
    r[g_cfg.capeAxis < 0 || g_cfg.capeAxis > 2 ? 0 : g_cfg.capeAxis] = units;
    int ret = g_api.SetBoneRotation(bodyInst, nullptr, cape, r[0], r[1], r[2], g_cfg.capeSpace, 1.0f);
    DLOG(6, "cape test: bone name idx=%u rot=(%d,%d,%d) space=%d -> returned %d", cape, r[0], r[1], r[2], g_cfg.capeSpace, ret);
}

// find the cloak mesh of a pawn: sub-mesh slots first, then the engine's own GetCloakMesh, then test mode
void* FindCloak(void* pawn, int* idx) {
    *idx = -1;
    __try {
        void** slots = reinterpret_cast<void**>(P(pawn, kPawnSubMeshes));
        for (int i = 0; i < kNumSubMeshes; ++i) {
            if (slots[i] && IsCloakMesh(slots[i])) { *idx = i; return slots[i]; }
        }
        void* m = g_api.GetCloakMesh(pawn, nullptr);
        if (m && IsCloakMesh(m)) return m;
        return ForcedCloak(*reinterpret_cast<void**>(P(pawn, kActorMesh)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---- rigid-section cloak: every section is drawn with ONE matrix (bone x mesh-to-world), passed to DrawSection as `const FMatrix&`.
// Multiplying a rotation about the cloak's top edge into that matrix swings the whole cloak like a pendulum.
thread_local float t_cloakBBox[6];                  // local-space bounding box of the cloak being drawn (min xyz, max xyz)
thread_local bool  t_cloakBBoxOk = false;

// FBox sits somewhere in the first bytes of the mesh object: min[3], max[3], IsValid byte. Found by scanning (and logged).
bool FindBBox(void* mesh, float* out6) {
    for (size_t o = 0x20; o < 0x240; o += 4) {
        __try {
            const float* f = reinterpret_cast<const float*>(P(mesh, o));
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i) ok = f[i] < f[i + 3] && fabsf(f[i]) < 5000.0f && fabsf(f[i + 3]) < 5000.0f;
            if (!ok || *reinterpret_cast<const uint8_t*>(P(mesh, o + 24)) != 1) continue;
            if ((f[3] - f[0]) < 4.0f && (f[4] - f[1]) < 4.0f && (f[5] - f[2]) < 4.0f) continue;
            memcpy(out6, f, 24);
            Log("cloak bbox at +0x%x: min(%.1f %.1f %.1f) max(%.1f %.1f %.1f)", (unsigned)o, f[0], f[1], f[2], f[3], f[4], f[5]);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return false;
}

static void Mul4(const float* A, const float* B, float* C) {      // C = A x B, row-major 4x4 (row-vector convention: v' = v x M)
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += A[i * 4 + k] * B[k * 4 + j];
            C[i * 4 + j] = s;
        }
}

static void AxisRot(int axis, float rad, float* R) {                // 4x4 rotation about a local axis (row-vector convention)
    float c = cosf(rad), s = sinf(rad);
    const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    memcpy(R, I, sizeof I);
    if (axis == 0)      { R[5] = c; R[6] = s; R[9] = -s; R[10] = c; }
    else if (axis == 1) { R[0] = c; R[2] = -s; R[8] = s; R[10] = c; }
    else                { R[0] = c; R[1] = s; R[4] = -s; R[5] = c; }
}

// out = L x M with L = T(-P) x Ra x Rb x T(P): two rotations about pivot P, expressed in M's input (local) space
static void SwayMatrix2(const float* M, int axisA, float radA, int axisB, float radB, const float* P3, float* out) {
    float Ra[16], Rb[16];
    AxisRot(axisA, radA, Ra); AxisRot(axisB, radB, Rb);
    const float Tm[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, -P3[0],-P3[1],-P3[2],1 };
    const float Tp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,  P3[0], P3[1], P3[2],1 };
    float A[16], B[16], L[16];
    Mul4(Tm, Ra, A); Mul4(A, Rb, B); Mul4(B, Tp, L); Mul4(L, M, out);
}

// ---- per-character spring state (keyed by the sub-mesh instance)
constexpr int kHist = 96;                                                       // spring-angle history (ring) used to delay the hem
struct SwayState { void* inst; bool init; ULONGLONG last; float pos[3]; float yaw; float th[2]; float om[2]; float phase; float vs[2]; float osm; int frames;
                   float rphase; double ht[kHist]; float hth[kHist][2]; int hhead, hn;
                   // cloth simulation inputs, all in the cloak's local axes (x lateral, y forward, z up), and the displacement field itself
                   float vs3[3], al3[3], alpha, dtf; bool newFrame; float* cd; float* cv; int cnp; const void* cmesh;
                   float Alin[9], Ainv[9]; bool haveA; };                       // pose of the cloak's bone relative to its rest pose (fitted from the engine's vertices)
static SwayState g_sway[48];
static int g_nsway = 0;
static SwayState* SwayFor(void* inst) {
    for (int i = 0; i < g_nsway; ++i) if (g_sway[i].inst == inst) return &g_sway[i];
    SwayState* s = (g_nsway < 48) ? &g_sway[g_nsway++] : &g_sway[0];
    memset(s, 0, sizeof *s);
    s->inst = inst;
    s->phase = (float)((reinterpret_cast<uintptr_t>(inst) >> 4) % 628) / 100.0f;
    return s;
}
static float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static double NowSec() { return (double)GetTickCount64() * 0.001; }

static void SwayPush(SwayState* s, double t) {
    s->ht[s->hhead] = t; s->hth[s->hhead][0] = s->th[0]; s->hth[s->hhead][1] = s->th[1];
    s->hhead = (s->hhead + 1) % kHist;
    if (s->hn < kHist) ++s->hn;
}

// spring angles (degrees) as they were `ago` seconds in the past (linear interpolation in the history ring)
static void SwayPast(const SwayState* s, double now, double ago, float* a0, float* a1) {
    if (!s || s->hn == 0) { *a0 = *a1 = 0.f; return; }
    const double target = now - ago;
    const int newest = (s->hhead + kHist - 1) % kHist;
    int oldest = newest;
    for (int j = 0; j < s->hn; ++j) {
        const int i = (newest + kHist - j) % kHist;
        oldest = i;
        if (s->ht[i] <= target) {
            if (i == newest) { *a0 = s->hth[i][0]; *a1 = s->hth[i][1]; return; }
            const int nx = (i + 1) % kHist;
            const double span = s->ht[nx] - s->ht[i];
            const float f = span > 1e-6 ? (float)((target - s->ht[i]) / span) : 0.f;
            *a0 = s->hth[i][0] + (s->hth[nx][0] - s->hth[i][0]) * f;
            *a1 = s->hth[i][1] + (s->hth[nx][1] - s->hth[i][1]) * f;
            return;
        }
    }
    *a0 = s->hth[oldest][0]; *a1 = s->hth[oldest][1];                  // older than anything stored
}

// Advances the per-character spring (once per rendered frame) from the cloak's MeshToWorld matrix: position -> velocity, rotation -> turning.
static SwayState* StepSway(void* inst, const float* M) {
    const float kPi = 3.14159265f;
    SwayState* s = SwayFor(inst);
    s->newFrame = false;
    const ULONGLONG now = GetTickCount64();
    const float yaw = atan2f(M[1], M[0]);
    if (!s->init) { s->init = true; s->last = now; s->pos[0] = M[12]; s->pos[1] = M[13]; s->pos[2] = M[14]; s->yaw = yaw; }
    const float dt = (float)(now - s->last) * 0.001f;
    if (dt < 0.004f) return s;                                         // several calls per frame share one step
    if (dt > 0.25f) {                                                  // stall / teleport: just resync
        s->last = now; s->pos[0] = M[12]; s->pos[1] = M[13]; s->pos[2] = M[14]; s->yaw = yaw; s->om[0] = s->om[1] = 0.f;
        SwayPush(s, NowSec());
        return s;
    }
    float vw[3] = { (M[12] - s->pos[0]) / dt, (M[13] - s->pos[1]) / dt, (M[14] - s->pos[2]) / dt };
    if (sqrtf(vw[0] * vw[0] + vw[1] * vw[1]) > 900.f) vw[0] = vw[1] = vw[2] = 0.f;     // implausible jump
    float vl[3];                                                        // world velocity -> the cloak's local axes
    for (int j = 0; j < 3; ++j) {
        const float r0 = M[j * 4], r1 = M[j * 4 + 1], r2 = M[j * 4 + 2];
        float n2 = r0 * r0 + r1 * r1 + r2 * r2; if (n2 < 1e-6f) n2 = 1.f;
        vl[j] = (vw[0] * r0 + vw[1] * r1 + vw[2] * r2) / n2;
    }
    float dyaw = yaw - s->yaw;
    while (dyaw > kPi) dyaw -= 2.f * kPi;
    while (dyaw < -kPi) dyaw += 2.f * kPi;
    const float omega = Clampf(dyaw / dt, -12.f, 12.f);

    const int f = g_cfg.swayFwd, l = 1 - f;                             // forward / lateral local axes
    const float kf = (f == 1 ? -1.f : 1.f) * g_cfg.swayFwdSign;         // hem trails backwards when running forwards
    const float kr = (f == 1 ? 1.f : -1.f) * g_cfg.swaySideSign;
    // low-pass the measurements (tau ~ 60 ms) and ignore the first frames of a character (spawn / position jumps)
    ++s->frames;
    float rvf = vl[f], rvl = vl[l], rom = omega;
    if (s->frames <= 2) { rvf = rvl = rom = 0.f; }
    const float a = 1.f - expf(-dt / 0.06f);
    s->vs[0] += (Clampf(rvf, -450.f, 450.f) - s->vs[0]) * a;
    s->vs[1] += (Clampf(rvl, -450.f, 450.f) - s->vs[1]) * a;
    const float osmOld = s->osm;
    s->osm   += (rom - s->osm) * a;
    for (int j = 0; j < 3; ++j) {                                       // local velocity / acceleration / yaw acceleration for the cloth
        const float old = s->vs3[j];
        s->vs3[j] += ((s->frames <= 2 ? 0.f : Clampf(vl[j], -450.f, 450.f)) - old) * a;
        s->al3[j] = Clampf((s->vs3[j] - old) / dt, -2500.f, 2500.f);
    }
    s->alpha = Clampf((s->osm - osmOld) / dt, -80.f, 80.f);
    s->dtf = dt; s->newFrame = true;
    const float tgt[2] = { Clampf(kf * g_cfg.swayKv * s->vs[0], -g_cfg.swayMax, g_cfg.swayMax),
                           Clampf(kr * g_cfg.swayKs * s->vs[1] + g_cfg.swayTurnSign * g_cfg.swayKw * s->osm, -g_cfg.swayMax, g_cfg.swayMax) };

    float rest = dt; const float w0 = g_cfg.swayStiff, z = g_cfg.swayDamp;
    while (rest > 0.f) {                                                // semi-implicit Euler, small steps
        const float h = rest > 0.016f ? 0.016f : rest; rest -= h;
        for (int k = 0; k < 2; ++k) {
            const float acc = w0 * w0 * (tgt[k] - s->th[k]) - 2.f * z * w0 * s->om[k];
            s->om[k] += acc * h; s->th[k] += s->om[k] * h;
            s->th[k] = Clampf(s->th[k], -g_cfg.swayMax, g_cfg.swayMax);
        }
    }
    s->rphase += dt * (3.0f + Clampf(sqrtf(s->vs[0] * s->vs[0] + s->vs[1] * s->vs[1]) / 60.f, 0.f, 8.f));
    s->last = now; s->pos[0] = M[12]; s->pos[1] = M[13]; s->pos[2] = M[14]; s->yaw = yaw;
    SwayPush(s, NowSec());
    static ULONGLONG lastSwayLog = 0; static int swayLogs = 0;
    if (g_cfg.debug && swayLogs < 400 && now - lastSwayLog >= 400 && (fabsf(s->vs[0]) > 3.f || fabsf(s->osm) > 0.05f || fabsf(s->th[0]) > 3.f)) {
        lastSwayLog = now; ++swayLogs;
        Log("sway %p: v_fwd=%.0f v_lat=%.0f turn=%.2f rad/s -> target(%.1f, %.1f) deg, angle(%.1f, %.1f)", inst, s->vs[0], s->vs[1], s->osm, tgt[0], tgt[1], s->th[0], s->th[1]);
    }
    return s;
}

// returns true and fills out[16] when the whole-cloak matrix sway applies (MatSway=1; off by default: it rotates the glued top edge too)
bool ApplyMatSway(void* inst, const void* mat, float* out) {
    if (!g_cfg.matSway || !mat || !t_cloakBBoxOk) return false;
    const float* M = static_cast<const float*>(mat);
    const int up = (g_cfg.swayUp >= 0 && g_cfg.swayUp <= 2) ? g_cfg.swayUp : 2;
    float piv[3] = { (t_cloakBBox[0] + t_cloakBBox[3]) * 0.5f, (t_cloakBBox[1] + t_cloakBBox[4]) * 0.5f, (t_cloakBBox[2] + t_cloakBBox[5]) * 0.5f };
    piv[up] = t_cloakBBox[3 + up];                                         // top edge
    const float kDeg = 0.0174532925f, kPi = 3.14159265f;

    if (!g_cfg.swayPhysics) {                                              // plain sine, for calibration
        float t = (float)(GetTickCount64() % 3600000ULL) / 1000.0f;
        float rad = sinf(t * 2.f * kPi * g_cfg.swayFreq) * g_cfg.swayAmp * kDeg;
        int ax = (g_cfg.swayAxis >= 0 && g_cfg.swayAxis <= 2) ? g_cfg.swayAxis : 0;
        SwayMatrix2(M, ax, rad, ax, 0.f, piv, out);
        return true;
    }
    SwayState* s = StepSway(inst, M);
    const float t = (float)(GetTickCount64() % 3600000ULL) / 1000.0f;
    const float idle = g_cfg.swayIdle * sinf(t * 2.f * kPi * 0.7f + s->phase);
    const int f = g_cfg.swayFwd, l = 1 - f;
    SwayMatrix2(M, l, (s->th[0] + idle) * kDeg, f, s->th[1] * kDeg, piv, out);   // pitch about the lateral axis, roll about the forward axis
    return true;
}

// ----------------------------------------------------------------------------------------------
// Direct3D 9: bend the cloak by rewriting its vertex buffer right before the draw call.
// Cloak sections are rigid (100% weighted to bip01_spine2), so the engine never deforms them. The vertices are in mesh space
// (z up, y forward, x lateral): the top edge stays glued to the body and the hem is displaced by the spring pose of that
// character, delayed along the height so the motion travels down the cloth like a wave.
// ----------------------------------------------------------------------------------------------
struct ClothMesh;
struct MeshInfo { void* mesh; float bb[6]; bool bbOk; float yTop; bool yTopOk; ClothMesh* cm; };
static MeshInfo g_mi[32];
static int g_nmi = 0;
static MeshInfo* InfoFor(void* mesh) {
    for (int i = 0; i < g_nmi; ++i) if (g_mi[i].mesh == mesh) return &g_mi[i];
    if (g_nmi >= 32) return nullptr;
    MeshInfo* m = &g_mi[g_nmi++];
    memset(m, 0, sizeof *m);
    m->mesh = mesh;
    m->bbOk = FindBBox(mesh, m->bb);
    return m;
}
thread_local MeshInfo*  t_mi = nullptr;                // mesh being drawn
thread_local SwayState* t_pose = nullptr;              // spring state of the character being drawn

using DP_t      = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DIP_t     = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DPUP_t    = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DIPUP_t   = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using SetVD_t   = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using SetFVF_t  = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD);
using SetSS_t   = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
using CreateDevice_t = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDevice_t g_origCreateDevice = nullptr;

enum { kVtDP = 81, kVtDIP = 82, kVtDPUP = 83, kVtDIPUP = 84, kVtSetVD = 87, kVtSetFVF = 89, kVtSetSS = 100 };
struct DevHooks { void** vt; void* dp; void* dip; void* dpup; void* dipup; void* setvd; void* setfvf; void* setss; };
static DevHooks g_dh[4];
static int g_ndh = 0;
static DevHooks* DH(IDirect3DDevice9* d) {
    void** vt = *reinterpret_cast<void***>(d);
    for (int i = 0; i < g_ndh; ++i) if (g_dh[i].vt == vt) return &g_dh[i];
    return nullptr;
}

// state the engine last bound (tracked ourselves: a PURE device does not support the Get* calls)
static IDirect3DVertexBuffer9*      g_s0vb = nullptr;
static UINT                         g_s0off = 0, g_s0stride = 0;
static IDirect3DVertexDeclaration9* g_decl = nullptr;
static DWORD                        g_fvf = 0;

struct VBEntry { IDirect3DVertexBuffer9* vb; UINT start, len, stride; BYTE* orig; bool bad, normals;
                 BYTE* last; bool haveLast; unsigned uploads, repeats; };      // cloth mode: orig = the engine's latest pose of these vertices, last = what we wrote
static VBEntry g_vbe[64];
static int g_nvbe = 0;

static bool Plausible(const BYTE* p, UINT n, UINT stride, const MeshInfo* mi, bool* normals) {
    for (UINT i = 0; i < n; ++i) {
        const float* v = reinterpret_cast<const float*>(p + (size_t)i * stride);
        for (int k = 0; k < 3; ++k)
            if (!(v[k] > mi->bb[k] - 2.0f && v[k] < mi->bb[3 + k] + 2.0f)) return false;     // also rejects NaN
    }
    *normals = false;
    if (stride >= 24) {
        bool ok = true;
        for (UINT i = 0; i < n && i < 16 && ok; ++i) {
            const float* v = reinterpret_cast<const float*>(p + (size_t)i * stride);
            const float l2 = v[3] * v[3] + v[4] * v[4] + v[5] * v[5];
            ok = l2 > 0.8f && l2 < 1.25f;
        }
        *normals = ok;
    }
    return true;
}

static void DeformVerts(const BYTE* src, BYTE* dst, UINT n, UINT stride, MeshInfo* mi, const SwayState* s, bool normals) {
    const float zTop = mi->bb[5], zBot = mi->bb[2], H = zTop - zBot;
    if (H < 1.0f) return;
    const float xC = (mi->bb[0] + mi->bb[3]) * 0.5f;
    if (!mi->yTopOk) {                                                  // pivot depth = mean y of the top row of vertices
        float sy = 0.f; int c = 0;
        for (UINT i = 0; i < n; ++i) { const float* v = reinterpret_cast<const float*>(src + (size_t)i * stride); if (v[2] >= zTop - 0.1f * H) { sy += v[1]; ++c; } }
        if (c >= 3) { mi->yTop = sy / c; mi->yTopOk = true; Log("cloak top-edge pivot y=%.2f (%d vertices), x centre %.2f, z %.2f..%.2f", mi->yTop, c, xC, zBot, zTop); }
    }
    const float yP = mi->yTopOk ? mi->yTop : (mi->bb[1] + mi->bb[4]) * 0.5f;
    const double now = NowSec();
    const float kDeg = 0.0174532925f, k2Pi = 6.2831853f;
    const float speed = s ? sqrtf(s->vs[0] * s->vs[0] + s->vs[1] * s->vs[1]) : 0.f;
    const float rippleA = g_cfg.rippleAmp * (0.3f + Clampf(speed / 250.f, 0.f, 1.f) * 1.4f);
    const float rphase = s ? s->rphase : 0.f, phase0 = s ? s->phase : 0.f;
    for (UINT i = 0; i < n; ++i) {
        const BYTE* sv = src + (size_t)i * stride; BYTE* dv = dst + (size_t)i * stride;
        memcpy(dv, sv, stride);
        const float* sp = reinterpret_cast<const float*>(sv); float* dp = reinterpret_cast<float*>(dv);
        const float x = sp[0], y = sp[1], z = sp[2];
        const float h = Clampf((zTop - z) / H, 0.f, 1.f);
        if (h < 0.004f) continue;                                       // the top edge is glued to the body
        const float w = powf(h, g_cfg.clothPow);
        const double ago = (double)(g_cfg.clothDelay * h);
        float a0, a1;
        if (g_cfg.vbMode == 0) { a0 = g_cfg.vbAmp * (float)sin(6.283185307 * (double)g_cfg.vbFreq * (now - ago)); a1 = 0.f; }   // calibration: pure sine
        else {
            SwayPast(s, now, ago, &a0, &a1);
            a0 += g_cfg.swayIdle * (float)sin(4.39822972 * (now - ago) + (double)phase0);
        }
        const float a = a0 * w * kDeg, b = a1 * w * kDeg;
        const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        // pitch about the lateral axis (x) through the top edge:  y' = y c - z s,  z' = y s + z c   (hem goes +y for a > 0)
        const float dy = y - yP, dz = z - zTop;
        const float y1 = yP + dy * ca - dz * sa, z1 = zTop + dy * sa + dz * ca;
        // roll about the forward axis (y) through the top edge:  x' = x c + z s,  z' = -x s + z c
        const float dx = x - xC, dz1 = z1 - zTop;
        const float x2 = xC + dx * cb + dz1 * sb, z2 = zTop - dx * sb + dz1 * cb;
        // ripples travelling down the cloth, growing with speed
        const float ripple = rippleA * h * h * sinf(k2Pi * (x - xC) / g_cfg.rippleLen + rphase - 2.5f * h);
        dp[0] = x2; dp[1] = y1 + ripple; dp[2] = z2;
        if (normals) {
            const float nx = sp[3], ny = sp[4], nz = sp[5];
            const float ny1 = ny * ca - nz * sa, nz1 = ny * sa + nz * ca;
            dp[3] = nx * cb + nz1 * sb; dp[4] = ny1; dp[5] = -nx * sb + nz1 * cb;
        }
    }
}

// ---- cloth: a displacement field d(p) over the welded vertices of the cloak (added to the rest positions in mesh space).
// Every particle has a spring back to its rest position (stiff at the top edge, loose at the hem) and is coupled to the neighbours
// given by the mesh triangles (so folds and waves travel across the cloth). It is driven by the character's motion seen from the
// cloak's local frame: inertia (-acceleration), air drag (-velocity), yaw acceleration / centrifugal force while turning and a gentle gust.
// Cloth does not stretch: every particle is tethered to its anchor on the top edge and may not get farther from it than at rest
// (+3%), so when the hem swings backwards it also rises, along an arc.
constexpr int kMaxParticles = 2048;
constexpr float kRefSpacing = 3.55f;                                    // the edge length the default ClothKN was tuned at (offline test grid)
struct ClothMesh {
    IDirect3DVertexBuffer9* vb; UINT nverts, stride;
    bool ready;
    float hsMax;                                                        // largest stable integration step
    int np;
    int*   v2p;                                                         // vertex -> particle
    float* rest;                                                        // np * 3
    float* hgt;                                                         // np: 0 at the top edge, 1 at the hem
    float* kk;                                                          // np: spring stiffness back to rest
    float* cc;                                                          // np: damping
    bool*  pin;                                                         // np: glued to the body
    int*   anchor;                                                      // np: pinned particle the tether is attached to
    float* rho;                                                         // np: rest distance to the anchor
    int ne, necap; int* ea; int* eb; float* ew;                         // links (triangle edges) and their stiffness
    unsigned* secKey; int nsec;                                         // index ranges already turned into links
    BYTE* orig;                                                         // nverts * stride pristine vertices
};

static int CmpKey(const void* a, const void* b) {
    const int* x = static_cast<const int*>(a); const int* y = static_cast<const int*>(b);       // 4 ints: qx qy qz idx
    for (int k = 0; k < 3; ++k) if (x[k] != y[k]) return x[k] < y[k] ? -1 : 1;
    return x[3] < y[3] ? -1 : (x[3] > y[3] ? 1 : 0);
}

// Reads the whole vertex buffer of the cloak (one buffer per mesh) and builds the particles; returns nullptr when the layout is not what we expect.
static ClothMesh* BuildCloth(MeshInfo* mi, IDirect3DVertexBuffer9* vb, UINT stride) {
    D3DVERTEXBUFFER_DESC d;
    if (!vb || !mi || !mi->bbOk || stride < 12 || stride > 64 || FAILED(vb->GetDesc(&d))) return nullptr;
    const UINT n = d.Size / stride;
    if (n < 8 || n > 20000) return nullptr;
    BYTE* p = nullptr;
    if (FAILED(vb->Lock(0, n * stride, reinterpret_cast<void**>(&p), 0)) || !p) { Log("cloth: cannot lock the whole vertex buffer"); return nullptr; }
    BYTE* orig = static_cast<BYTE*>(malloc((size_t)n * stride));
    if (orig) memcpy(orig, p, (size_t)n * stride);
    vb->Unlock();
    if (!orig) return nullptr;

    UINT inside = 0;
    float zMax = -1e30f, zMin = 1e30f;
    for (UINT i = 0; i < n; ++i) {
        const float* v = reinterpret_cast<const float*>(orig + (size_t)i * stride);
        bool ok = true;
        for (int k = 0; k < 3; ++k) ok = ok && v[k] > mi->bb[k] - 2.0f && v[k] < mi->bb[3 + k] + 2.0f;
        inside += ok;
        if (v[2] > zMax) zMax = v[2];
        if (v[2] < zMin) zMin = v[2];
    }
    if (inside < n * 99 / 100) { Log("cloth: only %u of %u vertices lie inside the mesh bbox - layout not understood", inside, n); free(orig); return nullptr; }
    const float H = zMax - zMin;
    if (H < 5.f) { free(orig); return nullptr; }

    // weld vertices that share a position (UV seams) into one particle
    int (*keys)[4] = static_cast<int(*)[4]>(malloc(sizeof(int) * 4 * n));
    int* v2p = static_cast<int*>(malloc(sizeof(int) * n));
    if (!keys || !v2p) { free(orig); free(keys); free(v2p); return nullptr; }
    int np = 0; static int firstOf[kMaxParticles + 1];
    float weldRes = 100.f;                                              // grid of 1/100 unit; coarser when the buffer has too many distinct positions
    for (int attempt = 0; attempt < 4; ++attempt, weldRes *= 0.4f) {
        for (UINT i = 0; i < n; ++i) {
            const float* v = reinterpret_cast<const float*>(orig + (size_t)i * stride);
            keys[i][0] = (int)floorf(v[0] * weldRes + 0.5f); keys[i][1] = (int)floorf(v[1] * weldRes + 0.5f); keys[i][2] = (int)floorf(v[2] * weldRes + 0.5f); keys[i][3] = (int)i;
        }
        qsort(keys, n, sizeof keys[0], CmpKey);
        np = 0;
        bool tooMany = false;
        for (UINT i = 0; i < n && !tooMany; ++i) {
            const bool same = i > 0 && keys[i][0] == keys[i - 1][0] && keys[i][1] == keys[i - 1][1] && keys[i][2] == keys[i - 1][2];
            if (!same) { if (np >= kMaxParticles) { tooMany = true; break; } firstOf[np++] = keys[i][3]; }
            v2p[keys[i][3]] = np - 1;
        }
        if (!tooMany) break;
        np = 0;
    }
    free(keys);
    if (np == 0) { Log("cloth: more than %d distinct positions even with a coarse weld - not simulated", kMaxParticles); free(orig); free(v2p); return nullptr; }

    ClothMesh* cm = static_cast<ClothMesh*>(calloc(1, sizeof(ClothMesh)));
    cm->vb = vb; cm->nverts = n; cm->stride = stride; cm->np = np; cm->v2p = v2p; cm->orig = orig;
    cm->rest = static_cast<float*>(malloc(sizeof(float) * 3 * np)); cm->hgt = static_cast<float*>(malloc(sizeof(float) * np));
    cm->kk = static_cast<float*>(malloc(sizeof(float) * np)); cm->cc = static_cast<float*>(malloc(sizeof(float) * np)); cm->pin = static_cast<bool*>(calloc(np, 1));
    cm->anchor = static_cast<int*>(malloc(sizeof(int) * np)); cm->rho = static_cast<float*>(malloc(sizeof(float) * np));
    int pinned = 0;
    for (int q = 0; q < np; ++q) {
        const float* v = reinterpret_cast<const float*>(orig + (size_t)firstOf[q] * stride);
        cm->rest[q * 3] = v[0]; cm->rest[q * 3 + 1] = v[1]; cm->rest[q * 3 + 2] = v[2];
        const float h = Clampf((zMax - v[2]) / H, 0.f, 1.f);            // measured on the real vertices, not on the bbox
        cm->hgt[q] = h;
        cm->pin[q] = h < g_cfg.clothPin; pinned += cm->pin[q];
        const float om = 1.f - h;
        cm->kk[q] = g_cfg.clothKHem + (g_cfg.clothKTop - g_cfg.clothKHem) * om * om;
        cm->cc[q] = 2.f * g_cfg.clothZeta * sqrtf(cm->kk[q] + 4.f * g_cfg.clothKN);
    }
    if (pinned == 0) {                                                  // never leave the cloak un-glued: pin the topmost particle(s)
        float best = 1e30f; int bq = 0;
        for (int q = 0; q < np; ++q) if (cm->hgt[q] < best) { best = cm->hgt[q]; bq = q; }
        for (int q = 0; q < np; ++q) if (cm->hgt[q] <= best + 0.02f) { cm->pin[q] = true; ++pinned; }
        (void)bq;
    }
    // tether anchors: the nearest pinned particle (3D, at rest) and the rest distance to it
    for (int q = 0; q < np; ++q) {
        float best = 1e30f; int bi = q;
        for (int a = 0; a < np; ++a) if (cm->pin[a]) {
            const float dx = cm->rest[q * 3] - cm->rest[a * 3], dy = cm->rest[q * 3 + 1] - cm->rest[a * 3 + 1], dz = cm->rest[q * 3 + 2] - cm->rest[a * 3 + 2];
            const float dd = dx * dx + dy * dy + dz * dz; if (dd < best) { best = dd; bi = a; }
        }
        cm->anchor[q] = bi; cm->rho[q] = sqrtf(best) > 1e-3f ? sqrtf(best) : 1e-3f;
    }
    cm->hsMax = 1.f / 60.f;
    cm->ready = false;                                                  // becomes true once the triangles (links) are known
    Log("cloth: %u vertices -> %d particles (%d pinned, %.0f%% of the height from the top: z %.1f..%.1f); waiting for the index buffer", n, np, pinned, g_cfg.clothPin * 100.f, zMin, zMax);
    return cm;
}

// Turns one index range (a section of the cloak) into links between particles; recomputes the link stiffness and the stable step.
static void AddSectionLinks(IDirect3DDevice9* dev, ClothMesh* cm, UINT startIdx, UINT prims, INT base) {
    if (!cm || prims == 0 || prims > 20000) return;
    const unsigned key = startIdx * 31u + prims;
    for (int i = 0; i < cm->nsec; ++i) if (cm->secKey[i] == key) return;
    if (cm->nsec >= 32) return;
    cm->secKey = static_cast<unsigned*>(realloc(cm->secKey, sizeof(unsigned) * (cm->nsec + 1)));
    cm->secKey[cm->nsec++] = key;

    IDirect3DIndexBuffer9* ib = nullptr;
    if (FAILED(dev->GetIndices(&ib)) || !ib) { Log("cloth: GetIndices failed"); return; }
    D3DINDEXBUFFER_DESC d;
    if (FAILED(ib->GetDesc(&d))) { ib->Release(); return; }
    const UINT isz = d.Format == D3DFMT_INDEX32 ? 4 : 2;
    BYTE* ip = nullptr;
    if (FAILED(ib->Lock(startIdx * isz, prims * 3 * isz, reinterpret_cast<void**>(&ip), 0)) || !ip) { Log("cloth: cannot lock the index buffer (usage 0x%x pool %d)", (unsigned)d.Usage, (int)d.Pool); ib->Release(); return; }

    static unsigned char linked[kMaxParticles][kMaxParticles / 8];      // dedupe of the links already present
    memset(linked, 0, sizeof linked);
    for (int e = 0; e < cm->ne; ++e) { const int a = cm->ea[e], b = cm->eb[e]; linked[a][b >> 3] |= (unsigned char)(1 << (b & 7)); }
    int added = 0;
    for (UINT t = 0; t < prims; ++t) {
        int pv[3];
        for (int k = 0; k < 3; ++k) {
            const UINT ix = isz == 4 ? reinterpret_cast<const UINT*>(ip)[t * 3 + k] : (UINT)reinterpret_cast<const unsigned short*>(ip)[t * 3 + k];
            const INT vi = base + (INT)ix;
            pv[k] = (vi >= 0 && (UINT)vi < cm->nverts) ? cm->v2p[vi] : -1;
        }
        for (int k = 0; k < 3; ++k) {
            const int u = pv[k], w = pv[(k + 1) % 3];
            if (u < 0 || w < 0 || u == w) continue;
            const int a = u < w ? u : w, b = u < w ? w : u;
            if (linked[a][b >> 3] & (1 << (b & 7))) continue;
            linked[a][b >> 3] |= (unsigned char)(1 << (b & 7));
            if (cm->ne >= cm->necap) {
                cm->necap = cm->necap ? cm->necap * 2 : 4096;
                cm->ea = static_cast<int*>(realloc(cm->ea, sizeof(int) * cm->necap)); cm->eb = static_cast<int*>(realloc(cm->eb, sizeof(int) * cm->necap));
                cm->ew = static_cast<float*>(realloc(cm->ew, sizeof(float) * cm->necap));
            }
            cm->ea[cm->ne] = a; cm->eb[cm->ne] = b; ++cm->ne; ++added;
        }
    }
    ib->Unlock(); ib->Release();

    // link stiffness ~ (reference edge length / edge length)^2: the cloth behaves the same whatever the mesh resolution
    static float S[kMaxParticles];
    memset(S, 0, sizeof(float) * cm->np);
    float lmin = 1e30f, lmax = 0.f, wmin = 1e30f, wmax = 0.f;
    for (int e = 0; e < cm->ne; ++e) {
        const int a = cm->ea[e], b = cm->eb[e];
        const float dx = cm->rest[a * 3] - cm->rest[b * 3], dy = cm->rest[a * 3 + 1] - cm->rest[b * 3 + 1], dz = cm->rest[a * 3 + 2] - cm->rest[b * 3 + 2];
        float len = sqrtf(dx * dx + dy * dy + dz * dz); if (len < 0.05f) len = 0.05f;
        const float r = kRefSpacing / len;
        const float w = Clampf(g_cfg.clothKN * r * r, 0.5f, 1500.f);
        cm->ew[e] = w; S[a] += w; S[b] += w;
        if (len < lmin) lmin = len; if (len > lmax) lmax = len; if (w < wmin) wmin = w; if (w > wmax) wmax = w;
    }
    float smax = 0.f; for (int q = 0; q < cm->np; ++q) if (S[q] > smax) smax = S[q];
    cm->hsMax = Clampf(0.9f / sqrtf(2.f * smax + g_cfg.clothKTop), 1.f / 600.f, 1.f / 60.f);
    cm->ready = cm->ne > 0;
    Log("cloth: index range start=%u prims=%u -> +%d links (total %d, edge length %.2f..%.2f, stiffness %.0f..%.0f), step <= %.1f ms, %s",
        startIdx, prims, added, cm->ne, lmin, lmax, wmin, wmax, cm->hsMax * 1000.f, cm->ready ? "simulating" : "no links yet");
}

// advance the displacement field of one character by the frame time measured in StepSway
static void ClothStep(SwayState* s, ClothMesh* cm) {
    if (!s || !cm || !cm->ready || !s->newFrame) return;
    const int np = cm->np;
    if (s->cmesh != cm || s->cnp != np || !s->cd || !s->cv) {
        free(s->cd); free(s->cv);
        s->cd = static_cast<float*>(calloc(np * 3, sizeof(float))); s->cv = static_cast<float*>(calloc(np * 3, sizeof(float)));
        s->cnp = np; s->cmesh = cm;
        if (!s->cd || !s->cv) { free(s->cd); free(s->cv); s->cd = s->cv = nullptr; s->cnp = 0; return; }
    }
    float* d = s->cd; float* v = s->cv;
    float dt = s->dtf; if (dt > 0.05f) dt = 0.05f;
    if (dt > 12.f * cm->hsMax) dt = 12.f * cm->hsMax;                  // never integrate with a step the stiffest link cannot take
    int steps = (int)ceilf(dt / cm->hsMax); if (steps < 1) steps = 1; if (steps > 12) steps = 12;
    const float hs = dt / (float)steps;
    static float acc[kMaxParticles * 3];
    static float vsum[kMaxParticles * 3];
    static float ncnt[kMaxParticles];
    static const ClothMesh* ncntFor = nullptr; static int ncntNe = -1;
    if (ncntFor != cm || ncntNe != cm->ne) {                            // neighbour counts for the velocity smoothing (links grow while the sections are first drawn)
        memset(ncnt, 0, sizeof(float) * np);
        for (int e = 0; e < cm->ne; ++e) { ncnt[cm->ea[e]] += 1.f; ncnt[cm->eb[e]] += 1.f; }
        ncntFor = cm; ncntNe = cm->ne;
    }

    const float w = Clampf(s->osm, -6.f, 6.f), alpha = s->alpha;
    const float speed = sqrtf(s->vs3[0] * s->vs3[0] + s->vs3[1] * s->vs3[1]);
    const float gustAmp = g_cfg.clothGust * (0.25f + Clampf(speed / 160.f, 0.f, 1.f));
    const double t = NowSec();
    const float ph = s->phase;
    const float gx = (float)sin(6.283185307 * 0.55 * t + (double)ph), gy = (float)sin(6.283185307 * 0.8 * t + (double)ph * 1.7);
    const float inert = g_cfg.clothInertia, dragY = g_cfg.clothDrag, dragX = g_cfg.clothDrag * 0.4f, dragZ = g_cfg.clothDrag * 0.075f;
    const float inertZ = inert * 0.25f;                                 // vertical bobbing must not throw the hem around
    const float dMax = g_cfg.clothMax, turn = g_cfg.clothTurn, zMax = g_cfg.clothZMax, tetherEps = g_cfg.clothTether;
    const float beta = Clampf(g_cfg.clothSmooth * hs, 0.f, 0.5f);

    // the driving quantities were measured along the actor's axes; the cloak's own frame is rotated by the bone pose (kneeling, leaning...)
    float vc[3] = { s->vs3[0], s->vs3[1], s->vs3[2] }, ac[3] = { s->al3[0], s->al3[1], s->al3[2] }, gl[3] = { 0.f, 0.f, -1.f };
    if (s->haveA) {
        const float* M = s->Ainv; const float up[3] = { 0.f, 0.f, -1.f };
        for (int i = 0; i < 3; ++i) {
            vc[i] = M[i * 3] * s->vs3[0] + M[i * 3 + 1] * s->vs3[1] + M[i * 3 + 2] * s->vs3[2];
            ac[i] = M[i * 3] * s->al3[0] + M[i * 3 + 1] * s->al3[1] + M[i * 3 + 2] * s->al3[2];
            gl[i] = M[i * 3] * up[0] + M[i * 3 + 1] * up[1] + M[i * 3 + 2] * up[2];
        }
        const float gn = sqrtf(gl[0] * gl[0] + gl[1] * gl[1] + gl[2] * gl[2]);
        if (gn > 1e-3f) { gl[0] /= gn; gl[1] /= gn; gl[2] /= gn; } else { gl[0] = 0.f; gl[1] = 0.f; gl[2] = -1.f; }
    }
    const float acx = Clampf(ac[0], -900.f, 900.f), acy = Clampf(ac[1], -900.f, 900.f), acz = Clampf(ac[2], -900.f, 900.f);
    const float gravF[3] = { g_cfg.clothGravity * gl[0], g_cfg.clothGravity * gl[1], g_cfg.clothGravity * (gl[2] + 1.f) };   // zero while the torso is upright

    for (int st = 0; st < steps; ++st) {
        for (int q = 0; q < np; ++q) {
            const float* r = &cm->rest[q * 3];
            const float dx = d[q * 3], dy = d[q * 3 + 1], dz = d[q * 3 + 2], vx = v[q * 3], vy = v[q * 3 + 1], vz = v[q * 3 + 2];
            const float h = cm->hgt[q];
            float ax = -cm->kk[q] * dx - cm->cc[q] * vx;
            float ay = -cm->kk[q] * dy - cm->cc[q] * vy;
            float az = -cm->kk[q] * dz - cm->cc[q] * vz;
            // driving terms; their size grows with the lever arm (distance to the top edge), so a short cloak moves by the same angle as a long one
            const float ks = Clampf(cm->rho[q] / 50.f, 0.2f, 1.5f);
            float fx = -inert * acx - dragX * vc[0] + gravF[0];         // the body accelerates / moves: the cloth lags and the air pushes it back
            float fy = -inert * acy - dragY * vc[1] + gravF[1];
            float fz = -inertZ * acz - dragZ * vc[2] + gravF[2];
            const float px = r[0], py = r[1];                           // rotating frame: Euler + centrifugal + Coriolis
            fx += turn * (alpha * py + w * w * px + 2.f * w * vy);
            fy += turn * (-alpha * px + w * w * py - 2.f * w * vx);
            const float gh = gustAmp * (0.3f + h);                      // gusts grow towards the hem and travel across the width
            fy += gh * (gy * 0.7f + 0.3f * (float)sin(0.12 * (double)px + 3.0 * (double)h + 6.283185307 * 0.8 * t));
            fx += gh * 0.35f * gx;
            ax += ks * fx; ay += ks * fy; az += ks * fz;
            acc[q * 3] = ax; acc[q * 3 + 1] = ay; acc[q * 3 + 2] = az;
        }
        for (int e = 0; e < cm->ne; ++e) {                              // coupling to the neighbours: spring on the displacement
            const int a = cm->ea[e], b = cm->eb[e];
            const float wgt = cm->ew[e];
            for (int k = 0; k < 3; ++k) {
                const float f = wgt * (d[b * 3 + k] - d[a * 3 + k]);
                acc[a * 3 + k] += f; acc[b * 3 + k] -= f;
            }
        }
        for (int q = 0; q < np * 3; ++q) v[q] += acc[q] * hs;
        if (beta > 0.f) {                                               // velocity smoothing: stable damping of the relative motion of neighbours
            memset(vsum, 0, sizeof(float) * np * 3);
            for (int e = 0; e < cm->ne; ++e) { const int a = cm->ea[e], b = cm->eb[e]; for (int k = 0; k < 3; ++k) { vsum[a * 3 + k] += v[b * 3 + k]; vsum[b * 3 + k] += v[a * 3 + k]; } }
            for (int q = 0; q < np; ++q) if (ncnt[q] > 0.f) for (int k = 0; k < 3; ++k) v[q * 3 + k] += beta * (vsum[q * 3 + k] / ncnt[q] - v[q * 3 + k]);
        }
        for (int q = 0; q < np; ++q) {
            if (cm->pin[q]) { d[q * 3] = d[q * 3 + 1] = d[q * 3 + 2] = 0.f; v[q * 3] = v[q * 3 + 1] = v[q * 3 + 2] = 0.f; continue; }
            for (int k = 0; k < 3; ++k) d[q * 3 + k] += v[q * 3 + k] * hs;
            if (d[q * 3 + 1] > 1.5f) { d[q * 3 + 1] = 1.5f; if (v[q * 3 + 1] > 0.f) v[q * 3 + 1] = 0.f; }       // the body is in front of the cloak: it cannot go through the back
            // tether: no farther from the anchor on the top edge than at rest (+eps); the outward velocity is removed
            const int an = cm->anchor[q];
            const float vx0 = cm->rest[q * 3] + d[q * 3] - cm->rest[an * 3], vy0 = cm->rest[q * 3 + 1] + d[q * 3 + 1] - cm->rest[an * 3 + 1], vz0 = cm->rest[q * 3 + 2] + d[q * 3 + 2] - cm->rest[an * 3 + 2];
            const float len = sqrtf(vx0 * vx0 + vy0 * vy0 + vz0 * vz0), lim = cm->rho[q] * (1.f + tetherEps);
            if (len > lim && len > 1e-4f) {
                const float k = lim / len;
                d[q * 3] = cm->rest[an * 3] + vx0 * k - cm->rest[q * 3];
                d[q * 3 + 1] = cm->rest[an * 3 + 1] + vy0 * k - cm->rest[q * 3 + 1];
                d[q * 3 + 2] = cm->rest[an * 3 + 2] + vz0 * k - cm->rest[q * 3 + 2];
                const float nx = vx0 / len, ny = vy0 / len, nz = vz0 / len;
                const float vr = v[q * 3] * nx + v[q * 3 + 1] * ny + v[q * 3 + 2] * nz;
                if (vr > 0.f) { v[q * 3] -= vr * nx; v[q * 3 + 1] -= vr * ny; v[q * 3 + 2] -= vr * nz; }
            }
            if (d[q * 3 + 2] > zMax) { d[q * 3 + 2] = zMax; if (v[q * 3 + 2] > 0.f) v[q * 3 + 2] = 0.f; }
            if (d[q * 3 + 2] < -zMax) { d[q * 3 + 2] = -zMax; if (v[q * 3 + 2] < 0.f) v[q * 3 + 2] = 0.f; }
            const float l2 = d[q * 3] * d[q * 3] + d[q * 3 + 1] * d[q * 3 + 1] + d[q * 3 + 2] * d[q * 3 + 2];
            if (l2 > dMax * dMax) { const float sc = dMax / sqrtf(l2); d[q * 3] *= sc; d[q * 3 + 1] *= sc; d[q * 3 + 2] *= sc; v[q * 3] *= 0.5f; v[q * 3 + 1] *= 0.5f; v[q * 3 + 2] *= 0.5f; }
        }
    }
    static ULONGLONG lastLog = 0; static int logs = 0;
    const ULONGLONG now = GetTickCount64();
    if (g_cfg.debug && logs < 300 && now - lastLog >= 500) {
        float mx = 0.f; int mq = 0; float stretch = 0.f;
        for (int q = 0; q < np; ++q) {
            const float l = fabsf(d[q * 3]) + fabsf(d[q * 3 + 1]) + fabsf(d[q * 3 + 2]); if (l > mx) { mx = l; mq = q; }
            if (!cm->pin[q] && cm->rho[q] > 8.f) {
                const int an = cm->anchor[q];
                const float ex = cm->rest[q * 3] + d[q * 3] - cm->rest[an * 3], ey = cm->rest[q * 3 + 1] + d[q * 3 + 1] - cm->rest[an * 3 + 1], ez = cm->rest[q * 3 + 2] + d[q * 3 + 2] - cm->rest[an * 3 + 2];
                const float sr = sqrtf(ex * ex + ey * ey + ez * ez) / cm->rho[q]; if (sr > stretch) stretch = sr;
            }
        }
        if (mx > 0.3f || speed > 5.f) {
            lastLog = now; ++logs;
            Log("cloth %p: v_local=(%.0f %.0f %.0f) a_local=(%.0f %.0f %.0f) turn=%.2f/%.1f | max displacement %.1f at particle %d (h=%.2f) d=(%.1f %.1f %.1f) | max stretch %.3f",
                s->inst, s->vs3[0], s->vs3[1], s->vs3[2], s->al3[0], s->al3[1], s->al3[2], s->osm, s->alpha, mx, mq, cm->hgt[mq], d[mq * 3], d[mq * 3 + 1], d[mq * 3 + 2], stretch);
        }
    }
}


static bool Inv3(const double* m, double* o) {
    const double c00 = m[4] * m[8] - m[5] * m[7], c01 = m[5] * m[6] - m[3] * m[8], c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (fabs(det) < 1e-12) return false;
    const double id = 1.0 / det;
    o[0] = c00 * id; o[1] = (m[2] * m[7] - m[1] * m[8]) * id; o[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    o[3] = c01 * id; o[4] = (m[0] * m[8] - m[2] * m[6]) * id; o[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    o[6] = c02 * id; o[7] = (m[1] * m[6] - m[0] * m[7]) * id; o[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return true;
}

// The engine re-skins the cloak every frame (rigid on one bone), so its vertices are  p = A r + t  with r the rest position and A the
// bone pose (rotation x scale). Least-squares fit of A over a sample of the range; `resid` is the largest distance left over.
static bool FitLinear(const ClothMesh* cm, const BYTE* cur, UINT first, UINT n, UINT stride, float* A, float* resid) {
    const UINT step = n > 96 ? n / 96 : 1;
    double rc[3] = { 0, 0, 0 }, pc[3] = { 0, 0, 0 }; int m = 0;
    for (UINT i = 0; i < n; i += step) {
        const UINT idx = first + i; if (idx >= cm->nverts) break;
        const float* r = reinterpret_cast<const float*>(cm->orig + (size_t)idx * stride); const float* p = reinterpret_cast<const float*>(cur + (size_t)i * stride);
        for (int k = 0; k < 3; ++k) { rc[k] += r[k]; pc[k] += p[k]; }
        ++m;
    }
    if (m < 6) return false;
    for (int k = 0; k < 3; ++k) { rc[k] /= m; pc[k] /= m; }
    double Mrr[9] = { 0 }, Mpr[9] = { 0 };
    for (UINT i = 0; i < n; i += step) {
        const UINT idx = first + i; if (idx >= cm->nverts) break;
        const float* r = reinterpret_cast<const float*>(cm->orig + (size_t)idx * stride); const float* p = reinterpret_cast<const float*>(cur + (size_t)i * stride);
        double dr[3] = { r[0] - rc[0], r[1] - rc[1], r[2] - rc[2] }, dp[3] = { p[0] - pc[0], p[1] - pc[1], p[2] - pc[2] };
        for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) { Mrr[a * 3 + b] += dr[a] * dr[b]; Mpr[a * 3 + b] += dp[a] * dr[b]; }
    }
    const double lam = 1e-4 * (Mrr[0] + Mrr[4] + Mrr[8]) + 1e-6;       // regularise the thin direction of the cloak
    Mrr[0] += lam; Mrr[4] += lam; Mrr[8] += lam;
    double inv[9]; if (!Inv3(Mrr, inv)) return false;
    double Ad[9];
    for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) Ad[a * 3 + b] = Mpr[a * 3] * inv[b] + Mpr[a * 3 + 1] * inv[3 + b] + Mpr[a * 3 + 2] * inv[6 + b];
    double worst = 0;
    for (UINT i = 0; i < n; i += step) {
        const UINT idx = first + i; if (idx >= cm->nverts) break;
        const float* r = reinterpret_cast<const float*>(cm->orig + (size_t)idx * stride); const float* p = reinterpret_cast<const float*>(cur + (size_t)i * stride);
        double e2 = 0;
        for (int a = 0; a < 3; ++a) {
            const double fit = pc[a] + Ad[a * 3] * (r[0] - rc[0]) + Ad[a * 3 + 1] * (r[1] - rc[1]) + Ad[a * 3 + 2] * (r[2] - rc[2]);
            e2 += (fit - p[a]) * (fit - p[a]);
        }
        if (e2 > worst) worst = e2;
    }
    for (int k = 0; k < 9; ++k) A[k] = (float)Ad[k];
    *resid = (float)sqrt(worst);
    return true;
}

// the cloth code assumes the stream layout seen in the logs: float3 position @0, float3 normal @12, float2 uv @24 (stride 32)
static bool DeclIsPosNormUV(IDirect3DVertexDeclaration9* decl, UINT stride) {
    static IDirect3DVertexDeclaration9* okFor = nullptr; static IDirect3DVertexDeclaration9* badFor = nullptr;
    if (!decl || stride != 32) return false;
    if (decl == okFor) return true;
    if (decl == badFor) return false;
    D3DVERTEXELEMENT9 el[32]; UINT cnt = 0;
    bool ok = SUCCEEDED(decl->GetDeclaration(el, &cnt)) && cnt >= 4;
    ok = ok && el[0].Stream == 0 && el[0].Offset == 0 && el[0].Type == D3DDECLTYPE_FLOAT3 && el[0].Usage == D3DDECLUSAGE_POSITION;
    ok = ok && el[1].Stream == 0 && el[1].Offset == 12 && el[1].Type == D3DDECLTYPE_FLOAT3 && el[1].Usage == D3DDECLUSAGE_NORMAL;
    ok = ok && el[2].Stream == 0 && el[2].Offset == 24 && el[2].Type == D3DDECLTYPE_FLOAT2 && el[2].Usage == D3DDECLUSAGE_TEXCOORD;
    if (ok) okFor = decl; else { badFor = decl; Log("cloth: vertex declaration %p is not position/normal/uv - not touched", decl); }
    return ok;
}

// out = engine pose of the vertices + A * displacement (the displacement is defined in the cloak's rest frame)
static void ApplyCloth(const ClothMesh* cm, const SwayState* s, UINT first, UINT n, UINT stride, const BYTE* engine, BYTE* out, const float* A) {
    const bool have = s && s->cmesh == cm && s->cd && s->cnp == cm->np;
    for (UINT i = 0; i < n; ++i) {
        const UINT idx = first + i;
        if (idx >= cm->nverts) break;
        const BYTE* sv = engine + (size_t)i * stride; BYTE* dv = out + (size_t)i * stride;
        if (!have) { if (dv != sv) memcpy(dv, sv, stride); continue; }
        memcpy(dv, sv, stride);
        const float* sp = reinterpret_cast<const float*>(sv); float* dp = reinterpret_cast<float*>(dv);
        const float* dd = &s->cd[cm->v2p[idx] * 3];
        dp[0] = sp[0] + A[0] * dd[0] + A[1] * dd[1] + A[2] * dd[2];
        dp[1] = sp[1] + A[3] * dd[0] + A[4] * dd[1] + A[5] * dd[2];
        dp[2] = sp[2] + A[6] * dd[0] + A[7] * dd[1] + A[8] * dd[2];
    }
}

static void LogVBFirst(const char* what, IDirect3DVertexBuffer9* vb, UINT start, UINT len, UINT stride, UINT n, const BYTE* p, const MeshInfo* mi) {
    D3DVERTEXBUFFER_DESC d; memset(&d, 0, sizeof d);
    if (vb) vb->GetDesc(&d);
    const float* v = reinterpret_cast<const float*>(p);
    Log("cloak VB %s: vb=%p start=%u len=%u stride=%u n=%u | desc: usage=0x%x pool=%d size=%u fvf=0x%x | v0=(%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f) bbox z %.1f..%.1f",
        what, vb, start, len, stride, n, (unsigned)d.Usage, (int)d.Pool, (unsigned)d.Size, (unsigned)d.FVF, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], mi->bb[2], mi->bb[5]);
}

static void LogDecl(IDirect3DVertexDeclaration9* decl) {
    if (!decl) return;
    D3DVERTEXELEMENT9 el[32]; UINT cnt = 0;
    if (FAILED(decl->GetDeclaration(el, &cnt))) return;
    char buf[512]; int o = 0; buf[0] = 0;
    for (UINT i = 0; i < cnt && i < 32 && el[i].Stream != 0xFF && o < 440; ++i)
        o += _snprintf_s(buf + o, sizeof buf - o, _TRUNCATE, " [s%u +%u type=%u usage=%u/%u]", el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage, el[i].UsageIndex);
    Log("cloak vertex declaration %p:%s", decl, buf);
}

// in place: rewrite the vertices [base+minIdx, +numV) of the bound stream 0 from a pristine copy
static void DeformVBRange(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* vb, UINT off, UINT stride, INT base, UINT minIdx, UINT numV, UINT startIdx, UINT primCount) {
    MeshInfo* mi = t_mi;
    if (!vb || !mi || !mi->bbOk || stride < 12 || stride > 64 || numV == 0 || numV > 20000) return;
    const INT first = base + (INT)minIdx;
    if (first < 0) return;
    const UINT start = off + (UINT)first * stride, len = numV * stride;
    if (g_cfg.vbMode == 2) {                                           // cloth: the whole buffer is one cloak mesh
        D3DVERTEXBUFFER_DESC dd;
        if (FAILED(vb->GetDesc(&dd)) || start + len > dd.Size || off != 0) return;
        if (!DeclIsPosNormUV(g_decl, stride)) return;
        ClothMesh* cm = mi->cm;
        if (cm && (cm->vb != vb || (cm->nverts * cm->stride) != dd.Size)) { cm = nullptr; mi->cm = nullptr; }   // the buffer was re-created
        if (!cm) {
            // the rest shape is taken from the first frame in which the cloak stands in its bind pose (checked against the mesh bbox)
            static ULONGLONG lastTry = 0; static int attempts = 0;
            const ULONGLONG now = GetTickCount64();
            if (attempts >= 400 || now - lastTry < 500) return;
            lastTry = now; ++attempts;
            cm = BuildCloth(mi, vb, stride);
            mi->cm = cm;
            if (!cm) return;
        }
        AddSectionLinks(dev, cm, startIdx, primCount, base);            // the triangles of every section drawn so far are the cloth's connectivity
        VBEntry* ce = nullptr;                                          // per range: the engine's latest pose (orig) and what we wrote last (last)
        for (int i = 0; i < g_nvbe; ++i) if (g_vbe[i].vb == vb && g_vbe[i].start == start && g_vbe[i].len == len) { ce = &g_vbe[i]; break; }
        if (!ce) { if (g_nvbe >= 64) return; ce = &g_vbe[g_nvbe++]; memset(ce, 0, sizeof *ce); ce->vb = vb; ce->start = start; ce->len = len; ce->stride = stride; }
        if (ce->bad) return;
        BYTE* q = nullptr;
        if (FAILED(vb->Lock(start, len, reinterpret_cast<void**>(&q), 0)) || !q) return;
        __try {
            if (!ce->orig) { ce->orig = static_cast<BYTE*>(malloc(len)); ce->last = static_cast<BYTE*>(malloc(len)); }
            if (!ce->orig || !ce->last) ce->bad = true;
            else {
                // a fresh upload by the engine (new pose) differs from what we wrote; identical bytes mean a second pass of the same frame
                const bool fresh = !ce->haveLast || memcmp(q, ce->last, len) != 0;
                if (fresh) { memcpy(ce->orig, q, len); ++ce->uploads; } else ++ce->repeats;
                float A[9], resid = 0.f;
                const float I3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                const bool fit = FitLinear(cm, ce->orig, (UINT)first, numV, stride, A, &resid);
                if (fit && t_pose && resid < 2.0f) {
                    memcpy(t_pose->Alin, A, sizeof A);
                    double Ad[9], Ai[9]; for (int k = 0; k < 9; ++k) Ad[k] = A[k];
                    if (Inv3(Ad, Ai)) { for (int k = 0; k < 9; ++k) t_pose->Ainv[k] = (float)Ai[k]; t_pose->haveA = true; }
                }
                DLOG(30, "cloth pose: range first=%d n=%u uploads=%u repeats=%u fit=%d resid=%.3f A=[%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f]",
                     (int)first, numV, ce->uploads, ce->repeats, (int)fit, resid, A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[8]);
                ApplyCloth(cm, t_pose, (UINT)first, numV, stride, ce->orig, q, (fit && resid < 2.0f) ? A : I3);
                memcpy(ce->last, q, len); ce->haveLast = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_cfg.vbDeform = false; Log("exception while writing the cloth vertices - VBDeform disabled"); }
        vb->Unlock();
        return;
    }
    VBEntry* e = nullptr;
    for (int i = 0; i < g_nvbe; ++i) if (g_vbe[i].vb == vb && g_vbe[i].start == start && g_vbe[i].len == len) { e = &g_vbe[i]; break; }
    if (!e) {
        if (g_nvbe >= 64) return;
        e = &g_vbe[g_nvbe++]; memset(e, 0, sizeof *e); e->vb = vb; e->start = start; e->len = len; e->stride = stride;
    }
    if (e->bad) return;
    D3DVERTEXBUFFER_DESC d;
    if (FAILED(vb->GetDesc(&d)) || start + len > d.Size) { e->bad = true; Log("cloak VB: range %u+%u outside buffer (size %u)", start, len, (unsigned)d.Size); return; }
    BYTE* p = nullptr;
    const HRESULT hr = vb->Lock(start, len, reinterpret_cast<void**>(&p), 0);
    if (FAILED(hr) || !p) { e->bad = true; Log("cloak VB: Lock failed hr=0x%08x (usage=0x%x pool=%d)", (unsigned)hr, (unsigned)d.Usage, (int)d.Pool); return; }
    __try {
        bool normals = false;
        if (!e->orig) {
            LogVBFirst("first sight", vb, start, len, stride, numV, p, mi);
            if (!Plausible(p, numV, stride, mi, &normals)) { e->bad = true; Log("cloak VB: vertices do not look like mesh-space positions inside the bbox - not touching it"); }
            else {
                e->orig = static_cast<BYTE*>(malloc(len));
                if (e->orig) { memcpy(e->orig, p, len); e->normals = normals; Log("cloak VB: captured %u vertices (stride %u, normals %d)", numV, stride, (int)normals); }
            }
        } else if (stride > 24) {                                       // the buffer may have been re-created: the non-position bytes must still match
            bool same = true;
            for (UINT i = 0; i < numV && same; ++i) same = memcmp(p + (size_t)i * stride + 24, e->orig + (size_t)i * stride + 24, stride - 24) == 0;
            if (!same) {
                bool nrm = false;
                if (Plausible(p, numV, stride, mi, &nrm)) { memcpy(e->orig, p, len); e->normals = nrm; Log("cloak VB: contents changed, re-captured"); }
                else { e->bad = true; free(e->orig); e->orig = nullptr; }
            }
        }
        if (e->orig && !e->bad) DeformVerts(e->orig, p, numV, stride, mi, t_pose, e->normals);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cfg.vbDeform = false;
        Log("exception while deforming the cloak vertex buffer - VBDeform disabled");
    }
    vb->Unlock();
}

// user-memory draws: copy the used vertices to a scratch buffer, deform the copy, and draw from it
static HRESULT DrawUPDeformed(DevHooks* h, IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minV, UINT numV, UINT primCount,
                              const void* idx, D3DFORMAT fmt, const void* verts, UINT stride) {
    static thread_local BYTE scratch[1 << 16]; static thread_local BYTE pristine[1 << 16];
    MeshInfo* mi = t_mi;
    const void* use = verts;
    if (verts && mi && mi->bbOk && stride >= 12 && stride <= 64 && (size_t)numV * stride <= sizeof scratch) {
        __try {
            const BYTE* src = static_cast<const BYTE*>(verts) + (size_t)minV * stride;
            bool normals = false;
            static volatile LONG once = 0;
            if (InterlockedExchange(&once, 1) == 0) LogVBFirst("user-memory vertices", nullptr, 0, numV * stride, stride, numV, src, mi);
            if (Plausible(src, numV, stride, mi, &normals)) {
                memcpy(pristine, src, (size_t)numV * stride);
                DeformVerts(pristine, scratch, numV, stride, mi, t_pose, normals);
                use = scratch - (size_t)minV * stride;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_cfg.vbDeform = false; Log("exception in user-memory cloak deformation - VBDeform disabled"); use = verts; }
    }
    return reinterpret_cast<DIPUP_t>(h->dipup)(dev, type, minV, numV, primCount, idx, fmt, use, stride);
}

// ----------------------------------------------------------------------------------------------
// Essence mantles: baked animated cloaks (tools/build_capes.py) played from the pawn's own animation state and drawn with Direct3D 9.
//   * the carrier cloak (LineShieldCloaks.<Body>_Cloak_<id>) still goes through the engine so the world matrix / pawn bookkeeping are
//     right; its draw calls are suppressed and the mantle is drawn instead, with its own textures and vertex/index buffers
//   * the pawn's animation channel table sits at instance+0x184 (count +0x188, 0x70 bytes per channel: FName sequence @+8, float frame @+0x10)
//   * the mantle's skeleton is animated in actor space (its root bone carries the body-following), so no extra attachment is needed
// ----------------------------------------------------------------------------------------------
using FNameCtor_t = void (__fastcall*)(int* self, void* edx, const wchar_t* s, int findType);
using FNameEntry_t = void* (__cdecl*)(int idx);
ess::Pack g_pack;
volatile LONG g_packState = 0;                          // 0 = not loaded, 1 = ready, -1 = failed

struct EssMantle {
    const ess::Mantle* m = nullptr; ess::Anim* anim = nullptr;
    std::vector<int> amap;
    int bodyIdx = -1;                                      // index into kBodies
    std::vector<float> snugW;                              // per point: how much of EssenceSnug it gets (1 at the shoulders, 0 near the hem)
    std::vector<const std::vector<ess::FxLayer>*> fxProg; bool hasEnv = false;       // effect layers of every material (null = none)
    std::vector<float> idleL; bool idleReady = false;      // local (root frame) coordinates of every point in the idle pose (reference of the cloth guide)
    float normSign = 1.f;                                  // -1 when the winding of the mesh gives normals that point towards the body
    bool fnamesReady = false;
    IDirect3DDevice9* dev = nullptr; IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DTexture9* tex[8] = {}; bool gpuOk = false; int gpuFails = 0;                 // tex[] points into g_gpuTex (shared, not owned)
    // attachment to the real torso: which engine array holds the bone coords and how its axes map to a rotation (auto-calibrated against the baked root)
    void* spineMesh = nullptr; int spineIdx = -1;
    void* spineCacheMesh[8] = {}; int spineCacheIdx[8] = {}; int nSpineCache = 0;     // Spine2 bone index per body mesh
};
struct EssCal { double angSum[2][8] = {}; int angN = 0; int attSrc = -1, attConv = -1; bool attFailed = false; int attLogs = 0; };
static EssCal g_cal;
static EssMantle g_em[256];
static int g_nem = 0;

struct EssPawn {
    void* inst = nullptr; const ess::Anim* animKey = nullptr; bool init = false; int seq = -1; ULONGLONG last = 0; float blendT = 1.f; ULONGLONG lastLog = 0; int logs = 0; float lastFrame = -9.f; ULONGLONG frameSince = 0;
    ess::Pose cur, from;
    ess::PoseOffset off; float offW = 0.f;                // inertialization: decaying offset between the pose we were showing and the new animation
    float swRp[9] = {}; bool swHave = false; ULONGLONG swLast = 0; float swA[3] = {}, swV[3] = {}, swWind = 0.f; float swPhase = 0.f;   // secondary motion: torso frame of the last update, hem swing angles about the torso x/y/z axes
    bool twOn = false; float tw0 = 0.f;                   // engine tween in progress: AnimFrame went negative at tw0 and climbs to 0; `from` is the pose we showed when it started
};
static EssPawn g_ep[48];
static int g_nep = 0;

static volatile LONG g_badId[16]; static volatile LONG g_nBadId = 0; static volatile LONG g_excCount = 0;
thread_local int t_curId = 0;                               // id of the cloak being drawn (for the exception handler)
static bool EssIsBadId(int id) { for (LONG i = 0; i < g_nBadId && i < 16; ++i) if (g_badId[i] == id) return true; return false; }
thread_local EssMantle* t_essM = nullptr;               // mantle being drawn for this cloak render (null = use the carrier)
thread_local EssPawn*   t_essP = nullptr;
thread_local bool       t_essDone = false, t_essOk = false;
thread_local EssMantle* t_wingM = nullptr;              // rigid wings drawn together with a cloth cloak (design 16)
thread_local EssPawn*   t_wingP = nullptr;
struct ClothPawn;
struct FxPawn;
thread_local FxPawn* t_fxP = nullptr;                     // particle effect of the cloak being drawn (null = none)
thread_local ClothPawn* t_clothP = nullptr;             // cloth of the standard cloak being drawn for this render (null = none)
// diagnostics: what happened to every cloak render (a summary line every few seconds + the reason of every failure, rate limited)
static volatile LONG g_stPrep = 0, g_stPrepMantle = 0, g_stPrepCloth = 0, g_stPrepNone = 0, g_stDip = 0, g_stEssTry = 0, g_stEssOk = 0, g_stClothTry = 0, g_stClothOk = 0, g_stCarrier = 0, g_stFxDrawn = 0, g_stFxSkipped = 0, g_stFxFailed = 0, g_fxDrawn = 0, g_fxFail = 0;
static void EssWhy(const char* what, int a = 0, int b = 0) {
    static volatile LONG n = 0;
    if (InterlockedIncrement(&n) <= 60) Log("essence: draw failed - %s (%d, %d)", what, a, b);
}
static void EssStats() {
    static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
    if (now - last < 5000) return;
    last = now;
    Log("essence stats: prepare %ld (mantle %ld, cloth %ld, carrier only %ld) | cloak DIP calls %ld: mantle draws %ld ok of %ld, cloth draws %ld ok of %ld, carrier passes %ld",
        g_stPrep, g_stPrepMantle, g_stPrepCloth, g_stPrepNone, g_stDip, g_stEssOk, g_stEssTry, g_stClothOk, g_stClothTry, g_stCarrier);
    Log("essence fx: %ld layer passes drawn, %ld skipped (texture missing), %ld failed | particle batches %ld drawn, %ld failed", g_stFxDrawn, g_stFxSkipped, g_stFxFailed, g_fxDrawn, g_fxFail);
}

static void FNameStr(int idx, wchar_t* out, int cap) {
    out[0] = 0;
    if (!g_api.fnameEntry) return;
    __try {
        const char* e = static_cast<const char*>(reinterpret_cast<FNameEntry_t>(g_api.fnameEntry)(idx));
        if (!e) return;
        static volatile LONG once = 0;
        if (InterlockedExchange(&once, 1) == 0) {
            const unsigned* u = reinterpret_cast<const unsigned*>(e);
            Log("essence: FNameEntry(%d) @%p: %08x %08x %08x %08x %08x %08x", idx, e, u[0], u[1], u[2], u[3], u[4], u[5]);
        }
        const wchar_t* w = reinterpret_cast<const wchar_t*>(e + 12);          // FNameEntry { int Index; DWORD Flags; void* HashNext; TCHAR Name[] }
        int n = 0;
        while (n < cap - 1 && w[n] >= 32 && w[n] < 127) { out[n] = w[n]; ++n; }
        out[n] = 0;
        if (n == 0) { const char* a = e + 12; while (n < cap - 1 && a[n] >= 32 && a[n] < 127) { out[n] = (wchar_t)a[n]; ++n; } out[n] = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}

// engine FName indices for the sequence names of one body's animation set
static bool EssEnsureFNames(EssMantle* g) {
    if (g->fnamesReady) return true;
    if (!g_api.fnameCtor || !g->anim) return false;
    int found = 0;
    for (size_t i = 0; i < g->anim->seqs.size(); ++i) {
        wchar_t w[96]; const std::string& n = g->anim->seqs[i].name;
        size_t k = 0; for (; k < n.size() && k < 95; ++k) w[k] = (wchar_t)(unsigned char)n[k];
        w[k] = 0;
        int idx = 0;
        reinterpret_cast<FNameCtor_t>(g_api.fnameCtor)(&idx, nullptr, w, 1 /*FNAME_Add: the body animation package may load later*/);
        g->anim->seqs[i].fname = idx;
        if (idx > 0) { g->anim->byFName[idx] = (int)i; ++found; }
    }
    g->fnamesReady = true;
    Log("essence: %s - %d of %d cape sequences exist as engine names", g->anim->body.c_str(), found, (int)g->anim->seqs.size());
    return true;
}

static EssMantle* EssFind(int bodyIdx, int design) {
    if (g_packState != 1 || bodyIdx < 0 || bodyIdx > 13) return nullptr;
    char name[40]; { const wchar_t* w = kBodies[bodyIdx]; int k = 0; for (; w[k] && k < 31; ++k) name[k] = (char)w[k]; name[k] = 0;
                     if (design == 10) strcat_s(name, "_aegis"); else if (design == 11) strcat_s(name, "_valakas"); else if (design == 12 || design == 13) strcpy_s(name, "JDK"); }
    for (int i = 0; i < g_nem; ++i) if (g_em[i].m && g_em[i].m->design == design && ess::Lower(g_em[i].m->body) == ess::Lower(name)) return &g_em[i];
    if (g_nem >= 256) return nullptr;
    const ess::Mantle* m = ess::FindMantle(g_pack, name, design);
    ess::Anim* a = ess::FindAnim(g_pack, name);
    if (!m || !a) return nullptr;
    EssMantle* g = &g_em[g_nem++];
    g->m = m; g->anim = a; g->bodyIdx = bodyIdx;
    ess::MapBones(*a, *m, g->amap);
    {   // outward = -Y in the rest pose (the mantle hangs behind the spine): flip the geometric normals if the winding says otherwise
        std::vector<float> bind(m->pts.begin(), m->pts.end()), nr;
        ess::PointNormals(*m, bind, nr);
        double s = 0.0; for (int i = 0; i < m->np; ++i) s -= nr[(size_t)i * 3 + 1];
        g->normSign = s < 0.0 ? -1.f : 1.f;
    }
    {   // the snug pull fades out with height measured in the rest pose: the upper back follows the torso, the hem keeps its drape
        float zlo = 1e9f, zhi = -1e9f;
        for (int i = 0; i < m->np; ++i) { const float z = m->pts[(size_t)i * 3 + 2]; if (z < zlo) zlo = z; if (z > zhi) zhi = z; }
        g->snugW.assign((size_t)m->np, 0.f);
        const float span = zhi > zlo ? zhi - zlo : 1.f;
        for (int i = 0; i < m->np; ++i) {
            float u = ((m->pts[(size_t)i * 3 + 2] - zlo) / span - 0.15f) / 0.55f; u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
            g->snugW[(size_t)i] = u * u * (3.f - 2.f * u);
        }
    }
    Log("essence: %s design %d ready (%d points, %d wedges, %d faces, %d sections)", name, design, m->np, m->nw, m->nf, (int)m->secs.size());
    return g;
}

struct ChanInfo { int n; int seq[4]; float frame[4]; unsigned raw0[28]; };
static bool ReadChannels(void* inst, ChanInfo* out) {   // POD + SEH: engine memory
    __try {
        char* ch = *reinterpret_cast<char**>(P(inst, 0x184));
        const int n = *reinterpret_cast<int*>(P(inst, 0x188));
        if (!ch || n < 0 || n > 32) return false;
        out->n = n < 4 ? n : 4;
        for (int i = 0; i < out->n; ++i) { out->seq[i] = *reinterpret_cast<int*>(ch + i * 0x70 + 8); out->frame[i] = *reinterpret_cast<float*>(ch + i * 0x70 + 0x10); }
        if (n > 0) memcpy(out->raw0, ch, sizeof out->raw0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- attachment of the mantle root to the pawn's real Spine2 bone
static int EssFindBoneIdx(void* bodyMesh, const wchar_t* const* names, int nn) {   // index of the first bone whose FName matches one of `names`
    if (!g_api.fnameCtor || !bodyMesh) return -1;
    int want[8]; int nw = 0;
    for (int i = 0; i < nn && nw < 8; ++i) { int idx = 0; reinterpret_cast<FNameCtor_t>(g_api.fnameCtor)(&idx, nullptr, names[i], 0 /*FNAME_Find*/); if (idx > 0) want[nw++] = idx; }
    if (!nw) return -1;
    __try {
        const char* data = *reinterpret_cast<char**>(P(bodyMesh, kBoneData));
        const int n = *reinterpret_cast<int*>(P(bodyMesh, kBoneNum));
        if (!data || n <= 0 || n > 200) return -1;
        for (int i = 0; i < n; ++i) { const int ni = *reinterpret_cast<const int*>(data + (size_t)i * kBoneStride); for (int k = 0; k < nw; ++k) if (ni == want[k]) return i; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

static bool EssReadBone(void* inst, int idx, float* out12) {     // FCoords (origin, X, Y, Z axes; 48 bytes) from the instance's bone array
    __try {
        if (!inst) return false;
        const char* arr = *reinterpret_cast<char**>(P(inst, 0xc4));
        const int n = *reinterpret_cast<int*>(P(inst, 0xc8));
        if (!arr || n <= 0 || n > 200 || idx < 0 || idx >= n) return false;
        memcpy(out12, arr + (size_t)idx * 48, 48);
        for (int i = 0; i < 12; ++i) if (!(fabsf(out12[i]) < 1.0e5f)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// conv: bit2 = rows instead of columns, bits0-1 = which axis to negate (0 none, 1 X, 2 Y, 3 Z) so a mirrored basis becomes a proper rotation
static bool EssCoordsToRot(const float* c, int conv, float* R) {
    float ax[3][3];
    for (int a = 0; a < 3; ++a) for (int k = 0; k < 3; ++k) ax[a][k] = c[3 + a * 3 + k];
    const int fl = conv & 3;
    if (fl) for (int k = 0; k < 3; ++k) ax[fl - 1][k] = -ax[fl - 1][k];
    for (int a = 0; a < 3; ++a) for (int k = 0; k < 3; ++k) { if (conv & 4) R[a * 3 + k] = ax[a][k]; else R[k * 3 + a] = ax[a][k]; }
    const float det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]);
    return det > 0.5f && det < 1.5f;
}
static float RotAngle(const float* A, const float* B) {           // angle (deg) between two rotations
    float t = 0.f; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t += A[i * 3 + j] * B[i * 3 + j];
    float c = (t - 1.f) * 0.5f; if (c > 1.f) c = 1.f; if (c < -1.f) c = -1.f;
    return acosf(c) * 57.29578f;
}

static void EssApplyAttach(EssMantle* g, void* bodyInst, void* pawn, ess::Pose& pose) {
    if (!g_cfg.essenceAttach || g_cal.attFailed || pose.nb < 1) return;
    void* bodyMesh = nullptr; void* subInst = nullptr;
    __try { bodyMesh = *reinterpret_cast<void**>(P(bodyInst, kInstMesh)); subInst = *reinterpret_cast<void**>(P(pawn, 0x134)); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    {   // the bone index belongs to the body mesh; the axis mapping found by the calibration belongs to the engine, so it is kept for every pawn
        int found = -2;
        for (int i = 0; i < g->nSpineCache; ++i) if (g->spineCacheMesh[i] == bodyMesh) { found = g->spineCacheIdx[i]; break; }
        if (found == -2) {
            static const wchar_t* const names[] = { L"Bip01 Spine2", L"Bip01_Spine2", L"bip01_spine2" };
            found = EssFindBoneIdx(bodyMesh, names, 3);
            const int slot = g->nSpineCache < 8 ? g->nSpineCache++ : 0;
            g->spineCacheMesh[slot] = bodyMesh; g->spineCacheIdx[slot] = found;
            Log("essence: body mesh %p: Spine2 is bone index %d", bodyMesh, found);
        }
        g->spineIdx = found;
    }
    if (g->spineIdx < 0) return;                                     // this body has no such bone: its cape keeps the baked root
    void* src[2] = { subInst, bodyInst };
    float c[2][12]; bool ok[2];
    for (int s = 0; s < 2; ++s) ok[s] = EssReadBone(src[s], g->spineIdx, c[s]);
    float RE[9]; ess::QuatToMat(&pose.q[0], RE);                     // the baked root: what the Essence body's Spine2 did in this animation
    if (g_cal.attSrc < 0) {                                             // calibration: pick the (source, axis mapping) closest to the baked root
        for (int s = 0; s < 2; ++s) if (ok[s]) for (int cv = 0; cv < 8; ++cv) {
            float R[9]; if (EssCoordsToRot(c[s], cv, R)) g_cal.angSum[s][cv] += RotAngle(R, RE); else g_cal.angSum[s][cv] += 180.0;
        }
        ++g_cal.angN;
        if (g_cal.attLogs < 6 && (g_cal.angN == 1 || g_cal.angN == 15)) {
            ++g_cal.attLogs;
            Log("essence: Spine2 probe: pawn+0x134=%p ok=%d | body inst ok=%d | engine origin %.1f %.1f %.1f axes X(%.2f %.2f %.2f) Y(%.2f %.2f %.2f) Z(%.2f %.2f %.2f) | baked root %.1f %.1f %.1f",
                subInst, (int)ok[0], (int)ok[1], c[0][0], c[0][1], c[0][2], c[0][3], c[0][4], c[0][5], c[0][6], c[0][7], c[0][8], c[0][9], c[0][10], c[0][11], pose.p[0], pose.p[1], pose.p[2]);
        }
        if (g_cal.angN >= 30) {
            double best = 1e9; int bs = -1, bc = -1;
            for (int s = 0; s < 2; ++s) for (int cv = 0; cv < 8; ++cv) { const double m = g_cal.angSum[s][cv] / g_cal.angN; if (m < best) { best = m; bs = s; bc = cv; } }
            Log("essence: Spine2 calibration over %d frames: best source %d (0 = pawn+0x134, 1 = body instance) conv %d, mean angle to the baked root %.1f deg", g_cal.angN, bs, bc, best);
            for (int s = 0; s < 2; ++s) Log("essence:   source %d means: %.0f %.0f %.0f %.0f | %.0f %.0f %.0f %.0f", s, g_cal.angSum[s][0] / g_cal.angN, g_cal.angSum[s][1] / g_cal.angN, g_cal.angSum[s][2] / g_cal.angN, g_cal.angSum[s][3] / g_cal.angN,
                                         g_cal.angSum[s][4] / g_cal.angN, g_cal.angSum[s][5] / g_cal.angN, g_cal.angSum[s][6] / g_cal.angN, g_cal.angSum[s][7] / g_cal.angN);
            if (best < 70.0 && bs >= 0) { g_cal.attSrc = bs; g_cal.attConv = bc; } else { g_cal.attFailed = true; Log("essence: no engine bone source matches the baked root - keeping the baked root"); }
        }
        return;                                                      // still calibrating: baked root
    }
    if (!ok[g_cal.attSrc]) return;
    float R[9];
    if (!EssCoordsToRot(c[g_cal.attSrc], g_cal.attConv, R)) return;
    ess::MatToQuat(R, &pose.q[0]);
    pose.p[0] = c[g_cal.attSrc][0]; pose.p[1] = c[g_cal.attSrc][1]; pose.p[2] = c[g_cal.attSrc][2];
    static volatile LONG al = 0;
    if (g_cfg.debug && InterlockedIncrement(&al) <= 40 && (al % 8) == 0)
        Log("essence: root from the engine bone: origin %.1f %.1f %.1f (angle to baked root %.0f deg)", pose.p[0], pose.p[1], pose.p[2], RotAngle(R, RE));
}

// ---- sequence resolution: by NAME (the engine can hold the same name under several FName indices), with a same-action fallback
static std::string EssLowerW(const wchar_t* w) { std::string s; for (; *w; ++w) s.push_back((char)((*w >= L'A' && *w <= L'Z') ? *w - L'A' + L'a' : *w)); return s; }

static int EssFallbackSeq(const ess::Anim& a, std::string name) {
    const std::string suf = "_" + ess::Lower(a.body);
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) name.resize(name.size() - suf.size());
    // tokens: action[_weapon...]
    std::string action = name.substr(0, name.find('_'));
    static const char* const weapons[] = { "hand", "1hs", "2hs", "dual", "pole", "bow", "bowgun", "dagger", "blunt", "rapier" };
    std::string weapon;
    for (const char* w : weapons) { const std::string tok = std::string("_") + w; if (name.find(tok) != std::string::npos) { weapon = w; break; } }
    auto find = [&](const std::string& act, const std::string& wpn) -> int {
        int best = -1;
        for (size_t i = 0; i < a.seqs.size(); ++i) {
            const std::string& n = a.seqs[i].name;
            if (n.compare(0, act.size(), act) != 0 || (n.size() > act.size() && n[act.size()] != '_')) continue;
            if (!wpn.empty() && n.find("_" + wpn) == std::string::npos) continue;
            if (n.find("_dk") != std::string::npos || n.find("_asn") != std::string::npos || n.find("_wild") != std::string::npos || n.find("_jdk") != std::string::npos) { if (best < 0) best = (int)i; continue; }   // class variants: last resort
            return (int)i;
        }
        return best;
    };
    if (action == "run" || action == "walk" || action == "fly" || action == "swim") { auto fl = a.byName.find("flap"); if (fl != a.byName.end()) return fl->second; }      // wing sets: flap while moving
    int r = find(action, weapon); if (r >= 0) return r;
    if (!weapon.empty()) { r = find(action, "hand"); if (r >= 0) return r; }
    r = find(action, ""); if (r >= 0) return r;
    // families without a baked equivalent: stand-in animations
    std::string alt = "wait";
    if (action.find("atk") == 0 || action.find("spatk") == 0 || action == "shieldatk" || action == "magic" || action == "skill") alt = "atkwait";
    else if (action == "run" || action == "walk" || action == "swim" || action == "fly") alt = "walk";
    else if (action.find("sit") == 0 || action.find("chair") == 0) alt = "sitwait";
    r = find(alt, weapon.empty() ? "hand" : weapon); if (r >= 0) return r;
    r = find(alt, ""); if (r >= 0) return r;
    r = find("wait", "hand"); return r >= 0 ? r : 0;
}

// FName index of a body sequence -> index into the cape animation set (-1 = not an animation name). Cached per FName index.
static int EssResolveSeq(EssMantle* g, int fidx) {
    if (fidx <= 0) return -1;
    ess::Anim& a = *g->anim;
    auto it = a.byFName.find(fidx);
    if (it != a.byFName.end()) return it->second;
    wchar_t w[128]; FNameStr(fidx, w, 128);
    const std::string name = EssLowerW(w);
    int si = -1; const char* how = "exact name";
    if (!name.empty()) {
        auto bn = a.byName.find(name);
        if (bn != a.byName.end()) si = bn->second;
        else { si = EssFallbackSeq(a, name); how = "same-action stand-in"; }
    } else { si = -1; how = "name unreadable"; }
    a.byFName[fidx] = si;
    static volatile LONG logs = 0;
    if (InterlockedIncrement(&logs) <= 120)
        Log("essence: engine sequence #%d '%s' -> cape sequence '%s' (%s)", fidx, name.c_str(), si >= 0 ? a.seqs[si].name.c_str() : "(none)", how);
    return si;
}

// evaluates the pose of this pawn for the current animation channel and blends it with the previous sequence

static int ClothWindClassFor(const ess::Anim&, const std::string& n) {            // 0 idle, 1 walk, 2 run, 3 sit, 4 attack
    auto sw = [&](const char* q) { return n.compare(0, strlen(q), q) == 0; };
    if (sw("atkwait") || sw("wait")) return 0;
    if (sw("run") || sw("fly") || sw("swim")) return 2;
    if (sw("walk")) return 1;
    if (sw("sit") || sw("chair")) return 3;
    if (sw("atk") || sw("spatk") || sw("shieldatk") || sw("magic") || sw("skill") || sw("social")) return 4;
    return 0;
}

// ---- secondary motion of the animated mantles ("leveza"): the hem lags the torso like a hanging cloth, flutters a little at rest and trails in walk/run.
// Angles about the torso's own x (pitch), y (roll) and z (yaw) axes, springs with ~1.5 Hz and light damping, impulses from the torso's rotation per frame.
static void EssSwayUpdate(EssMantle* g, EssPawn* p, ULONGLONG now, int windClass) {
    if (g_cfg.essenceSway <= 0.f || !g->m || g->m->design >= 10) return;
    float R[9]; ess::QuatToMat(&p->cur.q[0], R);
    float dt = p->swLast ? (float)(now - p->swLast) * 0.001f : 0.f; if (dt < 0.f) dt = 0.f;
    p->swLast = now;
    if (dt > 0.2f) { for (int k = 0; k < 3; ++k) { p->swA[k] = 0.f; p->swV[k] = 0.f; } p->swHave = false; dt = 0.f; }
    const float amp = g_cfg.essenceSway;
    if (p->swHave && dt > 0.f) {
        float D[9];                                                          // D = R * Rp^T: the torso rotation since the last update, in actor space
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) D[i * 3 + j] = R[i * 3] * p->swRp[j * 3] + R[i * 3 + 1] * p->swRp[j * 3 + 1] + R[i * 3 + 2] * p->swRp[j * 3 + 2];
        float w[3] = { D[7] - D[5], D[2] - D[6], D[3] - D[1] };            // 2*sin(angle) * axis (small angles: the rotation vector)
        for (int k = 0; k < 3; ++k) w[k] *= 0.5f;
        float ph[3];                                                         // into the torso frame: R^T * w
        for (int k = 0; k < 3; ++k) ph[k] = R[k] * w[0] + R[3 + k] * w[1] + R[6 + k] * w[2];
        const float gain[3] = { 1.1f, 0.9f, 0.8f };
        for (int k = 0; k < 3; ++k) { float x = ph[k]; if (x > 0.12f) x = 0.12f; if (x < -0.12f) x = -0.12f; p->swA[k] += -gain[k] * x * amp; }
        const float kk = 95.f, cc = 6.5f;                                   // ~1.55 Hz, damping ratio ~0.33
        const int steps = dt > 0.03f ? 3 : 1; const float h = dt / (float)steps;
        for (int s = 0; s < steps; ++s) for (int k = 0; k < 3; ++k) { p->swV[k] += (-kk * p->swA[k] - cc * p->swV[k]) * h; p->swA[k] += p->swV[k] * h; }
        for (int k = 0; k < 3; ++k) { if (p->swA[k] > 0.35f) { p->swA[k] = 0.35f; p->swV[k] = 0.f; } if (p->swA[k] < -0.35f) { p->swA[k] = -0.35f; p->swV[k] = 0.f; } }
        const float target = (windClass == 2 ? 0.07f : (windClass == 1 ? 0.035f : (windClass == 4 ? 0.04f : 0.f))) * amp;      // trailing, rad
        p->swWind += (target - p->swWind) * (1.f - expf(-dt / 0.3f));
        p->swPhase += dt;
    }
    memcpy(p->swRp, R, sizeof R); p->swHave = true;
}

static void EssSwayApply(EssMantle* g, EssPawn* p, std::vector<float>& pos) {
    if (g_cfg.essenceSway <= 0.f || !g->m || g->m->design >= 10 || g->snugW.size() != (size_t)g->m->np || !p->swHave) return;
    const int np = g->m->np;
    float R[9]; ess::QuatToMat(&p->cur.q[0], R);
    const float amp = g_cfg.essenceSway;
    const float fl1 = 0.012f * amp * sinf(p->swPhase * 2.f * 3.14159f * 0.43f), fl2 = 0.009f * amp * sinf(p->swPhase * 2.f * 3.14159f * 0.29f + 1.3f);   // idle flutter, rad
    const float a[3] = { p->swA[0] - p->swWind + fl1, p->swA[1] + fl2, p->swA[2] * 0.7f };              // pitch (hem back = negative rotation about +x), roll, yaw
    if (fabsf(a[0]) < 1e-4f && fabsf(a[1]) < 1e-4f && fabsf(a[2]) < 1e-4f) return;
    // pivot: the shoulder line (centroid of the top points)
    float piv[3] = { 0.f, 0.f, 0.f }; int cnt = 0;
    for (int i = 0; i < np; ++i) if (g->snugW[(size_t)i] > 0.97f) { for (int k = 0; k < 3; ++k) piv[k] += pos[(size_t)i * 3 + k]; ++cnt; }
    if (cnt == 0) return;
    for (int k = 0; k < 3; ++k) piv[k] /= (float)cnt;
    const float ax[3][3] = { { R[0], R[3], R[6] }, { R[1], R[4], R[7] }, { R[2], R[5], R[8] } };      // torso axes (columns of R) in actor space
    auto rot = [](const float* axis, float ang, const float* v, float* o) {
        const float c = cosf(ang), s = sinf(ang);
        const float cr[3] = { axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0] };
        const float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        for (int k = 0; k < 3; ++k) o[k] = v[k] * c + cr[k] * s + axis[k] * d * (1.f - c);
    };
    for (int i = 0; i < np; ++i) {
        const float w = 1.f - g->snugW[(size_t)i];                         // 0 at the shoulders .. 1 at the hem
        if (w <= 0.f) continue;
        const float ww = w * (0.35f + 0.65f * w);                          // the lower half moves most
        float* x = &pos[(size_t)i * 3];
        float v[3] = { x[0] - piv[0], x[1] - piv[1], x[2] - piv[2] }, o1[3], o2[3], o3[3];
        rot(ax[0], a[0] * ww, v, o1); rot(ax[1], a[1] * ww, o1, o2); rot(ax[2], a[2] * ww, o2, o3);
        x[0] = piv[0] + o3[0]; x[1] = piv[1] + o3[1]; x[2] = piv[2] + o3[2];
    }
}

static bool EssUpdate(EssMantle* g, void* bodyInst, void* pawnKey) {
    if (!EssEnsureFNames(g)) return false;
    ChanInfo ci; memset(&ci, 0, sizeof ci);
    if (!ReadChannels(bodyInst, &ci)) return false;
    ess::Anim& a = *g->anim;
    int si = -1; float frame = 0.f; int chosen = -1;
    for (int i = 0; i < ci.n; ++i) {
        const int r = EssResolveSeq(g, ci.seq[i]);
        if (r >= 0) { si = r; frame = ci.frame[i]; chosen = i; break; }
    }
    EssPawn* p = nullptr;
    for (int i = 0; i < g_nep; ++i) if (g_ep[i].inst == pawnKey) { p = &g_ep[i]; break; }
    if (!p) { p = (g_nep < 48) ? &g_ep[g_nep++] : &g_ep[0]; *p = EssPawn(); p->inst = pawnKey; }
    if (p->animKey != g->anim) { *p = EssPawn(); p->inst = pawnKey; p->animKey = g->anim; }          // another animation set (other cloak design): start over

    static volatile LONG logs = 0;
    if (g_cfg.debug && InterlockedIncrement(&logs) <= 8) {
        wchar_t nm[96], nm2[96];
        FNameStr(ci.seq[0], nm, 96); FNameStr(si >= 0 ? a.seqs[si].fname : 0, nm2, 96);
        Log("essence: pawn %p channels=%d ch0 seq=%d '%ls' frame=%.4f | chosen ch%d -> cape seq %d '%s' (%d frames) | raw ch0: %08x %08x %08x %08x %08x %08x %08x %08x",
            pawnKey, ci.n, ci.seq[0], nm, ci.frame[0], chosen, si, si >= 0 ? a.seqs[si].name.c_str() : "-", si >= 0 ? a.seqs[si].frames : 0,
            ci.raw0[0], ci.raw0[1], ci.raw0[2], ci.raw0[3], ci.raw0[4], ci.raw0[5], ci.raw0[6], ci.raw0[7]);
    }
    if (si < 0 && !p->init) {                       // nothing to play yet: hold the first idle-like sequence
        auto wh = a.byName.find("wait_hand_" + ess::Lower(a.body));
        si = wh != a.byName.end() ? wh->second : 0;
        frame = ci.n > 0 ? ci.frame[0] : 0.f;
    }
    if (si < 0) {                                   // the current sequence has no baked cape animation: keep the previous pose
        if (p->init) { t_essM = g; t_essP = p; return true; }
        return false;
    }
    const ess::Seq& s = a.seqs[si];
    if (si != p->seq || !p->init) {
        static volatile LONG tl = 0;
        if (g_cfg.debug && InterlockedIncrement(&tl) <= 400)
            Log("essence: pawn %p sequence %s -> %s (%d frames, body frame value %.4f, ch %d of %d)", pawnKey, (p->seq >= 0 && p->seq < (int)a.seqs.size()) ? a.seqs[(size_t)p->seq].name.c_str() : "(none)", s.name.c_str(), s.frames, frame, chosen, ci.n);
    }
    const bool frac = g_cfg.essenceFrameMode == 1 || (g_cfg.essenceFrameMode == 0 && frame <= 1.0001f);
    float f = frac ? frame * (float)(s.frames > 1 ? s.frames - 1 : 0) : frame;
    ess::Pose tgt; ess::EvalSeq(a, s, f, tgt);
    const ULONGLONG now = GetTickCount64();
    if (frame != p->lastFrame) { p->lastFrame = frame; p->frameSince = now; }
    if (g_cfg.debug && p->logs < 150 && now - p->lastLog >= 700) {              // periodic status: is the body frame advancing, which channel plays what?
        p->lastLog = now; ++p->logs;
        wchar_t n0[64]; FNameStr(ci.seq[0], n0, 64);
        Log("essence: pawn %p status: seq %s | body frame %.4f (unchanged for %llu ms) -> cape frame %.2f of %d | blend %.2f | channels %d: seq ids %d %d %d %d, frames %.3f %.3f %.3f %.3f",
            pawnKey, s.name.c_str(), frame, (unsigned long long)(now - p->frameSince), f, s.frames, p->blendT, ci.n, ci.seq[0], ci.seq[1], ci.seq[2], ci.seq[3], ci.frame[0], ci.frame[1], ci.frame[2], ci.frame[3]);
    }
    if (!p->init) { p->cur = tgt; p->init = true; p->seq = si; p->offW = 0.f; p->blendT = 1.f; p->last = now; }
    else {
        float dt = (float)(now - p->last) * 0.001f; if (dt < 0.f) dt = 0.f; if (dt > 0.1f) dt = 0.1f;
        const bool changed = si != p->seq;
        const bool tween = g_cfg.essenceTween && frame < 0.f;                 // the engine is tweening from the previous pose to the first frame of this sequence
        if (tween) {
            if (changed || !p->twOn || frame < p->tw0 - 1e-5f) { p->from = p->cur; p->tw0 = frame; p->twOn = true; p->offW = 0.f; }   // a (new) tween starts: remember what is on screen
            float alpha = 1.f - frame / p->tw0;                               // the engine's progress: tw0 -> 0
            if (alpha < 0.f) alpha = 0.f; if (alpha > 1.f) alpha = 1.f;
            ess::BlendPose(p->from, tgt, alpha, p->cur);
            p->blendT = alpha;
        } else {
            p->twOn = false;
            if (changed && g_cfg.essenceBlend > 0.f) { ess::MakeOffset(p->cur, tgt, p->off); p->offW = 1.f; }   // no engine tween: keep what is on screen, let the difference fade out
            if (p->offW > 0.f) {
                const float tau = (g_cfg.essenceBlend > 0.03f ? g_cfg.essenceBlend : 0.03f) / 3.f;                   // EssenceBlend = about three time constants
                p->offW *= expf(-dt / tau);
                if (p->offW < 0.003f) p->offW = 0.f;
            }
            if (p->offW > 0.f) ess::ApplyOffset(tgt, p->off, p->offW, p->cur); else p->cur = tgt;
            p->blendT = 1.f - p->offW;
        }
        p->seq = si;
        p->last = now;
    }
    EssApplyAttach(g, bodyInst, pawnKey, p->cur);
    EssSwayUpdate(g, p, now, ClothWindClassFor(a, s.name));
    t_essM = g; t_essP = p;
    return true;
}

// ---- Direct3D resources
static IDirect3DTexture9* g_gpuTex[512]; static IDirect3DDevice9* g_gpuTexDev = nullptr;
static void EssReleaseTextures() { for (auto& t : g_gpuTex) if (t) { t->Release(); t = nullptr; } g_gpuTexDev = nullptr; }
static void EssRelease(EssMantle* g) {
    for (auto& t : g->tex) t = nullptr;
    if (g->vb) { g->vb->Release(); g->vb = nullptr; }
    if (g->ib) { g->ib->Release(); g->ib = nullptr; }
    g->gpuOk = false; g->dev = nullptr;
}

static bool EssEnsureGPU(IDirect3DDevice9* dev, EssMantle* g) {
    if (g->gpuOk && g->dev == dev) return true;
    if (g->gpuFails >= 3) { EssWhy("mantle GPU gave up after 3 failures", g->m ? g->m->design : -1); return false; }
    if (g_gpuTexDev && g_gpuTexDev != dev) EssReleaseTextures();                       // the device was re-created: every texture belonged to the old one
    if (g->dev && g->dev != dev) EssRelease(g);
    g_gpuTexDev = dev;
    const ess::Mantle& m = *g->m;
    g->dev = dev;
    HRESULT hr = dev->CreateIndexBuffer((UINT)m.nf * 3 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g->ib, nullptr);
    if (FAILED(hr) || !g->ib) { Log("essence: CreateIndexBuffer failed 0x%08x", (unsigned)hr); ++g->gpuFails; return false; }
    void* p = nullptr;
    if (FAILED(g->ib->Lock(0, 0, &p, 0)) || !p) { ++g->gpuFails; EssWhy("mantle index buffer lock", m.design); return false; }
    memcpy(p, m.faces.data(), (size_t)m.nf * 6); g->ib->Unlock();
    hr = dev->CreateVertexBuffer((UINT)m.nw * 32, 0, 0, D3DPOOL_MANAGED, &g->vb, nullptr);
    if (FAILED(hr) || !g->vb) { Log("essence: CreateVertexBuffer failed 0x%08x", (unsigned)hr); ++g->gpuFails; return false; }
    for (size_t mi = 0; mi < m.mats.size() && mi < 8; ++mi) {
        const size_t ti = (size_t)m.mats[mi].tex;
        if (ti >= g_pack.texs.size() || ti >= 512) { ++g->gpuFails; EssWhy("mantle texture index out of range", (int)ti, (int)g_pack.texs.size()); return false; }
        if (g_gpuTex[ti]) { g->tex[mi] = g_gpuTex[ti]; continue; }                              // already uploaded for another body
        const ess::Tex& t = g_pack.texs[ti];
        D3DFORMAT fmt = t.fmt == 3 ? D3DFMT_DXT1 : (t.fmt == 8 ? D3DFMT_DXT5 : D3DFMT_DXT3);
        IDirect3DTexture9* tx = nullptr;
        hr = dev->CreateTexture(t.w, t.h, (UINT)t.levels, 0, fmt, D3DPOOL_MANAGED, &tx, nullptr);
        if (FAILED(hr) || !tx) { Log("essence: CreateTexture %s failed 0x%08x", t.name.c_str(), (unsigned)hr); ++g->gpuFails; return false; }
        for (int l = 0; l < t.levels; ++l) {
            D3DLOCKED_RECT lr;
            if (FAILED(tx->LockRect((UINT)l, &lr, nullptr, 0))) { tx->Release(); ++g->gpuFails; EssWhy("mantle texture LockRect", (int)ti, l); return false; }
            const UINT lw = t.w >> l ? t.w >> l : 1, lh = t.h >> l ? t.h >> l : 1;
            const UINT bpr = ((lw + 3) / 4) * (t.fmt == 3 ? 8 : 16), rows = (lh + 3) / 4;
            if (t.size[l] < bpr * rows) { tx->UnlockRect((UINT)l); tx->Release(); ++g->gpuFails; EssWhy("mantle texture level too small", (int)ti, l); return false; }
            for (UINT r = 0; r < rows; ++r) memcpy(static_cast<BYTE*>(lr.pBits) + (size_t)r * lr.Pitch, t.data[l] + (size_t)r * bpr, bpr);
            tx->UnlockRect((UINT)l);
        }
        g_gpuTex[ti] = tx; g->tex[mi] = tx;
        Log("essence: texture %s %ux%u levels %d uploaded", t.name.c_str(), t.w, t.h, t.levels);
    }
    g->fxProg.assign(m.mats.size(), nullptr); g->hasEnv = false;
    if (g_cfg.essenceFx) for (size_t mi = 0; mi < m.mats.size(); ++mi) {
        const size_t ti = (size_t)m.mats[mi].tex; if (ti >= g_pack.texs.size()) continue;
        auto it = g_pack.fx.find(ess::Lower(g_pack.texs[ti].name));
        if (it == g_pack.fx.end() || it->second.empty()) continue;
        g->fxProg[mi] = &it->second;
        for (const ess::FxLayer& l : it->second) for (const ess::FxTex& x : l.texs) if (x.env) g->hasEnv = true;
    }
    g->gpuOk = true;
    return true;
}

static const D3DRENDERSTATETYPE kEssRS[] = { D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE,
                                             D3DRS_LIGHTING, D3DRS_TEXTUREFACTOR, D3DRS_SPECULARENABLE,
                                             D3DRS_NORMALIZENORMALS, D3DRS_COLORVERTEX, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE };
constexpr int kEssNRS = sizeof(kEssRS) / sizeof(kEssRS[0]);
static const D3DTEXTURESTAGESTATETYPE kEssTSS[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
                                                    D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
constexpr int kEssNTSS = sizeof(kEssTSS) / sizeof(kEssTSS[0]);


// ---- effect layers (env-map shine, animated fire / glow): extra passes over the same geometry, drawn after the base pass
static IDirect3DTexture9* EssPackTexture(IDirect3DDevice9* dev, int ti) {
    if (ti < 0 || ti >= (int)g_pack.texs.size() || ti >= 512) return nullptr;
    if (g_gpuTexDev && g_gpuTexDev != dev) EssReleaseTextures();
    g_gpuTexDev = dev;
    if (g_gpuTex[ti]) return g_gpuTex[ti];
    const ess::Tex& t = g_pack.texs[(size_t)ti];
    D3DFORMAT fmt = t.fmt == 3 ? D3DFMT_DXT1 : (t.fmt == 8 ? D3DFMT_DXT5 : D3DFMT_DXT3);
    IDirect3DTexture9* tx = nullptr;
    if (FAILED(dev->CreateTexture(t.w, t.h, (UINT)t.levels, 0, fmt, D3DPOOL_MANAGED, &tx, nullptr)) || !tx) return nullptr;
    for (int l = 0; l < t.levels; ++l) {
        D3DLOCKED_RECT lr;
        if (FAILED(tx->LockRect((UINT)l, &lr, nullptr, 0))) { tx->Release(); return nullptr; }
        const UINT lw = t.w >> l ? t.w >> l : 1, lh = t.h >> l ? t.h >> l : 1;
        const UINT bpr = ((lw + 3) / 4) * (t.fmt == 3 ? 8 : 16), rows = (lh + 3) / 4;
        if (t.size[l] < bpr * rows) { tx->UnlockRect((UINT)l); tx->Release(); return nullptr; }
        for (UINT r = 0; r < rows; ++r) memcpy(static_cast<BYTE*>(lr.pBits) + (size_t)r * lr.Pitch, t.data[l] + (size_t)r * bpr, bpr);
        tx->UnlockRect((UINT)l);
    }
    g_gpuTex[ti] = tx;
    return tx;
}

// draws the effect layers of one material over the triangles [firstTri, firstTri + nTri) (vertex/index buffers are bound already).
// baseTex supplies the silhouette (its alpha is tested with the material's alpha reference so the glow never leaves the cloth).
// The texture-stage / render states touched here are saved and restored.
static bool EssDrawFxLayers(IDirect3DDevice9* dev, DevHooks* h, const std::vector<ess::FxLayer>& layers, IDirect3DTexture9* baseTex, int alphaRef, bool twoSided, DWORD cullKeep,
                            UINT numVerts, UINT firstTri, UINT nTri, IDirect3DTexture9* (*getTex)(IDirect3DDevice9*, int), IDirect3DTexture9* maskOverride = nullptr) {      // maskOverride: used instead of the UV-mapped (non env-map) layer textures (the shine mask `_sp` of the clan cloaks is the cloth colour itself)
    static const D3DTEXTURESTAGESTATETYPE kTss[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
    static const D3DRENDERSTATETYPE kRs[] = { D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_CULLMODE, D3DRS_SPECULARENABLE };
    DWORD oldTss[4][8]; IDirect3DBaseTexture9* oldTex[4] = {}; D3DMATRIX oldM[4]; DWORD oldRs[sizeof kRs / sizeof kRs[0]];
    for (int s = 0; s < 4; ++s) { for (int k = 0; k < 8; ++k) dev->GetTextureStageState((DWORD)s, kTss[k], &oldTss[s][k]); dev->GetTexture((DWORD)s, &oldTex[s]); dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &oldM[s]); }
    for (size_t k = 0; k < sizeof kRs / sizeof kRs[0]; ++k) dev->GetRenderState(kRs[k], &oldRs[k]);
    const double now = (double)GetTickCount64() * 0.001;
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE); dev->SetRenderState(D3DRS_FOGENABLE, FALSE); dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, twoSided ? D3DCULL_NONE : cullKeep);
    const DWORD gain = (DWORD)(g_cfg.essenceFxGain * 127.5f > 255.f ? 255.f : (g_cfg.essenceFxGain < 0.f ? 0.f : g_cfg.essenceFxGain * 127.5f));
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, gain, gain, gain));
    bool ok = true;
    for (const ess::FxLayer& L : layers) {
        const int nt = (int)L.texs.size() > 3 ? 3 : (int)L.texs.size();
        if (nt < 1) continue;
        IDirect3DTexture9* tex[3] = {}; bool have = true;
        for (int i = 0; i < nt; ++i) { tex[i] = (maskOverride && !L.texs[(size_t)i].env) ? maskOverride : getTex(dev, L.texs[(size_t)i].tex); if (!tex[i]) have = false; }
        if (!have) { InterlockedIncrement(&g_stFxSkipped); continue; }
        const bool lerp = L.kind == 1;
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        if (lerp) { dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA); dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); }
        else { dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE); dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); dev->SetRenderState(D3DRS_ALPHAREF, (DWORD)alphaRef); dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL); }
        int s = 0;
        for (int i = 0; i < nt; ++i, ++s) {
            const ess::FxTex& x = L.texs[(size_t)i];
            const bool isMask = i == nt - 1;
            dev->SetTexture((DWORD)s, tex[i]);
            if (s == 0) { dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_SELECTARG1); dev->SetTextureStageState(s, D3DTSS_COLORARG1, D3DTA_TEXTURE); }
            else if (lerp && isMask) { dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_SELECTARG2); dev->SetTextureStageState(s, D3DTSS_COLORARG2, D3DTA_CURRENT); dev->SetTextureStageState(s, D3DTSS_COLORARG1, D3DTA_TEXTURE); }
            else { dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_MODULATE); dev->SetTextureStageState(s, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev->SetTextureStageState(s, D3DTSS_COLORARG2, D3DTA_CURRENT); }
            dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(s, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            D3DMATRIX M; memset(&M, 0, sizeof M); M._11 = M._22 = M._33 = M._44 = 1.f;
            if (x.env) { M._11 = 0.5f; M._22 = -0.5f; M._41 = 0.5f; M._42 = 0.5f; dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL); dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2); }
            else if (x.panU != 0.f || x.panV != 0.f) { M._31 = (float)fmod((double)x.panU * now, 1.0); M._32 = (float)fmod((double)x.panV * now, 1.0); dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2); }
            else { dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE); }
            dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &M);
        }
        // the silhouette: alpha of the base texture (multiplied into the mask alpha for the blended layers, alpha-tested for the additive ones)
        if (baseTex && s < 4) {
            dev->SetTexture((DWORD)s, baseTex);
            dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_SELECTARG2); dev->SetTextureStageState(s, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev->SetTextureStageState(s, D3DTSS_COLORARG2, D3DTA_CURRENT);
            if (lerp) { dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_MODULATE); dev->SetTextureStageState(s, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); dev->SetTextureStageState(s, D3DTSS_ALPHAARG2, D3DTA_CURRENT); }
            else { dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(s, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); }
            dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            ++s;
        }
        if (!lerp && g_cfg.essenceFxGain != 1.f && s < 4) {                                         // gain of the additive layers
            dev->SetTexture((DWORD)s, nullptr);
            dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_MODULATE2X); dev->SetTextureStageState(s, D3DTSS_COLORARG1, D3DTA_CURRENT); dev->SetTextureStageState(s, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(s, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
            dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            ++s;
        }
        if (s < 4) { dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE); }
        const HRESULT hr = reinterpret_cast<DIP_t>(h->dip)(dev, D3DPT_TRIANGLELIST, 0, 0, numVerts, firstTri * 3, nTri);
        if (FAILED(hr)) { ok = false; InterlockedIncrement(&g_stFxFailed); static volatile LONG once = 0; if (InterlockedExchange(&once, 1) == 0) Log("essence fx: DrawIndexedPrimitive failed 0x%08x", (unsigned)hr); }
        else InterlockedIncrement(&g_stFxDrawn);
    }
    for (size_t k = 0; k < sizeof kRs / sizeof kRs[0]; ++k) dev->SetRenderState(kRs[k], oldRs[k]);
    for (int s = 0; s < 4; ++s) {
        for (int k = 0; k < 8; ++k) dev->SetTextureStageState((DWORD)s, kTss[k], oldTss[s][k]);
        dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &oldM[s]);
        dev->SetTexture((DWORD)s, oldTex[s]); if (oldTex[s]) oldTex[s]->Release();
    }
    return ok;
}

// Draws the mantle in place of the carrier. Every state we touch is read first and written back afterwards (the engine caches its own state).
static bool EssDraw(IDirect3DDevice9* dev, DevHooks* h) {
    EssMantle* g = t_essM; EssPawn* p = t_essP;
    if (!g || !p || !p->init) { EssWhy("mantle draw without pose", g != nullptr, p != nullptr); return false; }
    if (!EssEnsureGPU(dev, g)) { EssWhy("mantle GPU resources", g->m ? g->m->design : -1, g->gpuFails); return false; }
    const ess::Mantle& m = *g->m;
    static std::vector<float> pos, vtx;
    ess::SkinPoints(*g->anim, m, g->amap, p->cur, pos);
    EssSwayApply(g, p, pos);
    {   // pull the upper part towards the chest: +Y is the front in the body mesh's space (the mantle hangs at -Y of the spine)
        const float sb = g->bodyIdx >= 0 && g->bodyIdx < 14 ? g_cfg.essenceSnugBody[g->bodyIdx] : -999.f;
        const float snug = sb > -900.f ? sb : g_cfg.essenceSnug;
        if (snug != 0.f && g->snugW.size() == (size_t)m.np) for (int i = 0; i < m.np; ++i) pos[(size_t)i * 3 + 1] += snug * g->snugW[(size_t)i];
    }
    vtx.resize((size_t)m.nw * 8);
    // lighting: the engine lit the carrier with its own D3D lights; reuse them when they are active (auto) or whenever asked (1)
    bool lit = false; int nLights = 0; DWORD engineLighting = 0;
    if (g_cfg.essenceLight != 0) {
        dev->GetRenderState(D3DRS_LIGHTING, &engineLighting);
        for (DWORD i = 0; i < 16; ++i) { BOOL en = FALSE; if (SUCCEEDED(dev->GetLightEnable(i, &en)) && en) ++nLights; }
        lit = g_cfg.essenceLight == 1 || (engineLighting && nLights > 0);
    }
    static std::vector<float> nrm;
    const bool needN = lit || g->hasEnv;
    if (needN) {
        ess::PointNormals(m, pos, nrm);
        if (g->normSign < 0.f) for (float& v : nrm) v = -v;
    }
    ess::FillVertices(m, pos, needN ? &nrm : nullptr, g_cfg.essenceScale, g_cfg.essenceOffset, vtx.data());
    void* dst = nullptr;
    if (FAILED(g->vb->Lock(0, (UINT)m.nw * 32, &dst, 0)) || !dst) { EssWhy("mantle vertex buffer lock", m.design); return false; }
    memcpy(dst, vtx.data(), (size_t)m.nw * 32);
    g->vb->Unlock();

    IDirect3DVertexBuffer9* oldVB = nullptr; UINT oldOff = 0, oldStride = 0;
    IDirect3DIndexBuffer9* oldIB = nullptr; IDirect3DBaseTexture9* oldT0 = nullptr; IDirect3DBaseTexture9* oldT1 = nullptr; IDirect3DPixelShader9* oldPS = nullptr;
    dev->GetStreamSource(0, &oldVB, &oldOff, &oldStride);
    dev->GetIndices(&oldIB); dev->GetTexture(0, &oldT0); dev->GetTexture(1, &oldT1); dev->GetPixelShader(&oldPS);
    DWORD rs[kEssNRS]; DWORD ts0[kEssNTSS], ts1[2];
    for (int i = 0; i < kEssNRS; ++i) dev->GetRenderState(kEssRS[i], &rs[i]);
    for (int i = 0; i < kEssNTSS; ++i) dev->GetTextureStageState(0, kEssTSS[i], &ts0[i]);
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &ts1[0]); dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &ts1[1]);

    D3DMATERIAL9 oldMat; memset(&oldMat, 0, sizeof oldMat); const bool haveMat = SUCCEEDED(dev->GetMaterial(&oldMat));
    static volatile LONG lightLogs = 0;
    if (g_cfg.debug && InterlockedIncrement(&lightLogs) <= 4) {            // what lighting setup the engine had when it drew the carrier (for diagnosis)
        DWORD amb = 0, norm = 0; dev->GetRenderState(D3DRS_AMBIENT, &amb); norm = rs[9];
        DWORD c0 = 0, a1 = 0, a2 = 0; dev->GetTextureStageState(0, D3DTSS_COLOROP, &c0); dev->GetTextureStageState(0, D3DTSS_COLORARG1, &a1); dev->GetTextureStageState(0, D3DTSS_COLORARG2, &a2);
        Log("essence light: mode %d -> %s | engine LIGHTING=%u, enabled lights=%d, ambient=%08x, normalize=%u, colorvertex=%u, diffuse source=%u | carrier stage0 op=%u arg1=0x%x arg2=0x%x | material diffuse %.2f %.2f %.2f ambient %.2f %.2f %.2f emissive %.2f %.2f %.2f",
            g_cfg.essenceLight, lit ? "LIT" : "unlit", (unsigned)engineLighting, nLights, (unsigned)amb, (unsigned)norm, (unsigned)rs[10], (unsigned)rs[11], (unsigned)c0, (unsigned)a1, (unsigned)a2,
            oldMat.Diffuse.r, oldMat.Diffuse.g, oldMat.Diffuse.b, oldMat.Ambient.r, oldMat.Ambient.g, oldMat.Ambient.b, oldMat.Emissive.r, oldMat.Emissive.g, oldMat.Emissive.b);
        int shown = 0;
        for (DWORD i = 0; i < 16 && shown < 3; ++i) {
            BOOL en = FALSE; D3DLIGHT9 L;
            if (SUCCEEDED(dev->GetLightEnable(i, &en)) && en && SUCCEEDED(dev->GetLight(i, &L))) {
                ++shown;
                Log("essence light:   light %u type %d diffuse %.2f %.2f %.2f ambient %.2f %.2f %.2f dir %.2f %.2f %.2f pos %.0f %.0f %.0f range %.0f atten %.2f %.5f %.7f",
                    (unsigned)i, (int)L.Type, L.Diffuse.r, L.Diffuse.g, L.Diffuse.b, L.Ambient.r, L.Ambient.g, L.Ambient.b, L.Direction.x, L.Direction.y, L.Direction.z,
                    L.Position.x, L.Position.y, L.Position.z, L.Range, L.Attenuation0, L.Attenuation1, L.Attenuation2);
            }
        }
    }
    DWORD litOp = D3DTOP_MODULATE2X;                                         // what the engine uses for its lit characters (seen in the log: stage 0 op=5)
    if (lit) { DWORD eng = 0; dev->GetTextureStageState(0, D3DTSS_COLOROP, &eng); if (eng == D3DTOP_MODULATE || eng == D3DTOP_MODULATE2X || eng == D3DTOP_MODULATE4X) litOp = eng; }
    const DWORD bright = (DWORD)(g_cfg.essenceBright * 255.f > 255.f ? 255.f : g_cfg.essenceBright * 255.f);
    dev->SetPixelShader(nullptr);
    dev->SetStreamSource(0, g->vb, 0, 32);
    dev->SetIndices(g->ib);
    dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    if (lit) {
        const float lb = g_cfg.essenceLitBright < 0.f ? 0.f : g_cfg.essenceLitBright;
        D3DMATERIAL9 mat; memset(&mat, 0, sizeof mat);
        mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = lb; mat.Diffuse.a = 1.f;
        mat.Ambient = mat.Diffuse;                                                    // no emissive, no specular: the texture carries the look
        dev->SetMaterial(&mat);
        dev->SetRenderState(D3DRS_LIGHTING, TRUE);
        dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
        dev->SetRenderState(D3DRS_COLORVERTEX, FALSE);
        dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL); dev->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        dev->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL); dev->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    } else {
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    }
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, bright, bright, bright));
    dev->SetTextureStageState(0, D3DTSS_COLOROP, lit ? litOp : D3DTOP_MODULATE); dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_COLORARG2, lit ? D3DTA_DIFFUSE : D3DTA_TFACTOR);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetTexture(1, nullptr);

    bool ok = true;
    for (const ess::Section& sc : m.secs) {
        const int mi = sc.mat < (int)m.mats.size() ? sc.mat : 0;
        const ess::Material& mt = m.mats[(size_t)mi];
        dev->SetTexture(0, g->tex[mi < 8 ? mi : 0]);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, mt.alphaTest ? TRUE : FALSE);
        dev->SetRenderState(D3DRS_ALPHAREF, (DWORD)mt.alphaRef);
        dev->SetRenderState(D3DRS_CULLMODE, mt.twoSided ? D3DCULL_NONE : rs[5]);
        const HRESULT hr = reinterpret_cast<DIP_t>(h->dip)(dev, D3DPT_TRIANGLELIST, 0, 0, (UINT)m.nw, (UINT)sc.first * 3, (UINT)sc.count);
        if (FAILED(hr)) { ok = false; static volatile LONG once = 0; if (InterlockedExchange(&once, 1) == 0) Log("essence: DrawIndexedPrimitive failed 0x%08x", (unsigned)hr); }
        if (SUCCEEDED(hr) && (size_t)mi < g->fxProg.size() && g->fxProg[(size_t)mi])
            EssDrawFxLayers(dev, h, *g->fxProg[(size_t)mi], g->tex[mi < 8 ? mi : 0], mt.alphaRef, mt.twoSided != 0, rs[5], (UINT)m.nw, (UINT)sc.first, (UINT)sc.count, EssPackTexture);
    }

    for (int i = 0; i < kEssNRS; ++i) dev->SetRenderState(kEssRS[i], rs[i]);
    if (haveMat) dev->SetMaterial(&oldMat);
    for (int i = 0; i < kEssNTSS; ++i) dev->SetTextureStageState(0, kEssTSS[i], ts0[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, ts1[0]); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, ts1[1]);
    dev->SetTexture(0, oldT0); dev->SetTexture(1, oldT1);
    dev->SetPixelShader(oldPS);
    dev->SetIndices(oldIB);
    dev->SetStreamSource(0, oldVB, oldOff, oldStride);
    if (oldVB) oldVB->Release();
    if (oldIB) oldIB->Release();
    if (oldT0) oldT0->Release();
    if (oldT1) oldT1->Release();
    if (oldPS) oldPS->Release();
    static volatile LONG drawn = 0;
    if (g_cfg.debug && InterlockedIncrement(&drawn) <= 6) Log("essence: drew %s (%d sections) %s%s", m.meshName.c_str(), (int)m.secs.size(), ok ? "ok" : "WITH ERRORS", lit ? " [lit]" : " [unlit]");
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Standard Essence cloaks: cloth simulation (src/cloth.h) in the actor space of the body, pinned to the real Spine2 bone
// ---------------------------------------------------------------------------------------------------------------------------
static ess::ClothPack g_cpack; static volatile LONG g_cpackState = 0;          // 0 not loaded, 1 ready, -1 failed
static void EssLoadClothPackOnce() {
    if (!g_cfg.essenceCloth || g_cpackState != 0) return;
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s%s", g_dir, g_cfg.essenceClothPack);
    std::string err; const ULONGLONG t0 = GetTickCount64();
    if (!ess::LoadClothPack(path, g_cpack, &err)) { Log("essence cloth: pack %ls NOT loaded (%s)", path, err.c_str()); g_cpackState = -1; return; }
    Log("essence cloth: pack %ls loaded in %llu ms: %d textures, %d sets, %d looks", path, (unsigned long long)(GetTickCount64() - t0), (int)g_cpack.texs.size(), (int)g_cpack.sets.size(), (int)g_cpack.looks.size());
    g_cpackState = 1;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Clan crest in the window of the clan cloaks.  UNetworkHandler::GetPledgeCrestTex(id) returns the engine UTexture of a clan crest;
// the hook watches those calls (which crest ids are alive), finds the field of the pawn that holds the id (a dword equal to one of
// them), reads the UTexture's mip data and uploads it as a Direct3D texture.  The object layout is not documented: everything is
// detected at run time and dumped to the log (`crest:` lines) so it can be corrected.
// ---------------------------------------------------------------------------------------------------------------------------
using PledgeCrest_t = void* (__fastcall*)(void* self, void* edx, int id);
static PledgeCrest_t g_origPledgeCrest = nullptr, g_origPledgeCrestFromId = nullptr; static void* g_netHandler = nullptr;
struct CrestRec { int id; void* utex; int which; };           // which: 1 = GetPledgeCrestTex, 2 = GetPledgeCrestTexFromPledgeCrestID
static CrestRec g_crests[24]; static volatile LONG g_ncrests = 0;
static int g_crestPawnOff = -1;                                   // offset of the pledge crest id inside the pawn object (-1 = unknown)

static void CrestSeen(void* self, int id, void* tex, int which) {
    g_netHandler = self;
    if (id == 0) return;
    LONG n = g_ncrests; int k = 0;
    for (; k < n; ++k) if (g_crests[k].id == id) break;
    if (k < n) { if (tex) { g_crests[k].utex = tex; g_crests[k].which = which; } }
    else if (n < 24) { g_crests[n].id = id; g_crests[n].utex = tex; g_crests[n].which = which; g_ncrests = n + 1; Log("crest: engine asked (%s) for crest id %d -> UTexture %p", which == 1 ? "GetPledgeCrestTex" : "GetPledgeCrestTexFromPledgeCrestID", id, tex); }
}
static void* __fastcall HkPledgeCrest(void* self, void* edx, int id) { void* tex = g_origPledgeCrest(self, edx, id); CrestSeen(self, id, tex, 1); return tex; }
static void* __fastcall HkPledgeCrestFromId(void* self, void* edx, int id) { void* tex = g_origPledgeCrestFromId(self, edx, id); CrestSeen(self, id, tex, 2); return tex; }

static bool CrestReadable(const void* p, size_t n) { __try { volatile const BYTE* b = static_cast<const BYTE*>(p); for (size_t i = 0; i < n; i += 64) (void)b[i]; (void)b[n - 1]; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool PtrLike(DWORD v) { return v > 0x00100000u && v < 0x7FFF0000u && (v & 3) == 0; }

// pawn field that holds a pledge crest id: a dword equal to the id of a crest the engine has asked for - in the pawn itself or one pointer away
// (PlayerReplicationInfo / user info objects).  g_crestPawnOff1 = -1: the id is at pawn + off2; otherwise at *(pawn + off1) + off2.
static int g_crestPawnOff1 = -1;
static bool CrestSeenId(int v) { if (v < 100) return false; for (LONG k = 0; k < g_ncrests; ++k) if (g_crests[k].id == v) return true; return false; }
static bool CrestFindPawnOffset(void* pawn) {
    if (g_ncrests <= 0) return false;
    const BYTE* b = static_cast<const BYTE*>(pawn);
    if (!CrestReadable(b, 0x1800)) return false;
    for (int off = 0x40; off < 0x1800; off += 4) if (CrestSeenId(*reinterpret_cast<const int*>(b + off))) { g_crestPawnOff1 = -1; g_crestPawnOff = off; Log("crest: pawn %p holds crest id %d at offset 0x%x", pawn, *reinterpret_cast<const int*>(b + off), off); return true; }
    for (int off1 = 0x40; off1 < 0x1000; off1 += 4) {
        const DWORD p = *reinterpret_cast<const DWORD*>(b + off1);
        if (!PtrLike(p) || !CrestReadable(reinterpret_cast<const void*>(p), 0x800)) continue;
        const BYTE* q = reinterpret_cast<const BYTE*>(p);
        for (int off2 = 0; off2 < 0x800; off2 += 4) if (CrestSeenId(*reinterpret_cast<const int*>(q + off2))) {
            g_crestPawnOff1 = off1; g_crestPawnOff = off2; Log("crest: pawn %p holds crest id %d behind the pointer at +0x%x, offset 0x%x", pawn, *reinterpret_cast<const int*>(q + off2), off1, off2); return true;
        }
    }
    return false;
}
static int CrestPawnId(void* pawn) {
    if (g_crestPawnOff < 0) return 0;
    const BYTE* b = static_cast<const BYTE*>(pawn);
    if (g_crestPawnOff1 < 0) { if (!CrestReadable(b + g_crestPawnOff, 4)) return 0; return *reinterpret_cast<const int*>(b + g_crestPawnOff); }
    if (!CrestReadable(b + g_crestPawnOff1, 4)) return 0;
    const DWORD p = *reinterpret_cast<const DWORD*>(b + g_crestPawnOff1);
    if (!PtrLike(p) || !CrestReadable(reinterpret_cast<const void*>(p + g_crestPawnOff), 4)) return 0;
    return *reinterpret_cast<const int*>(p + g_crestPawnOff);
}

// dims / format / data pointer of an engine UTexture (layout found by pattern: USize, VSize, UBits, VBits)
struct CrestImage { int w = 0, h = 0, fmt = 0; const BYTE* data = nullptr; size_t size = 0; };
static bool CrestExtract(void* utex, CrestImage* out, bool dump) {
    const BYTE* b = static_cast<const BYTE*>(utex);
    if (!b || !CrestReadable(b, 0x200)) return false;
    if (dump) {
        char line[200];
        for (int o = 0; o < 0x1A0; o += 32) {
            int n = 0; for (int i = 0; i < 32; i += 4) n += _snprintf_s(line + n, sizeof line - (size_t)n, _TRUNCATE, "%08x ", *reinterpret_cast<const unsigned*>(b + o + i));
            Log("crest: UTexture %p +0x%03x: %s", utex, o, line);
        }
    }
    auto pow2 = [](int v) { return v >= 4 && v <= 2048 && (v & (v - 1)) == 0; };
    auto lg = [](int v) { int n = 0; while ((1 << n) < v) ++n; return n; };
    // 1) dimensions: USize, VSize (ints) followed or preceded by UBits, VBits bytes
    int w = 0, h = 0, wo = -1;
    for (int o = 0x28; o < 0x1B0; o += 4) {
        const int u = *reinterpret_cast<const int*>(b + o), v = *reinterpret_cast<const int*>(b + o + 4);
        if (!pow2(u) || !pow2(v)) continue;
        const BYTE* q = b + o + 8; const BYTE* p = b + o - 2;
        if ((q[0] == lg(u) && q[1] == lg(v)) || (p[0] == lg(u) && p[1] == lg(v))) { w = u; h = v; wo = o; break; }
    }
    if (!w) { if (dump) Log("crest: no USize/VSize pattern in UTexture %p", utex); return false; }
    // 2) format byte: just before the dimensions region, a small value among the known ETextureFormat codes (3 DXT1, 5 RGBA8, 7 DXT3, 8 DXT5)
    int fmt = 0;
    for (int o = 0x28; o < 0x60; ++o) { const int v = b[o]; if (v == 3 || v == 5 || v == 7 || v == 8) { fmt = v; break; } }
    // 3) data pointer: a pointer in the object, or inside a block it points to, whose target holds >= the size of the top mip
    size_t need = fmt == 3 ? (size_t)(w / 4) * (h / 4) * 8 : (fmt == 7 || fmt == 8) ? (size_t)(w / 4) * (h / 4) * 16 : (size_t)w * h * 4;
    const BYTE* best = nullptr; int bestOff = -1;
    for (int o = 0x28; o < 0x1A0 && !best; o += 4) {
        const DWORD v = *reinterpret_cast<const DWORD*>(b + o);
        if (!PtrLike(v)) continue;
        if (CrestReadable(reinterpret_cast<const void*>(v), need) ) {
            // an array header (ptr, num, max)? then the elements follow the pointer: look for the data inside the first element
            const DWORD* e = reinterpret_cast<const DWORD*>(v);
            if (CrestReadable(e, 0x40)) {
                for (int k = 0; k < 8; ++k) {                             // FMipmap = { data pointer, USize, VSize, UBits, VBits ... } or { lazy array ..., USize ... }
                    const DWORD d = e[k];
                    if (PtrLike(d) && k + 2 < 16 && e[k + 1] == (DWORD)w && e[k + 2] == (DWORD)h && CrestReadable(reinterpret_cast<const void*>(d), need)) { best = reinterpret_cast<const BYTE*>(d); bestOff = o; break; }
                }
            }
        }
    }
    if (dump) Log("crest: UTexture %p -> %dx%d (dims at +0x%x) format code %d, data %s (found via +0x%x)", utex, w, h, wo, fmt, best ? "located" : "NOT located", bestOff);
    if (!best) return false;
    out->w = w; out->h = h; out->fmt = fmt; out->data = best; out->size = need;
    return true;
}

struct CrestTex { int id; void* utex; IDirect3DTexture9* tex; IDirect3DDevice9* dev; int fails; };
static CrestTex g_ctexs[16];
static IDirect3DTexture9* CrestD3D(IDirect3DDevice9* dev, int id) {
    void* utex = nullptr;
    for (LONG k = 0; k < g_ncrests; ++k) if (g_crests[k].id == id) utex = g_crests[k].utex;
    if (!utex && g_netHandler) { if (g_origPledgeCrestFromId) utex = g_origPledgeCrestFromId(g_netHandler, nullptr, id); if (!utex && g_origPledgeCrest) utex = g_origPledgeCrest(g_netHandler, nullptr, id); }
    if (!utex) return nullptr;
    CrestTex* c = nullptr;
    for (auto& e : g_ctexs) if (e.id == id) { c = &e; break; }
    if (!c) { for (auto& e : g_ctexs) if (e.id == 0) { c = &e; break; } if (!c) c = &g_ctexs[0]; *c = CrestTex(); c->id = id; }
    if (c->tex && c->utex == utex && c->dev == dev) return c->tex;
    if (c->fails >= 3) return nullptr;
    if (c->tex) { c->tex->Release(); c->tex = nullptr; }
    CrestImage im;
    if (!CrestExtract(utex, &im, c->fails == 0)) { ++c->fails; return nullptr; }
    D3DFORMAT f = im.fmt == 3 ? D3DFMT_DXT1 : im.fmt == 7 ? D3DFMT_DXT3 : im.fmt == 8 ? D3DFMT_DXT5 : D3DFMT_A8R8G8B8;
    IDirect3DTexture9* tx = nullptr;
    if (FAILED(dev->CreateTexture((UINT)im.w, (UINT)im.h, 1, 0, f, D3DPOOL_MANAGED, &tx, nullptr)) || !tx) { ++c->fails; return nullptr; }
    D3DLOCKED_RECT lr;
    if (FAILED(tx->LockRect(0, &lr, nullptr, 0))) { tx->Release(); ++c->fails; return nullptr; }
    const bool comp = f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5;
    const UINT bpr = comp ? (UINT)(im.w / 4) * (f == D3DFMT_DXT1 ? 8 : 16) : (UINT)im.w * 4, rows = comp ? (UINT)im.h / 4 : (UINT)im.h;
    __try { for (UINT r = 0; r < rows; ++r) memcpy(static_cast<BYTE*>(lr.pBits) + (size_t)r * lr.Pitch, im.data + (size_t)r * bpr, bpr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { tx->UnlockRect(0); tx->Release(); ++c->fails; return nullptr; }
    tx->UnlockRect(0);
    c->tex = tx; c->utex = utex; c->dev = dev; c->fails = 0;
    Log("crest: id %d uploaded as %dx%d texture (D3D format %d)", id, im.w, im.h, (int)f);
    return tx;
}


struct ClothPawn {
    void* key = nullptr; const cloth::Set* set = nullptr; int look = -1;
    cloth::State st; ULONGLONG last = 0; float wind = -10.f;
    void* bodyMesh = nullptr; int capA[8] = {}, capB[8] = {}; int ncap = 0;
    int logs = 0; bool ready = false;
    bool refSet = false; float Rref[9];                   // the pawn's Spine2 orientation while idle (reference of the pinned pose)
    int poseLogs = 0; int runLogs = 0; ULONGLONG lastRunLog = 0;
    int crestId = 0; ULONGLONG crestCheck = 0; int dumps = 0; ULONGLONG lastDump = 0;
    bool skelDump = false;
    std::vector<float> partTarget; int idleFrames = 0; bool idleDump = false; const cloth::Set* wSet = nullptr; const ess::Collar* wCol = nullptr; float wAmount = -1.f;   // lateral stretch per row of the drawn cloth (collar and cloth meet without a step)
    float Bref[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; bool haveB = false; int bUA[2] = { -1, -1 }, bNeck = -1, bPel = -1;   // real torso frame (x = left, y = front, z = up) at the moment Rref was taken
    const EssMantle* gDrv = nullptr; const cloth::Set* gSet = nullptr; guide::Map gm; float guideK = 0.f;   // guide from the baked cape
    float posOut[256 * 3];                                // positions to draw
    cloth::Pose pose;                                     // the pinned pose of the last update (the rigid collar uses the same transform as the anchors)
    const ess::Collar* collar = nullptr;
};
static ClothPawn g_cp[48];
static int g_ncp = 0;

static int ClothBoneIdx(void* bodyMesh, const std::string& name) {
    wchar_t w1[80], w2[80]; int n = 0;
    for (; n < 78 && name[(size_t)n]; ++n) { w1[n] = (wchar_t)name[(size_t)n]; w2[n] = name[(size_t)n] == '_' ? L' ' : (wchar_t)name[(size_t)n]; }
    w1[n] = 0; w2[n] = 0;
    const wchar_t* list[2] = { w1, w2 };
    return EssFindBoneIdx(bodyMesh, list, 2);
}

static int ClothWindClass(const std::string& n) {            // 0 idle, 1 walk, 2 run, 3 sit, 4 attack
    auto sw = [&](const char* p) { return n.compare(0, strlen(p), p) == 0; };
    if (sw("atkwait") || sw("wait")) return 0;
    if (sw("run") || sw("fly") || sw("swim")) return 2;
    if (sw("walk")) return 1;
    if (sw("sit") || sw("chair")) return 3;
    if (sw("atk") || sw("spatk") || sw("shieldatk") || sw("magic") || sw("skill") || sw("social")) return 4;
    return 0;
}

static bool Inv4(const double* m, double* inv) {              // general 4x4 inverse (row-major)
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { a[i][j] = m[i * 4 + j]; a[i][4 + j] = (i == j) ? 1.0 : 0.0; }
    for (int c = 0; c < 4; ++c) {
        int piv = c; for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[piv][c])) piv = r;
        if (fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[piv][j]);
        const double d = a[c][c]; for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; if (f != 0.0) for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) inv[i * 4 + j] = a[i][4 + j];
    return true;
}

// the camera position in the actor space of the pawn (so the lit normals can face the viewer: the cloth has two sides, D3D lighting has one)
static bool ClothCameraPos(IDirect3DDevice9* dev, float* c) {
    D3DMATRIX W, V;
    if (FAILED(dev->GetTransform(D3DTS_WORLD, &W)) || FAILED(dev->GetTransform(D3DTS_VIEW, &V))) return false;
    double w[16], v[16], m[16], inv[16];
    memcpy(w, &W, 0); const float* wf = &W._11; const float* vf = &V._11;
    for (int i = 0; i < 16; ++i) { w[i] = wf[i]; v[i] = vf[i]; }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { double s = 0; for (int k = 0; k < 4; ++k) s += w[i * 4 + k] * v[k * 4 + j]; m[i * 4 + j] = s; }
    if (!Inv4(m, inv)) return false;
    c[0] = (float)(inv[12] / inv[15]); c[1] = (float)(inv[13] / inv[15]); c[2] = (float)(inv[14] / inv[15]);
    return true;
}

static bool ClothReadPtrs(void* bodyInst, void* pawn, void** bodyMesh, void** subInst) {   // POD + SEH: engine memory
    __try { *bodyMesh = *reinterpret_cast<void**>(P(bodyInst, kInstMesh)); *subInst = *reinterpret_cast<void**>(P(pawn, 0x134)); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}


static int ClothBoneNameIdx(void* bodyMesh, int i, int* count) {        // FName index of bone i of the body mesh (-1 = none); *count = number of bones
    __try {
        const char* data = *reinterpret_cast<char**>(P(bodyMesh, kBoneData));
        const int n = *reinterpret_cast<int*>(P(bodyMesh, kBoneNum));
        *count = n;
        if (!data || n <= 0 || n > 200 || i < 0 || i >= n) return -1;
        return *reinterpret_cast<const int*>(data + (size_t)i * kBoneStride);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// the torso frame in the actor space from the skeleton: x = towards the character's left (shoulder line), z = up (pelvis -> neck), y = front. R = [x y z] (columns).
static bool ClothTorsoFrame(void* src, const int* ids, float* R) {            // ids: left upper arm, right upper arm, neck, pelvis
    float ul[12], ur[12], nk[12], pv[12];
    if (ids[0] < 0 || ids[1] < 0 || ids[2] < 0 || ids[3] < 0) return false;
    if (!EssReadBone(src, ids[0], ul) || !EssReadBone(src, ids[1], ur) || !EssReadBone(src, ids[2], nk) || !EssReadBone(src, ids[3], pv)) return false;
    float l[3] = { ul[0] - ur[0], ul[1] - ur[1], ul[2] - ur[2] }, u[3] = { nk[0] - pv[0], nk[1] - pv[1], nk[2] - pv[2] };
    auto norm = [](float* v) { const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (n < 1e-4f) return false; v[0] /= n; v[1] /= n; v[2] /= n; return true; };
    if (!norm(u)) return false;
    const float d = l[0] * u[0] + l[1] * u[1] + l[2] * u[2];
    for (int k = 0; k < 3; ++k) l[k] -= d * u[k];
    if (!norm(l)) return false;
    float f[3] = { u[1] * l[2] - u[2] * l[1], u[2] * l[0] - u[0] * l[2], u[0] * l[1] - u[1] * l[0] };       // front = up x left
    if (!norm(f)) return false;
    for (int k = 0; k < 3; ++k) { R[k * 3] = l[k]; R[k * 3 + 1] = f[k]; R[k * 3 + 2] = u[k]; }
    return true;
}

static void ClothDumpBones(void* bodyMesh, void* src, void* pawn) {      // diagnostics: every bone of the body at rest (where is the body's centre line, the shoulders ...)
    int n = 0; ClothBoneNameIdx(bodyMesh, 0, &n);
    Log("essence cloth: SKELETON of pawn %p (%d bones; origin and X/Y/Z axes in the actor space)", pawn, n);
    for (int i = 0; i < n && i < 120; ++i) {
        wchar_t nm[64]; int cnt = 0; FNameStr(ClothBoneNameIdx(bodyMesh, i, &cnt), nm, 64);
        float c[12];
        if (!EssReadBone(src, i, c)) continue;
        Log("essence cloth: bone %3d %-22ls origin %7.2f %7.2f %7.2f | X %5.2f %5.2f %5.2f | Y %5.2f %5.2f %5.2f | Z %5.2f %5.2f %5.2f", i, nm, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11]);
    }
}


// look of this carrier id -> prepares the simulation; returns true when something has to be drawn (t_clothP set)
static bool EssPrepareCloth(int bodyIdx, int lookIdx, void* bodyInst, void* pawn, ULONGLONG now) {
    if (g_cpackState == 0) EssLoadClothPackOnce();
    if (g_cpackState != 1 || lookIdx < 0 || lookIdx >= (int)g_cpack.looks.size() || g_cal.attSrc < 0) {
        // the torso calibration (shared with the animated mantles) must be finished: run the driver below once so it can complete
        if (g_cpackState == 1 && lookIdx >= 0 && lookIdx < (int)g_cpack.looks.size()) { EssMantle* drv0 = EssFind(bodyIdx, 0); if (drv0) EssUpdate(drv0, bodyInst, pawn); }
        t_essM = nullptr; t_essP = nullptr;
        return false;
    }
    const ess::ClothLook& lk = g_cpack.looks[(size_t)lookIdx];
    const cloth::Set* set = ess::FindClothSet(g_cpack, kBodiesA(bodyIdx), lk.kind);
    if (!set) set = ess::FindClothSet(g_cpack, kBodiesA(bodyIdx), 'H');
    EssMantle* drv = EssFind(bodyIdx, 0);                      // the body's animated mantle only drives the pose (baked root + sequence name + engine Spine2)
    if (!set || !drv) return false;
    if (!EssUpdate(drv, bodyInst, pawn)) { t_essM = nullptr; t_essP = nullptr; return false; }
    EssPawn* ep = t_essP; t_essM = nullptr; t_essP = nullptr;
    if (!ep || !ep->init) return false;

    ClothPawn* cp = nullptr;
    for (int i = 0; i < g_ncp; ++i) if (g_cp[i].key == pawn) { cp = &g_cp[i]; break; }
    if (!cp) { cp = (g_ncp < 48) ? &g_cp[g_ncp++] : &g_cp[0]; *cp = ClothPawn(); cp->key = pawn; }
    if (cp->set != set) { cp->set = set; cp->st = cloth::State(); cp->bodyMesh = nullptr; cp->ready = false; }
    cp->look = lookIdx;

    // body bones of the colliders (resolved by name once per body mesh)
    void* bodyMesh = nullptr; void* subInst = nullptr;
    if (!ClothReadPtrs(bodyInst, pawn, &bodyMesh, &subInst)) return false;
    if (cp->bodyMesh != bodyMesh) {
        cp->bodyMesh = bodyMesh; cp->ncap = 0;
        cp->bUA[0] = ClothBoneIdx(bodyMesh, "Bip01_L_UpperArm"); cp->bUA[1] = ClothBoneIdx(bodyMesh, "Bip01_R_UpperArm"); cp->bNeck = ClothBoneIdx(bodyMesh, "Bip01_Neck"); cp->bPel = ClothBoneIdx(bodyMesh, "Bip01_Pelvis");
        for (size_t i = 0; i < set->caps.size() && cp->ncap < 8; ++i) { cp->capA[cp->ncap] = ClothBoneIdx(bodyMesh, set->caps[i].a); cp->capB[cp->ncap] = ClothBoneIdx(bodyMesh, set->caps[i].b); ++cp->ncap; }
        if (g_cfg.debug) { int ok = 0; for (int i = 0; i < cp->ncap; ++i) if (cp->capA[i] >= 0 && cp->capB[i] >= 0) ++ok; Log("essence cloth: pawn %p %s look %d '%s': %d of %d colliders resolved on body mesh %p", pawn, set->body.c_str(), lookIdx, lk.name.c_str(), ok, cp->ncap, bodyMesh); }
    }

    // pinned pose: x' = Rs * (x - tb) + gt with Rs = R(engine Spine2) * Rb^T, (Rb, tb) = bind of the mantle root (= the Spine2 bind in the Essence mesh space)
    const ess::Mantle& dm = *drv->m;
    int jr = 0; for (size_t j = 0; j < drv->amap.size(); ++j) if (drv->amap[j] == 0) { jr = (int)j; break; }
    float R0[9]; ess::QuatToMat(&ep->cur.q[0], R0);
    const float* Rb = &dm.bindR[(size_t)jr * 9];
    const float RbT[9] = { Rb[0], Rb[3], Rb[6], Rb[1], Rb[4], Rb[7], Rb[2], Rb[5], Rb[8] };
    int wc0 = 0;
    if (ep->seq >= 0 && ep->seq < (int)drv->anim->seqs.size()) wc0 = ClothWindClass(drv->anim->seqs[(size_t)ep->seq].name);
    if (!cp->refSet || (wc0 == 0 && !cp->ready)) {                                                   // first sample (or the first idle one) is the rest orientation
        memcpy(cp->Rref, R0, sizeof R0); cp->refSet = true;
        float B[9];
        const int tids[4] = { cp->bUA[0], cp->bUA[1], cp->bNeck, cp->bPel };
        cp->haveB = g_cfg.essenceClothTorso && ClothTorsoFrame(g_cal.attSrc == 0 ? subInst : bodyInst, tids, B);
        if (cp->haveB) {
            memcpy(cp->Bref, B, sizeof B);
            Log("essence cloth: pawn %p torso frame at the reference pose: left %.2f %.2f %.2f, front %.2f %.2f %.2f, up %.2f %.2f %.2f (yaw %.1f deg from the actor axes)", pawn, B[0], B[3], B[6], B[1], B[4], B[7], B[2], B[5], B[8], atan2f(-B[1], B[4]) * 57.29578f);
        } else Log("essence cloth: pawn %p torso frame unavailable (bones %d %d %d %d): cloak assumed to face straight ahead at the reference pose", pawn, cp->bUA[0], cp->bUA[1], cp->bNeck, cp->bPel);
    }
    const float* Rr = cp->Rref;
    const float RrT[9] = { Rr[0], Rr[3], Rr[6], Rr[1], Rr[4], Rr[7], Rr[2], Rr[5], Rr[8] };
    cloth::Pose po;
    if (g_cfg.essenceClothRef) { ess::Mul33(R0, RrT, po.Rs); if (cp->haveB) ess::Mul33(po.Rs, cp->Bref, po.Rs); } else ess::Mul33(R0, RbT, po.Rs);
    if (g_cfg.debug && cp->poseLogs < 6) {
        ++cp->poseLogs;
        float A[9]; ess::Mul33(R0, RbT, A);
        const float tr1 = (A[0] + A[4] + A[8] - 1.f) * 0.5f, tr2 = (po.Rs[0] + po.Rs[4] + po.Rs[8] - 1.f) * 0.5f;
        Log("essence cloth: pawn %p pinned pose: angle(engine Spine2 vs Essence bind) %.1f deg, angle used %.1f deg (%s), tb %.1f %.1f %.1f", pawn,
            acosf(tr1 > 1.f ? 1.f : (tr1 < -1.f ? -1.f : tr1)) * 57.29578f, acosf(tr2 > 1.f ? 1.f : (tr2 < -1.f ? -1.f : tr2)) * 57.29578f, g_cfg.essenceClothRef ? "idle reference" : "Essence bind",
            dm.bindT[(size_t)jr * 3], dm.bindT[(size_t)jr * 3 + 1], dm.bindT[(size_t)jr * 3 + 2]);
    }
    for (int k = 0; k < 3; ++k) { po.tb[k] = dm.bindT[(size_t)jr * 3 + k]; po.gt[k] = ep->cur.p[k]; }
    po.gt[1] += g_cfg.essenceClothSnug;

    cloth::CapsuleWorld caps[8]; int nc = 0; int thighIdx[2] = { -1, -1 }, nthigh = 0;
    for (int i = 0; i < cp->ncap; ++i) {
        float ca[12], cb[12];
        if (cp->capA[i] < 0 || cp->capB[i] < 0) continue;
        void* src = g_cal.attSrc == 0 ? subInst : bodyInst;
        if (!EssReadBone(src, cp->capA[i], ca) || !EssReadBone(src, cp->capB[i], cb)) continue;
        for (int k = 0; k < 3; ++k) { caps[nc].a[k] = ca[k]; caps[nc].b[k] = cb[k]; }
        caps[nc].r = set->caps[(size_t)i].r; caps[nc].valid = true;
        { const std::string& na = set->caps[(size_t)i].a; const std::string& nb2 = set->caps[(size_t)i].b;       // torso capsules: the cloak is never pushed through to the front
          caps[nc].backBias = ess::Lower(na).find("spine") != std::string::npos || ess::Lower(nb2).find("spine") != std::string::npos || ess::Lower(na).find("pelvis") != std::string::npos; }
        if (nthigh < 2 && ess::Lower(set->caps[(size_t)i].a).find("thigh") != std::string::npos) thighIdx[nthigh++] = nc;
        ++nc;
    }

    // wind from the action
    int wc = 0;
    if (ep->seq >= 0 && ep->seq < (int)drv->anim->seqs.size()) wc = ClothWindClass(drv->anim->seqs[(size_t)ep->seq].name);
    float dt = cp->last ? (float)(now - cp->last) * 0.001f : 0.016f; if (dt < 0.f) dt = 0.f; if (dt > 0.1f) dt = 0.1f;
    cp->last = now;
    const float target = g_cfg.essenceClothWind[wc];
    cp->wind += (target - cp->wind) * (1.f - expf(-dt / 0.25f));

    {
        if (g_cfg.debug && wc == 0 && !cp->skelDump) { static volatile LONG nd = 0; cp->skelDump = true; if (InterlockedIncrement(&nd) <= 3) ClothDumpBones(bodyMesh, g_cal.attSrc == 0 ? subInst : bodyInst, pawn); }
    }
    if (wc == 3 && g_cfg.essenceClothPelvisSit && nthigh == 2 && nc < 8) {                  // sitting: the hips are a collider too (the cloak lies on the seat behind them)
        for (int k = 0; k < 3; ++k) { caps[nc].a[k] = caps[thighIdx[0]].a[k]; caps[nc].b[k] = caps[thighIdx[1]].a[k]; }
        caps[nc].r = 6.5f; caps[nc].valid = true; caps[nc].backBias = true; ++nc;
    }
    cloth::Params pr;
    pr.gravity = g_cfg.essenceClothGravity; pr.wind[1] = cp->wind; pr.stiffness = g_cfg.essenceClothStiff; pr.damping = g_cfg.essenceClothDamp;
    pr.iterations = g_cfg.essenceClothIter; pr.skin = g_cfg.essenceClothSkin;
    if (g_cfg.essenceClothGuide[0] > 0.f || g_cfg.essenceClothGuide[1] > 0.f || g_cfg.essenceClothGuide[2] > 0.f || g_cfg.essenceClothGuide[3] > 0.f || g_cfg.essenceClothGuide[4] > 0.f) {
        const float gt_ = g_cfg.essenceClothGuide[wc];
        cp->guideK += (gt_ - cp->guideK) * (1.f - expf(-dt / 0.2f));
        if ((cp->gDrv != drv || cp->gSet != set) && drv->m && drv->anim) {
            cp->gDrv = drv; cp->gSet = set; cp->gm.ok = false;
            if (!drv->idleReady) drv->idleReady = guide::IdleLocal(*drv->anim, *drv->m, drv->amap, drv->idleL);
            if (drv->idleReady) { guide::Build(*set, *drv->m, drv->amap, drv->idleL, cp->gm); Log("essence cloth: guide built for %s %s: %d particles mapped on %d cape points", set->body.c_str(), set->kind.c_str(), set->np, (int)cp->gm.sub.size()); }
        }
        if (cp->gm.ok && cp->guideK > 0.002f) { guide::Update(*set, *drv->anim, *drv->m, drv->amap, drv->idleL, ep->cur, po, cp->gm); pr.guide = cp->gm.target.data(); pr.guideK = cp->guideK; }
    }
    if (g_cfg.essenceClothTopRamp > 0.1f && !set->anchors.empty()) {   // the torso capsule is only as thick as the pinned rows are far from its axis, growing to its full radius below them: no step under the anchors
        float zr = 0.f; float aw[64][3]; int na = 0;
        for (uint16_t ai : set->anchors) if (na < 64) {
            const float v[3] = { set->rest[(size_t)ai * 3] - po.tb[0], set->rest[(size_t)ai * 3 + 1] - po.tb[1], set->rest[(size_t)ai * 3 + 2] - po.tb[2] }; float o[3]; cloth::MulVec(po.Rs, v, o);
            for (int k = 0; k < 3; ++k) aw[na][k] = o[k] + po.gt[k];
            zr += aw[na][2]; ++na;
        }
        if (na > 0) for (int c = 0; c < nc; ++c) if (caps[c].valid && caps[c].backBias) {
            const float ab[3] = { caps[c].b[0] - caps[c].a[0], caps[c].b[1] - caps[c].a[1], caps[c].b[2] - caps[c].a[2] }; const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
            float dsum = 0.f;
            for (int i = 0; i < na; ++i) {
                const float ap[3] = { aw[i][0] - caps[c].a[0], aw[i][1] - caps[c].a[1], aw[i][2] - caps[c].a[2] };
                const float t0 = l2 > 1e-9f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.f;                    // distance from the axis LINE: the anchors sit above the capsule's end cap
                const float d[3] = { ap[0] - ab[0] * t0, ap[1] - ab[1] * t0, ap[2] - ab[2] * t0 }; dsum += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            }
            const float rt = dsum / (float)na - g_cfg.essenceClothSkin + 0.15f;
            if (rt > caps[c].r * 0.5f && rt < caps[c].r) { caps[c].rTop = rt; caps[c].zRef = zr / (float)na; caps[c].ramp = g_cfg.essenceClothTopRamp; }
        }
    }
    pr.widthK = g_cfg.essenceClothWidth; pr.follow = g_cfg.essenceClothFollow; pr.inelastic = g_cfg.essenceClothInelastic; pr.maxStep = g_cfg.essenceClothMaxStep;
    cloth::Step(*set, cp->st, pr, po, caps, nc, dt);
    memcpy(cp->posOut, cp->st.p.data(), (size_t)set->np * 3 * sizeof(float));
    cp->pose = po;
    {   // widen the top of the drawn cloth to the collar (visual only; the simulation and the colliders keep the cut width)
        const ess::Collar* wcol = g_cfg.essenceCollar ? ess::FindCollar(g_cpack, kBodiesA(bodyIdx), lk.family) : nullptr;
        if (cp->wSet != set || cp->wCol != wcol || cp->wAmount != g_cfg.essenceClothWiden) { cp->wSet = set; cp->wCol = wcol; cp->wAmount = g_cfg.essenceClothWiden; ess::ClothComputeWiden(*set, wcol, g_cfg.essenceClothWiden, cp->partTarget); }
        ess::ClothApplyWiden(*set, cp->partTarget, po, cp->posOut);
    }
    if (wc == 0) { if (cp->idleFrames < 100000) ++cp->idleFrames; } else cp->idleFrames = 0;
    if (g_cfg.essenceClothDump > 0 && wc == 0 && cp->idleFrames > 240 && !cp->idleDump && (g_cfg.debug || g_cfg.essenceClothDump > 1)) {       // standing still: the shape the cloak settles in, in the cut's own frame
        cp->idleDump = true;
        auto rest_of = [&](const float* x, float* r) { const float v[3] = { x[0] - po.gt[0], x[1] - po.gt[1], x[2] - po.gt[2] }; for (int k = 0; k < 3; ++k) r[k] = po.Rs[k] * v[0] + po.Rs[3 + k] * v[1] + po.Rs[6 + k] * v[2] + po.tb[k]; };
        Log("essence cloth: IDLE '%s' kind %c family %s | %d particles width %d | widen %.2f | Rs rows %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | tb %.2f %.2f %.2f", lk.name.c_str(), set->kind[0], lk.family.c_str(), set->np, set->width, g_cfg.essenceClothWiden,
            po.Rs[0], po.Rs[1], po.Rs[2], po.Rs[3], po.Rs[4], po.Rs[5], po.Rs[6], po.Rs[7], po.Rs[8], po.tb[0], po.tb[1], po.tb[2]);
        for (int k = 0; k < nc; ++k) { float a[3], b[3]; rest_of(caps[k].a, a); rest_of(caps[k].b, b); Log("essence cloth: IDLE collider %d a %.2f %.2f %.2f b %.2f %.2f %.2f r %.2f bias %d", k, a[0], a[1], a[2], b[0], b[1], b[2], caps[k].r, (int)caps[k].backBias); }
        for (int i = 0; i < set->np; ++i) {
            float a[3], d[3]; rest_of(&cp->st.p[(size_t)i * 3], a); rest_of(&cp->posOut[(size_t)i * 3], d);
            Log("essence cloth: IDLE p%d anchor %d rest %.2f %.2f %.2f | simulated %.2f %.2f %.2f | drawn %.2f %.2f %.2f | target half-width %.2f", i, (int)set->isAnchor[(size_t)i], set->rest[(size_t)i * 3], set->rest[(size_t)i * 3 + 1], set->rest[(size_t)i * 3 + 2], a[0], a[1], a[2], d[0], d[1], d[2],
                (size_t)i < cp->partTarget.size() ? cp->partTarget[(size_t)i] : 0.f);
        }
    }
    if (set->kind[0] == 'C' && g_cfg.essenceCrest && (g_origPledgeCrest || g_origPledgeCrestFromId) && now - cp->crestCheck > 1000) {            // crest id of this pawn, re-read once a second
        cp->crestCheck = now;
        if (g_crestPawnOff < 0) CrestFindPawnOffset(pawn);
        cp->crestId = CrestPawnId(pawn);
        if (cp->crestId == 0 && g_ncrests > 0) {                                  // the pawn field is unknown (yet): the most recently seen crest, i.e. normally the player's own clan
            cp->crestId = g_crests[g_ncrests - 1].id;
            static volatile LONG fl = 0; if (InterlockedIncrement(&fl) <= 3) Log("crest: pawn %p: no crest field found, using the last crest seen (id %d)", pawn, cp->crestId);
        }
    }
    cp->collar = g_cfg.essenceCollar ? ess::FindCollar(g_cpack, kBodiesA(bodyIdx), lk.family) : nullptr;
    cp->ready = true;
    if (g_cfg.debug && cp->logs < 12 && (now & 0x3FF) < 40) {
        ++cp->logs;
        Log("essence cloth: pawn %p wind class %d (%.1f) | root gt %.1f %.1f %.1f | %d colliders | particle 0 %.1f %.1f %.1f, last %.1f %.1f %.1f", pawn, wc, cp->wind, po.gt[0], po.gt[1], po.gt[2], nc,
            cp->st.p[0], cp->st.p[1], cp->st.p[2], cp->st.p[(size_t)(set->np - 1) * 3], cp->st.p[(size_t)(set->np - 1) * 3 + 1], cp->st.p[(size_t)(set->np - 1) * 3 + 2]);
    }
    if (g_cfg.debug && wc >= 1 && cp->runLogs < 60 && now - cp->lastRunLog > 350) {          // moving / sitting: where is the cloth relative to the torso, how stretched is it
        ++cp->runLogs; cp->lastRunLog = now;
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f }; int front = 0;
        for (int i = 0; i < set->np; ++i) {
            const float v[3] = { cp->st.p[(size_t)i * 3] - po.gt[0], cp->st.p[(size_t)i * 3 + 1] - po.gt[1], cp->st.p[(size_t)i * 3 + 2] - po.gt[2] };
            const float b[3] = { po.Rs[0] * v[0] + po.Rs[3] * v[1] + po.Rs[6] * v[2], po.Rs[1] * v[0] + po.Rs[4] * v[1] + po.Rs[7] * v[2], po.Rs[2] * v[0] + po.Rs[5] * v[1] + po.Rs[8] * v[2] };
            for (int k = 0; k < 3; ++k) { if (b[k] < mn[k]) mn[k] = b[k]; if (b[k] > mx[k]) mx[k] = b[k]; }
            if (b[1] > 2.f) ++front;
        }
        float worst = 0.f;
        for (const cloth::Spring& sp : set->springs) {
            const float* A = &cp->st.p[(size_t)sp.i * 3]; const float* B = &cp->st.p[(size_t)sp.j * 3];
            const float d = sqrtf((A[0] - B[0]) * (A[0] - B[0]) + (A[1] - B[1]) * (A[1] - B[1]) + (A[2] - B[2]) * (A[2] - B[2]));
            if (sp.rest > 0.5f && d / sp.rest > worst) worst = d / sp.rest;
        }
        const float tr = (po.Rs[0] + po.Rs[4] + po.Rs[8] - 1.f) * 0.5f;
        Log("essence cloth: pawn %p %s class %d seq '%s' kind %c | torso angle vs idle %.0f deg, gt z %.1f | cloth in torso frame x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f] | %d in front | worst stretch %.2fx | %d colliders",
            pawn, wc == 3 ? "SITTING" : "MOVING", wc, (ep->seq >= 0 && ep->seq < (int)drv->anim->seqs.size()) ? drv->anim->seqs[(size_t)ep->seq].name.c_str() : "?", set->kind[0],
            acosf(tr > 1.f ? 1.f : (tr < -1.f ? -1.f : tr)) * 57.29578f, po.gt[2], mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], front, worst, nc);
        if (g_cfg.essenceClothDump > 0 && cp->dumps < 3 && now - cp->lastDump > 1200 && acosf(tr > 1.f ? 1.f : (tr < -1.f ? -1.f : tr)) * 57.29578f > 55.f) {
            ++cp->dumps; cp->lastDump = now;
            Log("essence cloth: DUMP %d pawn %p %s seq '%s' | Rs rows %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | gt %.2f %.2f %.2f | tb %.2f %.2f %.2f | wind %.1f gravity %.1f", cp->dumps, pawn, wc == 3 ? "SITTING" : "MOVING",
                (ep->seq >= 0 && ep->seq < (int)drv->anim->seqs.size()) ? drv->anim->seqs[(size_t)ep->seq].name.c_str() : "?",
                po.Rs[0], po.Rs[1], po.Rs[2], po.Rs[3], po.Rs[4], po.Rs[5], po.Rs[6], po.Rs[7], po.Rs[8], po.gt[0], po.gt[1], po.gt[2], po.tb[0], po.tb[1], po.tb[2], pr.wind[1], pr.gravity);
            for (int k = 0; k < nc; ++k) Log("essence cloth: DUMP %d collider %d a %.2f %.2f %.2f b %.2f %.2f %.2f r %.2f bias %d", cp->dumps, k, caps[k].a[0], caps[k].a[1], caps[k].a[2], caps[k].b[0], caps[k].b[1], caps[k].b[2], caps[k].r, (int)caps[k].backBias);
            for (int i = 0; i < set->np; ++i) {
                const float* x = &cp->st.p[(size_t)i * 3]; const float v[3] = { x[0] - po.gt[0], x[1] - po.gt[1], x[2] - po.gt[2] };
                const float b[3] = { po.Rs[0] * v[0] + po.Rs[3] * v[1] + po.Rs[6] * v[2], po.Rs[1] * v[0] + po.Rs[4] * v[1] + po.Rs[7] * v[2], po.Rs[2] * v[0] + po.Rs[5] * v[1] + po.Rs[8] * v[2] };
                const float* q = &cp->st.q[(size_t)i * 3];
                Log("essence cloth: DUMP %d p%d anchor %d actor %.1f %.1f %.1f torso %.1f %.1f %.1f vel %.1f %.1f %.1f rest %.1f %.1f %.1f", cp->dumps, i, (int)set->isAnchor[(size_t)i], x[0], x[1], x[2], b[0], b[1], b[2],
                    (x[0] - q[0]) * 90.f, (x[1] - q[1]) * 90.f, (x[2] - q[2]) * 90.f, set->rest[(size_t)i * 3], set->rest[(size_t)i * 3 + 1], set->rest[(size_t)i * 3 + 2]);
            }
        }
    }
    t_clothP = cp;
    return true;
}


// ---------------------------------------------------------------------------------------------------------------------------
// Particle effects of the cloaks (wings, rays, glows): essence_fx.bin, simulated by fx.h and drawn as camera-facing sprites / textured meshes
// with additive-style blending, attached to the torso (shoulder line + spine, like the cloth).
// ---------------------------------------------------------------------------------------------------------------------------
static fx::Pack g_fxpack; static volatile LONG g_fxState = 0;                   // 0 not loaded, 1 ready, -1 failed
struct FxMapEntry { int id; int n; int eff[3]; };
static FxMapEntry g_fxMap[64]; static int g_nFxMap = 0;
// 'a+b+c' -> effect indices (up to 3); returns the count
static int FxParseEffects(const char* names, int* out) {
    int n = 0; char buf[160]; strncpy_s(buf, names, _TRUNCATE);
    for (char* ctx = nullptr, *tok = strtok_s(buf, "+", &ctx); tok && n < 3; tok = strtok_s(nullptr, "+", &ctx)) {
        const int ei = fx::FindEffect(g_fxpack, tok);
        if (ei >= 0) out[n++] = ei; else Log("essence fx: effect '%s' is not in the pack", tok);
    }
    return n;
}
static void EssLoadFxPackOnce() {
    if (g_fxState != 0) return;
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s%s", g_dir, g_cfg.essenceFxPack);
    std::string err; const ULONGLONG t0 = GetTickCount64();
    if (!fx::LoadPack(path, g_fxpack, &err)) { Log("essence fx: pack %ls NOT loaded (%s)", path, err.c_str()); g_fxState = -1; return; }
    g_nFxMap = 0;
    wchar_t buf[1024]; wcsncpy_s(buf, g_cfg.essenceFxMap, _TRUNCATE);
    for (wchar_t* p = buf; *p && g_nFxMap < 64;) {
        wchar_t* e = nullptr; const long id = wcstol(p, &e, 10); if (e == p || *e != L':') break;
        wchar_t* n = e + 1; wchar_t* z = n; while (*z && *z != L',' && *z != L' ') ++z; const wchar_t sv = *z; *z = 0;
        char nm[96]; WideCharToMultiByte(CP_ACP, 0, n, -1, nm, sizeof nm, nullptr, nullptr);
        FxMapEntry me; me.id = (int)id; me.n = FxParseEffects(nm, me.eff);
        if (me.n > 0) g_fxMap[g_nFxMap++] = me; else Log("essence fx: item %ld wants '%s' which is not in the pack", id, nm);
        *z = sv; p = z; while (*p == L',' || *p == L' ') ++p;
    }
    Log("essence fx: pack %ls loaded in %llu ms: %d textures, %d meshes, %d effects, %d items with an effect", path, (unsigned long long)(GetTickCount64() - t0), (int)g_fxpack.texs.size(), (int)g_fxpack.meshes.size(), (int)g_fxpack.effects.size(), g_nFxMap);
    g_fxState = 1;
}

struct FxPawn {
    void* key = nullptr; int n = 0, effs[3] = { -1, -1, -1 }; fx::Instance inst[3]; ULONGLONG last = 0;
    void* bodyMesh = nullptr; int ids[4] = { -1, -1, -1, -1 }; int spine2 = -1;       // left upper arm, right upper arm, neck, pelvis; Spine2
    float S[3] = {}, O[3] = {}, B[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; bool ok = false; int logs = 0;     // S = Spine2, O = origin of the effect
};
static FxPawn g_fxp[32]; static int g_nfxp = 0;

static bool EssFxPrepare(void* bodyInst, void* pawn) {
    if (g_fxState == 0) EssLoadFxPackOnce();
    if (g_fxState != 1 || g_cal.attSrc < 0) return false;
    int want[3] = { -1, -1, -1 }, nw = 0;
    for (int i = 0; i < g_nFxMap; ++i) if (g_fxMap[i].id == t_curId) { nw = g_fxMap[i].n; for (int k = 0; k < nw; ++k) want[k] = g_fxMap[i].eff[k]; break; }
    if (g_cfg.essenceFxForce[0]) {                                                     // test mode: these effects on every cloak
        static wchar_t lastName[64] = L""; static int lastEff[3], lastN = 0;
        if (wcscmp(lastName, g_cfg.essenceFxForce) != 0) { wcsncpy_s(lastName, g_cfg.essenceFxForce, _TRUNCATE); char nm[64]; WideCharToMultiByte(CP_ACP, 0, lastName, -1, nm, sizeof nm, nullptr, nullptr); lastN = FxParseEffects(nm, lastEff); Log("essence fx: EssenceFxForce '%s' -> %d effects", nm, lastN); }
        if (lastN > 0) { nw = lastN; for (int k = 0; k < nw; ++k) want[k] = lastEff[k]; }
    }
    if (nw <= 0) return false;
    void* bodyMesh = nullptr; void* subInst = nullptr;
    if (!ClothReadPtrs(bodyInst, pawn, &bodyMesh, &subInst)) return false;
    FxPawn* fp = nullptr;
    for (int i = 0; i < g_nfxp; ++i) if (g_fxp[i].key == pawn) { fp = &g_fxp[i]; break; }
    if (!fp) { fp = (g_nfxp < 32) ? &g_fxp[g_nfxp++] : &g_fxp[0]; *fp = FxPawn(); fp->key = pawn; }
    if (fp->bodyMesh != bodyMesh) {
        fp->bodyMesh = bodyMesh;
        fp->ids[0] = ClothBoneIdx(bodyMesh, "Bip01_L_UpperArm"); fp->ids[1] = ClothBoneIdx(bodyMesh, "Bip01_R_UpperArm"); fp->ids[2] = ClothBoneIdx(bodyMesh, "Bip01_Neck"); fp->ids[3] = ClothBoneIdx(bodyMesh, "Bip01_Pelvis");
        fp->spine2 = ClothBoneIdx(bodyMesh, "Bip01_Spine2");
    }
    void* src = g_cal.attSrc == 0 ? subInst : bodyInst;
    float c[12];
    fp->ok = ClothTorsoFrame(src, fp->ids, fp->B) && fp->spine2 >= 0 && EssReadBone(src, fp->spine2, c);
    if (!fp->ok) return false;
    fp->S[0] = c[0]; fp->S[1] = c[1]; fp->S[2] = c[2];
    {   // origin: the middle of the top edge of the cloth cloak when there is one (like the engine hangs it on the cloak), else the upper back
        const float* off = g_cfg.essenceFxOffset; float base[3] = { fp->S[0], fp->S[1], fp->S[2] };
        const ClothPawn* cp = t_clothP;
        if (cp && cp->set && cp->ready && !cp->set->anchors.empty()) {
            float a[3] = { 0.f, 0.f, 0.f };
            for (uint16_t ai : cp->set->anchors) for (int k = 0; k < 3; ++k) a[k] += cp->posOut[(size_t)ai * 3 + k];
            const float inv = 1.f / (float)cp->set->anchors.size();
            for (int k = 0; k < 3; ++k) base[k] = a[k] * inv;
            off = g_cfg.essenceFxClothOffset;
        }
        for (int k = 0; k < 3; ++k) fp->O[k] = base[k] + fp->B[k * 3] * off[0] + fp->B[k * 3 + 1] * off[1] + fp->B[k * 3 + 2] * off[2];
    }
    const ULONGLONG now = GetTickCount64();
    if (fp->n != nw || memcmp(fp->effs, want, sizeof want) != 0) {
        fp->n = nw; memcpy(fp->effs, want, sizeof want);
        for (int k = 0; k < nw; ++k) { fp->inst[k].Reset(&g_fxpack.effects[(size_t)want[k]], (uint32_t)((size_t)pawn * 2654435761u + (uint32_t)k * 97u) | 1u); fx::Warmup(fp->inst[k], 3.f); }
        fp->last = now; Log("essence fx: pawn %p now shows %d effect(s), first '%s'", pawn, nw, g_fxpack.effects[(size_t)want[0]].name.c_str());
    }
    float dt = fp->last ? (float)(now - fp->last) * 0.001f : 0.016f; if (dt < 0.f) dt = 0.f; fp->last = now;
    for (int k = 0; k < fp->n; ++k) fx::Step(fp->inst[k], dt);
    t_fxP = fp;
    return true;
}

static IDirect3DTexture9* g_fxtex[256]; static IDirect3DDevice9* g_fxtexDev = nullptr;
static IDirect3DVertexBuffer9* g_fxvb = nullptr; static IDirect3DIndexBuffer9* g_fxib = nullptr; static IDirect3DDevice9* g_fxbufDev = nullptr;
constexpr UINT kFxMaxV = 12288, kFxMaxI = 24576;
struct FxVert { float x, y, z; DWORD col; float u, v; };
constexpr DWORD kFxFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

static IDirect3DTexture9* FxTexture(IDirect3DDevice9* dev, int ti) {
    if (ti < 0 || ti >= (int)g_fxpack.texs.size() || ti >= 256) return nullptr;
    if (g_fxtexDev && g_fxtexDev != dev) { for (auto& x : g_fxtex) if (x) { x->Release(); x = nullptr; } }
    g_fxtexDev = dev;
    if (g_fxtex[ti]) return g_fxtex[ti];
    const ess::Tex& t = g_fxpack.texs[(size_t)ti];
    D3DFORMAT fmt = t.fmt == 3 ? D3DFMT_DXT1 : (t.fmt == 8 ? D3DFMT_DXT5 : D3DFMT_DXT3);
    IDirect3DTexture9* tx = nullptr;
    if (FAILED(dev->CreateTexture(t.w, t.h, (UINT)t.levels, 0, fmt, D3DPOOL_MANAGED, &tx, nullptr)) || !tx) { Log("essence fx: CreateTexture %s failed", t.name.c_str()); return nullptr; }
    for (int l = 0; l < t.levels; ++l) {
        D3DLOCKED_RECT lr;
        if (FAILED(tx->LockRect((UINT)l, &lr, nullptr, 0))) { tx->Release(); return nullptr; }
        const UINT lw = t.w >> l ? t.w >> l : 1, lh = t.h >> l ? t.h >> l : 1;
        const UINT bpr = ((lw + 3) / 4) * (t.fmt == 3 ? 8 : 16), rows = (lh + 3) / 4;
        if (t.size[l] < bpr * rows) { tx->UnlockRect((UINT)l); tx->Release(); return nullptr; }
        for (UINT r = 0; r < rows; ++r) memcpy(static_cast<BYTE*>(lr.pBits) + (size_t)r * lr.Pitch, t.data[l] + (size_t)r * bpr, bpr);
        tx->UnlockRect((UINT)l);
    }
    g_fxtex[ti] = tx;
    return tx;
}

static bool ClothCameraAxes(IDirect3DDevice9* dev, float* right, float* up, float* fwd) {      // camera right / up directions in the actor space
    D3DMATRIX W, V;
    if (FAILED(dev->GetTransform(D3DTS_WORLD, &W)) || FAILED(dev->GetTransform(D3DTS_VIEW, &V))) return false;
    double w[16], v[16], m[16], inv[16];
    const float* wf = &W._11; const float* vf = &V._11;
    for (int i = 0; i < 16; ++i) { w[i] = wf[i]; v[i] = vf[i]; }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { double s = 0; for (int k = 0; k < 4; ++k) s += w[i * 4 + k] * v[k * 4 + j]; m[i * 4 + j] = s; }
    if (!Inv4(m, inv)) return false;
    for (int k = 0; k < 3; ++k) { right[k] = (float)inv[k]; up[k] = (float)inv[4 + k]; fwd[k] = (float)inv[8 + k]; }
    for (float* a : { right, up, fwd }) { const float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); if (n > 1e-6f) { a[0] /= n; a[1] /= n; a[2] /= n; } }
    return true;
}

static void FxBlend(IDirect3DDevice9* dev, int style) {
    DWORD s = D3DBLEND_ONE, d = D3DBLEND_ONE;
    switch (style) {
        case fx::kRegular: case fx::kAlphaBlend: case fx::kAlphaModulate: s = D3DBLEND_SRCALPHA; d = D3DBLEND_INVSRCALPHA; break;
        case fx::kModulated: s = D3DBLEND_DESTCOLOR; d = D3DBLEND_SRCCOLOR; break;
        case fx::kDarken: s = D3DBLEND_ZERO; d = D3DBLEND_INVSRCCOLOR; break;
        case fx::kBrighten: s = D3DBLEND_ONE; d = D3DBLEND_INVSRCCOLOR; break;
        default: break;                                                      // translucent: additive
    }
    dev->SetRenderState(D3DRS_SRCBLEND, s); dev->SetRenderState(D3DRS_DESTBLEND, d);
}

static bool EssDrawFx(IDirect3DDevice9* dev, DevHooks* h) {
    FxPawn* fp = t_fxP;
    if (!fp || !fp->ok || fp->n <= 0) return false;
    if (g_fxbufDev != dev || !g_fxvb || !g_fxib) {
        if (g_fxvb) { g_fxvb->Release(); g_fxvb = nullptr; } if (g_fxib) { g_fxib->Release(); g_fxib = nullptr; }
        if (FAILED(dev->CreateVertexBuffer(kFxMaxV * sizeof(FxVert), 0, kFxFVF, D3DPOOL_MANAGED, &g_fxvb, nullptr)) || FAILED(dev->CreateIndexBuffer(kFxMaxI * 2, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_fxib, nullptr))) { EssWhy("fx buffers"); return false; }
        g_fxbufDev = dev;
    }
    float camR[3] = { 1.f, 0.f, 0.f }, camU[3] = { 0.f, 0.f, 1.f }, camF[3] = { 0.f, 1.f, 0.f };
    ClothCameraAxes(dev, camR, camU, camF);
    static_assert(sizeof(fx::Vertex) == sizeof(FxVert), "effect vertex layout");
    fx::Frame fr; memcpy(fr.O, fp->O, sizeof fr.O); memcpy(fr.B, fp->B, sizeof fr.B); fr.scale = g_cfg.essenceFxScale; fr.gain = g_cfg.essenceFxPGain; fr.axis = g_cfg.essenceFxAxis; fr.wingFlip = g_cfg.essenceFxWingFlip;
    fx::View vw; memcpy(vw.right, camR, sizeof camR); memcpy(vw.up, camU, sizeof camU); memcpy(vw.fwd, camF, sizeof camF);
    static std::vector<fx::Vertex> vs; static std::vector<uint16_t> is; static std::vector<fx::Batch> bs;
    vs.clear(); is.clear(); bs.clear();
    for (int k = 0; k < fp->n; ++k) fx::Gather(g_fxpack, fp->inst[k], fr, vw, vs, is, bs, kFxMaxV, kFxMaxI);
    if (bs.empty()) return true;
    void* p = nullptr;
    if (FAILED(g_fxvb->Lock(0, (UINT)vs.size() * sizeof(fx::Vertex), &p, 0)) || !p) return false;
    memcpy(p, vs.data(), vs.size() * sizeof(fx::Vertex)); g_fxvb->Unlock();
    if (FAILED(g_fxib->Lock(0, (UINT)is.size() * 2, &p, 0)) || !p) return false;
    memcpy(p, is.data(), is.size() * 2); g_fxib->Unlock();

    // state: saved here, restored below (the engine caches its own state)
    static const D3DRENDERSTATETYPE kRs[] = { D3DRS_ZWRITEENABLE, D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_CULLMODE, D3DRS_SPECULARENABLE, D3DRS_COLORVERTEX };
    static const D3DTEXTURESTAGESTATETYPE kTss[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
    DWORD oldRs[sizeof kRs / sizeof kRs[0]]; DWORD oldTss[8], oldTss1[8]; IDirect3DBaseTexture9* oldTex = nullptr; IDirect3DBaseTexture9* oldTex1 = nullptr; D3DMATRIX oldM1;
    for (size_t k = 0; k < sizeof kRs / sizeof kRs[0]; ++k) dev->GetRenderState(kRs[k], &oldRs[k]);
    for (int k = 0; k < 8; ++k) dev->GetTextureStageState(0, kTss[k], &oldTss[k]);
    for (int k = 0; k < 8; ++k) dev->GetTextureStageState(1, kTss[k], &oldTss1[k]);
    dev->GetTexture(0, &oldTex); dev->GetTexture(1, &oldTex1); dev->GetTransform(D3DTS_TEXTURE1, &oldM1);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE); dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE); dev->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    reinterpret_cast<SetFVF_t>(h->setfvf)(dev, kFxFVF);
    reinterpret_cast<SetSS_t>(h->setss)(dev, 0, g_fxvb, 0, sizeof(FxVert));
    dev->SetIndices(g_fxib);
    bool ok = true;
    for (const fx::Batch& b : bs) {
        IDirect3DTexture9* tx = FxTexture(dev, b.tex);
        if (!tx) continue;
        dev->SetTexture(0, tx); FxBlend(dev, b.style);
        IDirect3DTexture9* tx2 = b.tex2 >= 0 ? FxTexture(dev, b.tex2) : nullptr;
        if (tx2) {                                                                // a second texture multiplied over the first, scrolling (UE2 TexPanner)
            D3DMATRIX M; memset(&M, 0, sizeof M); M._11 = M._22 = M._33 = M._44 = 1.f;
            const double now = (double)GetTickCount64() * 0.001;
            M._31 = (float)fmod((double)b.panU * now, 1.0); M._32 = (float)fmod((double)b.panV * now, 1.0);
            dev->SetTransform(D3DTS_TEXTURE1, &M);
            dev->SetTexture(1, tx2);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE); dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
            dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); dev->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
            dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        } else dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        const HRESULT hr = reinterpret_cast<DIP_t>(h->dip)(dev, D3DPT_TRIANGLELIST, 0, b.v0, b.nv, b.i0, b.ntri);
        if (FAILED(hr)) { ok = false; InterlockedIncrement(&g_fxFail); static volatile LONG once = 0; if (InterlockedExchange(&once, 1) == 0) Log("essence fx: DrawIndexedPrimitive failed 0x%08x", (unsigned)hr); } else InterlockedIncrement(&g_fxDrawn);
    }
    for (size_t k = 0; k < sizeof kRs / sizeof kRs[0]; ++k) dev->SetRenderState(kRs[k], oldRs[k]);
    for (int k = 0; k < 8; ++k) dev->SetTextureStageState(0, kTss[k], oldTss[k]);
    for (int k = 0; k < 8; ++k) dev->SetTextureStageState(1, kTss[k], oldTss1[k]);
    dev->SetTransform(D3DTS_TEXTURE1, &oldM1); dev->SetTexture(1, oldTex1); if (oldTex1) oldTex1->Release();
    dev->SetTexture(0, oldTex); if (oldTex) oldTex->Release();
    if (g_decl) reinterpret_cast<SetVD_t>(h->setvd)(dev, g_decl); else reinterpret_cast<SetFVF_t>(h->setfvf)(dev, g_fvf);
    if (g_s0vb) reinterpret_cast<SetSS_t>(h->setss)(dev, 0, g_s0vb, g_s0off, g_s0stride);
    return ok;
}

// ---- Direct3D resources of the cloth
struct ClothGpu { const cloth::Set* set = nullptr; IDirect3DDevice9* dev = nullptr; IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr; int fails = 0; };
static ClothGpu g_cgpu[64]; static int g_ncgpu = 0;
struct CollarGpu { const ess::Collar* c = nullptr; IDirect3DDevice9* dev = nullptr; IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr; int fails = 0; };
static CollarGpu g_ccgpu[160]; static int g_nccgpu = 0;
static IDirect3DTexture9* g_ctex[512]; static IDirect3DDevice9* g_ctexDev = nullptr;

static IDirect3DTexture9* ClothTexture(IDirect3DDevice9* dev, int ti) {
    if (ti < 0 || ti >= (int)g_cpack.texs.size() || ti >= 512) { EssWhy("cloth texture index", ti, (int)g_cpack.texs.size()); return nullptr; }
    if (g_ctexDev && g_ctexDev != dev) { for (auto& t : g_ctex) if (t) { t->Release(); t = nullptr; } }
    g_ctexDev = dev;
    if (g_ctex[ti]) return g_ctex[ti];
    const ess::Tex& t = g_cpack.texs[(size_t)ti];
    D3DFORMAT fmt = t.fmt == 3 ? D3DFMT_DXT1 : (t.fmt == 8 ? D3DFMT_DXT5 : D3DFMT_DXT3);
    IDirect3DTexture9* tx = nullptr;
    HRESULT hr = dev->CreateTexture(t.w, t.h, (UINT)t.levels, 0, fmt, D3DPOOL_MANAGED, &tx, nullptr);
    if (FAILED(hr) || !tx) { Log("essence cloth: CreateTexture %s failed 0x%08x", t.name.c_str(), (unsigned)hr); return nullptr; }
    for (int l = 0; l < t.levels; ++l) {
        D3DLOCKED_RECT lr;
        if (FAILED(tx->LockRect((UINT)l, &lr, nullptr, 0))) { tx->Release(); EssWhy("cloth texture LockRect", ti, l); return nullptr; }
        const UINT lw = t.w >> l ? t.w >> l : 1, lh = t.h >> l ? t.h >> l : 1;
        const UINT bpr = ((lw + 3) / 4) * (t.fmt == 3 ? 8 : 16), rows = (lh + 3) / 4;
        if (t.size[l] < bpr * rows) { tx->UnlockRect((UINT)l); tx->Release(); EssWhy("cloth texture level too small", ti, l); return nullptr; }
        for (UINT r = 0; r < rows; ++r) memcpy(static_cast<BYTE*>(lr.pBits) + (size_t)r * lr.Pitch, t.data[l] + (size_t)r * bpr, bpr);
        tx->UnlockRect((UINT)l);
    }
    g_ctex[ti] = tx;
    Log("essence cloth: texture %s %ux%u levels %d uploaded", t.name.c_str(), t.w, t.h, t.levels);
    return tx;
}

static ClothGpu* ClothEnsureGpu(IDirect3DDevice9* dev, const cloth::Set* s) {
    ClothGpu* g = nullptr;
    for (int i = 0; i < g_ncgpu; ++i) if (g_cgpu[i].set == s) { g = &g_cgpu[i]; break; }
    if (!g) { if (g_ncgpu >= 64) return nullptr; g = &g_cgpu[g_ncgpu++]; g->set = s; }
    if (g->dev == dev && g->vb && g->ib) return g;
    if (g->fails >= 3) { EssWhy("cloth GPU gave up after 3 failures"); return nullptr; }
    if (g->vb) { g->vb->Release(); g->vb = nullptr; } if (g->ib) { g->ib->Release(); g->ib = nullptr; }
    g->dev = dev;
    HRESULT hr = dev->CreateIndexBuffer((UINT)s->nt * 6, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g->ib, nullptr);
    void* p = nullptr;
    if (FAILED(hr) || !g->ib || FAILED(g->ib->Lock(0, 0, &p, 0)) || !p) { ++g->fails; EssWhy("cloth index buffer", (int)hr); return nullptr; }
    memcpy(p, s->tris.data(), (size_t)s->nt * 6); g->ib->Unlock();
    hr = dev->CreateVertexBuffer((UINT)s->np * 32, 0, 0, D3DPOOL_MANAGED, &g->vb, nullptr);
    if (FAILED(hr) || !g->vb) { ++g->fails; EssWhy("cloth vertex buffer", (int)hr); return nullptr; }
    return g;
}

static CollarGpu* CollarEnsureGpu(IDirect3DDevice9* dev, const ess::Collar* c) {
    CollarGpu* g = nullptr;
    for (int i = 0; i < g_nccgpu; ++i) if (g_ccgpu[i].c == c) { g = &g_ccgpu[i]; break; }
    if (!g) { if (g_nccgpu >= 160) return nullptr; g = &g_ccgpu[g_nccgpu++]; g->c = c; }
    if (g->dev == dev && g->vb && g->ib) return g;
    if (g->fails >= 3) return nullptr;
    if (g->vb) { g->vb->Release(); g->vb = nullptr; } if (g->ib) { g->ib->Release(); g->ib = nullptr; }
    g->dev = dev;
    HRESULT hr = dev->CreateIndexBuffer((UINT)c->nt * 6, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g->ib, nullptr);
    void* p = nullptr;
    if (FAILED(hr) || !g->ib || FAILED(g->ib->Lock(0, 0, &p, 0)) || !p) { ++g->fails; EssWhy("collar index buffer", (int)hr); return nullptr; }
    memcpy(p, c->idx.data(), (size_t)c->nt * 6); g->ib->Unlock();
    hr = dev->CreateVertexBuffer((UINT)c->nv * 32, 0, 0, D3DPOOL_MANAGED, &g->vb, nullptr);
    if (FAILED(hr) || !g->vb) { ++g->fails; EssWhy("collar vertex buffer", (int)hr); return nullptr; }
    return g;
}

static bool EssDrawCloth(IDirect3DDevice9* dev, DevHooks* h) {
    ClothPawn* cp = t_clothP;
    if (!cp || !cp->ready || !cp->set) { EssWhy("cloth draw without state", cp != nullptr); return false; }
    const cloth::Set& s = *cp->set;
    ClothGpu* g = ClothEnsureGpu(dev, &s);
    const ess::ClothLook& look = g_cpack.looks[(size_t)cp->look];
    IDirect3DTexture9* tex = ClothTexture(dev, look.tex);
    if (!g || !tex) { EssWhy("cloth GPU/texture", g != nullptr, tex != nullptr); return false; }
    const int nt0 = (s.sec1 > 0 && s.sec1 < s.nt) ? s.sec1 : s.nt;                      // triangles of the cloth proper; the rest is the crest window
    IDirect3DTexture9* texCrest = (nt0 < s.nt && look.crestTex >= 0) ? ClothTexture(dev, look.crestTex) : nullptr;
    const std::vector<ess::FxLayer>* fxCloth = nullptr; const std::vector<ess::FxLayer>* fxCollar = nullptr; bool fxEnv = false;
    if (g_cfg.essenceFx) {
        auto prog = [&](int ti) -> const std::vector<ess::FxLayer>* { if (ti < 0 || ti >= (int)g_cpack.texs.size()) return nullptr; auto it = g_cpack.fx.find(ess::Lower(g_cpack.texs[(size_t)ti].name)); return it != g_cpack.fx.end() && !it->second.empty() ? &it->second : nullptr; };
        fxCloth = prog(look.tex); fxCollar = prog(look.collarTex);
        for (const auto* pr : { fxCloth, fxCollar }) if (pr) for (const ess::FxLayer& l : *pr) for (const ess::FxTex& x : l.texs) if (x.env) fxEnv = true;
    }
    const ess::Collar* col = cp->collar; CollarGpu* cg = nullptr; IDirect3DTexture9* texCol = nullptr;
    if (col && look.collarTex >= 0) { cg = CollarEnsureGpu(dev, col); texCol = cg ? ClothTexture(dev, look.collarTex) : nullptr; if (!texCol) cg = nullptr; }

    bool lit = false; int nLights = 0; DWORD engineLighting = 0;
    if (g_cfg.essenceLight != 0) {
        dev->GetRenderState(D3DRS_LIGHTING, &engineLighting);
        for (DWORD i = 0; i < 16; ++i) { BOOL en = FALSE; if (SUCCEEDED(dev->GetLightEnable(i, &en)) && en) ++nLights; }
        lit = g_cfg.essenceLight == 1 || (engineLighting && nLights > 0);
    }
    static std::vector<float> vtx, nrm;
    std::vector<float> pv(cp->posOut, cp->posOut + (size_t)s.np * 3);
    vtx.resize((size_t)s.np * 8);
    const bool needN = lit || fxEnv;
    if (needN) {
        cloth::Normals(s, pv, nrm);
        float cam[3]; const bool haveCam = ClothCameraPos(dev, cam);
        if (haveCam) for (int i = 0; i < s.np; ++i) {                       // the lit side is the one the viewer sees
            float* n = &nrm[(size_t)i * 3]; const float* p = &pv[(size_t)i * 3];
            if (n[0] * (cam[0] - p[0]) + n[1] * (cam[1] - p[1]) + n[2] * (cam[2] - p[2]) < 0.f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
        }
    }
    for (int i = 0; i < s.np; ++i) {
        float* o = &vtx[(size_t)i * 8];
        o[0] = pv[(size_t)i * 3]; o[1] = pv[(size_t)i * 3 + 1]; o[2] = pv[(size_t)i * 3 + 2];
        if (needN) { o[3] = nrm[(size_t)i * 3]; o[4] = nrm[(size_t)i * 3 + 1]; o[5] = nrm[(size_t)i * 3 + 2]; } else { o[3] = 0.f; o[4] = -1.f; o[5] = 0.f; }
        o[6] = s.uv[(size_t)i * 2]; o[7] = s.uv[(size_t)i * 2 + 1];
    }
    void* dst = nullptr;
    if (FAILED(g->vb->Lock(0, (UINT)s.np * 32, &dst, 0)) || !dst) return false;
    memcpy(dst, vtx.data(), (size_t)s.np * 32); g->vb->Unlock();
    if (cg) {                                                                        // collar: the same rigid transform as the anchors, x' = Rs (x - tb) + gt
        static std::vector<float> cv;
        cv.resize((size_t)col->nv * 8);
        float cam[3] = { 0.f, 0.f, 0.f }; const bool haveCam = needN && ClothCameraPos(dev, cam);
        const cloth::Pose& po = cp->pose;
        for (int i = 0; i < col->nv; ++i) {
            const float* p0 = &col->pos[(size_t)i * 3]; const float v[3] = { p0[0] - po.tb[0], p0[1] - po.tb[1], p0[2] - po.tb[2] };
            float o[3]; cloth::MulVec(po.Rs, v, o);
            float* w = &cv[(size_t)i * 8];
            w[0] = o[0] + po.gt[0]; w[1] = o[1] + po.gt[1]; w[2] = o[2] + po.gt[2];
            if (needN) {
                float n[3]; cloth::MulVec(po.Rs, &col->nrm[(size_t)i * 3], n);
                if (haveCam && n[0] * (cam[0] - w[0]) + n[1] * (cam[1] - w[1]) + n[2] * (cam[2] - w[2]) < 0.f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
                w[3] = n[0]; w[4] = n[1]; w[5] = n[2];
            } else { w[3] = 0.f; w[4] = -1.f; w[5] = 0.f; }
            w[6] = col->uv[(size_t)i * 2]; w[7] = col->uv[(size_t)i * 2 + 1];
        }
        void* cd = nullptr;
        if (SUCCEEDED(cg->vb->Lock(0, (UINT)col->nv * 32, &cd, 0)) && cd) { memcpy(cd, cv.data(), (size_t)col->nv * 32); cg->vb->Unlock(); } else cg = nullptr;
    }

    IDirect3DVertexBuffer9* oldVB = nullptr; UINT oldOff = 0, oldStride = 0;
    IDirect3DIndexBuffer9* oldIB = nullptr; IDirect3DBaseTexture9* oldT0 = nullptr; IDirect3DBaseTexture9* oldT1 = nullptr; IDirect3DPixelShader9* oldPS = nullptr;
    dev->GetStreamSource(0, &oldVB, &oldOff, &oldStride);
    dev->GetIndices(&oldIB); dev->GetTexture(0, &oldT0); dev->GetTexture(1, &oldT1); dev->GetPixelShader(&oldPS);
    DWORD rs[kEssNRS]; DWORD ts0[kEssNTSS], ts1[2];
    for (int i = 0; i < kEssNRS; ++i) dev->GetRenderState(kEssRS[i], &rs[i]);
    for (int i = 0; i < kEssNTSS; ++i) dev->GetTextureStageState(0, kEssTSS[i], &ts0[i]);
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &ts1[0]); dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &ts1[1]);
    D3DMATERIAL9 oldMat; memset(&oldMat, 0, sizeof oldMat); const bool haveMat = SUCCEEDED(dev->GetMaterial(&oldMat));
    DWORD litOp = D3DTOP_MODULATE2X;
    if (lit) { DWORD eng = 0; dev->GetTextureStageState(0, D3DTSS_COLOROP, &eng); if (eng == D3DTOP_MODULATE || eng == D3DTOP_MODULATE2X || eng == D3DTOP_MODULATE4X) litOp = eng; }
    const DWORD bright = (DWORD)(g_cfg.essenceBright * 255.f > 255.f ? 255.f : g_cfg.essenceBright * 255.f);

    dev->SetPixelShader(nullptr);
    dev->SetStreamSource(0, g->vb, 0, 32);
    dev->SetIndices(g->ib);
    dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    if (lit) {
        const float lb = g_cfg.essenceLitBright < 0.f ? 0.f : g_cfg.essenceLitBright;
        D3DMATERIAL9 mat; memset(&mat, 0, sizeof mat);
        mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = lb; mat.Diffuse.a = 1.f; mat.Ambient = mat.Diffuse;
        dev->SetMaterial(&mat);
        dev->SetRenderState(D3DRS_LIGHTING, TRUE); dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE); dev->SetRenderState(D3DRS_COLORVERTEX, FALSE);
        dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL); dev->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        dev->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL); dev->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    } else {
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    }
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); dev->SetRenderState(D3DRS_ALPHAREF, 160); dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, bright, bright, bright));
    dev->SetTextureStageState(0, D3DTSS_COLOROP, lit ? litOp : D3DTOP_MODULATE); dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_COLORARG2, lit ? D3DTA_DIFFUSE : D3DTA_TFACTOR);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetTexture(1, nullptr);
    dev->SetTexture(0, tex);
    HRESULT hr = reinterpret_cast<DIP_t>(h->dip)(dev, D3DPT_TRIANGLELIST, 0, 0, (UINT)s.np, 0, (UINT)nt0);
    if (SUCCEEDED(hr) && texCrest) { dev->SetTexture(0, texCrest); hr = reinterpret_cast<DIP_t>(h->dip)(dev, D3DPT_TRIANGLELIST, 0, 0, (UINT)s.np, (UINT)nt0 * 3, (UINT)(s.nt - nt0)); }
    if (SUCCEEDED(hr) && nt0 < s.nt && cp->crestId != 0 && g_cfg.essenceCrest) {                                  // the clan's own crest over the background
        IDirect3DTexture9* crest = CrestD3D(dev, cp->crestId);
        if (crest) {
            DWORD zf = 0; dev->GetRenderState(D3DRS_ZFUNC, &zf); dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetTexture(0, crest);
            hr = reinterpret_cast<DIP_t>(h->dip)(dev, D3DPT_TRIANGLELIST, 0, 0, (UINT)s.np, (UINT)nt0 * 3, (UINT)(s.nt - nt0));
            dev->SetRenderState(D3DRS_ZFUNC, zf);
        }
    }
    if (SUCCEEDED(hr) && cg) {
        dev->SetStreamSource(0, cg->vb, 0, 32); dev->SetIndices(cg->ib); dev->SetTexture(0, texCol);
        hr = reinterpret_cast<DIP_t>(h->dip)(dev, D3DPT_TRIANGLELIST, 0, 0, (UINT)col->nv, 0, (UINT)col->nt);
    }
    if (SUCCEEDED(hr) && fxCloth) {                                                                     // effect layers (env-map shine, animation) over the cloth proper
        dev->SetStreamSource(0, g->vb, 0, 32); dev->SetIndices(g->ib);
        DWORD cullNow = D3DCULL_NONE; dev->GetRenderState(D3DRS_CULLMODE, &cullNow);
        EssDrawFxLayers(dev, h, *fxCloth, tex, 160, true, cullNow, (UINT)s.np, 0, (UINT)nt0, ClothTexture);
        if (nt0 < s.nt && texCrest) EssDrawFxLayers(dev, h, *fxCloth, texCrest, 160, true, cullNow, (UINT)s.np, (UINT)nt0, (UINT)(s.nt - nt0), ClothTexture, texCrest);       // the crest window: the same shine (env map x the cloth colour; the mask of the cloth is black over the window, so the window's own picture is the mask)
    }
    if (SUCCEEDED(hr) && fxCollar && cg && texCol) {
        dev->SetStreamSource(0, cg->vb, 0, 32); dev->SetIndices(cg->ib);
        DWORD cullNow = D3DCULL_NONE; dev->GetRenderState(D3DRS_CULLMODE, &cullNow);
        EssDrawFxLayers(dev, h, *fxCollar, texCol, 160, true, cullNow, (UINT)col->nv, 0, (UINT)col->nt, ClothTexture);
    }
    const bool ok = SUCCEEDED(hr);
    if (!ok) { static volatile LONG once = 0; if (InterlockedExchange(&once, 1) == 0) Log("essence cloth: DrawIndexedPrimitive failed 0x%08x", (unsigned)hr); }

    for (int i = 0; i < kEssNRS; ++i) dev->SetRenderState(kEssRS[i], rs[i]);
    if (haveMat) dev->SetMaterial(&oldMat);
    for (int i = 0; i < kEssNTSS; ++i) dev->SetTextureStageState(0, kEssTSS[i], ts0[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, ts1[0]); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, ts1[1]);
    dev->SetTexture(0, oldT0); dev->SetTexture(1, oldT1);
    dev->SetPixelShader(oldPS); dev->SetIndices(oldIB); dev->SetStreamSource(0, oldVB, oldOff, oldStride);
    if (oldVB) oldVB->Release(); if (oldIB) oldIB->Release(); if (oldT0) oldT0->Release(); if (oldT1) oldT1->Release(); if (oldPS) oldPS->Release();
    static volatile LONG drawn = 0;
    if (g_cfg.debug && InterlockedIncrement(&drawn) <= 6) Log("essence cloth: drew %s %s look %d '%s' (%d particles, %d triangles%s, collar %s %d tris) %s%s", s.body.c_str(), s.kind.c_str(), cp->look, look.name.c_str(), s.np, s.nt, texCrest ? " + crest window" : "", cg ? col->family.c_str() : "none", cg ? col->nt : 0, ok ? "ok" : "WITH ERRORS", lit ? " [lit]" : " [unlit]");
    return ok;
}

// ini polling (offsets / scale / brightness can be tuned while the game runs)
static void EssPollIni() {
    static ULONGLONG lastCheck = 0; static FILETIME lastWrite = {};
    const ULONGLONG now = GetTickCount64();
    if (now - lastCheck < 1000) return;
    lastCheck = now;
    wchar_t ini[MAX_PATH]; _snwprintf_s(ini, _TRUNCATE, L"%scloakhook.ini", g_dir);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(ini, GetFileExInfoStandard, &fa)) return;
    if (lastWrite.dwLowDateTime == 0 && lastWrite.dwHighDateTime == 0) { lastWrite = fa.ftLastWriteTime; return; }
    if (CompareFileTime(&lastWrite, &fa.ftLastWriteTime) == 0) return;
    lastWrite = fa.ftLastWriteTime;
    ReadEssenceCfg();
    Log("essence: ini reloaded (offset %.1f %.1f %.1f, scale %.2f, bright %.2f, blend %.2f, frame mode %d, snug %.1f, FElf snug %.1f)", g_cfg.essenceOffset[0], g_cfg.essenceOffset[1], g_cfg.essenceOffset[2],
        g_cfg.essenceScale, g_cfg.essenceBright, g_cfg.essenceBlend, g_cfg.essenceFrameMode, g_cfg.essenceSnug, g_cfg.essenceSnugBody[5]);
}

// DrawCloak calls this before the carrier is rendered; when it returns true, t_essM/t_essP are set for the D3D hook
static void EssLoadPackOnce();
static bool EssPrepareCloak(void* cloakMesh, void* bodyInst, void* pawn) {
    t_essM = nullptr; t_essP = nullptr; t_clothP = nullptr; t_wingM = nullptr; t_wingP = nullptr; t_fxP = nullptr; t_essDone = false; t_essOk = false;
    InterlockedIncrement(&g_stPrep);
    EssStats();
    EssPollIni();
    if (g_cfg.essence && g_packState == 0) EssLoadPackOnce();
    if (!g_cfg.essence || g_packState != 1) return false;
    static void* cacheMesh[64]; static int cacheBody[64], cacheId[64]; static int nc = 0;
    int ci = -1;
    for (int i = 0; i < nc; ++i) if (cacheMesh[i] == cloakMesh) { ci = i; break; }
    if (ci < 0) {
        wchar_t buf[4096]; buf[0] = 0;
        const wchar_t* n = g_api.GetFullName(cloakMesh, nullptr, buf);
        int b = -1, id = 0;
        if (!ParseCloakName(n, &b, &id)) b = -1;
        if (nc >= 64) nc = 0;
        ci = nc++; cacheMesh[ci] = cloakMesh; cacheBody[ci] = b; cacheId[ci] = id;
    }
    if (cacheBody[ci] < 0) return false;
    t_curId = cacheId[ci];
    if (EssIsBadId(cacheId[ci])) return false;                                                    // this cloak crashed before: show the carrier instead
    int design;
    if (g_cfg.essenceForceDesign >= 0) design = g_cfg.essenceForceDesign;
    else {
        const int rel = cacheId[ci] - g_cfg.essenceFirstId;                                       // every item id gets its own design, cycling through the packed ones
        if (rel < 0) return false;
        if (g_cfg.essenceWingBase > 0 && rel >= g_cfg.essenceWingBase) {
            design = g_cfg.essenceWingMap[(rel - g_cfg.essenceWingBase) % g_cfg.essenceWingCount];                  // wing-like cloaks
        } else
        if (g_cfg.essenceCloth && rel >= g_cfg.essenceClothBase) {                                // standard Essence cloaks: cloth simulation
            try { const bool r = EssPrepareCloth(cacheBody[ci], rel - g_cfg.essenceClothBase, bodyInst, pawn, GetTickCount64()); InterlockedIncrement(r ? &g_stPrepCloth : &g_stPrepNone);
                  if (!r) { static volatile LONG n = 0; if (InterlockedIncrement(&n) <= 12) Log("essence cloth: nothing to draw for id %d (look %d, cal %d, pack %ld) - the carrier stays visible", cacheId[ci], rel - g_cfg.essenceClothBase, g_cal.attSrc, g_cpackState); }
                  return r; }
            catch (...) { g_cfg.essenceCloth = false; Log("essence cloth: exception - cloth disabled"); return false; }
        }
        if (!(g_cfg.essenceWingBase > 0 && rel >= g_cfg.essenceWingBase)) design = g_cfg.essenceDesignMap[rel % (g_cfg.essenceDesigns > 0 ? g_cfg.essenceDesigns : 1)];
    }
    if (design == 16 && g_cfg.essenceCloth && EssFind(cacheBody[ci], 16)) {                          // rigid wings over a cloth cape
        try {
            const bool rc = EssPrepareCloth(cacheBody[ci], g_cfg.essenceComboLook, bodyInst, pawn, GetTickCount64());
            EssMantle* gw = EssFind(cacheBody[ci], 16);
            if (gw && EssUpdate(gw, bodyInst, pawn)) { t_wingM = t_essM; t_wingP = t_essP; }
            t_essM = nullptr; t_essP = nullptr;
            InterlockedIncrement((rc || t_wingM) ? &g_stPrepCloth : &g_stPrepNone);
            return rc || t_wingM != nullptr;
        } catch (...) { return false; }
    }
    EssMantle* g = EssFind(cacheBody[ci], design);
    if (!g && design != 0) g = EssFind(cacheBody[ci], 0);                                         // that design is not packed for this body: use the first one
    if (!g) return false;
    try { const bool r = EssUpdate(g, bodyInst, pawn); InterlockedIncrement(r ? &g_stPrepMantle : &g_stPrepNone);
          if (!r) { static volatile LONG n = 0; if (InterlockedIncrement(&n) <= 12) Log("essence: nothing to draw for id %d (design %d) - the carrier stays visible", cacheId[ci], design); }
          return r; }
    catch (...) { g_cfg.essence = false; Log("essence: exception while updating the pose - essence disabled"); return false; }
}

static bool EssPrepare(void* cloakMesh, void* bodyInst, void* pawn) {
    t_fxP = nullptr;
    const bool r = EssPrepareCloak(cloakMesh, bodyInst, pawn);
    if (r && g_cfg.essenceFxParticles && t_curId > 0) { try { EssFxPrepare(bodyInst, pawn); } catch (...) { g_cfg.essenceFxParticles = false; Log("essence fx: exception - particle effects disabled"); } }
    return r;
}

static void EssLoadPackOnce() {
    if (!g_cfg.essence || g_packState != 0) return;
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s%s", g_dir, g_cfg.essencePack);
    std::string err;
    const ULONGLONG t0 = GetTickCount64();
    if (!ess::LoadPack(path, g_pack, &err)) { Log("essence: pack %ls NOT loaded (%s)", path, err.c_str()); g_packState = -1; return; }
    Log("essence: pack %ls loaded in %llu ms: %d bodies, %d textures, %d mantles", path, (unsigned long long)(GetTickCount64() - t0), (int)g_pack.anims.size(), (int)g_pack.texs.size(), (int)g_pack.mantles.size());
    g_packState = 1;
}

static HRESULT STDMETHODCALLTYPE HkDIP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT minIdx, UINT numV, UINT startIdx, UINT primCount) {
    DevHooks* h = DH(dev);
    if (t_inCloak) InterlockedIncrement(&g_stDip);
    if (t_inCloak && !g_disabled && (t_essM || t_clothP || t_wingM || t_fxP)) {            // Essence mantle / cloth: draw it once, suppress every draw call of the carrier
        if (!t_essDone) {
            t_essDone = true;
            bool ok = true, any = false;
            if (t_clothP) { InterlockedIncrement(&g_stClothTry); const bool r = EssDrawCloth(dev, h); if (r) InterlockedIncrement(&g_stClothOk); ok = ok && r; any = true; }
            if (t_wingM) { EssMantle* sm = t_essM; EssPawn* sp = t_essP; t_essM = t_wingM; t_essP = t_wingP; InterlockedIncrement(&g_stEssTry); const bool r = EssDraw(dev, h); if (r) InterlockedIncrement(&g_stEssOk); ok = ok && r; any = true; t_essM = sm; t_essP = sp; }
            if (t_essM && !t_wingM) { InterlockedIncrement(&g_stEssTry); const bool r = EssDraw(dev, h); if (r) InterlockedIncrement(&g_stEssOk); ok = ok && r; any = true; }
            if (t_fxP) { const bool rf = EssDrawFx(dev, h); (void)rf; }
            t_essOk = any && ok;
        }
        if (t_essOk && !g_cfg.essenceShowCarrier) return D3D_OK;
    }
    if (t_inCloak) InterlockedIncrement(&g_stCarrier);
    if (t_inCloak && !g_disabled) {
        DLOG(24, "D3D cloak DrawIndexedPrimitive: type=%d base=%d min=%u numV=%u start=%u prims=%u | stream0 vb=%p off=%u stride=%u decl=%p fvf=0x%x",
             (int)type, (int)base, minIdx, numV, startIdx, primCount, g_s0vb, g_s0off, g_s0stride, g_decl, (unsigned)g_fvf);
        {   // what the engine binds for the cloak: world matrix (does it include the bone?) and shader (fixed function or programmable)
            static volatile LONG probes = 0;
            if (g_cfg.debug && InterlockedIncrement(&probes) <= 3) {
                D3DMATRIX wm; memset(&wm, 0, sizeof wm);
                const HRESULT hr = dev->GetTransform(D3DTS_WORLD, &wm);
                IDirect3DVertexShader9* vsh = nullptr; const HRESULT hr2 = dev->GetVertexShader(&vsh);
                Log("D3D cloak state: GetTransform(WORLD) hr=0x%x [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.2f %.2f %.2f %.2f]  vertex shader hr=0x%x %p",
                    (unsigned)hr, wm._11, wm._12, wm._13, wm._14, wm._21, wm._22, wm._23, wm._24, wm._31, wm._32, wm._33, wm._34, wm._41, wm._42, wm._43, wm._44, (unsigned)hr2, vsh);
                if (vsh) vsh->Release();
            }
        }
        if (g_decl) { static IDirect3DVertexDeclaration9* seen[8]; static int ns = 0; bool dup = false; for (int i = 0; i < ns; ++i) dup |= seen[i] == g_decl; if (!dup && ns < 8) { seen[ns++] = g_decl; LogDecl(g_decl); } }
        if (g_cfg.vbDeform) DeformVBRange(dev, g_s0vb, g_s0off, g_s0stride, base, minIdx, numV, startIdx, primCount);
    }
    return reinterpret_cast<DIP_t>(h->dip)(dev, type, base, minIdx, numV, startIdx, primCount);
}
static HRESULT STDMETHODCALLTYPE HkDP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT startV, UINT primCount) {
    DevHooks* h = DH(dev);
    if (t_inCloak) DLOG(12, "D3D cloak DrawPrimitive: type=%d start=%u prims=%u | stream0 vb=%p stride=%u", (int)type, startV, primCount, g_s0vb, g_s0stride);
    return reinterpret_cast<DP_t>(h->dp)(dev, type, startV, primCount);
}
static HRESULT STDMETHODCALLTYPE HkDPUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT primCount, const void* verts, UINT stride) {
    DevHooks* h = DH(dev);
    if (t_inCloak) DLOG(12, "D3D cloak DrawPrimitiveUP: type=%d prims=%u stride=%u", (int)type, primCount, stride);
    return reinterpret_cast<DPUP_t>(h->dpup)(dev, type, primCount, verts, stride);
}
static HRESULT STDMETHODCALLTYPE HkDIPUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minV, UINT numV, UINT primCount, const void* idx, D3DFORMAT fmt, const void* verts, UINT stride) {
    DevHooks* h = DH(dev);
    if (t_inCloak && !g_disabled) {
        DLOG(12, "D3D cloak DrawIndexedPrimitiveUP: type=%d min=%u numV=%u prims=%u stride=%u fmt=%d", (int)type, minV, numV, primCount, stride, (int)fmt);
        if (g_cfg.vbDeform) return DrawUPDeformed(h, dev, type, minV, numV, primCount, idx, fmt, verts, stride);
    }
    return reinterpret_cast<DIPUP_t>(h->dipup)(dev, type, minV, numV, primCount, idx, fmt, verts, stride);
}
static HRESULT STDMETHODCALLTYPE HkSetVD(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* d) { g_decl = d; return reinterpret_cast<SetVD_t>(DH(dev)->setvd)(dev, d); }
static HRESULT STDMETHODCALLTYPE HkSetFVF(IDirect3DDevice9* dev, DWORD fvf) { g_fvf = fvf; return reinterpret_cast<SetFVF_t>(DH(dev)->setfvf)(dev, fvf); }
static HRESULT STDMETHODCALLTYPE HkSetSS(IDirect3DDevice9* dev, UINT stream, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    if (stream == 0) { g_s0vb = vb; g_s0off = off; g_s0stride = stride; }
    return reinterpret_cast<SetSS_t>(DH(dev)->setss)(dev, stream, vb, off, stride);
}

static void* PatchVtable(void** vt, int idx, void* hook) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* orig = vt[idx]; vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return orig;
}

static void HookDevice(IDirect3DDevice9* dev, DWORD behaviour) {
    void** vt = *reinterpret_cast<void***>(dev);
    if (DH(dev)) return;                                               // this vtable is already patched
    if (g_ndh >= 4) return;
    DevHooks* h = &g_dh[g_ndh];
    memset(h, 0, sizeof *h); h->vt = vt;
    h->dp = PatchVtable(vt, kVtDP, reinterpret_cast<void*>(&HkDP));
    h->dip = PatchVtable(vt, kVtDIP, reinterpret_cast<void*>(&HkDIP));
    h->dpup = PatchVtable(vt, kVtDPUP, reinterpret_cast<void*>(&HkDPUP));
    h->dipup = PatchVtable(vt, kVtDIPUP, reinterpret_cast<void*>(&HkDIPUP));
    h->setvd = PatchVtable(vt, kVtSetVD, reinterpret_cast<void*>(&HkSetVD));
    h->setfvf = PatchVtable(vt, kVtSetFVF, reinterpret_cast<void*>(&HkSetFVF));
    h->setss = PatchVtable(vt, kVtSetSS, reinterpret_cast<void*>(&HkSetSS));
    if (!h->dp || !h->dip || !h->dpup || !h->dipup || !h->setvd || !h->setfvf || !h->setss) { Log("D3D9: vtable patch failed"); return; }
    ++g_ndh;
    Log("D3D9 device %p created (behaviour 0x%x, pure=%d), vtable %p patched: DIP %p, DIPUP %p, SetStreamSource %p", dev, (unsigned)behaviour, (int)((behaviour & D3DCREATE_PUREDEVICE) != 0), vt, h->dip, h->dipup, h->setss);
}

static HRESULT STDMETHODCALLTYPE HkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    const HRESULT hr = g_origCreateDevice(d3d, adapter, type, hwnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out, flags);
    else Log("D3D9 CreateDevice failed hr=0x%08x", (unsigned)hr);
    return hr;
}

// IDirect3D9::CreateDevice (vtable slot 16) lives in d3d9.dll and is shared by every IDirect3D9: hook it before the engine creates its device
static bool InstallD3DHooks() {
    HMODULE m = LoadLibraryW(L"d3d9.dll");
    if (!m) { Log("d3d9.dll not available"); return false; }
    using Create9_t = IDirect3D9* (WINAPI*)(UINT);
    Create9_t create = reinterpret_cast<Create9_t>(GetProcAddress(m, "Direct3DCreate9"));
    if (!create) { Log("Direct3DCreate9 missing"); return false; }
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) { Log("Direct3DCreate9 failed"); return false; }
    void* fn = (*reinterpret_cast<void***>(d3d))[16];
    d3d->Release();
    if (MH_CreateHook(fn, reinterpret_cast<void*>(&HkCreateDevice), reinterpret_cast<void**>(&g_origCreateDevice)) != MH_OK || MH_EnableHook(fn) != MH_OK) { Log("hook of IDirect3D9::CreateDevice failed"); return false; }
    Log("D3D9 hook ready: IDirect3D9::CreateDevice at %p (d3d9.dll %p)", fn, m);
    return true;
}

// draw the cloak with the engine's own sub-mesh renderer
void DrawCloak(const RenderCtx* c, void* self, void* actor, void* scene, void* proj, void* ri) {
    static void* lastBody = nullptr; static void* lastCloak = nullptr; static bool lastOk = false;
    void** slotPtr = nullptr; void* savedSlot = nullptr;           // pawn sub-mesh slot we borrow while drawing
    __try {
        void* pawn = PawnOf(actor);
        if (!pawn || !c->cloak) { DLOG(5, "DrawCloak: no pawn/cloak pawn=%p cloak=%p", pawn, c->cloak); return; }
        void* body = *reinterpret_cast<void**>(P(pawn, kActorMesh));
        void* instActor = *reinterpret_cast<void**>(P(self, kInstActor));
        void* instMesh  = *reinterpret_cast<void**>(P(self, kInstMesh));
        if (instActor != pawn || instMesh != body) {
            DLOG(10, "DrawCloak: self mismatch self=%p self.Actor=%p pawn=%p self.Mesh=%p body=%p", self, instActor, pawn, instMesh, body);
            return;
        }
        if (body != lastBody || c->cloak != lastCloak) {
            lastBody = body; lastCloak = c->cloak; lastOk = SkeletonCompatible(body, c->cloak);
            if (!lastOk) Log("skeleton mismatch body=%p cloak=%p", body, c->cloak);
        }
        if (!lastOk) return;

        void* inst = g_api.CloakMeshGetInstance(c->cloak, nullptr, pawn);
        void* instVtbl = inst ? *reinterpret_cast<void**>(inst) : nullptr;
        void* iMesh  = inst ? *reinterpret_cast<void**>(P(inst, kInstMesh)) : nullptr;
        void* iActor = inst ? *reinterpret_cast<void**>(P(inst, kInstActor)) : nullptr;
        DLOG(10, "DrawCloak: inst=%p vtbl=%p (want %p) inst.Mesh=%p (want %p) inst.Actor=%p (want %p)", inst, instVtbl, g_api.subVtbl, iMesh, c->cloak, iActor, pawn);
        if (!inst) return;
        if (instVtbl != g_api.subVtbl) return;                                              // must be a USubSkeletalMeshInstance
        if (iMesh != c->cloak || iActor != pawn) return;

        DLOG(10, "DrawCloak: rendering cloak %p on pawn %p (slot %d, idx %d)", c->cloak, pawn, g_cfg.slot, c->idx);
        // skins: a real item already filled CloakSkins; otherwise (test mode) borrow the mesh's own materials
        // The engine's item loader only fills ONE CloakSkins slot (with the last texture of the item's list) and leaves the
        // other null, but the cloak meshes have two sections. The mesh's own Materials[] are authoritative: always apply both.
        void** skins = reinterpret_cast<void**>(P(pawn, kPawnCloakSkins));
        {
            static void* cacheMesh[32]; static void* cacheMat[32][2]; static bool cacheOk[32]; static size_t cacheOff[32]; static int ncache = 0;
            int ci = -1;
            for (int i = 0; i < ncache; ++i) if (cacheMesh[i] == c->cloak) { ci = i; break; }
            static bool cacheTried[32];
            if (ci < 0 && ncache < 32) { ci = ncache++; cacheMesh[ci] = c->cloak; cacheOk[ci] = false; cacheTried[ci] = false; cacheMat[ci][0] = cacheMat[ci][1] = nullptr; cacheOff[ci] = 0; }
            if (ci >= 0 && (!cacheTried[ci] || (cacheOk[ci] && !MaterialsStillThere(c->cloak, cacheOff[ci], cacheMat[ci])))) {
                cacheTried[ci] = true;
                cacheOk[ci] = FindMeshMaterials(c->cloak, cacheMat[ci], &cacheOff[ci]);
                Log("materials of cloak mesh %p: %s %p %p (at +0x%x)", c->cloak, cacheOk[ci] ? "found" : "NOT found", cacheMat[ci][0], cacheMat[ci][1], (unsigned)cacheOff[ci]);
            }
            if (ci >= 0 && cacheOk[ci]) {
                if (skins[0] != cacheMat[ci][0] || skins[1] != cacheMat[ci][1]) {
                    DLOG(10, "DrawCloak: CloakSkins %p %p -> %p %p (mesh materials)", skins[0], skins[1], cacheMat[ci][0], cacheMat[ci][1]);
                    skins[0] = cacheMat[ci][0]; skins[1] = cacheMat[ci][1];
                }
            }
        }

        // USubSkeletalMeshInstance::Render draws actor->GetSubMesh(SubMeshIndex) == *(pawn+0x3bc+4*index), NOT the
        // instance's own mesh (and returns silently when that slot is empty), so the cloak must sit in that slot.
        slotPtr = reinterpret_cast<void**>(P(pawn, kPawnSubMeshes)) + g_cfg.slot;
        savedSlot = *slotPtr;
        *slotPtr = c->cloak;
        DLOG(10, "DrawCloak: slot %d: %p -> %p", g_cfg.slot, savedSlot, c->cloak);

        g_api.SetSubMeshIndex(inst, nullptr, g_cfg.slot);
        void** vt = *reinterpret_cast<void***>(ri);                                           // FRenderInterface vtable
        RIFunc_t pushState = reinterpret_cast<RIFunc_t>(vt[0]);
        RIFunc_t popState  = reinterpret_cast<RIFunc_t>(vt[1]);
        // local bounding box of this cloak mesh (the pivot/extent of the bend), cached per mesh
        t_mi = InfoFor(c->cloak);
        t_cloakBBoxOk = t_mi && t_mi->bbOk;
        if (t_cloakBBoxOk) memcpy(t_cloakBBox, t_mi->bb, sizeof t_cloakBBox);
        t_pose = nullptr;
        EssPrepare(c->cloak, self, pawn);
        pushState(ri, nullptr);
        t_inCloak = true;
        g_api.SubRender(inst, nullptr, actor, scene, c->lights, proj, ri);
        t_inCloak = false;
        t_pose = nullptr; t_mi = nullptr; t_essM = nullptr; t_essP = nullptr; t_clothP = nullptr; t_wingM = nullptr; t_wingP = nullptr; t_fxP = nullptr;
        if (c->idx >= 0 && c->idx <= 12) g_api.SetSubMeshIndex(inst, nullptr, c->idx);
        popState(ri, nullptr);
        *slotPtr = savedSlot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_inCloak = false; t_essM = nullptr; t_essP = nullptr; t_clothP = nullptr; t_wingM = nullptr; t_wingP = nullptr; t_fxP = nullptr;
        if (slotPtr) *slotPtr = savedSlot;
        const LONG ne = InterlockedIncrement(&g_excCount);
        if (t_curId != 0 && !EssIsBadId(t_curId) && g_nBadId < 16) { g_badId[g_nBadId] = t_curId; InterlockedIncrement(&g_nBadId); }
        Log("exception while drawing cloak id %d (#%ld) - that cloak is skipped from now on%s", t_curId, (long)ne, ne >= 6 ? "; too many exceptions - cloaks disabled for this session" : "");
        if (ne >= 6) InterlockedExchange(&g_disabled, 1);
    }
}

// ----------------------------------------------------------------------------------------------
// hooks
// ----------------------------------------------------------------------------------------------
// ---- experiment: deform the already-skinned vertices of the cloak (32-byte vertices: pos[3] normal[3] uv[2])
void DeformCloakStream(void* stream, void* dest) {
    int n = *reinterpret_cast<int*>(P(stream, 0x24));
    if (n <= 0 || n > 60000) return;
    float* v = static_cast<float*>(dest);
    const int up = (g_cfg.vertUp >= 0 && g_cfg.vertUp <= 2) ? g_cfg.vertUp : 2;
    const int ax = (g_cfg.vertAxis >= 0 && g_cfg.vertAxis <= 2) ? g_cfg.vertAxis : 0;
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < n; ++i) { float z = v[i * 8 + up]; if (z < lo) lo = z; if (z > hi) hi = z; }
    DLOG(6, "cloak stream %p: %d vertices, extent along axis %d = %.2f .. %.2f; v0=(%.2f %.2f %.2f) n0=(%.2f %.2f %.2f) uv0=(%.2f %.2f)",
         stream, n, up, lo, hi, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    float t = (float)(GetTickCount64() % 3600000ULL) / 1000.0f;
    float sway = sinf(t * 6.2831853f * g_cfg.vertFreq) * g_cfg.vertAmp;
    float span = hi - lo; if (span < 1e-3f) return;
    for (int i = 0; i < n; ++i) {
        float h = (hi - v[i * 8 + up]) / span;                 // 0 at the top, 1 at the hem
        v[i * 8 + ax] += sway * h * h;
    }
}

void* Follow(void* p);                                  // defined in the install section

// ---- GPU skinning: bone matrices reach the vertex shader through UD3DRenderDevice::SetVertexShaderVectorConstants(float*, start, count)
using SetVSConst_t = void (__fastcall*)(void* self, void* edx, float* data, unsigned start, unsigned count);
SetVSConst_t g_origSetVSConst = nullptr;

void __fastcall HkSetVSConst(void* self, void* edx, float* data, unsigned start, unsigned count) {
    if (t_inCloak && g_cfg.constProbe) {
        __try {
            if (count >= 3 && data)
                DLOG(60, "VSConst start=%u count=%u  [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]", start, count,
                     data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11]);
            else
                DLOG(60, "VSConst start=%u count=%u", start, count);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_origSetVSConst(self, edx, data, start, count);
}

DWORD WINAPI LateInstallThread(LPVOID) {                  // D3DDrv.dll is loaded when the render device is created
    HMODULE d3d = nullptr;
    for (int i = 0; i < 2400 && !d3d; ++i) { d3d = GetModuleHandleW(L"D3DDrv.dll"); if (!d3d) Sleep(250); }
    if (!d3d) { Log("D3DDrv.dll never loaded"); return 0; }
    void* p = Follow(reinterpret_cast<void*>(GetProcAddress(d3d, "?SetVertexShaderVectorConstants@UD3DRenderDevice@@QAEXPAMII@Z")));
    if (!p) { Log("SetVertexShaderVectorConstants not exported"); return 0; }
    if (MH_CreateHook(p, reinterpret_cast<void*>(&HkSetVSConst), reinterpret_cast<void**>(&g_origSetVSConst)) != MH_OK || MH_EnableHook(p) != MH_OK) { Log("hook of SetVertexShaderVectorConstants failed"); return 0; }
    Log("D3DDrv hook ready: SetVertexShaderVectorConstants at %p (module %p)", p, d3d);
    return 0;
}

void __fastcall HkGetStream(void* self, void* edx, void* dest) {
    g_origGetStream(self, edx, dest);
    if (t_inCloak && g_cfg.vertTest && !g_disabled) {
        __try { DeformCloakStream(self, dest); } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (InterlockedExchange(&g_disabled, 1) == 0) Log("exception while deforming the cloak stream - cloaks disabled");
        }
    }
}

void __fastcall HkRender(void* self, void* edx, void* actor, void* scene, void* lights, void* proj, void* ri) {
    if (t_busy || g_disabled) { g_origRender(self, edx, actor, scene, lights, proj, ri); return; }
    RenderCtx ctx = { self, actor, scene, lights, proj, ri, nullptr, -1, false };
    void* pawn = nullptr; void* slotMesh = nullptr;
    __try {
        pawn = PawnOf(actor);
        if (pawn) slotMesh = reinterpret_cast<void**>(P(pawn, kPawnSubMeshes))[13];
    } __except (EXCEPTION_EXECUTE_HANDLER) { pawn = nullptr; }
    DLOG(4, "Render: self=%p actor=%p pawn=%p", self, actor, pawn);
    if (pawn && g_cfg.debug) Census(pawn);
    if (pawn && g_cfg.scanIds) { __try { ScanIds(pawn); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (slotMesh) DLOG(40, "Render: pawn=%p has a mesh in sub-mesh slot 13: %p (the engine put it there)", pawn, slotMesh);
    if (pawn) ctx.cloak = FindCloak(pawn, &ctx.idx);
    if (ctx.cloak) DLOG(40, "Render: pawn=%p cloak=%p idx=%d", pawn, ctx.cloak, ctx.idx);
    if (ctx.cloak && g_cfg.capeTest && g_api.SetBoneRotation) { __try { ApplyCapeTest(self, pawn); } __except (EXCEPTION_EXECUTE_HANDLER) { if (InterlockedExchange(&g_disabled, 1) == 0) Log("exception in cape test"); } }
    RenderCtx* prev = t_ctx;
    t_ctx = ctx.cloak ? &ctx : nullptr;
    g_origRender(self, edx, actor, scene, lights, proj, ri);
    t_ctx = prev;
}

void LogMat(const char* tag, void* self, void* mesh, const void* mat, int a, int b, int c) {
    __try {
        const float* m = static_cast<const float*>(mat);
        DLOG(10, "%s: self=%p mesh=%p a=%d b=%d c=%d M=[%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]",
             tag, self, mesh, a, b, c, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void __fastcall HkDrawSection(void* self, void* edx, void* actor, void* mesh, void* scene, const void* mat, void* proj, void* ri, int a, int b, int c) {
    RenderCtx* ctx = t_ctx;
    if (ctx && !ctx->drawn) {
        void* selfMesh = nullptr;
        __try { selfMesh = *reinterpret_cast<void**>(P(self, kInstMesh)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        DLOG(12, "DrawSection: self=%d actor=%d scene=%d proj=%d ri=%d mesh=%d busy=%d (self=%p ctx.self=%p, selfMesh=%p mesh=%p)",
             self == ctx->self, actor == ctx->actor, scene == ctx->scene, proj == ctx->proj, ri == ctx->ri, selfMesh == mesh, (int)t_busy, self, ctx->self, selfMesh, mesh);
    }
    if (ctx && !ctx->drawn && !t_busy && !g_disabled && self == ctx->self && actor == ctx->actor && scene == ctx->scene &&
        proj == ctx->proj && ri == ctx->ri && *reinterpret_cast<void**>(P(self, kInstMesh)) == mesh) {
        ctx->drawn = true;
        t_busy = true;
        DrawCloak(ctx, self, actor, scene, proj, ri);
        t_busy = false;
    }
    const void* useMat = mat;
    alignas(16) float swung[16];
    if (t_inCloak) {
        LogMat("cloak DrawSection(9)", self, mesh, mat, a, b, c);
        if (ApplyMatSway(self, mat, swung)) useMat = swung;
    }
    g_origDrawSection(self, edx, actor, mesh, scene, useMat, proj, ri, a, b, c);
}

// USubSkeletalMeshInstance::Render fetches the matrix it hands to RI->SetTransform(LocalToWorld) with MeshToWorld(float scale)
// (vtable +0x128, hidden return pointer first, float second).  This is where the cloak's single rigid matrix can be swung.
using MeshToWorld_t = void* (__fastcall*)(void* self, void* edx, float* ret, float scale);
MeshToWorld_t g_origMeshToWorld = nullptr;
void* __fastcall HkMeshToWorld(void* self, void* edx, float* ret, float scale) {
    void* r = g_origMeshToWorld(self, edx, ret, scale);
    if (t_inCloak && !g_disabled) {
        __try {
            float* m = ret ? ret : static_cast<float*>(r);
            LogMat("cloak MeshToWorld", self, nullptr, m, 0, 0, 0);
            if (g_cfg.vbDeform) {                                          // the pose / cloth state of this character, used when its vertex buffer is drawn
                t_pose = StepSway(self, m);
                if (g_cfg.vbMode == 2 && t_mi && t_mi->cm) ClothStep(t_pose, t_mi->cm);
            }
            alignas(16) float sw[16];
            if (ApplyMatSway(self, m, sw)) memcpy(m, sw, sizeof sw);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (InterlockedExchange(&g_disabled, 1) == 0) Log("exception in MeshToWorld hook - cloaks disabled");
        }
    }
    return r;
}

// USubSkeletalMeshInstance::DrawSection has a second overload (11 args: ..., int a, int b, int c, float, EActorEffectType)
using DrawSection11_t = void (__fastcall*)(void* self, void* edx, void* actor, void* mesh, void* scene, const void* mat, void* proj, void* ri, int a, int b, int c, float f, int effect);
DrawSection11_t g_origDrawSection11 = nullptr;
void __fastcall HkDrawSection11(void* self, void* edx, void* actor, void* mesh, void* scene, const void* mat, void* proj, void* ri, int a, int b, int c, float f, int effect) {
    const void* useMat = mat;
    alignas(16) float swung[16];
    if (t_inCloak) {
        LogMat("cloak DrawSection(11)", self, mesh, mat, a, b, c);
        if (ApplyMatSway(self, mat, swung)) useMat = swung;
    }
    g_origDrawSection11(self, edx, actor, mesh, scene, useMat, proj, ri, a, b, c, f, effect);
}

// ----------------------------------------------------------------------------------------------
// install
// ----------------------------------------------------------------------------------------------
void* Follow(void* p) {                                  // follow incremental-link jmp thunks
    for (int i = 0; i < 4 && p && *static_cast<uint8_t*>(p) == 0xE9; ++i)
        p = static_cast<uint8_t*>(p) + 5 + *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(p) + 1);
    return p;
}

struct Need { const char* sym; uintptr_t rva; };
const Need kEngineChecks[] = {   // exported function must live at this RVA, otherwise this engine.dll is not the one we analysed
    { "?Render@USkeletalMeshInstance@@UAEXPAVFDynamicActor@@PAVFLevelSceneNode@@PAV?$TList@PAVFDynamicLight@@@@PAV?$TList@PAUFProjectorRenderInfo@@@@PAVFRenderInterface@@@Z", 0x3dcfd0 },
    { "?DrawSection@USkeletalMeshInstance@@UAEXPAVFDynamicActor@@PAVUSkeletalMesh@@PAVFLevelSceneNode@@ABVFMatrix@@PAV?$TList@PAUFProjectorRenderInfo@@@@PAVFRenderInterface@@HHH@Z", 0x3c24d0 },
    { "?Render@USubSkeletalMeshInstance@@UAEXPAVFDynamicActor@@PAVFLevelSceneNode@@PAV?$TList@PAVFDynamicLight@@@@PAV?$TList@PAUFProjectorRenderInfo@@@@PAVFRenderInterface@@@Z", 0x3caca0 },
    { "?GetCloakMesh@APawn@@UAEPAVUMesh@@XZ", 0x41500 },
    { "?CloakMeshGetInstance@UMesh@@UAEPAVUMeshInstance@@PAVAActor@@@Z", 0x2e59f0 },
    { "?SetSubMeshIndex@USubSkeletalMeshInstance@@UAEXH@Z", 0x63770 },
};
const char kVtbl[] = "??_7USubSkeletalMeshInstance@@6B@";
constexpr uintptr_t kVtblRva = 0x55cde4;

bool Install() {
    HMODULE eng = nullptr, core = nullptr;
    for (int i = 0; i < 600 && !(eng && core); ++i) {      // up to 30 s
        eng = GetModuleHandleW(L"Engine.dll"); core = GetModuleHandleW(L"Core.dll");
        if (!(eng && core)) Sleep(50);
    }
    if (!eng || !core) { Log("Engine.dll/Core.dll not loaded"); return false; }
    uintptr_t base = reinterpret_cast<uintptr_t>(eng);
    {
        HMODULE mods[2] = { eng, core };
        for (int i = 0; i < 2; ++i) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mods[i]);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const char*>(mods[i]) + dos->e_lfanew);
            g_modLo[i] = reinterpret_cast<uintptr_t>(mods[i]);
            g_modHi[i] = g_modLo[i] + nt->OptionalHeader.SizeOfImage;
        }
    }

    void* real[6] = {};
    for (int i = 0; i < 6; ++i) {
        void* p = reinterpret_cast<void*>(GetProcAddress(eng, kEngineChecks[i].sym));
        if (!p) { Log("missing export: %s", kEngineChecks[i].sym); return false; }
        real[i] = Follow(p);
        uintptr_t rva = reinterpret_cast<uintptr_t>(real[i]) - base;
        if (rva != kEngineChecks[i].rva) { Log("RVA mismatch (%08x != %08x) for %.60s - unsupported engine.dll", (unsigned)rva, (unsigned)kEngineChecks[i].rva, kEngineChecks[i].sym); return false; }
    }
    g_api.subVtbl = reinterpret_cast<void*>(GetProcAddress(eng, kVtbl));
    if (!g_api.subVtbl || reinterpret_cast<uintptr_t>(g_api.subVtbl) - base != kVtblRva) { Log("USubSkeletalMeshInstance vtable not where expected"); return false; }
    g_api.SetBoneRotation = reinterpret_cast<SetBoneRot_t>(Follow(reinterpret_cast<void*>(GetProcAddress(eng, "?SetBoneRotation@USkeletalMeshInstance@@QAEHVFName@@VFRotator@@HM@Z"))));
    Log("SetBoneRotation: %p", reinterpret_cast<void*>(g_api.SetBoneRotation));
    g_api.pawnClass     = reinterpret_cast<void*>(GetProcAddress(eng, "?PrivateStaticClass@APawn@@0VUClass@@A"));
    g_api.skelMeshClass = reinterpret_cast<void*>(GetProcAddress(eng, "?PrivateStaticClass@USkeletalMesh@@0VUClass@@A"));
    g_api.GetFullName   = reinterpret_cast<GetFullName_t>(GetProcAddress(core, "?GetFullName@UObject@@QBEPBGPAG@Z"));
    g_api.IsA           = reinterpret_cast<IsA_t>(GetProcAddress(core, "?IsA@UObject@@QBEHPAVUClass@@@Z"));
    g_api.AddToRoot     = reinterpret_cast<AddToRoot_t>(GetProcAddress(core, "?AddToRoot@UObject@@QAEXXZ"));
    g_api.fnameCtor     = reinterpret_cast<void*>(GetProcAddress(core, "??0FName@@QAE@PBGW4EFindName@@@Z"));
    g_api.fnameEntry    = reinterpret_cast<void*>(GetProcAddress(core, "?GetEntry@FName@@SAPAUFNameEntry@@H@Z"));
    g_api.LoadObject    = reinterpret_cast<LoadObject_t>(GetProcAddress(core, "?StaticLoadObject@UObject@@SAPAV1@PAVUClass@@PAV1@PBG2KPAVUPackageMap@@@Z"));
    if (!g_api.pawnClass || !g_api.GetFullName || !g_api.IsA) { Log("core exports missing"); return false; }

    g_api.SubRender            = reinterpret_cast<Render_t>(real[2]);
    g_api.GetCloakMesh         = reinterpret_cast<GetCloakMesh_t>(real[3]);
    g_api.CloakMeshGetInstance = reinterpret_cast<CloakInst_t>(real[4]);
    g_api.SetSubMeshIndex      = reinterpret_cast<SetSubIdx_t>(real[5]);

    { const MH_STATUS st = MH_Initialize(); if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { Log("MH_Initialize failed"); return false; } }
    if (MH_CreateHook(real[0], reinterpret_cast<void*>(&HkRender), reinterpret_cast<void**>(&g_origRender)) != MH_OK ||
        MH_CreateHook(real[1], reinterpret_cast<void*>(&HkDrawSection), reinterpret_cast<void**>(&g_origDrawSection)) != MH_OK) {
        Log("MH_CreateHook failed"); return false;
    }
    {   // the sub-mesh DrawSection overload with the extra (float, EActorEffectType) arguments
        void* ds11 = Follow(reinterpret_cast<void*>(GetProcAddress(eng,
            "?DrawSection@USubSkeletalMeshInstance@@UAEXPAVFDynamicActor@@PAVUSkeletalMesh@@PAVFLevelSceneNode@@ABVFMatrix@@PAV?$TList@PAUFProjectorRenderInfo@@@@PAVFRenderInterface@@HHHMW4EActorEffectType@@@Z")));
        if (ds11 && MH_CreateHook(ds11, reinterpret_cast<void*>(&HkDrawSection11), reinterpret_cast<void**>(&g_origDrawSection11)) == MH_OK)
            Log("DrawSection(11) hook ready at %p (rva 0x%x)", ds11, (unsigned)(reinterpret_cast<uintptr_t>(ds11) - base));
        else Log("DrawSection(11) hook NOT installed (%p)", ds11);
    }
    {
        void* m2w = Follow(reinterpret_cast<void*>(GetProcAddress(eng, "?MeshToWorld@USubSkeletalMeshInstance@@UAE?AVFMatrix@@M@Z")));
        if (m2w && MH_CreateHook(m2w, reinterpret_cast<void*>(&HkMeshToWorld), reinterpret_cast<void**>(&g_origMeshToWorld)) == MH_OK)
            Log("MeshToWorld(float) hook ready at %p (rva 0x%x)", m2w, (unsigned)(reinterpret_cast<uintptr_t>(m2w) - base));
        else Log("MeshToWorld(float) hook NOT installed (%p)", m2w);
    }
    if (g_cfg.essenceCrest) {
        void* pc = Follow(reinterpret_cast<void*>(GetProcAddress(eng, "?GetPledgeCrestTex@UNetworkHandler@@UAEPAVUTexture@@H@Z")));
        if (pc && MH_CreateHook(pc, reinterpret_cast<void*>(&HkPledgeCrest), reinterpret_cast<void**>(&g_origPledgeCrest)) == MH_OK) Log("crest: GetPledgeCrestTex hook ready at %p (rva 0x%x)", pc, (unsigned)(reinterpret_cast<uintptr_t>(pc) - base));
        else Log("crest: GetPledgeCrestTex hook NOT installed (%p)", pc);
        void* pc2 = Follow(reinterpret_cast<void*>(GetProcAddress(eng, "?GetPledgeCrestTexFromPledgeCrestID@UNetworkHandler@@UAEPAVUTexture@@H@Z")));
        if (pc2 && pc2 != pc && MH_CreateHook(pc2, reinterpret_cast<void*>(&HkPledgeCrestFromId), reinterpret_cast<void**>(&g_origPledgeCrestFromId)) == MH_OK) Log("crest: GetPledgeCrestTexFromPledgeCrestID hook ready at %p (rva 0x%x)", pc2, (unsigned)(reinterpret_cast<uintptr_t>(pc2) - base));
        else Log("crest: GetPledgeCrestTexFromPledgeCrestID hook NOT installed (%p)", pc2);
    }
    if (g_cfg.vertTest) {
        void* gs = Follow(reinterpret_cast<void*>(GetProcAddress(eng, "?GetStreamData@FSkinVertexStream@@UAEXPAX@Z")));
        if (gs && reinterpret_cast<uintptr_t>(gs) - base == 0x3b1ee0) {
            if (MH_CreateHook(gs, reinterpret_cast<void*>(&HkGetStream), reinterpret_cast<void**>(&g_origGetStream)) == MH_OK) Log("vertex stream hook ready at %p", gs);
            else Log("MH_CreateHook(GetStreamData) failed");
        } else Log("GetStreamData not at the expected RVA (%p) - vertex test disabled", gs);
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { Log("MH_EnableHook failed"); return false; }
    Log("hooks installed (engine base %p, package '%ls', ids %d..%d, slot %d, forceId %d)", eng, g_cfg.package, g_cfg.idMin, g_cfg.idMax, g_cfg.slot, g_cfg.forceId);
    return true;
}

DWORD WINAPI InitThread(LPVOID) {
    LoadConfig();
    Log("cloakhook starting (VBDeform=%d mode=%d)", (int)g_cfg.vbDeform, g_cfg.vbMode);
    EssLoadPackOnce();
    if (g_cfg.vbDeform || g_cfg.essence) {                 // before the engine creates its Direct3D device
        const MH_STATUS st = MH_Initialize();
        if (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) InstallD3DHooks();
        else Log("MH_Initialize failed (%d) - vertex deformation unavailable", (int)st);
    }
    if (!Install()) { Log("cloakhook NOT active"); return 0; }
    if (g_cfg.constProbe) { HANDLE t = CreateThread(nullptr, 0, LateInstallThread, nullptr, 0, nullptr); if (t) CloseHandle(t); }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameW(h, g_dir, MAX_PATH);
        wchar_t* slash = wcsrchr(g_dir, L'\\');
        if (slash) slash[1] = 0;
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);   // never do real work under the loader lock
        if (t) CloseHandle(t);
    }
    return TRUE;
}
