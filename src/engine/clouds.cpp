// clouds.cpp: ray-marched volumetric clouds

#include "FastNoiseLite.h"
#include "engine.h"

extern GLuint hdrfbo, mshdrfbo;
extern int atmo;
extern float atmoplanetsize, atmoheight, atmobright, atmosunlightscale, atmohaze, atmodensity, atmoozone, atmoalpha;
extern bvec atmosunlight;
extern float hdrgamma;

namespace volumetricClouds
{
    struct WeatherPixel
    {
        unsigned char r, g, b, a;
    };

    GLuint vctex = 0, crsourcetex = 0, vcfbo = 0;
    GLuint vcdepthtex = 0, vcdepthfbo = 0;
    GLuint vcatroustex = 0, vcatrousfbo = 0;
    GLuint vcbilateraltex = 0, vcbilateralfbo = 0;
    GLuint vcbilateraltemptex = 0, vcbilateraltempfbo = 0;
    GLuint vcclaritytex = 0, vcclarityfbo = 0;
    GLuint vcshadowtex = 0, vcshadowfbo = 0;
    GLuint vcweathertex = 0;
    GLuint vcfbmtex = 0;
    GLuint vccompositetex = 0;
    int vcw = 0, vch = 0, vcfullw = 0, vcfullh = 0;
    int vcshadowsz = 0;
    vec4 vccompositetexparams(0, 0, 0, 0);
    vec4 vcshadowmapworld(0, 0, 1, 0);
    float vcshadowmapstrength = 0.0f;
    vec2 vcscrolloffset(0, 0);
    int vcscrolllastmillis = -1;
    int vcweatherseed = 1337, vcfbmtexseed = -1, vcfbmtexsize = 0;
    bool vcweatherdirty = true;

    static void cleanupbuffers();

    static const int VC_WEATHER_MAP_SIZE = 512, VC_FBM_PERIOD = 16, VC_FBM_OCTAVES = 4;
    enum VCDebugPass
    {
        VC_DEBUG_RAYMARCH = 0,
        VC_DEBUG_DEPTH_CACHE,
        VC_DEBUG_ATROUS,
        VC_DEBUG_UPSCALE,
        VC_DEBUG_BILATERAL,
        VC_DEBUG_CLARITY,
        VC_DEBUG_SHADOW_MAP,
        VC_DEBUG_SHADOW_APPLY,
        VC_DEBUG_COMPOSITE,
        VC_DEBUG_PASS_COUNT
    };

    static const int VC_DEBUG_QUERY_COUNT = 3, VC_DEBUG_TIMESTAMPS = 2 + 2 * VC_DEBUG_PASS_COUNT;
    static const char * const vcdebugpassnames[VC_DEBUG_PASS_COUNT] =
    {
        "main raymarch", "depth cache", "atrous filtering", "upscale", "bilateral blur", "clarity", "shadow map", "shadow application", "final composite"
    };
    GLuint vcdebugquery[VC_DEBUG_QUERY_COUNT] = { 0, 0, 0 };
    GLuint vcdebugtimestampquery[VC_DEBUG_QUERY_COUNT][VC_DEBUG_TIMESTAMPS] = { { 0 } };
    int vcdebugquerycycle = 0, vcdebugquerywaiting = 0, vcdebugcpustart = 0;
    int vcdebugtimestampcycle = 0, vcdebugtimestampwaiting = 0, vcdebugtimestampactive = -1;
    int vcdebugpassmask[VC_DEBUG_QUERY_COUNT] = { 0, 0, 0 };
    bool vcdebuggpuquery = false, vcdebugtimestamps = false, vcdebugcputimer = false;
    float vcdebugms = -1.0f, vcdebugpassms[VC_DEBUG_PASS_COUNT] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };

    // graphic settings
    VARP(volumetricclouds, 0, 1, 1);
    VAR(debugvc, 0, 0, 1);
    VARP(vcblur, 0, 0, 1);
    VARP(vcblurscale, 1, 1, 4);
    VARP(vcatrous, 0, 1, 1);
    VARP(vcatrousiter, 1, 2, 3);
    FVARP(vcatrousalphak, 0.0f, 16.0f, 256.0f);
    FVARP(vcscale, 0.125f, 0.25f, 1.0f);
    FVARP(vcbudget, 0.0f, 0.0f, 2000.0f);          // target GPU time in milliseconds, 0 disables adaptation
    FVARP(vcbilateraledge, 1e-5f, 0.02f, 1.0f);
    VARP(vcsteps, 4, 24, 48);
    FVARP(vcmaxviewstep, 0.0f, 0.0f, 1.0e7f);       // maximum far-view step in world units, 0 derives it from cloud/world scale
    VARP(vcsunsteps, 4, 4, 64);
    VARP(vcfbmresolution, 16, 128, 256);
    VARP(vcfbmseed, -0x100000, 0, 0x100000);
    VARP(vcshadow, 0, 1, 1);
    VARP(vcshadowradius, 10, 100, 1000);              // shadow footprint half-width as a percentage of world size
    VARP(vcshadowmapsize, 64, 128, 4096);
    VARP(vcshadowsamples, 1, 4, 8);
    VARP(vcshadowpcf, 0, 1, 2);
    VARP(vcclarity, 0, 1, 1);
    FVARP(vcclaritystrength, 0.0f, 0.22f, 1.0f);
    FVARP(vcclarityradius, 0.5f, 16.0f, 32.0f);
    FVARP(vcclarityalphak, 0.0f, 18.0f, 64.0f);
    FVARP(vcclaritylumak, 0.0f, 6.0f, 32.0f);
    VARP(vccompositedetail, 0, 1, 1);
    FVAR(vcdetailstrength, 0.0f, 0.6f, 1.0f);
    FVAR(vcdetailfrequency, 1.0f, 4.0f, 128.0f);
    FVAR(vcsecondarydetailfrequency, 1.0f, 24.0f, 256.0f);
    FVAR(vcdetailscrollspeed, 0.0f, 0.1f, 1.0f);
    FVAR(vcedgeerosionstrength, 0.0f, 0.6f, 1.0f);
    FVAR(vcinternaldarkeningstrength, 0.0f, 0.6f, 1.0f);
    FVAR(vcsilverliningbreakupstrength, 0.0f, 0.5f, 1.0f);
    FVAR(vcdetailnearscalemultiplier, 1.0f, 3.0f, 4.0f);
    FVAR(vcdetailfadestartdistance, 0.0f, 16384.0f, 1.0e7f);
    FVAR(vcdetailfadeenddistance, 0.0f, 8192.0f, 1.0e7f);
    FVAR(vcdistantdetailscalestart, 0.0f, 32768.0f, 1.0e7f);
    FVAR(vcdistantdetailscaleend, 0.0f, 131072.0f, 1.0e7f);
    FVAR(vcdistantdetailmaxfrequency, 1.0f, 4.0f, 8.0f);
    FVAR(vcfinedetailfadestart, 0.0f, 32768.0f, 1.0e7f);
    FVAR(vcfinedetailfadeend, 0.0f, 131072.0f, 1.0e7f);
    FVAR(vcmediumdetailfadestart, 0.0f, 131072.0f, 1.0e7f);
    FVAR(vcmediumdetailfadeend, 0.0f, 524288.0f, 1.0e7f);
    FVAR(vcdetailmipbias, -4.0f, 0.0f, 4.0f);
    VAR(vccompositedetaildebug, 0, 0, 5);

    // map settings
    VARP(vcmultiscatoctaves, 1, 3, 4);
    FVAR(vcmultiscat, 0.0f, 1.0f, 1.0f);            // a: scattering attenuation per octave
    FVAR(vcmultiscatext, 0.0f, 1.0f, 1.0f);         // b: extinction attenuation per octave
    FVAR(vcmultiscatphase, 0.0f, 0.0f, 1.0f);       // c: phase-angle attenuation per octave
    FVAR(vcphaseg, -0.95f, 0.55f, 0.95f);
    FVAR(vcphaseg2, -0.95f, -0.25f, 0.95f);
    FVAR(vcphaseblend, 0.0f, 0.18f, 1.0f);
    FVARR(vcfogdistmul, 0.25f, 12.0f, 64.0f);
    VARR(vcatmoblendmin, 0, 10, 100);
    VARR(vcatmoblendmax, 0, 100, 100);
    VARR(vcconfigured, 0, 0, 1);
    VARR(vcdensity, 0, 50, 200);
    FVARR(vcalpha, 0.0f, 0.75f, 1.0f);
    VARR(vcheight, -1000, 400, 1000);
    VARR(vcthickness, 0, 50, 300);
    VARR(vcradius, 0, 3000, 10000);                  // 100 = worldsize, 0 disables the cloud layer
    VARR(vcscrollx, -1000, 0, 1000);
    VARR(vcscrolly, -1000, 0, 1000);
    VARR(vcdome, -1000, 300, 1000);
    VARR(vcstructure, 0, 75, 200);                 // 75 = default shaping, lower = macro, higher = micro
    VARR(vcsilverradius, 0, 30, 100);              // radius of the sun mask as % of min screen dimension, 0 disables
    FVARR(vcsilvercontrast, 1.0f, 10.0f, 50.0f);
    FVARR(vcdarkness, 0.1f, 0.5f, 2.0f);
    FVARR(vcshadowstrength, 0.0f, 1.35f, 2.0f);
    CVARR(vccolour, 0xFFFFFF);
    VARR(vcnoisescale, -1000, 0, 1000);

    static float normalizenoise(float n)
    {
        return clamp(n * 0.5f + 0.5f, 0.0f, 1.0f);
    }

    static float cloudnoisehash(int x, int y, int z, int seed)
    {
        float phase = x * 127.1f + y * 311.7f + z * 74.7f + seed;
        float n = sinf(phase) * 43758.5453123f;
        return n - floorf(n);
    }

    static int wrapnoiseindex(int n, int period)
    {
        n %= period;
        return n < 0 ? n + period : n;
    }

    static float sampleperiodicnoise(const float *lattice, float x, float y, float z)
    {
        int ix = int(floorf(x)), iy = int(floorf(y)), iz = int(floorf(z));
        float fx = x - float(ix), fy = y - float(iy), fz = z - float(iz);
        float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy), uz = fz * fz * (3.0f - 2.0f * fz);
        int x0 = wrapnoiseindex(ix, VC_FBM_PERIOD), x1 = wrapnoiseindex(ix + 1, VC_FBM_PERIOD),
            y0 = wrapnoiseindex(iy, VC_FBM_PERIOD), y1 = wrapnoiseindex(iy + 1, VC_FBM_PERIOD),
            z0 = wrapnoiseindex(iz, VC_FBM_PERIOD), z1 = wrapnoiseindex(iz + 1, VC_FBM_PERIOD);
#define VC_FBM_LATTICE(x, y, z) lattice[((z) * VC_FBM_PERIOD + (y)) * VC_FBM_PERIOD + (x)]
        float n000 = VC_FBM_LATTICE(x0, y0, z0), n100 = VC_FBM_LATTICE(x1, y0, z0),
              n010 = VC_FBM_LATTICE(x0, y1, z0), n110 = VC_FBM_LATTICE(x1, y1, z0),
              n001 = VC_FBM_LATTICE(x0, y0, z1), n101 = VC_FBM_LATTICE(x1, y0, z1),
              n011 = VC_FBM_LATTICE(x0, y1, z1), n111 = VC_FBM_LATTICE(x1, y1, z1);
#undef VC_FBM_LATTICE
        float nx00 = n000 + (n100 - n000) * ux, nx10 = n010 + (n110 - n010) * ux,
              nx01 = n001 + (n101 - n001) * ux, nx11 = n011 + (n111 - n011) * ux,
              nxy0 = nx00 + (nx10 - nx00) * uy, nxy1 = nx01 + (nx11 - nx01) * uy;
        return nxy0 + (nxy1 - nxy0) * uz;
    }

    static float sampleperiodicfbm(const float *lattice, float x, float y, float z)
    {
        float value = 0.0f, amplitude = 0.5f;
        loopi(VC_FBM_OCTAVES)
        {
            value += amplitude * sampleperiodicnoise(lattice, x, y, z);
            // Integer lacunarity preserves frequency doubling while keeping the
            // complete FBM exactly periodic.
            x = x * 2.0f + 7.3f;
            y = y * 2.0f + 19.1f;
            z = z * 2.0f + 3.7f;
            amplitude *= 0.5f;
        }
        return value;
    }

    static void cleanupfbmtexture()
    {
        if(vcfbmtex) glDeleteTextures(1, &vcfbmtex);
        vcfbmtex = 0;
        vcfbmtexseed = -1;
        vcfbmtexsize = 0;
    }

    static void ensurefbmtexture()
    {
        int size = clamp(vcfbmresolution, 16, 256);
        if(vcfbmtex && (vcfbmtexseed != vcfbmseed || vcfbmtexsize != size))
        {
            glDeleteTextures(1, &vcfbmtex);
            vcfbmtex = 0;
        }

        if(!vcfbmtex)
        {
            vector<float> lattice;
            float *values = lattice.pad(VC_FBM_PERIOD * VC_FBM_PERIOD * VC_FBM_PERIOD);
            loopk(VC_FBM_PERIOD) loopj(VC_FBM_PERIOD) loopi(VC_FBM_PERIOD)
                values[(k * VC_FBM_PERIOD + j) * VC_FBM_PERIOD + i] = cloudnoisehash(i, j, k, vcfbmseed);

            vector<uchar> pixels;
            uchar *volume = pixels.pad(size * size * size);
            float voxelstep = float(VC_FBM_PERIOD) / float(size);
            loopk(size) loopj(size) loopi(size)
            {
                float x = (float(i) + 0.5f) * voxelstep,
                      y = (float(j) + 0.5f) * voxelstep,
                      z = (float(k) + 0.5f) * voxelstep;
                volume[(k * size + j) * size + i] = uchar(clamp(sampleperiodicfbm(values, x, y, z), 0.0f, 1.0f) * 255.0f + 0.5f);
            }

            glGenTextures(1, &vcfbmtex);
            create3dtexture(vcfbmtex, size, size, size, volume, 0, 2, GL_R8);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glGenerateMipmap_(GL_TEXTURE_3D);
            glBindTexture(GL_TEXTURE_3D, 0);
            vcfbmtexseed = vcfbmseed;
            vcfbmtexsize = size;
        }
    }

    static float noisesizemul()
    {
        return clamp(exp2f(float(vcnoisescale) / 100.0f), 1.0f / 1024.0f, 1024.0f);
    }

    static const float VC_BUDGET_MIN_SCALE = 0.125f, VC_BUDGET_MAX_SCALE = 0.5f;
    static const int VC_BUDGET_MIN_STEPS = 8, VC_BUDGET_MAX_STEPS = 128;
    static bool vcbudgetinitialized = false;
    static float vcbudgetscale = 0.25f, vcbudgetsteps = 32.0f;
    static float vcbudgetfilteredms = -1.0f, vcbudgetlasttarget = -1.0f;
    static int vcbudgetlastadjust = 0;
    static float vceffectivescale = 0.25f;
    static int vceffectivesteps = 32;

    static void preparebudgetsettings()
    {
        if(vcbudget <= 0.0f)
        {
            vcbudgetinitialized = false;
            vcbudgetfilteredms = -1.0f;
            vcbudgetlasttarget = vcbudget;
            vceffectivescale = clamp(vcscale, VC_BUDGET_MIN_SCALE, VC_BUDGET_MAX_SCALE);
            vceffectivesteps = clamp(vcsteps, VC_BUDGET_MIN_STEPS, VC_BUDGET_MAX_STEPS);
            return;
        }

        if(!vcbudgetinitialized)
        {
            vcbudgetscale = clamp(vcscale, VC_BUDGET_MIN_SCALE, VC_BUDGET_MAX_SCALE);
            vcbudgetsteps = float(clamp(vcsteps, VC_BUDGET_MIN_STEPS, VC_BUDGET_MAX_STEPS));
            vcbudgetfilteredms = -1.0f;
            vcbudgetlastadjust = getclockmillis();
            vcbudgetinitialized = true;
        }
        if(vcbudgetlasttarget != vcbudget)
        {
            vcbudgetfilteredms = -1.0f;
            vcbudgetlasttarget = vcbudget;
        }

        // Quantize scale to avoid reallocating every render target for tiny feedback changes.
        vceffectivescale = clamp(floorf(vcbudgetscale * 64.0f + 0.5f) / 64.0f,
                                     VC_BUDGET_MIN_SCALE, VC_BUDGET_MAX_SCALE);
        vceffectivesteps = clamp(int(floorf(vcbudgetsteps + 0.5f)), VC_BUDGET_MIN_STEPS, VC_BUDGET_MAX_STEPS);
    }

    static void updatebudgetcontroller(float measuredms)
    {
        if(vcbudget <= 0.0f || !vcbudgetinitialized || !(measuredms > 0.0f)) return;

        vcbudgetfilteredms = vcbudgetfilteredms > 0.0f ? vcbudgetfilteredms + (measuredms - vcbudgetfilteredms) * 0.20f : measuredms;
        int now = getclockmillis();
        if(now - vcbudgetlastadjust < 250) return;
        vcbudgetlastadjust = now;

        float ratio = vcbudget / max(vcbudgetfilteredms, 1.0e-3f);
        if(ratio >= 0.92f && ratio <= 1.08f) return;

        // Move scale and primary samples together with a conservative exponent
        // so delayed GPU queries do not turn a transient spike into a large
        // quality swing. Nested sun samples remain entirely user-controlled.
        float factor = powf(clamp(ratio, 0.25f, 4.0f), 0.12f);
        vcbudgetscale = clamp(vcbudgetscale * factor, VC_BUDGET_MIN_SCALE, VC_BUDGET_MAX_SCALE);
        vcbudgetsteps = clamp(vcbudgetsteps * factor, float(VC_BUDGET_MIN_STEPS), float(VC_BUDGET_MAX_STEPS));
    }

    static void polldebugtimer()
    {
        loopi(VC_DEBUG_QUERY_COUNT) if(vcdebugtimestampwaiting&(1<<i))
        {
            GLint available = 0;
            glGetQueryObjectiv_(vcdebugtimestampquery[i][1], GL_QUERY_RESULT_AVAILABLE, &available);
            if(!available) continue;

            GLuint64EXT totalstart = 0, totalend = 0;
            glGetQueryObjectui64v_(vcdebugtimestampquery[i][0], GL_QUERY_RESULT, &totalstart);
            glGetQueryObjectui64v_(vcdebugtimestampquery[i][1], GL_QUERY_RESULT, &totalend);
            vcdebugms = max(float(totalend - totalstart) * 1.0e-6f, 0.0f);
            updatebudgetcontroller(vcdebugms);
            loopj(VC_DEBUG_PASS_COUNT) if(vcdebugpassmask[i]&(1<<j))
            {
                GLuint64EXT passstart = 0, passend = 0;
                glGetQueryObjectui64v_(vcdebugtimestampquery[i][2 + 2*j], GL_QUERY_RESULT, &passstart);
                glGetQueryObjectui64v_(vcdebugtimestampquery[i][2 + 2*j + 1], GL_QUERY_RESULT, &passend);
                vcdebugpassms[j] = max(float(passend - passstart) * 1.0e-6f, 0.0f);
            }
            else vcdebugpassms[j] = 0.0f;
            vcdebugtimestampwaiting &= ~(1<<i);
        }

        if(!vcdebugquery[0]) return;
        loopi(VC_DEBUG_QUERY_COUNT) if(vcdebugquerywaiting&(1<<i))
        {
            GLint available = 0;
            glGetQueryObjectiv_(vcdebugquery[i], GL_QUERY_RESULT_AVAILABLE, &available);
            if(!available) continue;

            GLuint64EXT result = 0;
            glGetQueryObjectui64v_(vcdebugquery[i], GL_QUERY_RESULT, &result);
            vcdebugms = max(float(result) * 1.0e-6f, 0.0f);
            updatebudgetcontroller(vcdebugms);
            vcdebugquerywaiting &= ~(1<<i);
        }
    }

    static void begindebugtimer()
    {
        vcdebuggpuquery = false;
        vcdebugtimestamps = false;
        vcdebugcputimer = false;
        vcdebugtimestampactive = -1;
        if(!debugvc && vcbudget <= 0.0f) return;

        polldebugtimer();
        if(debugvc && hasTQ && glQueryCounter_)
        {
            if(!vcdebugtimestampquery[0][0]) glGenQueries_(VC_DEBUG_QUERY_COUNT * VC_DEBUG_TIMESTAMPS, &vcdebugtimestampquery[0][0]);
            if(!(vcdebugtimestampwaiting&(1<<vcdebugtimestampcycle)))
            {
                vcdebugtimestampactive = vcdebugtimestampcycle;
                vcdebugpassmask[vcdebugtimestampactive] = 0;
                glQueryCounter_(vcdebugtimestampquery[vcdebugtimestampactive][0], GL_TIMESTAMP);
                vcdebugtimestamps = true;
                return;
            }
        }

        if(hasTQ && !deferquery)
        {
            if(!vcdebugquery[0]) glGenQueries_(VC_DEBUG_QUERY_COUNT, vcdebugquery);
            if(!(vcdebugquerywaiting&(1<<vcdebugquerycycle)))
            {
                deferquery++;
                glBeginQuery_(GL_TIME_ELAPSED_EXT, vcdebugquery[vcdebugquerycycle]);
                vcdebuggpuquery = true;
                return;
            }
        }

        // A temporarily unavailable GPU query is not comparable to GPU
        // milliseconds, so only fall back to CPU timing on hardware without TQ.
        if(hasTQ) return;
        vcdebugcpustart = getclockmillis();
        vcdebugcputimer = true;
    }

    static void begindebugpass(VCDebugPass pass)
    {
        if(!vcdebugtimestamps) return;
        vcdebugpassmask[vcdebugtimestampactive] |= 1<<pass;
        glQueryCounter_(vcdebugtimestampquery[vcdebugtimestampactive][2 + 2*pass], GL_TIMESTAMP);
    }

    static void enddebugpass(VCDebugPass pass)
    {
        if(!vcdebugtimestamps) return;
        glQueryCounter_(vcdebugtimestampquery[vcdebugtimestampactive][2 + 2*pass + 1], GL_TIMESTAMP);
    }

    static void enddebugtimer()
    {
        if(!debugvc && vcbudget <= 0.0f) return;
        if(vcdebugtimestamps)
        {
            glQueryCounter_(vcdebugtimestampquery[vcdebugtimestampactive][1], GL_TIMESTAMP);
            vcdebugtimestampwaiting |= 1<<vcdebugtimestampactive;
            vcdebugtimestampcycle = (vcdebugtimestampactive + 1) % VC_DEBUG_QUERY_COUNT;
            vcdebugtimestampactive = -1;
            vcdebugtimestamps = false;
        }
        else if(vcdebuggpuquery)
        {
            glEndQuery_(GL_TIME_ELAPSED_EXT);
            deferquery--;
            vcdebugquerywaiting |= 1<<vcdebugquerycycle;
            vcdebugquerycycle = (vcdebugquerycycle + 1) % VC_DEBUG_QUERY_COUNT;
            vcdebuggpuquery = false;
        }
        else if(vcdebugcputimer)
        {
            vcdebugms = max(float(getclockmillis() - vcdebugcpustart), 0.0f);
            updatebudgetcontroller(vcdebugms);
            vcdebugcputimer = false;
        }
    }

    static void cleanupdebugtimer()
    {
        if(vcdebugquery[0]) glDeleteQueries_(VC_DEBUG_QUERY_COUNT, vcdebugquery);
        if(vcdebugtimestampquery[0][0]) glDeleteQueries_(VC_DEBUG_QUERY_COUNT * VC_DEBUG_TIMESTAMPS, &vcdebugtimestampquery[0][0]);
        memset(vcdebugquery, 0, sizeof(vcdebugquery));
        memset(vcdebugtimestampquery, 0, sizeof(vcdebugtimestampquery));
        vcdebugquerywaiting = 0;
        vcdebugquerycycle = 0;
        vcdebugtimestampwaiting = 0;
        vcdebugtimestampcycle = 0;
        vcdebugtimestampactive = -1;
        memset(vcdebugpassmask, 0, sizeof(vcdebugpassmask));
        vcdebuggpuquery = false;
        vcdebugtimestamps = false;
        vcdebugcputimer = false;
        vcdebugms = -1.0f;
        loopi(VC_DEBUG_PASS_COUNT) vcdebugpassms[i] = -1.0f;
    }

    static uchar weatherbyte(float n)
    {
        return uchar(clamp(n, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    static void setupweathernoise(FastNoiseLite &noise, FastNoiseLite::NoiseType type, float frequency, int octaves, float gain = 0.5f, float lacunarity = 2.0f)
    {
        noise.SetNoiseType(type);
        noise.SetFractalType(FastNoiseLite::FractalType_FBm);
        noise.SetFrequency(frequency);
        noise.SetFractalOctaves(octaves);
        noise.SetFractalGain(gain);
        noise.SetFractalLacunarity(lacunarity);
    }

    static float sampletileablenoise(FastNoiseLite &noise, float x, float y, float tilex, float tiley)
    {
        if(tilex <= 1.0e-4f || tiley <= 1.0e-4f) return noise.GetNoise(x, y);

        float qx = x - floor(x / tilex) * tilex;
        float qy = y - floor(y / tiley) * tiley;
        float fx = qx / tilex;
        float fy = qy / tiley;

        float n00 = noise.GetNoise(qx, qy);
        float n10 = noise.GetNoise(qx - tilex, qy);
        float n01 = noise.GetNoise(qx, qy - tiley);
        float n11 = noise.GetNoise(qx - tilex, qy - tiley);

        float nx0 = n00 + (n10 - n00) * fx;
        float nx1 = n01 + (n11 - n01) * fx;
        return nx0 + (nx1 - nx0) * fy;
    }

    static void generateWeatherMap(int seed, int size, vector<WeatherPixel> &pixels)
    {
        FastNoiseLite coverageNoise(seed);
        FastNoiseLite typeNoise(seed + 101);
        FastNoiseLite moistureNoise(seed + 202);
        FastNoiseLite erosionNoise(seed + 303);

        setupweathernoise(coverageNoise, FastNoiseLite::NoiseType_OpenSimplex2, 0.0021f, 4, 0.52f);
        setupweathernoise(typeNoise, FastNoiseLite::NoiseType_OpenSimplex2, 0.00075f, 2, 0.50f);
        setupweathernoise(moistureNoise, FastNoiseLite::NoiseType_Perlin, 0.0045f, 3, 0.50f);
        setupweathernoise(erosionNoise, FastNoiseLite::NoiseType_Perlin, 0.0100f, 4, 0.48f);

        int total = size * size;
        pixels.shrink(0);
        WeatherPixel *weather = pixels.pad(total);
        loopi(size) loopj(size)
        {
            float x = float(j), y = float(i);

            float coverage = normalizenoise(sampletileablenoise(coverageNoise, x, y, float(size), float(size)));
            coverage = clamp(pow(coverage, 1.18f), 0.0f, 1.0f);

            float type = normalizenoise(sampletileablenoise(typeNoise, x, y, float(size), float(size)));
            type = clamp(type * type * (3.0f - 2.0f * type), 0.0f, 1.0f);

            float moisture = normalizenoise(sampletileablenoise(moistureNoise, x, y, float(size), float(size)));
            moisture = clamp(moisture * 0.80f + coverage * 0.20f, 0.0f, 1.0f);

            float erosion0 = normalizenoise(sampletileablenoise(erosionNoise, x, y, float(size), float(size)));
            float erosion1 = normalizenoise(sampletileablenoise(erosionNoise, x * 2.17f + 19.7f, y * 2.17f - 11.3f, float(size) * 2.17f, float(size) * 2.17f));
            float erosion = clamp(pow(erosion0 * 0.78f + erosion1 * 0.22f, 1.05f), 0.0f, 1.0f);

            WeatherPixel &pixel = weather[i * size + j];
            pixel.r = weatherbyte(coverage);
            pixel.g = weatherbyte(type);
            pixel.b = weatherbyte(moisture);
            pixel.a = weatherbyte(erosion);
        }
    }

    static void saveWeatherMapDebug(vector<WeatherPixel> &pixels, int size, int seed)
    {
        string dir;
        copystring(dir, "screenshot/");
        const char *outdir = findfile(dir, "w");
        if(!fileexists(outdir, "w")) createdir(outdir);

        string filename;
        formatstring(filename, "screenshot/volumetricclouds_weather_%d.png", seed);
        path(filename);

        ImageData image(size, size, 4, reinterpret_cast<uchar *>(pixels.getbuf()));
        savepng(filename, image, true);
        conoutf(CON_DEBUG, "saved volumetric cloud weather map to %s", filename);
    }

    static void cleanupweathermap()
    {
        if(vcweathertex)
        {
            glDeleteTextures(1, &vcweathertex);
            vcweathertex = 0;
        }
        vcweatherdirty = true;
    }

    static bool regenerateWeatherMap(int seed, bool saveDebug)
    {
        vector<WeatherPixel> pixels;
        generateWeatherMap(seed, VC_WEATHER_MAP_SIZE, pixels);
        if(saveDebug) saveWeatherMapDebug(pixels, VC_WEATHER_MAP_SIZE, seed);

        if(!vcweathertex) glGenTextures(1, &vcweathertex);
        createtexture(
            vcweathertex,
            VC_WEATHER_MAP_SIZE,
            VC_WEATHER_MAP_SIZE,
            pixels.getbuf(),
            0,
            2,
            GL_RGBA8,
            GL_TEXTURE_2D,
            0,
            0,
            0,
            false
        );

        vcweatherseed = seed;
        vcweatherdirty = false;
        return true;
    }

    static bool ensureWeatherMap()
    {
        if(vcweathertex && !vcweatherdirty) return true;
        return regenerateWeatherMap(vcweatherseed, false);
    }

    static float cloudlayerradius()
    {
        return max(float(worldsize), 1.0f) * max(float(vcradius), 0.0f) / 100.0f;
    }

    static void calcatmosphereparams(vec4 &opticaldepthparams, vec &sunweight, vec &mieparams, vec &betarayleigh, vec &betamie, vec &betaozone, vec4 &sunlightparams)
    {
        if(!atmo || atmoalpha <= 1.0e-4f)
        {
            opticaldepthparams = vec4(0, 0, 0, 1);
            sunweight = mieparams = betarayleigh = betamie = betaozone = vec(0, 0, 0);
            sunlightparams = vec4(0, 0, 0, 0);
            return;
        }

        const float earthradius = 6371e3f, earthairheight = 8.4e3f, earthhazeheight = 1.25e3f, earthozoneheight = 50e3f;
        float planetradius = earthradius * atmoplanetsize;
        vec atmoshells = vec(earthairheight, earthhazeheight, earthozoneheight).mul(atmoheight).add(planetradius).square().sub(planetradius * planetradius);
        opticaldepthparams = vec4(atmoshells, planetradius);

        float gm = max(0.95f - 0.2f * atmohaze, 0.65f);
        float miescale = pow((1 - gm) * (1 - gm) / (4 * M_PI), -2.0f / 3.0f);
        mieparams = vec(miescale * (1 + gm * gm), miescale * -2 * gm, 1.0f);

        static const vec lambda(680e-9f, 550e-9f, 450e-9f),
                         k(0.686f, 0.678f, 0.666f),
                         ozone(3.426f, 8.298f, 0.356f);
        betarayleigh = vec(lambda).square().square().recip().mul(1.241e-30f / M_LN2 * atmodensity);
        betamie = vec(lambda).recip().square().mul(k).mul(9.072e-17f / M_LN2 * atmohaze);
        betaozone = vec(ozone).mul(1.5e-7f / M_LN2 * atmoozone);

        vec sdir = sunlightdir;
        float slen = sdir.magnitude();
        if(slen > 1.0e-4f) sdir.div(slen);
        else sdir = vec(0, 0, 1);

        float sunoffset = sdir.z * planetradius;
        vec sundepth = vec(atmoshells).add(sunoffset * sunoffset).sqrt().sub(sunoffset);
        sunweight = vec(betarayleigh).mul(sundepth.x).madd(betamie, sundepth.y).madd(betaozone, sundepth.z - sundepth.x);
        vec sunextinction = vec(sunweight).neg().exp2();
        vec suncolor = !atmosunlight.iszero() ? atmosunlight.tocolor().mul(max(atmosunlightscale, 0.0f)) : sunlight.tocolor().mul(max(sunlightscale, 0.0f));
        vec sunscale = vec(suncolor).mul(ldrscale).pow(hdrgamma).mul(atmobright * 16).mul(sunextinction);
        float maxsunweight = max(max(sunweight.x, sunweight.y), sunweight.z);
        if(maxsunweight > 127) sunweight.mul(127 / maxsunweight);
        sunweight.add(1e-4f);
        sunlightparams = vec4(sunscale, atmoalpha);
    }

    static void calcshadowparams(vec4 &bounds, vec4 &dome)
    {
        float ws = max(float(worldsize), 1.0f);
        float cloudmid = ws * (vcheight / 100.0f);
        float halfthickness = 0.5f * ws * (vcthickness / 100.0f);
        float base = cloudmid - halfthickness;
        float top = cloudmid + halfthickness;
        if(top <= base + 1.0f) top = base + 1.0f;

        float maxclouddist = max(cloudlayerradius(), 0.0f);
        float domek = maxclouddist > 0.0f ? -float(vcdome) * (ws / max(maxclouddist * maxclouddist, 1.0f)) / 100.0f : 0.0f;

        bounds = vec4(base, top, maxclouddist, lastmillis / 1000.0f);
        dome = vec4(domek, camera1->o.x, camera1->o.y, maxclouddist);
    }

    static vec2 calcshadowmapcenter(float cloudmidz, float domek, const vec &sdir)
    {
        if(sdir.z <= 1.0e-4f) return vec2(camera1->o.x, camera1->o.y);

        float testimate = (cloudmidz - camera1->o.z) / sdir.z;
        float t = testimate;
        float a = -domek * (sdir.x * sdir.x + sdir.y * sdir.y);
        float b = sdir.z;
        float c = camera1->o.z - cloudmidz;
        if(fabsf(a) > 1.0e-8f)
        {
            float disc = b * b - 4.0f * a * c;
            if(disc >= 0.0f)
            {
                float s = sqrtf(max(disc, 0.0f));
                float q = -0.5f * (b + (b < 0.0f ? -s : s));
                float root0 = q / a;
                float root1 = fabsf(q) > 1.0e-8f ? c / q : (-b + s) / (2.0f * a);
                t = fabsf(root1 - testimate) < fabsf(root0 - testimate) ? root1 : root0;
            }
        }
        return vec2(camera1->o.x + sdir.x * t, camera1->o.y + sdir.y * t);
    }

    static float getsilverfovscale()
    {
        static const float referencefov = 100.0f;
        return clamp(tanf(0.5f * referencefov * RAD) / max(tanf(0.5f * curfov * RAD), 1.0e-4f), 0.25f, 8.0f);
    }

    static void updatecloudscroll()
    {
        if(vcscrolllastmillis < 0)
        {
            vcscrolllastmillis = lastmillis;
            return;
        }

        int deltamillis = max(lastmillis - vcscrolllastmillis, 0);
        vcscrolllastmillis = lastmillis;
        if(!deltamillis) return;

        float deltaseconds = deltamillis / 1000.0f;
        vcscrolloffset.madd(vec2(float(vcscrollx), float(vcscrolly)), deltaseconds);
    }

    struct CloudSunParams
    {
        vec direction, tint, ambientTint, directTint, silverWarm, phase, multiscatParams;
        vec4 silver0, silver1, multiscatExtScale, sunExtScale;
        float strength, ambientBoost, shadowHorizonFade;
    };

    static const CloudSunParams &calccloudsunparams()
    {
        static CloudSunParams params;
        static vec cacheddirection, cachedcolor, cachedphase;
        static vec4 cachedmultiscat;
        static float cachedcolorscale = 0.0f;
        static bool valid = false;

        vec sourcecolor(float(sunlight.x), float(sunlight.y), float(sunlight.z));
        vec phase(vcphaseg, vcphaseg2, vcphaseblend);
        vec4 multiscat(vcmultiscat, vcmultiscatext, vcmultiscatphase, float(vcmultiscatoctaves));
        float colorscale = 2.0f * ldrscaleb * sunlightscale;
        if(valid && cacheddirection.x == sunlightdir.x && cacheddirection.y == sunlightdir.y && cacheddirection.z == sunlightdir.z &&
           cachedcolor.x == sourcecolor.x && cachedcolor.y == sourcecolor.y && cachedcolor.z == sourcecolor.z &&
           cachedphase.x == phase.x && cachedphase.y == phase.y && cachedphase.z == phase.z &&
           cachedmultiscat.x == multiscat.x && cachedmultiscat.y == multiscat.y && cachedmultiscat.z == multiscat.z &&
           cachedmultiscat.w == multiscat.w && cachedcolorscale == colorscale) return params;

        valid = true;
        cacheddirection = sunlightdir;
        cachedcolor = sourcecolor;
        cachedphase = phase;
        cachedmultiscat = multiscat;
        cachedcolorscale = colorscale;

        params.direction = cacheddirection;
        float sundirlen = params.direction.magnitude();
        if(sundirlen > 1.0e-4f) params.direction.div(sundirlen);
        else params.direction.div(1.0e-4f);

        vec suncolor(max(cachedcolor.x * colorscale, 0.0f), max(cachedcolor.y * colorscale, 0.0f), max(cachedcolor.z * colorscale, 0.0f));
        float sunscale = max(max(suncolor.x, suncolor.y), suncolor.z);
        float sunlum = suncolor.dot(vec(0.299f, 0.587f, 0.114f));
        params.tint = sunscale > 1.0e-4f ? vec(suncolor).div(sunscale) : vec(1, 1, 1);
        params.strength = max(sunlum, 0.85f * sunscale);

        float sunup = clamp(params.direction.z * 0.5f + 0.5f, 0.0f, 1.0f);
        float sunheight = clamp(params.direction.z, 0.0f, 1.0f);
        float ambientmix = 0.18f + 0.12f * sunup;
        params.ambientBoost = 0.03f + 0.09f * sunup;
        params.ambientTint = vec(1.0f + (params.tint.x - 1.0f) * ambientmix, 1.0f + (params.tint.y - 1.0f) * ambientmix,
                                 1.0f + (params.tint.z - 1.0f) * ambientmix);
        params.directTint = vec(0.25f + 0.75f * params.tint.x, 0.25f + 0.75f * params.tint.y, 0.25f + 0.75f * params.tint.z);
        params.silverWarm = vec(0.35f + 0.65f * params.tint.x * 1.15f, 0.35f + 0.65f * params.tint.y * 1.15f,
                                0.35f + 0.65f * params.tint.z * 1.15f);
        params.silver0 = vec4(2.5f + 4.5f * sunheight, 1.18f - 0.18f * sunheight, 0.34f - 0.10f * sunheight,
                              0.54f - 0.14f * sunheight);
        params.silver1 = vec4(0.94f - 0.10f * sunheight, 0.28f - 0.08f * sunheight, 0.46f - 0.12f * sunheight,
                              0.90f - 0.10f * sunheight);

        float multiscatext = clamp(cachedmultiscat.y, 0.0f, 1.0f);
        float multiscatscatter = clamp(cachedmultiscat.x, 0.0f, multiscatext);
        float multiscatphase = clamp(cachedmultiscat.z, 0.0f, 1.0f);
        int configuredoctaves = clamp(int(cachedmultiscat.w), 1, 4), activeoctaves = 1;
        float contribution = multiscatscatter;
        for(int i = 1; i < 4; ++i)
        {
            if(i >= configuredoctaves || contribution <= 1.0e-5f) break;
            activeoctaves++;
            contribution *= multiscatscatter;
        }
        float multiscatext2 = multiscatext * multiscatext;
        params.multiscatExtScale = vec4(1.0f, multiscatext, multiscatext2, multiscatext2 * multiscatext);
        params.sunExtScale = vec4(params.multiscatExtScale.x, activeoctaves > 1 ? params.multiscatExtScale.y : 0.0f,
                                  activeoctaves > 2 ? params.multiscatExtScale.z : 0.0f,
                                  activeoctaves > 3 ? params.multiscatExtScale.w : 0.0f);
        params.multiscatParams = vec(multiscatscatter, multiscatphase, float(activeoctaves));
        params.phase = vec(clamp(cachedphase.x, -0.95f, 0.95f), clamp(cachedphase.y, -0.95f, 0.95f),
                           clamp(cachedphase.z, 0.0f, 1.0f));

        float horizonfade = clamp((params.direction.z - 0.08f) / 0.12f, 0.0f, 1.0f);
        params.shadowHorizonFade = horizonfade * horizonfade * (3.0f - 2.0f * horizonfade);
        return params;
    }

    static vec4 calcsilverscreenparams(const vec &sdir)
    {
        if(vcsilverradius <= 0 || sunlight.iszero() || sunlightscale <= 1.0e-4f) return vec4(0, 0, 0, 0);

        vec sunpoint(camera1->o);
        sunpoint.madd(sdir, max(nearplane * 4.0f, 1.0f));

        vec4 sunclip;
        camprojmatrix.transform(sunpoint, sunclip);
        if(sunclip.w <= 1.0e-4f || sunclip.z < -sunclip.w) return vec4(0, 0, 0, 0);

        vec2 sunndc(sunclip.x / sunclip.w, sunclip.y / sunclip.w);
        if(fabsf(sunndc.x) > 1.35f || fabsf(sunndc.y) > 1.35f) return vec4(0, 0, 0, 0);

        float screenedge = max(fabsf(sunndc.x), fabsf(sunndc.y));
        float edgefade = clamp(1.0f - max(screenedge - 0.90f, 0.0f) / 0.40f, 0.0f, 1.0f);
        float horizonfade = clamp((sdir.z - 0.02f) / 0.10f, 0.0f, 1.0f);
        float screenfade = edgefade * horizonfade;
        if(screenfade <= 1.0e-4f) return vec4(0, 0, 0, 0);

        float radiuspixels = min(vieww, viewh) * (float(vcsilverradius) / 100.0f) * getsilverfovscale();
        if(radiuspixels <= 1.0e-4f) return vec4(0, 0, 0, 0);

        return vec4((sunndc.x * 0.5f + 0.5f) * vieww, (sunndc.y * 0.5f + 0.5f) * viewh, radiuspixels, screenfade);
    }

    struct CloudScissor
    {
        int x1, y1, x2, y2, lowx1, lowy1, lowx2, lowy2;
    };

    static void ndctopixelrect(float sx1, float sy1, float sx2, float sy2, int w, int h, int margin, int &x1, int &y1, int &x2, int &y2)
    {
        x1 = clamp(int(floorf((sx1 * 0.5f + 0.5f) * w)) - margin, 0, w);
        y1 = clamp(int(floorf((sy1 * 0.5f + 0.5f) * h)) - margin, 0, h);
        x2 = clamp(int(ceilf((sx2 * 0.5f + 0.5f) * w)) + margin, 0, w);
        y2 = clamp(int(ceilf((sy2 * 0.5f + 0.5f) * h)) + margin, 0, h);
    }

    static bool calccloudscissor(const vec4 &bounds, const vec4 &dome, int targetw, int targeth, CloudScissor &scissor)
    {
        float radius = max(dome.w, 0.0f);
        if(radius <= 1.0e-4f) return false;

        float edgeoffset = dome.x * radius * radius;
        float minz = min(bounds.x, bounds.y) + min(edgeoffset, 0.0f) - 2.0f,
              maxz = max(bounds.x, bounds.y) + max(edgeoffset, 0.0f) + 2.0f,
              minx = dome.y - radius, maxx = dome.y + radius,
              miny = dome.z - radius, maxy = dome.z + radius;

        const vec &cam = camera1->o;
        if(cam.x >= minx && cam.x <= maxx && cam.y >= miny && cam.y <= maxy && cam.z >= minz && cam.z <= maxz)
        {
            scissor.x1 = scissor.y1 = scissor.lowx1 = scissor.lowy1 = 0;
            scissor.x2 = vieww;
            scissor.y2 = viewh;
            scissor.lowx2 = targetw;
            scissor.lowy2 = targeth;
            return true;
        }

        float sx1, sy1, sx2, sy2;
        if(!calcbbscissor(ivec::floor(vec(minx, miny, minz)), ivec::ceil(vec(maxx, maxy, maxz)), sx1, sy1, sx2, sy2))
            return false;

        ndctopixelrect(sx1, sy1, sx2, sy2, vieww, viewh, 2, scissor.x1, scissor.y1, scissor.x2, scissor.y2);
        ndctopixelrect(sx1, sy1, sx2, sy2, targetw, targeth, 1, scissor.lowx1, scissor.lowy1, scissor.lowx2, scissor.lowy2);
        return scissor.x2 > scissor.x1 && scissor.y2 > scissor.y1 && scissor.lowx2 > scissor.lowx1 && scissor.lowy2 > scissor.lowy1;
    }

    static void setcloudscissor(const CloudScissor &scissor, bool lowres, int margin = 0)
    {
        int x1 = lowres ? scissor.lowx1 : scissor.x1,
            y1 = lowres ? scissor.lowy1 : scissor.y1,
            x2 = lowres ? scissor.lowx2 : scissor.x2,
            y2 = lowres ? scissor.lowy2 : scissor.y2,
            w = lowres ? vcw : vieww,
            h = lowres ? vch : viewh;
        x1 = max(x1 - margin, 0);
        y1 = max(y1 - margin, 0);
        x2 = min(x2 + margin, w);
        y2 = min(y2 + margin, h);
        glScissor(x1, y1, max(x2 - x1, 0), max(y2 - y1, 0));
    }

    static void clearcloudtarget()
    {
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    static void cleanupshadowmap()
    {
        if(vcshadowfbo)
        {
            glDeleteFramebuffers_(1, &vcshadowfbo);
            vcshadowfbo = 0;
        }
        if(vcshadowtex)
        {
            glDeleteTextures(1, &vcshadowtex);
            vcshadowtex = 0;
        }
        vcshadowsz = 0;
        vcshadowmapworld = vec4(0, 0, 1, 0);
        vcshadowmapstrength = 0.0f;
    }

    ICOMMAND(vcregen, "i", (int *seed),
    {
        regenerateWeatherMap(*seed, true);
        conoutf(CON_INFO, "regenerated volumetric cloud weather map with seed %d", *seed);
    });

    ICOMMAND(vcregenfbm, "", (),
    {
        cleanupfbmtexture();
        ensurefbmtexture();
        conoutf(CON_INFO, "regenerated %d^3 volumetric cloud FBM volume with seed %d", vcfbmtexsize, vcfbmseed);
    });

    void initnoise()
    {
        ensurefbmtexture();
    }

    void init()
    {
        if(!volumetricclouds || !vcconfigured) return;
        useshaderbyname("volumetricclouds");
        useshaderbyname("volumetricclouddepth");
        useshaderbyname("atrousfilter");
        useshaderbyname("volumetriccloudsupscale");
        useshaderbyname("volumetriccloudsbilateral");
        useshaderbyname("volumetriccloudclarity");
        useshaderbyname("volumetriccloudcomposite");
        useshaderbyname("volumetriccloudshadowmap");
        useshaderbyname("volumetriccloudshadowapply");
        useshaderbyname("scalelinear");
        godrays::crepuscular::init();
    }

    bool hasshadowmap()
    {
        return volumetricclouds && vcconfigured && vcdensity > 0 && vcradius > 0 && vcshadowtex && vcshadowfbo && vcshadowmapworld.w > 0.0f && vcshadowmapstrength > 1e-4f;
    }

    void getshadowparams(vec4 &bounds, vec4 &dome)
    {
        calcshadowparams(bounds, dome);
    }

    bool bindshadowmap(int tmu)
    {
        if(!hasshadowmap()) return false;

        glActiveTexture_(GL_TEXTURE0 + tmu);
        glBindTexture(GL_TEXTURE_RECTANGLE, vcshadowtex);
        glActiveTexture_(GL_TEXTURE0);
        return true;
    }

    bool bindcomposite(int tmu)
    {
        if(!vccompositetex || vccompositetexparams.z <= 0.0f || vccompositetexparams.w <= 0.0f) return false;

        glActiveTexture_(GL_TEXTURE0 + tmu);
        glBindTexture(GL_TEXTURE_RECTANGLE, vccompositetex);
        glActiveTexture_(GL_TEXTURE0);
        return true;
    }

    const vec4 &compositetexparams()
    {
        return vccompositetexparams;
    }

    const vec4 &shadowmapworld()
    {
        return vcshadowmapworld;
    }

    float shadowmapstrength()
    {
        return vcshadowmapstrength;
    }

    void render()
    {
        vccompositetex = 0;
        vccompositetexparams = vec4(0, 0, 0, 0);
        vcshadowmapstrength = 0.0f;
        updatecloudscroll();
        if(!volumetricclouds || !vcconfigured || vcdensity <= 0 || vcradius <= 0)
        {
            if(vcshadowtex || vcshadowfbo) cleanupshadowmap();
            return;
        }
        preparebudgetsettings();

        const CloudSunParams &cloudsun = calccloudsunparams();
        float shadowstrength = vcshadowstrength * clamp(vcalpha, 0.0f, 1.0f);
        float shadowamount = clamp(shadowstrength * cloudsun.shadowHorizonFade, 0.0f, 1.0f);

        Shader *cloudshader = useshaderbyname("volumetricclouds");
        Shader *depthshader = useshaderbyname("volumetricclouddepth");
        Shader *atrousshader = vcatrous ? useshaderbyname("atrousfilter") : NULL;
        Shader *upscaleshader = useshaderbyname("volumetriccloudsupscale");
        Shader *bilateralshader = useshaderbyname("volumetriccloudsbilateral");
        Shader *clarityshader = vcclarity ? useshaderbyname("volumetriccloudclarity") : NULL;
        Shader *compositeshader = vccompositedetail ? useshaderbyname("volumetriccloudcomposite") : NULL;
        Shader *shadowmapshader = vcshadow && shadowamount > 1.0e-4f ? useshaderbyname("volumetriccloudshadowmap") : NULL;
        Shader *shadowapplyshader = vcshadow && shadowamount > 1.0e-4f ? useshaderbyname("volumetriccloudshadowapply") : NULL;
        bool useclarity = vcclarity && clarityshader && vcclaritystrength > 1e-4f;
        bool doshadow = shadowmapshader && shadowapplyshader;
        if(!cloudshader) return;

        int targetw = max(int(ceilf(vieww * vceffectivescale)), 1),
            targeth = max(int(ceilf(viewh * vceffectivescale)), 1);
        if(targetw != vcw || targeth != vch || vieww != vcfullw || viewh != vcfullh)
        {
            cleanupbuffers();
            vcw = targetw;
            vch = targeth;
            vcfullw = vieww;
            vcfullh = viewh;
        }
        if(!ensureWeatherMap()) return;
        ensurefbmtexture();

        vec4 cloudbounds, clouddome;
        calcshadowparams(cloudbounds, clouddome);

        CloudScissor cloudscissor;
        bool drawclouds = calccloudscissor(cloudbounds, clouddome, vcw, vch, cloudscissor);
        bool usebilateral = drawclouds && vcblur && bilateralshader && depthshader;
        bool useupscale = drawclouds && (vcw < vieww || vch < viewh) && upscaleshader && depthshader;
        bool needdepthcache = usebilateral || useupscale;

        if(!doshadow && (vcshadowtex || vcshadowfbo))
            cleanupshadowmap();
        if(!drawclouds && !doshadow) return;

        if(drawclouds && !vctex)
        {
            glGenTextures(1, &vctex);
            createtexture(vctex, vcw, vch, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(drawclouds && !crsourcetex)
        {
            glGenTextures(1, &crsourcetex);
            createtexture(crsourcetex, vcw, vch, NULL, 3, 1, GL_R8, GL_TEXTURE_RECTANGLE);
        }
        if(needdepthcache && !vcdepthtex)
        {
            glGenTextures(1, &vcdepthtex);
            createtexture(vcdepthtex, vcw, vch, NULL, 3, 0, GL_R32F, GL_TEXTURE_RECTANGLE);
        }
        if(drawclouds && !vcatroustex)
        {
            glGenTextures(1, &vcatroustex);
            createtexture(vcatroustex, vcw, vch, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(drawclouds && !vcbilateraltex)
        {
            glGenTextures(1, &vcbilateraltex);
            createtexture(vcbilateraltex, vieww, viewh, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(drawclouds && !vcbilateraltemptex)
        {
            glGenTextures(1, &vcbilateraltemptex);
            createtexture(vcbilateraltemptex, vieww, viewh, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(drawclouds && useclarity && !vcclaritytex)
        {
            glGenTextures(1, &vcclaritytex);
            createtexture(vcclaritytex, vieww, viewh, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }

        if(drawclouds && !vcfbo)
        {
            glGenFramebuffers_(1, &vcfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vctex, 0);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_RECTANGLE, crsourcetex, 0);
            static const GLenum drawbufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers_(2, drawbufs);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(needdepthcache && !vcdepthfbo)
        {
            glGenFramebuffers_(1, &vcdepthfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcdepthfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcdepthtex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud depth cache!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(drawclouds && !vcatrousfbo)
        {
            glGenFramebuffers_(1, &vcatrousfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcatrousfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcatroustex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud atrous buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(drawclouds && !vcbilateralfbo)
        {
            glGenFramebuffers_(1, &vcbilateralfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateralfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcbilateraltex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud bilateral buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(drawclouds && !vcbilateraltempfbo)
        {
            glGenFramebuffers_(1, &vcbilateraltempfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateraltempfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcbilateraltemptex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud bilateral temp buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(drawclouds && useclarity && !vcclarityfbo)
        {
            glGenFramebuffers_(1, &vcclarityfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcclarityfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcclaritytex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud clarity buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(doshadow)
        {
            int shadowsz = max(vcshadowmapsize, 1);
            if(shadowsz != vcshadowsz) cleanupshadowmap();
            if(!vcshadowtex)
            {
                vcshadowsz = shadowsz;
                glGenTextures(1, &vcshadowtex);
                createtexture(vcshadowtex, vcshadowsz, vcshadowsz, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
            }
            if(!vcshadowfbo)
            {
                glGenFramebuffers_(1, &vcshadowfbo);
                glBindFramebuffer_(GL_FRAMEBUFFER, vcshadowfbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcshadowtex, 0);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud shadow map buffer!");
                glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
            }
        }

        glActiveTexture_(GL_TEXTURE8);
        if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msnormaltex);
        else glBindTexture(GL_TEXTURE_RECTANGLE, gnormaltex);
        glActiveTexture_(GL_TEXTURE9);
        if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
        else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
        glActiveTexture_(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_3D, vcfbmtex);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vcweathertex);

        GLOBALPARAMF(tvcloudbounds, cloudbounds.x, cloudbounds.y, cloudbounds.z, cloudbounds.w);
        GLOBALPARAM(tvcloudinvcamprojmatrix, invcamprojmatrix);
        GLOBALPARAMF(tvclouddome, clouddome.x, clouddome.y, clouddome.z, clouddome.w);
        GLOBALPARAMF(tvcloudscroll, vcscrolloffset.x, vcscrolloffset.y);
        float ws = max(float(worldsize), 1.0f);
        float noisemul = noisesizemul();
        float basewavelength = max(ws * 0.30f * noisemul, 1.0f);
        float basenoisescale = 1.0f / basewavelength;
        float detailnoisescale = 1.0f / max(ws * 0.12f * noisemul, 1.0f);
        float fbmtexelsperunit = float(vcfbmtexsize) / float(VC_FBM_PERIOD);
        float baselodoffset = logf(max(basenoisescale * fbmtexelsperunit, 1.0e-20f)) / M_LN2;
        float macrolodoffset = logf(max(basenoisescale * 0.22f * fbmtexelsperunit, 1.0e-20f)) / M_LN2;
        float detaillodoffset = logf(max(detailnoisescale * fbmtexelsperunit, 1.0e-20f)) / M_LN2;
        float lightlodoffset = logf(max(basenoisescale * 0.35f * fbmtexelsperunit, 1.0e-20f)) / M_LN2;
        float silverlodoffset = logf(max(basenoisescale * 0.55f * fbmtexelsperunit, 1.0e-20f)) / M_LN2;
        GLOBALPARAMF(tvcloudnoise, basenoisescale, detailnoisescale, 0.50f, 0.95f);
        GLOBALPARAMF(tvcloudfbmparams, 1.0f / float(VC_FBM_PERIOD), fbmtexelsperunit, logf(float(vcfbmtexsize)) / M_LN2);
        GLOBALPARAMF(tvcloudlodoffsets, baselodoffset, macrolodoffset, detaillodoffset, lightlodoffset);
        GLOBALPARAMF(tvcloudsilverlodoffset, silverlodoffset);
        GLOBALPARAMF(tvcloudstructure, float(vcstructure) / 100.0f);
        GLOBALPARAMF(tvcloudscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
        GLOBALPARAMF(vclouddensity, float(vcdensity) / 100.0f);
        GLOBALPARAMF(vcloudalpha, vcalpha);
        GLOBALPARAMF(vcloudthickness, vcdarkness);
        GLOBALPARAMF(tvcloudsundir, cloudsun.direction.x, cloudsun.direction.y, cloudsun.direction.z, fabsf(cloudsun.direction.z));
        GLOBALPARAMF(tvcloudsuntintstrength, cloudsun.tint.x, cloudsun.tint.y, cloudsun.tint.z, cloudsun.strength);
        GLOBALPARAM(tvcloudambienttint, cloudsun.ambientTint);
        GLOBALPARAMF(tvcloudambientboost, cloudsun.ambientBoost);
        GLOBALPARAM(tvclouddirecttint, cloudsun.directTint);
        GLOBALPARAM(tvcloudsilverwarm, cloudsun.silverWarm);
        GLOBALPARAM(tvcloudphaseparams, cloudsun.phase);
        GLOBALPARAMF(tvcloudmultiscatparams, cloudsun.multiscatParams.x, cloudsun.multiscatParams.y, cloudsun.multiscatParams.z);
        GLOBALPARAMF(tvcloudmultiscatextscale, cloudsun.multiscatExtScale.x, cloudsun.multiscatExtScale.y,
                     cloudsun.multiscatExtScale.z, cloudsun.multiscatExtScale.w);
        GLOBALPARAMF(tvcloudsunextscale, cloudsun.sunExtScale.x, cloudsun.sunExtScale.y, cloudsun.sunExtScale.z, cloudsun.sunExtScale.w);
        GLOBALPARAMF(tvcloudsilverparams0, cloudsun.silver0.x, cloudsun.silver0.y, cloudsun.silver0.z, cloudsun.silver0.w);
        GLOBALPARAMF(tvcloudsilverparams1, cloudsun.silver1.x, cloudsun.silver1.y, cloudsun.silver1.z, cloudsun.silver1.w);
        GLOBALPARAMF(tvcloudfogdistmul, max(vcfogdistmul, 1.0e-3f));
        GLOBALPARAMF(tvcloudatmoblend, float(vcatmoblendmin) / 100.0f, float(vcatmoblendmax) / 100.0f);
        vec4 silverscreen = calcsilverscreenparams(cloudsun.direction);
        GLOBALPARAMF(tvcloudsilvermask, silverscreen.x, silverscreen.y, silverscreen.z, silverscreen.w);
        GLOBALPARAMF(tvcloudcrmask, godrays::crepuscular::enabled() ? 1.0f : 0.0f);
        GLOBALPARAMF(tvcloudsilvercontrast, max(vcsilvercontrast, 1.0f));
        GLOBALPARAMF(tvcloudsteps, float(vceffectivesteps));
        float cloudthickness = max(cloudbounds.y - cloudbounds.x, 1.0f);
        float automaxviewstep = min(min(cloudthickness / 3.0f, basewavelength / 3.0f), ws / 8.0f);
        float maxviewstep = max(vcmaxviewstep > 0.0f ? vcmaxviewstep : automaxviewstep, 1.0e-3f);
        float nearviewstep = min(maxviewstep, min(min(cloudthickness / 24.0f, basewavelength / 16.0f), ws / 128.0f));
        float mediumviewstep = min(maxviewstep, min(min(cloudthickness / 8.0f, basewavelength / 8.0f), ws / 32.0f));
        // When the primary step budget falls, relax the world-space subdivision
        // caps too; otherwise these inner subdivisions can defeat the controller.
        float budgetsteprelax = vcbudget > 0.0f ? float(VC_BUDGET_MAX_STEPS) / float(vceffectivesteps) : 1.0f;
        nearviewstep *= budgetsteprelax;
        mediumviewstep *= budgetsteprelax;
        maxviewstep *= budgetsteprelax;
        GLOBALPARAMF(tvcloudviewsteps, max(nearviewstep, 1.0e-3f), max(mediumviewstep, 1.0e-3f), maxviewstep, ws);
        GLOBALPARAMF(tvcloudsunsteps, float(vcsunsteps));
        GLOBALPARAM(vcloudcolour, vccolour.tocolor());
        vec4 atmoopticaldepthparams, atmosunlightparams;
        vec atmosunweight, atmomieparams, atmobetarayleigh, atmobetamie, atmobetaozone;
        calcatmosphereparams(atmoopticaldepthparams, atmosunweight, atmomieparams, atmobetarayleigh, atmobetamie, atmobetaozone, atmosunlightparams);
        GLOBALPARAMF(vcloudatmoopticaldepthparams, atmoopticaldepthparams.x, atmoopticaldepthparams.y, atmoopticaldepthparams.z, atmoopticaldepthparams.w);
        GLOBALPARAMF(vcloudatmosunlight, atmosunlightparams.x, atmosunlightparams.y, atmosunlightparams.z, atmosunlightparams.w);
        GLOBALPARAMF(vcloudatmosunweight, atmosunweight.x, atmosunweight.y, atmosunweight.z);
        GLOBALPARAMF(vcloudatmomieparams, atmomieparams.x, atmomieparams.y, atmomieparams.z);
        GLOBALPARAMF(vcloudatmobetarayleigh, atmobetarayleigh.x, atmobetarayleigh.y, atmobetarayleigh.z);
        GLOBALPARAMF(vcloudatmobetamie, atmobetamie.x, atmobetamie.y, atmobetamie.z);
        GLOBALPARAMF(vcloudatmobetaozone, atmobetaozone.x, atmobetaozone.y, atmobetaozone.z);

        begindebugtimer();

        GLuint compositetex = 0;
        int compositetexw = 0, compositetexh = 0;
        glDisable(GL_DEPTH_TEST);

        if(drawclouds)
        {
            if(needdepthcache)
            {
                begindebugpass(VC_DEBUG_DEPTH_CACHE);
                glBindFramebuffer_(GL_FRAMEBUFFER, vcdepthfbo);
                glViewport(0, 0, vcw, vch);
                glDisable(GL_BLEND);
                glDisable(GL_SCISSOR_TEST);
                depthshader->set();
                screenquad(vcw, vch);
                glActiveTexture_(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_RECTANGLE, vcdepthtex);
                glActiveTexture_(GL_TEXTURE0);
                GLOBALPARAMF(tvclouddepthscale, float(vcw)/vieww, float(vch)/viewh);
                enddebugpass(VC_DEBUG_DEPTH_CACHE);
            }

            begindebugpass(VC_DEBUG_RAYMARCH);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcfbo);
            glViewport(0, 0, vcw, vch);
            glDisable(GL_BLEND);
            clearcloudtarget();
            glEnable(GL_SCISSOR_TEST);
            setcloudscissor(cloudscissor, true);
            int multiscatoctaves = clamp(int(cloudsun.multiscatParams.z), 1, 4);
            // The base program is one lane; variant columns 0..2 are two through four lanes.
            if(multiscatoctaves > 1) cloudshader->setvariant(multiscatoctaves - 2, 0);
            else cloudshader->set();
            screenquad(vcw, vch);
            enddebugpass(VC_DEBUG_RAYMARCH);

            GLuint lowrestex = vctex;
            if(vcatrous && atrousshader)
            {
                begindebugpass(VC_DEBUG_ATROUS);
                int iterations = clamp(vcatrousiter, 1, 3);
                loopi(iterations)
                {
                    bool writetomain = lowrestex == vcatroustex;
                    glBindFramebuffer_(GL_FRAMEBUFFER, writetomain ? vcfbo : vcatrousfbo);
                    glViewport(0, 0, vcw, vch);
                    glDisable(GL_BLEND);
                    clearcloudtarget();
                    glEnable(GL_SCISSOR_TEST);
                    setcloudscissor(cloudscissor, true, 2 * ((1<<(i + 1)) - 1));
                    glActiveTexture_(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_RECTANGLE, lowrestex);
                    GLOBALPARAMF(tatroussize, float(vcw), float(vch));
                    GLOBALPARAMF(tatrousparams, float(1<<i), vcatrousalphak, 0.0f, 0.0f);
                    atrousshader->set();
                    screenquad(vcw, vch);
                    lowrestex = writetomain ? vctex : vcatroustex;
                }
                enddebugpass(VC_DEBUG_ATROUS);
            }

            compositetex = lowrestex;
            compositetexw = vcw;
            compositetexh = vch;

            if(usebilateral)
            {
                begindebugpass(VC_DEBUG_BILATERAL);
                GLOBALPARAMF(tvbilateraledge, vcbilateraledge);
                GLOBALPARAMF(vcblurscale, float(vcblurscale));

                // Pass 1: horizontal bilateral blur + upscale from low-res cloud buffer.
                glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateraltempfbo);
                glViewport(0, 0, vieww, viewh);
                glDisable(GL_BLEND);
                clearcloudtarget();
                glEnable(GL_SCISSOR_TEST);
                setcloudscissor(cloudscissor, false, int(ceilf(max(float(vcblurscale), 1.0f) * 4.0f)) + 2);

                glActiveTexture_(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_RECTANGLE, lowrestex);
                GLOBALPARAMF(tvcloudscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
                GLOBALPARAMF(tvcloudblurdir, 1.0f, 0.0f);
                bilateralshader->set();
                screenquad(vieww, viewh);

                // Pass 2: vertical bilateral blur on full-res intermediate.
                glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateralfbo);
                glViewport(0, 0, vieww, viewh);
                glDisable(GL_BLEND);
                clearcloudtarget();
                glEnable(GL_SCISSOR_TEST);
                setcloudscissor(cloudscissor, false, int(ceilf(max(float(vcblurscale), 1.0f) * 8.0f)) + 2);

                glActiveTexture_(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_RECTANGLE, vcbilateraltemptex);
                GLOBALPARAMF(tvcloudscale, 1.0f, 1.0f, 1.0f, 1.0f);
                GLOBALPARAMF(tvcloudblurdir, 0.0f, 1.0f);
                bilateralshader->set();
                screenquad(vieww, viewh);

                compositetex = vcbilateraltex;
                compositetexw = vieww;
                compositetexh = viewh;
                enddebugpass(VC_DEBUG_BILATERAL);
            }
            else if(useupscale)
            {
                begindebugpass(VC_DEBUG_UPSCALE);
                // Depth-aware upsample to avoid low-res cloud alpha bleeding over
                // foreground geometry silhouettes when vcscale < 1.
                GLOBALPARAMF(tvbilateraledge, vcbilateraledge);

                glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateralfbo);
                glViewport(0, 0, vieww, viewh);
                glDisable(GL_BLEND);
                clearcloudtarget();
                glEnable(GL_SCISSOR_TEST);
                setcloudscissor(cloudscissor, false, 2);

                glActiveTexture_(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_RECTANGLE, lowrestex);
                GLOBALPARAMF(tvcloudscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
                upscaleshader->set();
                screenquad(vieww, viewh);

                compositetex = vcbilateraltex;
                compositetexw = vieww;
                compositetexh = viewh;
                enddebugpass(VC_DEBUG_UPSCALE);
            }

            // Full-res edge-aware clarity pass. Only run after the cloud chain has
            // produced a full-resolution working texture; never sharpen low-res RTs.
            if(useclarity && compositetexw == vieww && compositetexh == viewh)
            {
                begindebugpass(VC_DEBUG_CLARITY);
                glBindFramebuffer_(GL_FRAMEBUFFER, vcclarityfbo);
                glViewport(0, 0, vieww, viewh);
                glDisable(GL_BLEND);
                clearcloudtarget();
                glEnable(GL_SCISSOR_TEST);
                setcloudscissor(cloudscissor, false, int(ceilf(max(vcclarityradius, 0.5f))) + 2);

                glActiveTexture_(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);
                GLOBALPARAMF(vcclarityparams, vcclaritystrength, vcclarityradius, vcclarityalphak, vcclaritylumak);
                clarityshader->set();
                screenquad(vieww, viewh);

                compositetex = vcclaritytex;
                compositetexw = vieww;
                compositetexh = viewh;
                enddebugpass(VC_DEBUG_CLARITY);
            }

            glDisable(GL_SCISSOR_TEST);
            vccompositetex = compositetex;
            vccompositetexparams = vec4(
                float(compositetexw) / max(float(vieww), 1.0f),
                float(compositetexh) / max(float(viewh), 1.0f),
                float(compositetexw),
                float(compositetexh)
            );
        }

        if(doshadow && vcshadowtex && vcshadowfbo)
        {
            float ws = max(float(worldsize), 1.0f);
            float shadowradius = max(ws * float(vcshadowradius) / 100.0f, 1.0f);
            float shadowworld = shadowradius * 2.0f;
            float worldpertexel = shadowworld / max(float(vcshadowsz), 1.0f);
            float cloudmidz = 0.5f * (cloudbounds.x + cloudbounds.y);
            vec2 shadowcenter = calcshadowmapcenter(cloudmidz, clouddome.x, cloudsun.direction);
            float snappedx = floorf(shadowcenter.x / worldpertexel) * worldpertexel;
            float snappedy = floorf(shadowcenter.y / worldpertexel) * worldpertexel;
            float minx = snappedx - shadowworld * 0.5f;
            float miny = snappedy - shadowworld * 0.5f;
            vcshadowmapworld = vec4(minx, miny, worldpertexel, float(vcshadowsz));
            vcshadowmapstrength = shadowamount;

            GLOBALPARAMF(tvshadowmapworld, minx, miny, worldpertexel, float(vcshadowsz));
            GLOBALPARAMF(tvcloudshadowsamples, float(vcshadowsamples));

            begindebugpass(VC_DEBUG_SHADOW_MAP);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcshadowfbo);
            glViewport(0, 0, vcshadowsz, vcshadowsz);
            glDisable(GL_BLEND);
            glClearColor(1, 1, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, vcweathertex);
            shadowmapshader->set();
            screenquad(vcshadowsz, vcshadowsz);
            enddebugpass(VC_DEBUG_SHADOW_MAP);

            begindebugpass(VC_DEBUG_SHADOW_APPLY);
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
            glViewport(0, 0, vieww, viewh);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, vcshadowtex);
            GLOBALPARAMF(tvcloudshadowparams, shadowamount, cloudmidz);
            GLOBALPARAMF(tvcloudshadowpcf, float(vcshadowpcf));

            glEnable(GL_BLEND);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR);
            shadowapplyshader->set();
            screenquad(vieww, viewh);
            glDisable(GL_BLEND);
            enddebugpass(VC_DEBUG_SHADOW_APPLY);
        }

        glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        glViewport(0, 0, vieww, viewh);

        if(drawclouds)
        {
            godrays::crepuscular::render(crsourcetex, vcw, vch, silverscreen, cloudsun.silverWarm);

            begindebugpass(VC_DEBUG_COMPOSITE);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);

            if(compositeshader && vcdetailstrength > 1.0e-4f)
            {
                // Reuse the raymarch's periodic FBM volume. The final pass only
                // consumes cloud RGBA, so no additional cloud render target is needed.
                glActiveTexture_(GL_TEXTURE10);
                glBindTexture(GL_TEXTURE_3D, vcfbmtex);
                glActiveTexture_(GL_TEXTURE0);
                GLOBALPARAMF(tvcloudcompositedetail0, vcdetailstrength, vcdetailfrequency, vcsecondarydetailfrequency, vcdetailscrollspeed);
                GLOBALPARAMF(tvcloudcompositedetail1, vcedgeerosionstrength, vcinternaldarkeningstrength,
                             vcsilverliningbreakupstrength, vcdetailnearscalemultiplier);
                GLOBALPARAMF(tvcloudcompositedetail2, vcdetailfadestartdistance, vcdetailfadeenddistance);
                GLOBALPARAMF(tvcloudcompositedistance0, vcdistantdetailscalestart, vcdistantdetailscaleend,
                             vcdistantdetailmaxfrequency, vcdetailmipbias);
                GLOBALPARAMF(tvcloudcompositedistance1, vcfinedetailfadestart, vcfinedetailfadeend,
                             vcmediumdetailfadestart, vcmediumdetailfadeend);
                GLOBALPARAMF(tvcloudcompositedetaildebug, float(vccompositedetaildebug));
            }

            glEnable(GL_SCISSOR_TEST);
            setcloudscissor(cloudscissor, false, 2);
            glEnable(GL_BLEND);
            // Cloud shader output is premultiplied (rgb already multiplied by alpha/transmittance).
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            if(compositeshader && vcdetailstrength > 1.0e-4f) compositeshader->set();
            else SETSHADER(scalelinear);
            screenquad(compositetexw, compositetexh);

            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            enddebugpass(VC_DEBUG_COMPOSITE);
        }
        glEnable(GL_DEPTH_TEST);

        enddebugtimer();
    }

    bool debugview()
    {
        if(godrays::crepuscular::debugview()) return true;
        if(!debugvc) return false;

        polldebugtimer();
        if(vccompositetex && vccompositetexparams.z > 0.0f && vccompositetexparams.w > 0.0f)
        {
            int w = max(min(hudw, hudh)/3, 1),
                h = max(int(ceilf(w * vccompositetexparams.w / max(vccompositetexparams.z, 1.0f))), 1);
            SETSHADER(hudrect);
            gle::colorf(1, 1, 1);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, vccompositetex);
            debugquad(0, 0, w, h, 0, 0, vccompositetexparams.z, vccompositetexparams.w);

            glEnable(GL_BLEND);
            int texty = h + FONTH/4;
            draw_textf("volumetric clouds total %.3f ms", 0, texty, max(vcdebugms, 0.0f));
            if(vcbudget > 0.0f)
            {
                texty += FONTH;
                draw_textf("  budget %.3f ms filtered %.3f ms", 0, texty, vcbudget, max(vcbudgetfilteredms, 0.0f));
                texty += FONTH;
                draw_textf("  adaptive scale %.3f steps %d", 0, texty, vceffectivescale, vceffectivesteps);
            }
            texty += FONTH;
            draw_textf("  FBM precomputed R8 %d^3", 0, texty, vcfbmtexsize);
            loopi(VC_DEBUG_PASS_COUNT)
            {
                texty += FONTH;
                if(vcdebugpassms[i] >= 0.0f) draw_textf("  %s %.3f ms", 0, texty, vcdebugpassnames[i], vcdebugpassms[i]);
                else draw_textf("  %s n/a", 0, texty, vcdebugpassnames[i]);
            }
        }
        else
        {
            glEnable(GL_BLEND);
            draw_text("volumetric clouds inactive", 0, 0);
        }
        return true;
    }

    static void cleanupbuffers()
    {
        if(vcfbo)
        {
            glDeleteFramebuffers_(1, &vcfbo);
            vcfbo = 0;
        }
        if(vctex)
        {
            glDeleteTextures(1, &vctex);
            vctex = 0;
        }
        if(crsourcetex)
        {
            glDeleteTextures(1, &crsourcetex);
            crsourcetex = 0;
        }
        if(vcdepthfbo)
        {
            glDeleteFramebuffers_(1, &vcdepthfbo);
            vcdepthfbo = 0;
        }
        if(vcdepthtex)
        {
            glDeleteTextures(1, &vcdepthtex);
            vcdepthtex = 0;
        }
        if(vcatrousfbo)
        {
            glDeleteFramebuffers_(1, &vcatrousfbo);
            vcatrousfbo = 0;
        }
        if(vcatroustex)
        {
            glDeleteTextures(1, &vcatroustex);
            vcatroustex = 0;
        }
        if(vcbilateralfbo)
        {
            glDeleteFramebuffers_(1, &vcbilateralfbo);
            vcbilateralfbo = 0;
        }
        if(vcbilateraltex)
        {
            glDeleteTextures(1, &vcbilateraltex);
            vcbilateraltex = 0;
        }
        if(vcbilateraltempfbo)
        {
            glDeleteFramebuffers_(1, &vcbilateraltempfbo);
            vcbilateraltempfbo = 0;
        }
        if(vcbilateraltemptex)
        {
            glDeleteTextures(1, &vcbilateraltemptex);
            vcbilateraltemptex = 0;
        }
        if(vcclarityfbo)
        {
            glDeleteFramebuffers_(1, &vcclarityfbo);
            vcclarityfbo = 0;
        }
        if(vcclaritytex)
        {
            glDeleteTextures(1, &vcclaritytex);
            vcclaritytex = 0;
        }
        vccompositetex = 0;
        vccompositetexparams = vec4(0, 0, 0, 0);
        vcw = vch = vcfullw = vcfullh = 0;
    }

    void cleanup(bool shutdown)
    {
        godrays::crepuscular::cleanup();
        cleanupbuffers();
        cleanupshadowmap();
        cleanupweathermap();
        if(shutdown) cleanupfbmtexture();
        cleanupdebugtimer();
        vcbudgetinitialized = false;
        vcbudgetfilteredms = -1.0f;
    }
}
