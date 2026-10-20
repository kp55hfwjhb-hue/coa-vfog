// refcore — Stage 1a : outil de référence cross-platform du cœur CPU de CoAVolFog.
//
// Rejoue les entrées FIXES de la scène harbour du harness Windows (constantes de
// tests/harness.cpp) à travers le cœur porté (fog_data + fog_model), puis les
// fonctions de référence CPU du harness (intégration 8192 pas), et dump tout en
// texte déterministe (%.9g). Le même outil compilé en Win32 côté CI produit la
// référence ; le diff (diff_dump.py) compare à tolérance relative 1e-5 — jamais
// au bit près (libm x86/MSVC ≠ arm64/clang, et le côté macOS compile avec
// -ffp-contract=off pour isoler la contraction).
//
//   refcore <fogdata.bin> <dump.txt>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "engine.h"
#include "fog_data.h"
#include "fog_model.h"

// --- constantes de la scène harbour (copiées de tests/harness.cpp) ----------

namespace harbour
{
    struct Vec3 { float x, y, z; };
    constexpr float kPi = 3.14159265358979f;
    constexpr float kDegree = kPi / 180.0f;
    constexpr Vec3 kEye = { -8576.0f, 1007.0f, 104.0f };
    constexpr Vec3 kToLight = { 0.673f, 0.673f, 0.307f };
    constexpr float kFar = 791.6f;
    constexpr float kNear = 0.2f;
    constexpr float kLoggedP11 = 1.511f;
    constexpr float kDayFraction = 0.7802f;
    constexpr float kContinentFogStart = 197.9f;
    constexpr int kMap = 0;                      // kEasternKingdoms
    constexpr unsigned kWidth = 2560, kHeight = 1440;
    constexpr int kIntegrationSteps = 8192;      // kReferenceIntegrationSteps

    Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    Vec3 Add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    float Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
    Vec3 Norm(Vec3 a) { const float l = Len(a); return { a.x / l, a.y / l, a.z / l }; }
    Vec3 Dir(float azimuthDeg, float elevationDeg)
    {
        const float a = azimuthDeg * kDegree, e = elevationDeg * kDegree;
        return { std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e) };
    }
    float SmoothStep(float e0, float e1, float x)
    {
        const float t = std::fmin(std::fmax((x - e0) / (e1 - e0), 0.0f), 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    void CameraRelativeLookAt(Vec3 eye, Vec3 at, float* view)
    {
        const Vec3 forward = Norm(Sub(at, eye));
        const Vec3 right = Norm({ forward.y, -forward.x, 0.0f }); // cross(forward, up(0,0,1))
        const Vec3 up = { right.y * forward.z - right.z * forward.y,
                          right.z * forward.x - right.x * forward.z,
                          right.x * forward.y - right.y * forward.x };
        const float rows[16] = { right.x, up.x, forward.x, 0, right.y, up.y, forward.y, 0,
                                 right.z, up.z, forward.z, 0, 0, 0, 0, 1 };
        std::memcpy(view, rows, sizeof(rows));
    }

    void GlDepthProjection(float yScale, float aspect, float zNear, float zFar, float* m)
    {
        const float p[16] = { yScale / aspect, 0, 0, 0, 0, yScale, 0, 0, 0, 0,
                              (zFar + zNear) / (zFar - zNear), 1,
                              0, 0, -2.0f * zFar * zNear / (zFar - zNear), 0 };
        std::memcpy(m, p, sizeof(p));
    }

    FrameInputs SunsetInputs(const float* view, const float* proj, Vec3 at, float stormBlend)
    {
        const D3DVIEWPORT9 vp = { 0, 0, kWidth, kHeight, 0.0f, 1.0f };
        FrameInputs in = {};
        std::memcpy(in.cameraRelativeView, view, sizeof(in.cameraRelativeView));
        std::memcpy(in.glProjection, proj, sizeof(in.glProjection));
        in.camPos[0] = kEye.x; in.camPos[1] = kEye.y; in.camPos[2] = kEye.z;
        const Vec3 f = Norm(Sub(at, kEye));
        const Vec3 oneYard = Add(kEye, f);
        in.camTarget[0] = oneYard.x; in.camTarget[1] = oneYard.y; in.camTarget[2] = oneYard.z;
        in.viewport = vp;
        in.dayFraction = kDayFraction;
        const Vec3 l = Norm(kToLight);
        in.toLight[0] = l.x; in.toLight[1] = l.y; in.toLight[2] = l.z;
        in.lightIsMoon = false;
        in.fogColor = 0xFF574C5C;
        in.sunColor = 0xFFFFE7B6;
        in.directColor = 0xFFFF7400;
        in.ambientColor = 0xFF676680;
        in.fogStart = kContinentFogStart;
        in.fogEnd = 791.7f;
        in.zoneFogDistance = 791.7f;
        in.farClip = kFar;
        in.inLiquid = false;
        in.mapId = kMap;
        in.lightParams.stormBlend = stormBlend;
        in.lightParams.screenEffectSlot = kNoScreenEffectLightSlot;
        return in;
    }

    // --- sondes (kHarbourProbes) -------------------------------------------

    enum class ProbeKind { World, Sky, SunAzimuth };
    struct Probe
    {
        const char* name;
        ProbeKind kind;
        float degreesLeftOfView, horizontalDistance, height, elevationDeg, elevationFromSunDeg;
    };
    const Probe kProbes[] = {
        { "ship80z55",        ProbeKind::World,      18.0f,  80.0f,  55.0f, 0.0f, 0.0f },
        { "shiphull80z10",    ProbeKind::World,      18.0f,  80.0f,  10.0f, 0.0f, 0.0f },
        { "lighthouse350z60", ProbeKind::World,      -6.0f, 350.0f,  60.0f, 0.0f, 0.0f },
        { "lightstop350z125", ProbeKind::World,      -6.0f, 350.0f, 125.0f, 0.0f, 0.0f },
        { "ridge650z60",      ProbeKind::World,     -25.0f, 650.0f,  60.0f, 0.0f, 0.0f },
        { "ridgetop650z170",  ProbeKind::World,     -25.0f, 650.0f, 170.0f, 0.0f, 0.0f },
        { "sea100",           ProbeKind::World,       3.0f, 100.0f,  0.0f, 0.0f, 0.0f },
        { "sea160",           ProbeKind::World,       3.0f, 160.0f,  0.0f, 0.0f, 0.0f },
        { "sea400",           ProbeKind::World,       3.0f, 400.0f,  0.0f, 0.0f, 0.0f },
        { "sea700",           ProbeKind::World,       3.0f, 700.0f,  0.0f, 0.0f, 0.0f },
        { "skym4deg",         ProbeKind::Sky,         3.0f,  0.0f,  0.0f, -4.0f, 0.0f },
        { "skyp1deg",         ProbeKind::Sky,        30.0f,  0.0f,  0.0f,  1.0f, 0.0f },
        { "sky10deg",         ProbeKind::Sky,        30.0f,  0.0f,  0.0f, 10.0f, 0.0f },
        { "sky40deg",         ProbeKind::Sky,        30.0f,  0.0f,  0.0f, 40.0f, 0.0f },
        { "sunm6deg",         ProbeKind::SunAzimuth,  0.0f,  0.0f,  0.0f, 0.0f, -6.0f },
        { "sunm15deg",        ProbeKind::SunAzimuth,  0.0f,  0.0f,  0.0f, 0.0f, -15.0f },
    };
    constexpr int kProbeCount = sizeof(kProbes) / sizeof(kProbes[0]);

    Vec3 ProbeOffset(const Probe& p, float viewAzimuth, float sunAzimuth, float sunElevation)
    {
        if (p.kind == ProbeKind::World)
        {
            const Vec3 q = Dir(viewAzimuth + p.degreesLeftOfView, 0.0f);
            return { q.x * p.horizontalDistance, q.y * p.horizontalDistance, p.height - kEye.z };
        }
        if (p.kind == ProbeKind::Sky)
            return Dir(viewAzimuth + p.degreesLeftOfView, p.elevationDeg);
        return Dir(sunAzimuth, sunElevation + p.elevationFromSunDeg);
    }

    // --- références CPU du harness ------------------------------------------

    // ReferenceUnshadowedLayerOpticalDepths (harness.cpp)
    void LayerOpticalDepths(const FogParams& fog, float camZ, Vec3 dirW, float viewZ, float rayLen,
                            bool sky, float* tau)
    {
        float z = sky ? fog.maxDistance : viewZ;
        const float horizon = sky ? 1.0f : SmoothStep(fog.horizonStart, fog.farClip, z);
        z = z + (fog.maxDistance - z) * horizon;
        const float tMax = std::fmin(z * rayLen, fog.maxDistance);
        const float up = std::fmax(dirW.z, 0.0f);
        const float dirZ = dirW.z + (up - dirW.z) * horizon;
        const float dt = tMax / kIntegrationSteps;
        for (int j = 0; j < kFogLayers; ++j)
            tau[j] = 0.0f;
        for (int i = 0; i < kIntegrationSteps; ++i)
        {
            const float t = (i + 0.5f) * dt;
            const float h = camZ + dirZ * t;
            for (int j = 0; j < kFogLayers; ++j)
            {
                const FogLayer& l = fog.layers[j];
                if (t < l.start || t > l.endDistance)
                    continue;
                const float scale = sky ? std::exp(-up * l.skyFalloff) : 1.0f;
                const float curve = 1.0f + l.strength * std::pow(std::fmin(std::fmax(t - l.start, 0.0f) / fog.maxDistance, 1.0f) + 1e-6f, l.exponent);
                const float heightF = std::fmin(std::exp((l.upperHeight - h) * l.upperFalloff), 1.0f) *
                                      std::fmin(std::exp((h - l.lowerHeight) * l.lowerFalloff), 1.0f);
                tau[j] += l.density * scale * curve * heightF * dt;
            }
        }
    }

    // ReferenceUnshadowedSkyTransmittance (harness.cpp) — px,py d'un pixel virtuel
    float SkyTransmittance(const FrameInputs& in, const Config& cfg, const AuthoredFog* authored,
                           float px, float py)
    {
        const FogParams fog = BuildFogParams(in, cfg, authored);
        const float* P = in.glProjection;
        const D3DVIEWPORT9& vp = in.viewport;
        const float ndcX = (px - static_cast<float>(vp.X)) / static_cast<float>(vp.Width) * 2.0f - 1.0f;
        const float ndcY = 1.0f - (py - static_cast<float>(vp.Y)) / static_cast<float>(vp.Height) * 2.0f;
        const Vec3 ray = { (ndcX - P[8]) / P[0], (ndcY - P[9]) / P[5], 1.0f };
        const float len = std::sqrt(Dot(ray, ray));
        const Vec3 v = { ray.x / len, ray.y / len, ray.z / len };
        const float* m = in.cameraRelativeView;
        const Vec3 dirW = { v.x * m[0] + v.y * m[1] + v.z * m[2],
                            v.x * m[4] + v.y * m[5] + v.z * m[6],
                            v.x * m[8] + v.y * m[9] + v.z * m[10] };
        const double upward = std::fmax(dirW.z, 0.0);
        double tau = 0.0;
        for (int li = 0; li < kFogLayers; ++li)
        {
            const FogLayer& layer = fog.layers[li];
            const double begin = std::fmax(layer.start, 0.0);
            const double end = std::fmin(layer.endDistance, fog.maxDistance);
            if (layer.density == 0.0f || end <= begin)
                continue;
            const double dt = (end - begin) / kIntegrationSteps;
            const double skyScale = std::exp(-upward * layer.skyFalloff);
            for (int sample = 0; sample < kIntegrationSteps; ++sample)
            {
                const double distance = begin + (sample + 0.5) * dt;
                const double height = in.camPos[2] + upward * distance;
                const double ramp = std::fmin((distance - layer.start) / fog.maxDistance, 1.0);
                const double curve = 1.0 + layer.strength * std::pow(ramp, layer.exponent);
                const double heightFactor = std::exp(std::fmin((layer.upperHeight - height) * layer.upperFalloff, 0.0) +
                                                     std::fmin((height - layer.lowerHeight) * layer.lowerFalloff, 0.0));
                tau += layer.density * skyScale * dt * curve * heightFactor;
            }
        }
        return static_cast<float>(std::exp(-tau));
    }
}

// --- stub du logger du mod (fog_data journalise son chargement) -------------
#include "log.h"
#include <cstdarg>
void LogWrite(LogLevel, const char* fmt, ...)
{
    if (!std::getenv("COVCORE_VERBOSE")) return;   // le dump doit rester pur
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

// --- dump déterministe -------------------------------------------------------

static FILE* g_out = nullptr;
static void DumpF(const char* key, float v) { std::fprintf(g_out, "%s\t%.9g\n", key, v); }
static void DumpI(const char* key, long long v) { std::fprintf(g_out, "%s\t%lld\n", key, v); }

static void DumpFogParams(const char* prefix, const FogParams& fog)
{
    for (int j = 0; j < kFogLayers; ++j)
    {
        char k[96];
        const FogLayer& l = fog.layers[j];
        static const char* names[] = { "start", "density", "g", "isotropic", "emissive0", "emissive1",
            "emissive2", "strength", "diffuse0", "diffuse1", "diffuse2", "exponent", "upperHeight",
            "upperFalloff", "lowerHeight", "lowerFalloff", "shadowEmissive0", "shadowEmissive1",
            "shadowEmissive2", "shadowDensity", "shadowed", "skyFalloff", "endDistance",
            "densityVariation" };
        const float vals[] = { l.start, l.density, l.g, l.isotropic, l.emissive[0], l.emissive[1],
            l.emissive[2], l.strength, l.diffuse[0], l.diffuse[1], l.diffuse[2], l.exponent,
            l.upperHeight, l.upperFalloff, l.lowerHeight, l.lowerFalloff, l.shadowEmissive[0],
            l.shadowEmissive[1], l.shadowEmissive[2], l.shadowDensity, l.shadowed, l.skyFalloff,
            l.endDistance, l.densityVariation };
        for (size_t f = 0; f < sizeof(vals) / sizeof(vals[0]); ++f)
        {
            std::snprintf(k, sizeof(k), "%s.layer%d.%s", prefix, j, names[f]);
            DumpF(k, vals[f]);
        }
    }
    for (int n = 0; n < kSceneLayers; ++n)
    {
        char k[96];
        const LayerNoise& ln = fog.noise[n];
        std::snprintf(k, sizeof(k), "%s.noise%d.alpha", prefix, n); DumpF(k, ln.alpha);
        for (int o = 0; o < kAuthoredNoiseOctaves; ++o)
        {
            std::snprintf(k, sizeof(k), "%s.noise%d.octaveWeight%d", prefix, n, o); DumpF(k, ln.octaveWeight[o]);
            std::snprintf(k, sizeof(k), "%s.noise%d.invTile%d", prefix, n, o); DumpF(k, ln.inverseTileYards[o]);
            for (int a = 0; a < 3; ++a)
            {
                std::snprintf(k, sizeof(k), "%s.noise%d.velocity%d.%d", prefix, n, o, a); DumpF(k, ln.velocity[o][a]);
            }
        }
        for (int a = 0; a < 3; ++a)
        {
            std::snprintf(k, sizeof(k), "%s.noise%d.fade%d", prefix, n, a); DumpF(k, ln.fade[a]);
        }
    }
    static const char* names[] = { "lightColor0", "lightColor1", "lightColor2", "rayColor0",
        "rayColor1", "rayColor2", "lightVisibility", "lightAboveHorizon", "directLightMatch",
        "maxDistance", "horizonStart", "farClip", "referenceZ", "farLimit" };
    const float vals[] = { fog.lightColor[0], fog.lightColor[1], fog.lightColor[2], fog.rayColor[0],
        fog.rayColor[1], fog.rayColor[2], fog.lightVisibility, fog.lightAboveHorizon,
        fog.directLightMatch, fog.maxDistance, fog.horizonStart, fog.farClip, fog.referenceZ, fog.farLimit };
    for (size_t f = 0; f < sizeof(vals) / sizeof(vals[0]); ++f)
    {
        char k[96];
        std::snprintf(k, sizeof(k), "%s.%s", prefix, names[f]);
        DumpF(k, vals[f]);
    }
    char k[96];
    std::snprintf(k, sizeof(k), "%s.linear", prefix); DumpI(k, fog.linear ? 1 : 0);
    std::snprintf(k, sizeof(k), "%s.authored", prefix); DumpI(k, fog.authored ? 1 : 0);
}

static void DumpAuthored(const char* prefix, const AuthoredFog& a)
{
    char k[96];
    std::snprintf(k, sizeof(k), "%s.layerCount", prefix); DumpI(k, a.layerCount);
    std::snprintf(k, sizeof(k), "%s.lightCount", prefix); DumpI(k, a.lightCount);
    std::snprintf(k, sizeof(k), "%s.coverage", prefix); DumpF(k, a.coverage);
    for (int i = 0; i < a.lightCount && i < kMaxBlendedLights; ++i)
    {
        std::snprintf(k, sizeof(k), "%s.lightId%d", prefix, i); DumpI(k, a.lightIds[i]);
        std::snprintf(k, sizeof(k), "%s.lightWeight%d", prefix, i); DumpF(k, a.lightWeights[i]);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: refcore <fogdata.bin> <dump.txt>\n");
        return 2;
    }
    FogData classic;
    if (!classic.Load(argv[1]))
    {
        std::fprintf(stderr, "fogdata %s : chargement impossible\n", argv[1]);
        return 1;
    }
    g_out = std::fopen(argv[2], "w");
    if (!g_out)
    {
        std::fprintf(stderr, "écriture %s impossible\n", argv[2]);
        return 1;
    }

    namespace hb = harbour;
    const hb::Vec3 toLight = hb::Norm(hb::kToLight);
    const float sunAz = std::atan2(toLight.y, toLight.x) / hb::kDegree;
    const float sunEl = std::asin(toLight.z) / hb::kDegree;
    const float pitch = sunEl - 20.0f;
    const float aspect = static_cast<float>(hb::kWidth) / static_cast<float>(hb::kHeight);

    Config cfg;   // valeurs par défaut (classicPhase compris)

    // --- scénario A : harbour au couchant, fog Classic résolu ---------------
    {
        float view[16], proj[16];
        const hb::Vec3 at = hb::Add(hb::kEye, hb::Dir(sunAz, pitch));
        hb::CameraRelativeLookAt(hb::kEye, at, view);
        hb::GlDepthProjection(hb::kLoggedP11, aspect, hb::kNear, hb::kFar, proj);
        FrameInputs in = hb::SunsetInputs(view, proj, at, 0.0f);

        for (int i = 0; i < 16; ++i)
        {
            char k[32];
            std::snprintf(k, sizeof(k), "A.view.%d", i);
            DumpF(k, view[i]);
            std::snprintf(k, sizeof(k), "A.proj.%d", i);
            DumpF(k, proj[i]);
        }

        AuthoredFog authored = {};
        const bool resolved = classic.Resolve(hb::kMap, in.camPos, in.dayFraction, in.lightParams, authored);
        DumpI("A.resolved", resolved ? 1 : 0);
        if (resolved)
            DumpAuthored("A.authored", authored);

        const FogParams fog = BuildFogParams(in, cfg, resolved ? &authored : nullptr);
        DumpFogParams("A.fog", fog);

        // sondes sur les deux azimuths de base (soleil et travers)
        const float azimuths[2] = { sunAz, sunAz - 90.0f };
        for (int v = 0; v < 2; ++v)
        {
            for (int p = 0; p < hb::kProbeCount; ++p)
            {
                const hb::Probe& pr = hb::kProbes[p];
                const hb::Vec3 off = hb::ProbeOffset(pr, azimuths[v], sunAz, sunEl);
                const hb::Vec3 dirW = hb::Norm(off);
                const float rayLen = hb::Len(off);
                float tau[kFogLayers];
                // viewZ 1000 : hors carte, l'intégration s'arrête à maxDistance
                hb::LayerOpticalDepths(fog, in.camPos[2], dirW, 1000.0f, rayLen,
                                       pr.kind == hb::ProbeKind::Sky, tau);
                for (int j = 0; j < kFogLayers; ++j)
                {
                    char k[96];
                    std::snprintf(k, sizeof(k), "A.tau.v%d.%s.l%d", v, pr.name, j);
                    DumpF(k, tau[j]);
                }
            }
        }

        // transmittance ciel : grille 4x4 de pixels virtuels (choix arbitraire
        // mais FIXE — la référence est rejouée à l'identique côté Win32)
        for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px)
            {
                char k[96];
                const float fx = (px + 0.5f) / 4.0f * static_cast<float>(hb::kWidth);
                const float fy = (py + 0.5f) / 4.0f * static_cast<float>(hb::kHeight);
                std::snprintf(k, sizeof(k), "A.sky.%d.%d", py, px);
                DumpF(k, hb::SkyTransmittance(in, cfg, resolved ? &authored : nullptr, fx, fy));
            }
    }

    // --- scénario B : harbour sans fog Classic (couches dérivées) + orage ----
    {
        float view[16], proj[16];
        const hb::Vec3 at = hb::Add(hb::kEye, hb::Dir(sunAz, pitch));
        hb::CameraRelativeLookAt(hb::kEye, at, view);
        hb::GlDepthProjection(hb::kLoggedP11, aspect, hb::kNear, hb::kFar, proj);
        FrameInputs in = hb::SunsetInputs(view, proj, at, 0.8f);
        const FogParams fog = BuildFogParams(in, cfg, nullptr);
        DumpFogParams("B.fog", fog);
        float tau[kFogLayers];
        const hb::Vec3 dirW = hb::Norm(hb::Dir(sunAz, 5.0f));
        hb::LayerOpticalDepths(fog, in.camPos[2], dirW, 1000.0f, 791.6f, false, tau);
        for (int j = 0; j < kFogLayers; ++j)
        {
            char k[64];
            std::snprintf(k, sizeof(k), "B.tau.l%d", j);
            DumpF(k, tau[j]);
        }
    }

    // --- scénario C : nuit lunaire, couches dérivées -------------------------
    {
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const D3DVIEWPORT9 vp = { 0, 0, 1280, 720, 0.0f, 0.94f };
        FrameInputs in = {};
        std::memcpy(in.cameraRelativeView, identity, sizeof(in.cameraRelativeView));
        std::memcpy(in.glProjection, identity, sizeof(in.glProjection));
        in.camPos[0] = -9456.8f; in.camPos[1] = 54.7f; in.camPos[2] = 59.6f;   // Goldshire
        in.camTarget[0] = in.camPos[0] + 100.0f;
        in.camTarget[1] = in.camPos[1];
        in.camTarget[2] = in.camPos[2];
        in.viewport = vp;
        in.dayFraction = 0.05f;
        const hb::Vec3 l = hb::Norm(hb::Vec3{ 0.673f, 0.673f, -0.307f });
        in.toLight[0] = l.x; in.toLight[1] = l.y; in.toLight[2] = l.z;
        in.lightIsMoon = true;                     // chemin kMoonLightScale
        in.fogColor = 0xFF101828;
        in.sunColor = 0xFF8090B8;
        in.directColor = 0xFF607090;
        in.ambientColor = 0xFF181C28;
        in.fogStart = hb::kContinentFogStart;
        in.fogEnd = 791.6f;
        in.zoneFogDistance = 791.6f;
        in.farClip = hb::kFar;
        in.inLiquid = false;
        in.mapId = hb::kMap;
        in.lightParams.stormBlend = 0.0f;
        in.lightParams.screenEffectSlot = kNoScreenEffectLightSlot;

        AuthoredFog authored = {};
        const bool resolved = classic.Resolve(hb::kMap, in.camPos, in.dayFraction, in.lightParams, authored);
        DumpI("C.resolved", resolved ? 1 : 0);
        if (resolved)
            DumpAuthored("C.authored", authored);
        const FogParams fog = BuildFogParams(in, cfg, resolved ? &authored : nullptr);
        DumpFogParams("C.fog", fog);
        float tau[kFogLayers];
        const hb::Vec3 dirW = hb::Norm(hb::Vec3{ 0.6f, 0.2f, 0.4f });
        hb::LayerOpticalDepths(fog, in.camPos[2], dirW, 1000.0f, 791.6f, true, tau);
        for (int j = 0; j < kFogLayers; ++j)
        {
            char k[64];
            std::snprintf(k, sizeof(k), "C.tau.l%d", j);
            DumpF(k, tau[j]);
        }
    }

    std::fclose(g_out);
    return 0;
}
