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

    GLuint vctex = 0, vcfbo = 0;
    GLuint vcatroustex = 0, vcatrousfbo = 0;
    GLuint vcbilateraltex = 0, vcbilateralfbo = 0;
    GLuint vcbilateraltemptex = 0, vcbilateraltempfbo = 0;
    GLuint vcclaritytex = 0, vcclarityfbo = 0;
    GLuint vcshadowtex = 0, vcshadowfbo = 0;
    GLuint vcweathertex = 0;
    GLuint vccompositetex = 0;
    int vcw = 0, vch = 0, vcfullw = 0, vcfullh = 0;
    int vcshadowsz = 0;
    vec4 vccompositetexparams(0, 0, 0, 0);
    vec4 vcshadowmapworld(0, 0, 1, 0);
    float vcshadowmapstrength = 0.0f;
    vec2 vcscrolloffset(0, 0);
    int vcscrolllastmillis = -1;
    int vcweatherseed = 1337;
    bool vcweatherdirty = true;

    static const int VC_WEATHER_MAP_SIZE = 512;

    // graphic settings
    VARP(volumetricclouds, 0, 1, 1);
    VARP(vcblur, 0, 1, 1);
    VARP(vcblurscale, 1, 1, 4);
    VARP(vcatrous, 0, 1, 1);
    VARP(vcatrousiter, 1, 2, 3);
    FVARP(vcatrousalphak, 0.0f, 16.0f, 256.0f);
    FVARP(vcscale, 0.25f, 0.5f, 2.0f);
    FVARP(vcbilateraledge, 1e-5f, 0.02f, 1.0f);
    VARP(vcsteps, 4, 16, 128);
    VARP(vcsunsteps, 4, 4, 64);
    VARP(vcshadow, 0, 1, 1);
    VARP(vcshadowmapsize, 64, 512, 2048);
    VARP(vcshadowsamples, 1, 4, 8);
    VARP(vcshadowpcf, 0, 1, 2);
    VARP(vcclarity, 0, 1, 1);
    FVARP(vcclaritystrength, 0.0f, 0.22f, 1.0f);
    FVARP(vcclarityradius, 0.5f, 16.0f, 32.0f);
    FVARP(vcclarityalphak, 0.0f, 18.0f, 64.0f);
    FVARP(vcclaritylumak, 0.0f, 6.0f, 32.0f);

    // map settings
    VARP(vcmultiscatoctaves, 1, 3, 4);
    FVAR(vcmultiscat, 0.0f, 1.0f, 1.0f);            // a: scattering attenuation per octave
    FVAR(vcmultiscatext, 0.0f, 1.0f, 1.0f);         // b: extinction attenuation per octave
    FVAR(vcmultiscatphase, 0.0f, 0.0f, 1.0f);       // c: phase-angle attenuation per octave
    FVAR(vcphaseg, -0.95f, 0.55f, 0.95f);
    FVAR(vcphaseg2, -0.95f, -0.25f, 0.95f);
    FVAR(vcphaseblend, 0.0f, 0.18f, 1.0f);
    FVARR(vcfogdistmul, 0.25f, 4.0f, 64.0f);
    VARR(vcatmoblendmin, 0, 70, 100);
    VARR(vcatmoblendmax, 0, 100, 100);
    VARR(vcdensity, 0, 50, 200);
    FVARR(vcalpha, 0.0f, 0.75f, 1.0f);
    VARR(vcheight, -1000, 80, 1000);
    VARR(vcthickness, 0, 20, 300);
    VARR(vcradius, 0, 100, 10000);                  // 100 = worldsize, 0 disables the cloud layer
    VARR(vcscrollx, -1000, 0, 1000);
    VARR(vcscrolly, -1000, 0, 1000);
    VARR(vcdome, -1000, 0, 1000);
    VARR(vcstructure, 0, 75, 200);                 // 75 = default shaping, lower = macro, higher = micro
    VARR(vcsilverradius, 0, 30, 100);              // radius of the sun mask as % of min screen dimension, 0 disables
    FVARR(vcsilvercontrast, 1.0f, 35.0f, 50.0f);
    FVARR(vcdarkness, 0.1f, 1.0f, 2.0f);
    FVARR(vcshadowstrength, 0.0f, 0.65f, 1.0f);
    CVARR(vccolour, 0xFFFFFF);

    static float normalizenoise(float n)
    {
        return clamp(n * 0.5f + 0.5f, 0.0f, 1.0f);
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

    static vec4 calcsilverscreenparams()
    {
        if(vcsilverradius <= 0 || sunlight.iszero() || sunlightscale <= 1.0e-4f) return vec4(0, 0, 0, 0);

        vec sunpoint(camera1->o);
        sunpoint.madd(sunlightdir, max(nearplane * 4.0f, 1.0f));

        vec4 sunclip;
        camprojmatrix.transform(sunpoint, sunclip);
        if(sunclip.w <= 1.0e-4f || sunclip.z < -sunclip.w) return vec4(0, 0, 0, 0);

        vec2 sunndc(sunclip.x / sunclip.w, sunclip.y / sunclip.w);
        if(fabsf(sunndc.x) > 1.35f || fabsf(sunndc.y) > 1.35f) return vec4(0, 0, 0, 0);

        float screenedge = max(fabsf(sunndc.x), fabsf(sunndc.y));
        float edgefade = clamp(1.0f - max(screenedge - 0.90f, 0.0f) / 0.40f, 0.0f, 1.0f);
        float horizonfade = clamp((sunlightdir.z - 0.02f) / 0.10f, 0.0f, 1.0f);
        float screenfade = edgefade * horizonfade;
        if(screenfade <= 1.0e-4f) return vec4(0, 0, 0, 0);

        float radiuspixels = min(vieww, viewh) * (float(vcsilverradius) / 100.0f) * getsilverfovscale();
        if(radiuspixels <= 1.0e-4f) return vec4(0, 0, 0, 0);

        return vec4((sunndc.x * 0.5f + 0.5f) * vieww, (sunndc.y * 0.5f + 0.5f) * viewh, radiuspixels, screenfade);
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

    void init()
    {
        if(!volumetricclouds) return;
        useshaderbyname("volumetricclouds");
        useshaderbyname("atrousfilter");
        useshaderbyname("volumetriccloudsupscale");
        useshaderbyname("volumetriccloudsbilateral");
        useshaderbyname("volumetriccloudclarity");
        useshaderbyname("volumetriccloudshadowmap");
        useshaderbyname("volumetriccloudshadowapply");
        useshaderbyname("scalelinear");
    }

    bool hasshadowmap()
    {
        return volumetricclouds && vcdensity > 0 && vcradius > 0 && vcshadowtex && vcshadowfbo && vcshadowmapworld.w > 0.0f && vcshadowmapstrength > 1e-4f;
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
        if(!volumetricclouds || vcdensity <= 0 || vcradius <= 0)
        {
            if(vcshadowtex || vcshadowfbo) cleanupshadowmap();
            return;
        }

        Shader *cloudshader = useshaderbyname("volumetricclouds");
        Shader *atrousshader = vcatrous ? useshaderbyname("atrousfilter") : NULL;
        Shader *upscaleshader = useshaderbyname("volumetriccloudsupscale");
        Shader *bilateralshader = useshaderbyname("volumetriccloudsbilateral");
        Shader *clarityshader = vcclarity ? useshaderbyname("volumetriccloudclarity") : NULL;
        Shader *shadowmapshader = vcshadow ? useshaderbyname("volumetriccloudshadowmap") : NULL;
        Shader *shadowapplyshader = vcshadow ? useshaderbyname("volumetriccloudshadowapply") : NULL;
        bool useclarity = vcclarity && clarityshader && vcclaritystrength > 1e-4f;
        float shadowstrength = vcshadowstrength * clamp(vcalpha, 0.0f, 1.0f);
        if(!cloudshader) return;

        int targetw = max(int(ceilf(vieww * vcscale)), 1),
            targeth = max(int(ceilf(viewh * vcscale)), 1);
        if(targetw != vcw || targeth != vch || vieww != vcfullw || viewh != vcfullh)
        {
            cleanup();
            vcw = targetw;
            vch = targeth;
            vcfullw = vieww;
            vcfullh = viewh;
        }
        if(!ensureWeatherMap()) return;

        if((!vcshadow || !shadowmapshader || !shadowapplyshader || shadowstrength <= 1e-4f) && (vcshadowtex || vcshadowfbo))
            cleanupshadowmap();

        if(!vctex)
        {
            glGenTextures(1, &vctex);
            createtexture(vctex, vcw, vch, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(!vcatroustex)
        {
            glGenTextures(1, &vcatroustex);
            createtexture(vcatroustex, vcw, vch, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(!vcbilateraltex)
        {
            glGenTextures(1, &vcbilateraltex);
            createtexture(vcbilateraltex, vieww, viewh, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(!vcbilateraltemptex)
        {
            glGenTextures(1, &vcbilateraltemptex);
            createtexture(vcbilateraltemptex, vieww, viewh, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(useclarity && !vcclaritytex)
        {
            glGenTextures(1, &vcclaritytex);
            createtexture(vcclaritytex, vieww, viewh, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }

        if(!vcfbo)
        {
            glGenFramebuffers_(1, &vcfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vctex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(!vcatrousfbo)
        {
            glGenFramebuffers_(1, &vcatrousfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcatrousfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcatroustex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud atrous buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(!vcbilateralfbo)
        {
            glGenFramebuffers_(1, &vcbilateralfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateralfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcbilateraltex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud bilateral buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(!vcbilateraltempfbo)
        {
            glGenFramebuffers_(1, &vcbilateraltempfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateraltempfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcbilateraltemptex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud bilateral temp buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(useclarity && !vcclarityfbo)
        {
            glGenFramebuffers_(1, &vcclarityfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcclarityfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcclaritytex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud clarity buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(vcshadow && shadowmapshader && shadowapplyshader && shadowstrength > 1e-4f)
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
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vcweathertex);

        vec4 cloudbounds, clouddome;
        calcshadowparams(cloudbounds, clouddome);

        GLOBALPARAMF(tvcloudbounds, cloudbounds.x, cloudbounds.y, cloudbounds.z, cloudbounds.w);
        GLOBALPARAMF(tvclouddome, clouddome.x, clouddome.y, clouddome.z, clouddome.w);
        GLOBALPARAMF(tvcloudscroll, vcscrolloffset.x, vcscrolloffset.y);
        float ws = max(float(worldsize), 1.0f);
        GLOBALPARAMF(tvcloudnoise, 1.0f / max(ws * 0.30f, 1.0f), 1.0f / max(ws * 0.12f, 1.0f), 0.50f, 0.95f);
        GLOBALPARAMF(tvcloudstructure, float(vcstructure) / 100.0f);
        GLOBALPARAMF(tvcloudscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
        GLOBALPARAMF(vclouddensity, float(vcdensity) / 100.0f);
        GLOBALPARAMF(vcloudalpha, vcalpha);
        GLOBALPARAMF(vcloudthickness, vcdarkness);
        GLOBALPARAMF(vcloudphaseparams, vcphaseg, vcphaseg2, vcphaseblend);
        GLOBALPARAMF(vcloudmultiscatparams, vcmultiscat, vcmultiscatext, vcmultiscatphase, float(vcmultiscatoctaves));
        GLOBALPARAMF(tvcloudfogdistmul, max(vcfogdistmul, 1.0e-3f));
        GLOBALPARAMF(tvcloudatmoblend, float(vcatmoblendmin) / 100.0f, float(vcatmoblendmax) / 100.0f);
        vec4 silverscreen = calcsilverscreenparams();
        GLOBALPARAMF(tvcloudsilvermask, silverscreen.x, silverscreen.y, silverscreen.z, silverscreen.w);
        GLOBALPARAMF(tvcloudsilvercontrast, max(vcsilvercontrast, 1.0f));
        GLOBALPARAMF(tvcloudsteps, float(vcsteps));
        GLOBALPARAMF(tvcloudsunsteps, float(vcsunsteps));
        GLOBALPARAM(vcloudcolour, vccolour.tocolor());
        GLOBALPARAM(sunlightdir, sunlightdir);
        GLOBALPARAMF(sunlightcolor, sunlight.x*(2.0f*ldrscaleb)*sunlightscale, sunlight.y*(2.0f*ldrscaleb)*sunlightscale, sunlight.z*(2.0f*ldrscaleb)*sunlightscale);
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

        glBindFramebuffer_(GL_FRAMEBUFFER, vcfbo);
        glViewport(0, 0, vcw, vch);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        cloudshader->set();
        screenquad(vcw, vch);

        GLuint lowrestex = vctex;
        if(vcatrous && atrousshader)
        {
            int iterations = clamp(vcatrousiter, 1, 3);
            loopi(iterations)
            {
                bool writetomain = lowrestex == vcatroustex;
                glBindFramebuffer_(GL_FRAMEBUFFER, writetomain ? vcfbo : vcatrousfbo);
                glViewport(0, 0, vcw, vch);
                glDisable(GL_BLEND);
                glActiveTexture_(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_RECTANGLE, lowrestex);
                GLOBALPARAMF(tatroussize, float(vcw), float(vch));
                GLOBALPARAMF(tatrousparams, float(1<<i), vcatrousalphak, 0.0f, 0.0f);
                atrousshader->set();
                screenquad(vcw, vch);
                lowrestex = writetomain ? vctex : vcatroustex;
            }
        }

        GLuint compositetex = lowrestex;
        int compositetexw = vcw, compositetexh = vch;

        if(vcblur && bilateralshader)
        {
            GLOBALPARAMF(tvbilateraldepthscale, 1.0f / max(float(farplane) * vcbilateraledge, 1e-4f));
            GLOBALPARAMF(vcblurscale, float(vcblurscale));

            // Pass 1: horizontal bilateral blur + upscale from low-res cloud buffer.
            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateraltempfbo);
            glViewport(0, 0, vieww, viewh);
            glDisable(GL_BLEND);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);

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
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);

            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, vcbilateraltemptex);
            GLOBALPARAMF(tvcloudscale, 1.0f, 1.0f, 1.0f, 1.0f);
            GLOBALPARAMF(tvcloudblurdir, 0.0f, 1.0f);
            bilateralshader->set();
            screenquad(vieww, viewh);

            compositetex = vcbilateraltex;
            compositetexw = vieww;
            compositetexh = viewh;
        }
        else if((vcw < vieww || vch < viewh) && upscaleshader)
        {
            // Depth-aware upsample to avoid low-res cloud alpha bleeding over
            // foreground geometry silhouettes when vcscale < 1.
            GLOBALPARAMF(tvbilateraldepthscale, 1.0f / max(float(farplane) * vcbilateraledge, 1e-4f));

            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateralfbo);
            glViewport(0, 0, vieww, viewh);
            glDisable(GL_BLEND);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);

            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, lowrestex);
            GLOBALPARAMF(tvcloudscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
            upscaleshader->set();
            screenquad(vieww, viewh);

            compositetex = vcbilateraltex;
            compositetexw = vieww;
            compositetexh = viewh;
        }

        // Full-res edge-aware clarity pass. Only run after the cloud chain has
        // produced a full-resolution working texture; never sharpen low-res RTs.
        if(useclarity && compositetexw == vieww && compositetexh == viewh)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, vcclarityfbo);
            glViewport(0, 0, vieww, viewh);
            glDisable(GL_BLEND);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);

            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);
            GLOBALPARAMF(vcclarityparams, vcclaritystrength, vcclarityradius, vcclarityalphak, vcclaritylumak);
            clarityshader->set();
            screenquad(vieww, viewh);

            compositetex = vcclaritytex;
            compositetexw = vieww;
            compositetexh = viewh;
        }

        vccompositetex = compositetex;
        vccompositetexparams = vec4(
            float(compositetexw) / max(float(vieww), 1.0f),
            float(compositetexh) / max(float(viewh), 1.0f),
            float(compositetexw),
            float(compositetexh)
        );

        if(vcshadow && vcshadowtex && vcshadowfbo && shadowmapshader && shadowapplyshader && shadowstrength > 1e-4f)
        {
            float shadowworld = max(float(worldsize) * 2.0f, 1.0f);
            float worldpertexel = shadowworld / max(float(vcshadowsz), 1.0f);
            float snappedx = floorf(camera1->o.x / worldpertexel) * worldpertexel;
            float snappedy = floorf(camera1->o.y / worldpertexel) * worldpertexel;
            float minx = snappedx - shadowworld * 0.5f;
            float miny = snappedy - shadowworld * 0.5f;
            float cloudmidz = 0.5f * (cloudbounds.x + cloudbounds.y);
            vcshadowmapworld = vec4(minx, miny, worldpertexel, float(vcshadowsz));
            vcshadowmapstrength = shadowstrength;

            GLOBALPARAMF(tvshadowmapworld, minx, miny, worldpertexel, float(vcshadowsz));
            GLOBALPARAMF(tvcloudshadowsamples, float(vcshadowsamples));

            glBindFramebuffer_(GL_FRAMEBUFFER, vcshadowfbo);
            glViewport(0, 0, vcshadowsz, vcshadowsz);
            glDisable(GL_BLEND);
            glClearColor(1, 1, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, vcweathertex);
            shadowmapshader->set();
            screenquad(vcshadowsz, vcshadowsz);

            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
            glViewport(0, 0, vieww, viewh);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, vcshadowtex);
            GLOBALPARAMF(tvcloudshadowparams, shadowstrength, cloudmidz, 0.08f, 0.20f);
            GLOBALPARAMF(tvcloudshadowpcf, float(vcshadowpcf));

            glEnable(GL_BLEND);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR);
            shadowapplyshader->set();
            screenquad(vieww, viewh);
            glDisable(GL_BLEND);
        }

        glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        glViewport(0, 0, vieww, viewh);

        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);

        glEnable(GL_BLEND);
        // Cloud shader output is premultiplied (rgb already multiplied by alpha/transmittance).
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        SETSHADER(scalelinear);
        screenquad(compositetexw, compositetexh);

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    void cleanup()
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
        cleanupshadowmap();
        cleanupweathermap();
        vccompositetex = 0;
        vccompositetexparams = vec4(0, 0, 0, 0);
        vcw = vch = vcfullw = vcfullh = 0;
    }
}
