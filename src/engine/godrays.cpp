// godrays.cpp: dedicated screen-space cloud crepuscular rays, separate from world volumetric lights

#include "engine.h"

extern GLuint hdrfbo, mshdrfbo;

namespace godrays
{
namespace crepuscular
{
    GLuint crtex = 0, crfbo = 0, crdepthtex = 0, crdepthfbo = 0;
    int crw = 0, crh = 0;

    static const int CR_DEBUG_QUERY_COUNT = 3;
    GLuint crdebugquery[CR_DEBUG_QUERY_COUNT][2] = { { 0 } };
    int crdebugquerycycle = 0, crdebugquerywaiting = 0, crdebugqueryactive = -1;
    int crdebugcpustart = 0;
    bool crdebugcputimer = false;
    float crdebugms = -1.0f;

    VARP(crepuscularrays, 0, 1, 1);
    VARP(crsteps, 8, 48, 128);
    FVARP(crscale, 0.125f, 0.25f, 1.0f);
    FVARP(crbilateraledge, 1e-5f, 0.02f, 1.0f);

    FVARP(crvariation, 0.0f, 0.5f, 1.0f);
    FVARP(crfreq, 0.25f, 32.0f, 64.0f);
    FVARP(crdetailfreq, 0.5f, 64.0f, 128.0f);
    FVARP(craniso, 1.0f, 16.0f, 32.0f);
    FVARP(crdistortion, 0.0f, 0.08f, 0.5f);
    FVARP(crbandcontrast, 0.25f, 1.35f, 4.0f);
    FVARP(crradialfade, 0.1f, 1.0f, 4.0f);
    FVARP(crnoisejitter, 0.0f, 0.0f, 1.0f);
    VAR(debugcr, 0, 0, 1);

    FVARR(crstrength, 0.0f, 0.2f, 2.0f);

    static void cleanupbuffer()
    {
        if(crdepthfbo)
        {
            glDeleteFramebuffers_(1, &crdepthfbo);
            crdepthfbo = 0;
        }
        if(crfbo)
        {
            glDeleteFramebuffers_(1, &crfbo);
            crfbo = 0;
        }
        if(crtex)
        {
            glDeleteTextures(1, &crtex);
            crtex = 0;
        }
        if(crdepthtex)
        {
            glDeleteTextures(1, &crdepthtex);
            crdepthtex = 0;
        }
        crw = crh = 0;
    }

    static bool ensurebuffer(int w, int h, bool needdepth)
    {
        if(w != crw || h != crh) cleanupbuffer();
        if(!crtex)
        {
            crw = w;
            crh = h;
            glGenTextures(1, &crtex);
            createtexture(crtex, crw, crh, NULL, 3, 1, hasTF ? GL_RGBA16F : GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(!crfbo)
        {
            glGenFramebuffers_(1, &crfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, crfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, crtex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                fatal("Failed allocating volumetric cloud crepuscular-rays buffer!");
        }
        if(needdepth && !crdepthtex)
        {
            glGenTextures(1, &crdepthtex);
            createtexture(crdepthtex, crw, crh, NULL, 3, 0, GL_R32F, GL_TEXTURE_RECTANGLE);
        }
        if(needdepth && !crdepthfbo)
        {
            glGenFramebuffers_(1, &crdepthfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, crdepthfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, crdepthtex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                fatal("Failed allocating crepuscular-rays depth cache!");
        }
        return true;
    }

    static void polldebugtimer()
    {
        if(!debugcr || !crdebugquery[0][0]) return;
        loopi(CR_DEBUG_QUERY_COUNT) if(crdebugquerywaiting&(1<<i))
        {
            GLint available = 0;
            glGetQueryObjectiv_(crdebugquery[i][1], GL_QUERY_RESULT_AVAILABLE, &available);
            if(!available) continue;

            GLuint64EXT start = 0, end = 0;
            glGetQueryObjectui64v_(crdebugquery[i][0], GL_QUERY_RESULT, &start);
            glGetQueryObjectui64v_(crdebugquery[i][1], GL_QUERY_RESULT, &end);
            crdebugms = max(float(end - start) * 1.0e-6f, 0.0f);
            crdebugquerywaiting &= ~(1<<i);
        }
    }

    static void begindebugtimer()
    {
        crdebugqueryactive = -1;
        crdebugcputimer = false;
        if(!debugcr) return;

        polldebugtimer();
        if(hasTQ && glQueryCounter_)
        {
            if(!crdebugquery[0][0]) glGenQueries_(CR_DEBUG_QUERY_COUNT * 2, &crdebugquery[0][0]);
            if(!(crdebugquerywaiting&(1<<crdebugquerycycle)))
            {
                crdebugqueryactive = crdebugquerycycle;
                glQueryCounter_(crdebugquery[crdebugqueryactive][0], GL_TIMESTAMP);
            }
            return;
        }

        crdebugcpustart = getclockmillis();
        crdebugcputimer = true;
    }

    static void enddebugtimer()
    {
        if(crdebugqueryactive >= 0)
        {
            glQueryCounter_(crdebugquery[crdebugqueryactive][1], GL_TIMESTAMP);
            crdebugquerywaiting |= 1<<crdebugqueryactive;
            crdebugquerycycle = (crdebugqueryactive + 1) % CR_DEBUG_QUERY_COUNT;
            crdebugqueryactive = -1;
        }
        else if(crdebugcputimer)
        {
            crdebugms = max(float(getclockmillis() - crdebugcpustart), 0.0f);
            crdebugcputimer = false;
        }
    }

    static void cleanupdebugtimer()
    {
        if(crdebugquery[0][0]) glDeleteQueries_(CR_DEBUG_QUERY_COUNT * 2, &crdebugquery[0][0]);
        memset(crdebugquery, 0, sizeof(crdebugquery));
        crdebugquerycycle = 0;
        crdebugquerywaiting = 0;
        crdebugqueryactive = -1;
        crdebugcputimer = false;
        crdebugms = -1.0f;
    }

    void init()
    {
        useshaderbyname("crepuscularrays");
        useshaderbyname("crepuscularraysdepth");
        useshaderbyname("crepuscularrayscomposite");
        useshaderbyname("crepuscularraysdebug");
    }

    bool enabled()
    {
        return crepuscularrays != 0;
    }

    void render(GLuint sourcetex, int sourcew, int sourceh, const vec4 &silverscreen, const vec &raytint)
    {
        if(!crepuscularrays || !sourcetex || sourcew <= 0 || sourceh <= 0 || silverscreen.w <= 1.0e-4f ||
           crstrength <= 1.0e-4f) return;

        int targetw = max(int(ceilf(vieww * crscale)), 1),
            targeth = max(int(ceilf(viewh * crscale)), 1);
        bool useupscale = targetw < vieww || targeth < viewh;
        Shader *rayshader = useshaderbyname("crepuscularrays");
        Shader *depthshader = useupscale ? useshaderbyname("crepuscularraysdepth") : NULL;
        Shader *compositeshader = useshaderbyname("crepuscularrayscomposite");
        if(!rayshader || !compositeshader || (useupscale && !depthshader) || !ensurebuffer(targetw, targeth, useupscale)) return;

        float scalex = float(sourcew) / max(float(vieww), 1.0f),
              scaley = float(sourceh) / max(float(viewh), 1.0f);
        vec4 sourcesun(silverscreen.x * scalex, silverscreen.y * scaley,
                       silverscreen.z * min(scalex, scaley), silverscreen.w);
        vec4 scaleparams(float(vieww) / crw, float(viewh) / crh,
                         float(crw) / max(float(vieww), 1.0f), float(crh) / max(float(viewh), 1.0f));

        begindebugtimer();
        if(useupscale)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, crdepthfbo);
            glViewport(0, 0, crw, crh);
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            glActiveTexture_(GL_TEXTURE9);
            if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
            else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
            GLOBALPARAMF(crscaleparams, scaleparams.x, scaleparams.y, scaleparams.z, scaleparams.w);
            depthshader->set();
            screenquad(crw, crh);
        }

        glBindFramebuffer_(GL_FRAMEBUFFER, crfbo);
        glViewport(0, 0, crw, crh);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_RECTANGLE, sourcetex);
        GLOBALPARAMF(crsource, float(sourcew), float(sourceh), sourcesun.x, sourcesun.y);
        GLOBALPARAMF(crrayscale, float(sourcew) / crw, float(sourceh) / crh,
                     float(crw) / sourcew, float(crh) / sourceh);
        GLOBALPARAMF(crsun, sourcesun.z, sourcesun.w, float(min(sourcew, sourceh)), float(crsteps));
        GLOBALPARAMF(crvariationparams, crvariation, crfreq, crdetailfreq, craniso);
        GLOBALPARAMF(crvariationparams2, crdistortion, crbandcontrast, crradialfade, crnoisejitter);
        rayshader->set();
        screenquad(crw, crh);

        glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        glViewport(0, 0, vieww, viewh);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_RECTANGLE, crtex);
        glActiveTexture_(GL_TEXTURE8);
        if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msnormaltex);
        else glBindTexture(GL_TEXTURE_RECTANGLE, gnormaltex);
        if(useupscale)
        {
            glActiveTexture_(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_RECTANGLE, crdepthtex);
            glActiveTexture_(GL_TEXTURE9);
            if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
            else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
        }
        glActiveTexture_(GL_TEXTURE0);
        GLOBALPARAMF(crcompositeparams, float(crw), float(crh), scaleparams.z, scaleparams.w);
        GLOBALPARAMF(crscaleparams, scaleparams.x, scaleparams.y, scaleparams.z, scaleparams.w);
        GLOBALPARAMF(crbilateralparams, crbilateraledge, useupscale ? 1.0f : 0.0f);
        GLOBALPARAMF(crtint, raytint.x, raytint.y, raytint.z, crstrength);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        compositeshader->set();
        screenquad(vieww, viewh);
        glDisable(GL_BLEND);
        enddebugtimer();
    }

    bool debugview()
    {
        if(!debugcr) return false;

        polldebugtimer();

        glEnable(GL_BLEND);
        if(!crtex || crw <= 0 || crh <= 0)
        {
            draw_text("cloud crepuscular rays inactive", 0, 0);
            return true;
        }

        Shader *debugshader = useshaderbyname("crepuscularraysdebug");
        if(!debugshader) return true;

        static const char * const labels[4] =
        {
            "raw silver source", "smooth radial rays", "shaft modulation", "final modulated rays"
        };
        int gap = FONTH, tilew = max((min(hudw, hudh) - gap) / 2, 1);
        int tileh = max(int(ceilf(tilew * float(crh) / max(float(crw), 1.0f))), 1);
        gle::colorf(1, 1, 1);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_RECTANGLE, crtex);
        loopi(4)
        {
            int x = (i & 1) * (tilew + gap), y = (i >> 1) * (tileh + FONTH + gap);
            GLOBALPARAMI(crdebugchannel, i);
            debugshader->set();
            debugquad(x, y, tilew, tileh, 0, 0, crw, crh);
            draw_text(labels[i], x, y + tileh + FONTH/4);
        }
        if(crdebugms >= 0.0f) draw_textf("crepuscular rays %.3f ms", 0, 2*(tileh + FONTH + gap), crdebugms);
        else draw_text("crepuscular rays n/a", 0, 2*(tileh + FONTH + gap));
        return true;
    }

    void cleanup()
    {
        cleanupdebugtimer();
        cleanupbuffer();
    }
}
}

namespace godrays
{
namespace geometry
{
    void cleanup();

    // settings
    VARFP(godraysgeom, 0, 1, 1, if(!godraysgeom) cleanup());
    VARP(grgsteps, 1, 8, 64);
    FVARP(grgscale, 0.125f, 0.5f, 1.0f);
    VARP(grgatrous, 0, 1, 1);
    VARP(grgatrousiter, 1, 3, 3);
    VARP(grgglobalstrength, 0, 0, 3);

    // tunables
    FVAR(grgshadowbias, 0.0f, 0.0f, 4.0f);
    FVAR(grgshadowbiasdist, 0.0f, 0.0f, 1.0f);
    FVAR(grgforwardexp, 0.25f, 0.25f, 32.0f);
    FVAR(grgisolationradius, 0.0f, 128.0f, 256.0f);
    FVAR(grgisolationpower, 0.01f, 1.0f, 4.0f);
    FVAR(grgbaseatmosphere, 0.0f, 0.02f, 0.25f);
    FVAR(grgshaftboost, 0.0f, 1.0f, 4.0f);
    FVAR(grgdetailboost, 0.0f, 0.0f, 1.0f);
    FVAR(grgcsmfade, 0.0f, 0.05f, 0.25f);
    FVAR(grgatrousalphak, 0.0f, 2.0f, 256.0f);
    FVAR(grgatrousdepth, 0.0f, 4096.0f, 8192.0f);
    FVAR(grgupscaleedge, 0.0f, 0.02f, 1.0f);
    FVAR(grgdecay, 0.0f, 0.93f, 1.0f);
    FVAR(grgthreshold, 0.0f, 0.05f, 1.0f);

    // map vars
    FVARR(grgstrength, 0.0f, 2.0f, 4.0f);
    FVARR(grgdensity, 0.25f, 2.0f, 4.0f);
    FVARR(grgmaxdist, 0.01f, 0.8f, 1.0f);
    CVARR(grgcolour, 0);

    VAR(debuggrg, 0, 0, 7);

    static int bufferwidth = -1, bufferheight = -1, reconstructionwidth = -1, reconstructionheight = -1;
    static GLuint rayfbo = 0, raytex = 0, rayupsamplefbo = 0, rayupsampletex = 0;
    static GLuint rayfilterfbo[2] = { 0, 0 }, rayfiltertex[2] = { 0, 0 }, rayguidefbo = 0, rayguidetex = 0;
    static GLuint raydebugfbo = 0, raydebugtex = 0;
    static GLenum passformat = GL_RGBA8, guideformat = GL_RGBA8;
    static GLuint debugcompositetex = 0;
    static bool debugrendered = false;

    enum GRGDebugPass
    {
        GRG_DEBUG_SETUP = 0,
        GRG_DEBUG_CLEAR,
        GRG_DEBUG_RAYMARCH,
        GRG_DEBUG_FILTER,
        GRG_DEBUG_UPSCALE,
        GRG_DEBUG_COMPOSITE,
        GRG_DEBUG_RESTORE,
        GRG_DEBUG_PASS_COUNT
    };

    static const int GRG_DEBUG_QUERY_COUNT = 3, GRG_DEBUG_TIMESTAMPS = 2 + 2 * GRG_DEBUG_PASS_COUNT;
    static const char * const grgdebugpassnames[GRG_DEBUG_PASS_COUNT] =
    {
        "FBO/setup", "clear", "raymarch draw", "a-trous filter (ray res)", "upscale", "composite", "framebuffer restore"
    };
    static GLuint grgdebugquery[GRG_DEBUG_QUERY_COUNT][GRG_DEBUG_TIMESTAMPS] = { { 0 } };
    static int grgdebugquerycycle = 0, grgdebugquerywaiting = 0, grgdebugqueryactive = -1;
    static int grgdebugpassmask[GRG_DEBUG_QUERY_COUNT] = { 0, 0, 0 };
    static int grgdebugcpustart = 0;
    static bool grgdebugcputimer = false;
    static float grgdebugms = -1.0f;
    static float grgdebugpassms[GRG_DEBUG_PASS_COUNT] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };

    static void polldebugtimer()
    {
        if(!debuggrg || !grgdebugquery[0][0]) return;
        loopi(GRG_DEBUG_QUERY_COUNT) if(grgdebugquerywaiting&(1<<i))
        {
            GLint available = 0;
            glGetQueryObjectiv_(grgdebugquery[i][1], GL_QUERY_RESULT_AVAILABLE, &available);
            if(!available) continue;

            GLuint64EXT start = 0, end = 0;
            glGetQueryObjectui64v_(grgdebugquery[i][0], GL_QUERY_RESULT, &start);
            glGetQueryObjectui64v_(grgdebugquery[i][1], GL_QUERY_RESULT, &end);
            grgdebugms = max(float(end - start) * 1.0e-6f, 0.0f);
            loopj(GRG_DEBUG_PASS_COUNT) if(grgdebugpassmask[i]&(1<<j))
            {
                GLuint64EXT passstart = 0, passend = 0;
                glGetQueryObjectui64v_(grgdebugquery[i][2 + 2*j], GL_QUERY_RESULT, &passstart);
                glGetQueryObjectui64v_(grgdebugquery[i][2 + 2*j + 1], GL_QUERY_RESULT, &passend);
                grgdebugpassms[j] = max(float(passend - passstart) * 1.0e-6f, 0.0f);
            }
            else grgdebugpassms[j] = 0.0f;
            grgdebugquerywaiting &= ~(1<<i);
        }
    }

    static void begindebugtimer()
    {
        grgdebugqueryactive = -1;
        grgdebugcputimer = false;
        if(!debuggrg) return;

        polldebugtimer();
        if(hasTQ && glQueryCounter_)
        {
            if(!grgdebugquery[0][0]) glGenQueries_(GRG_DEBUG_QUERY_COUNT * GRG_DEBUG_TIMESTAMPS, &grgdebugquery[0][0]);
            if(!(grgdebugquerywaiting&(1<<grgdebugquerycycle)))
            {
                grgdebugqueryactive = grgdebugquerycycle;
                grgdebugpassmask[grgdebugqueryactive] = 0;
                glQueryCounter_(grgdebugquery[grgdebugqueryactive][0], GL_TIMESTAMP);
            }
            return;
        }

        grgdebugcpustart = getclockmillis();
        grgdebugcputimer = true;
    }

    static void begindebugpass(GRGDebugPass pass)
    {
        if(grgdebugqueryactive < 0) return;
        grgdebugpassmask[grgdebugqueryactive] |= 1<<pass;
        glQueryCounter_(grgdebugquery[grgdebugqueryactive][2 + 2*pass], GL_TIMESTAMP);
    }

    static void enddebugpass(GRGDebugPass pass)
    {
        if(grgdebugqueryactive < 0) return;
        glQueryCounter_(grgdebugquery[grgdebugqueryactive][2 + 2*pass + 1], GL_TIMESTAMP);
    }

    static void enddebugtimer()
    {
        if(grgdebugqueryactive >= 0)
        {
            glQueryCounter_(grgdebugquery[grgdebugqueryactive][1], GL_TIMESTAMP);
            grgdebugquerywaiting |= 1<<grgdebugqueryactive;
            grgdebugquerycycle = (grgdebugqueryactive + 1) % GRG_DEBUG_QUERY_COUNT;
            grgdebugqueryactive = -1;
        }
        else if(grgdebugcputimer)
        {
            grgdebugms = max(float(getclockmillis() - grgdebugcpustart), 0.0f);
            grgdebugcputimer = false;
        }
    }

    static void cleanupdebugtimer()
    {
        if(grgdebugquery[0][0]) glDeleteQueries_(GRG_DEBUG_QUERY_COUNT * GRG_DEBUG_TIMESTAMPS, &grgdebugquery[0][0]);
        memset(grgdebugquery, 0, sizeof(grgdebugquery));
        grgdebugquerycycle = 0;
        grgdebugquerywaiting = 0;
        grgdebugqueryactive = -1;
        memset(grgdebugpassmask, 0, sizeof(grgdebugpassmask));
        grgdebugcputimer = false;
        grgdebugms = -1.0f;
        loopi(GRG_DEBUG_PASS_COUNT) grgdebugpassms[i] = -1.0f;
    }

    static bool disable(const char *msg)
    {
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        cleanup();
        godraysgeom = 0;
        conoutf(CON_ERROR, "%s, geometry god rays deactivated", msg);
        return false;
    }

    static bool needsupsamplebuffer(int targetwidth, int targetheight)
    {
        return debuggrg && (targetwidth < vieww || targetheight < viewh);
    }

    static bool needssecondfilterbuffer(int targetwidth, int targetheight)
    {
        return grgatrous && grgatrousiter > 1;
    }

    static bool setupbuffers(int targetwidth, int targetheight, GLenum targetpassformat, GLenum targetguideformat)
    {
        bufferwidth = targetwidth;
        bufferheight = targetheight;
        reconstructionwidth = vieww;
        reconstructionheight = viewh;
        passformat = targetpassformat;
        guideformat = targetguideformat;
        const int passfilter = bufferwidth < vieww || bufferheight < viewh ? 1 : 0;
        const bool needupsample = needsupsamplebuffer(bufferwidth, bufferheight), needfilter = grgatrous != 0,
                   needsecondfilter = needssecondfilterbuffer(bufferwidth, bufferheight);

        if(!raytex) glGenTextures(1, &raytex);
        if(!rayfbo) glGenFramebuffers_(1, &rayfbo);
        if(needupsample && !rayupsampletex) glGenTextures(1, &rayupsampletex);
        if(needupsample && !rayupsamplefbo) glGenFramebuffers_(1, &rayupsamplefbo);
        loopi(needsecondfilter ? 2 : 1)
        {
            if(needfilter && !rayfiltertex[i]) glGenTextures(1, &rayfiltertex[i]);
            if(needfilter && !rayfilterfbo[i]) glGenFramebuffers_(1, &rayfilterfbo[i]);
        }
        if(needfilter && !rayguidetex) glGenTextures(1, &rayguidetex);
        if(needfilter && !rayguidefbo) glGenFramebuffers_(1, &rayguidefbo);
        if(debuggrg && !raydebugtex) glGenTextures(1, &raydebugtex);
        if(debuggrg && !raydebugfbo) glGenFramebuffers_(1, &raydebugfbo);

        glBindFramebuffer_(GL_FRAMEBUFFER, rayfbo);
        createtexture(raytex, bufferwidth, bufferheight, NULL, 3, passfilter, passformat, GL_TEXTURE_RECTANGLE);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, raytex, 0);
        if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return disable("failed allocating geometry god rays buffer");

        if(needupsample)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, rayupsamplefbo);
            createtexture(rayupsampletex, reconstructionwidth, reconstructionheight, NULL, 3, 1, passformat, GL_TEXTURE_RECTANGLE);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rayupsampletex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                return disable("failed allocating geometry god rays reconstruction buffer");
        }

        if(needfilter)
        {
            loopi(needsecondfilter ? 2 : 1)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, rayfilterfbo[i]);
                createtexture(rayfiltertex[i], bufferwidth, bufferheight, NULL, 3, 1, passformat, GL_TEXTURE_RECTANGLE);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rayfiltertex[i], 0);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    return disable("failed allocating geometry god rays filter buffer");
            }

            glBindFramebuffer_(GL_FRAMEBUFFER, rayguidefbo);
            createtexture(rayguidetex, bufferwidth, bufferheight, NULL, 3, 0, guideformat, GL_TEXTURE_RECTANGLE);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rayguidetex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                return disable("failed allocating geometry god rays depth guide");
        }

        if(debuggrg)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, raydebugfbo);
            createtexture(raydebugtex, bufferwidth, bufferheight, NULL, 3, 1, passformat, GL_TEXTURE_RECTANGLE);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, raydebugtex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                return disable("failed allocating geometry god rays debug snapshot");
        }

        glBindFramebuffer_(GL_FRAMEBUFFER, 0);

        return true;
    }

    static bool ensurebuffers()
    {
        if(vieww <= 0 || viewh <= 0) return false;

        const int targetwidth = max(int(ceilf(vieww*grgscale)), 1), targetheight = max(int(ceilf(viewh*grgscale)), 1);
        const GLenum targetpassformat = hasAFBO && hasTF ? GL_RGBA16F : GL_RGBA8;
        // A scalar float guide preserves linear depth while halving bandwidth versus RGBA16F.
        const GLenum targetguideformat = hasAFBO && hasTF && hasTRG ? GL_R32F : (hasAFBO && hasTF ? GL_RGBA16F : GL_RGBA8);
        const bool needupsample = needsupsamplebuffer(targetwidth, targetheight), needfilter = grgatrous != 0,
                   needsecondfilter = needssecondfilterbuffer(targetwidth, targetheight);

        if(raytex && rayfbo && (!needupsample || (rayupsampletex && rayupsamplefbo)) &&
           (!needfilter || (rayfiltertex[0] && rayfilterfbo[0] && (!needsecondfilter || (rayfiltertex[1] && rayfilterfbo[1])) &&
                            rayguidetex && rayguidefbo)) &&
           (!debuggrg || (raydebugtex && raydebugfbo)) &&
           bufferwidth == targetwidth && bufferheight == targetheight && reconstructionwidth == vieww && reconstructionheight == viewh &&
           passformat == targetpassformat && guideformat == targetguideformat) return true;

        cleanup();

        return setupbuffers(targetwidth, targetheight, targetpassformat, targetguideformat);
    }

    void cleanup()
    {
        cleanupdebugtimer();
        if(rayfbo) { glDeleteFramebuffers_(1, &rayfbo); rayfbo = 0; }
        if(raytex) { glDeleteTextures(1, &raytex); raytex = 0; }
        if(rayupsamplefbo) { glDeleteFramebuffers_(1, &rayupsamplefbo); rayupsamplefbo = 0; }
        if(rayupsampletex) { glDeleteTextures(1, &rayupsampletex); rayupsampletex = 0; }
        loopi(2)
        {
            if(rayfilterfbo[i]) { glDeleteFramebuffers_(1, &rayfilterfbo[i]); rayfilterfbo[i] = 0; }
            if(rayfiltertex[i]) { glDeleteTextures(1, &rayfiltertex[i]); rayfiltertex[i] = 0; }
        }
        if(rayguidefbo) { glDeleteFramebuffers_(1, &rayguidefbo); rayguidefbo = 0; }
        if(rayguidetex) { glDeleteTextures(1, &rayguidetex); rayguidetex = 0; }
        if(raydebugfbo) { glDeleteFramebuffers_(1, &raydebugfbo); raydebugfbo = 0; }
        if(raydebugtex) { glDeleteTextures(1, &raydebugtex); raydebugtex = 0; }

        passformat = guideformat = GL_RGBA8;
        debugcompositetex = 0;
        debugrendered = false;
        bufferwidth = bufferheight = reconstructionwidth = reconstructionheight = -1;
    }

    static float strengthscale()
    {
        return 0.5f + 0.5f*grgglobalstrength;
    }

    static void renderraw(int debugmode, float maxdistance, const vec &suncolor)
    {
        glBindFramebuffer_(GL_FRAMEBUFFER, rayfbo);
        glViewport(0, 0, bufferwidth, bufferheight);
        // The full-screen raymarch shader overwrites every pixel, so no clear is needed.

        glActiveTexture_(GL_TEXTURE0);
        if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
        else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
        glActiveTexture_(GL_TEXTURE2);
        glBindTexture(shadowatlastarget, shadowatlastex);
        if(shadowatlastarget == GL_TEXTURE_2D)
        {
            glTexParameteri(shadowatlastarget, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(shadowatlastarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(shadowatlastarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
        glActiveTexture_(GL_TEXTURE0);

        if(shadowatlastarget == GL_TEXTURE_2D) SETSHADER(geometryGodRays2D);
        else SETSHADER(geometryGodRaysRect);
        LOCALPARAM(sunDir, sunlightdir);
        LOCALPARAM(sunColor, suncolor);
        LOCALPARAMF(godRayDepthScale, float(vieww)/bufferwidth, float(viewh)/bufferheight);
        LOCALPARAMF(godRayGeomParams, max(grgdensity, 0.25f), clamp(grgdecay, 0.0f, 1.0f), maxdistance, max(grgforwardexp, 0.25f));
        LOCALPARAMI(godRayGeomSteps, grgsteps);
        LOCALPARAMI(godRayGeomDebug, debugmode);
        LOCALPARAMF(godRayGeomDistanceParams, grgstrength*strengthscale(), max(grgshaftboost, 0.0f), max(grgdetailboost, 0.0f),
                    max(grgisolationpower, 0.25f));
        LOCALPARAMF(godRayGeomShapeParams, clamp(grgbaseatmosphere, 0.0f, 0.25f), clamp(grgthreshold, 0.0f, 1.0f),
                    max(grgisolationradius, 0.0f), clamp(grgcsmfade, 0.0f, 0.25f));
        LOCALPARAMF(godRayGeomBiasParams, clamp(grgshadowbias, 0.0f, 4.0f), clamp(grgshadowbiasdist, 0.0f, 1.0f));
        LOCALPARAMI(csmcount, csmsplits);
        enddebugpass(GRG_DEBUG_SETUP);

        begindebugpass(GRG_DEBUG_RAYMARCH);
        screenquad();
        enddebugpass(GRG_DEBUG_RAYMARCH);
    }

    static GLuint reconstruct(bool snapshot, bool directcomposite)
    {
        const bool reducedresolution = bufferwidth < vieww || bufferheight < viewh;
        GLuint compositetex = raytex;

        if(grgatrous)
        {
            begindebugpass(GRG_DEBUG_FILTER);
            glBindFramebuffer_(GL_FRAMEBUFFER, rayguidefbo);
            glViewport(0, 0, bufferwidth, bufferheight);
            // The depth-guide shader overwrites the complete ray-resolution attachment.
            glActiveTexture_(GL_TEXTURE0);
            if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
            else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
            if(guideformat == GL_R32F) SETSHADER(geometrygodraysdepthguidefloat);
            else SETSHADER(geometrygodraysdepthguide);
            screenquad(vieww, viewh);

            const int iterations = grgatrousiter;
            loopi(iterations)
            {
                const int targetindex = i&1;
                glBindFramebuffer_(GL_FRAMEBUFFER, rayfilterfbo[targetindex]);
                glViewport(0, 0, bufferwidth, bufferheight);
                glActiveTexture_(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);
                glActiveTexture_(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_RECTANGLE, rayguidetex);
                glActiveTexture_(GL_TEXTURE0);
                if(guideformat == GL_R32F) SETSHADER(geometrygodraysatrousfloat);
                else SETSHADER(geometrygodraysatrous);
                LOCALPARAMF(aTrousSize, float(bufferwidth), float(bufferheight));
                LOCALPARAMF(aTrousParams, float(1<<i), grgatrousalphak, grgatrousdepth, 0.0f);
                screenquad(bufferwidth, bufferheight);
                compositetex = rayfiltertex[targetindex];
            }
            enddebugpass(GRG_DEBUG_FILTER);
        }

        if(snapshot)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, raydebugfbo);
            glViewport(0, 0, bufferwidth, bufferheight);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);
            SETSHADER(geometrygodrayscomposite);
            screenquad(bufferwidth, bufferheight);
        }

        if(reducedresolution)
        {
            begindebugpass(GRG_DEBUG_UPSCALE);
            // The normal pass upscales directly into HDR, avoiding a full-resolution write/read/composite dependency chain.
            glBindFramebuffer_(GL_FRAMEBUFFER, directcomposite ? (msaalight ? mshdrfbo : hdrfbo) : rayupsamplefbo);
            glViewport(0, 0, reconstructionwidth, reconstructionheight);
            if(directcomposite)
            {
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            }
            else
            {
                glDisable(GL_BLEND);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);
            glActiveTexture_(GL_TEXTURE1);
            if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
            else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
            if(grgatrous)
            {
                // The a-trous pass already populated this ray-resolution depth guide;
                // reuse it instead of resampling full-resolution depth four times.
                glActiveTexture_(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_RECTANGLE, rayguidetex);
            }
            glActiveTexture_(GL_TEXTURE0);
            if(grgatrous)
            {
                if(guideformat == GL_R32F) SETSHADER(geometrygodraysupsampleguidefloat);
                else SETSHADER(geometrygodraysupsampleguide);
            }
            else SETSHADER(geometrygodraysupsample);
            LOCALPARAMF(godrayScale, float(vieww)/bufferwidth, float(viewh)/bufferheight, float(bufferwidth)/vieww, float(bufferheight)/viewh);
            LOCALPARAMF(bilateralDepthScale, grgupscaleedge);
            screenquad(bufferwidth, bufferheight, vieww, viewh);
            if(directcomposite)
            {
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDisable(GL_BLEND);
                compositetex = 0;
            }
            else compositetex = rayupsampletex;
            enddebugpass(GRG_DEBUG_UPSCALE);
        }

        return compositetex;
    }

    static void composite(GLuint compositetex)
    {
        begindebugpass(GRG_DEBUG_COMPOSITE);
        glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        glViewport(0, 0, vieww, viewh);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);
        SETSHADER(geometrygodrayscomposite);
        screenquad();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_BLEND);
        enddebugpass(GRG_DEBUG_COMPOSITE);
    }

    static void restoreframebuffer(bool rebind)
    {
        begindebugpass(GRG_DEBUG_RESTORE);
        if(rebind)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
            glViewport(0, 0, vieww, viewh);
        }
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glActiveTexture_(GL_TEXTURE0);
        enddebugpass(GRG_DEBUG_RESTORE);
    }

    void render()
    {
        debugrendered = false;
        if(drawtex || !godraysgeom || !csmshadowmap || !shadowatlastex || csmsplits <= 0 || grgstrength <= 0.0f || grgdensity <= 0.0f ||
           grgsteps <= 0 || grgmaxdist <= 0.0f)
            return;

        const bool customcolour = _grgcolour != 0;
        if((!customcolour && sunlight.iszero()) || sunlightscale <= 0.0f || sunlightdir.z <= 1.0e-4f || !ensurebuffers())
            return;

        vec suncolor = (customcolour ? grgcolour.tocolor() : sunlight.tocolor()).mul(max(sunlightscale, 0.0f)).mul(ldrscale * 2.0f);

        if(suncolor.squaredlen() <= 1.0e-8f) return;

        const float maxdistance = clamp(float(farplane)*max(grgmaxdist, 0.01f), 1.0f, float(farplane));
        timer *raytimer = debuggrg ? NULL : begintimer("geometry god rays");
        begindebugtimer();
        begindebugpass(GRG_DEBUG_SETUP);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDepthMask(GL_FALSE);

        renderraw(0, maxdistance, suncolor);
        GLuint compositetex = reconstruct(false, true);
        if(compositetex) composite(compositetex);
        restoreframebuffer(false);
        enddebugtimer();
        endtimer(raytimer);

        if(debuggrg)
        {
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glDepthMask(GL_FALSE);
            renderraw(debuggrg, maxdistance, suncolor);
            debugcompositetex = reconstruct(true, false);
            debugrendered = true;
            restoreframebuffer(true);
        }
    }

    bool debugview()
    {
        if(!debuggrg) return false;

        polldebugtimer();
        if(!debugrendered || !raytex || !raydebugtex || !debugcompositetex || bufferwidth <= 0 || bufferheight <= 0)
        {
            draw_text("geometry god rays inactive", 0, 0);
            return true;
        }

        static const char * const modelabels[7] =
        {
            "raw visibility", "base atmosphere", "isolated shaft + detail accent", "final volumetric signal",
            "CSM coverage (green inside, red outside)", "shadow bias (0 blue, 1 green, 2+ red)", "local isolation mask"
        };
        const char *stagelabels[3] = { "raw raymarch", grgatrous ? "a-trous filtered" : "raymarch source", "final reconstruction" };
        GLuint textures[3] = { raytex, raydebugtex, debugcompositetex };
        int widths[3] = { bufferwidth, bufferwidth, reconstructionwidth };
        int heights[3] = { bufferheight, bufferheight, reconstructionheight };
        const int statsheight = (2 + GRG_DEBUG_PASS_COUNT) * FONTH;
        int gap = FONTH, tilew = max((hudw - 2*gap) / 3, 1);
        int tileh = max(int(ceilf(tilew * float(viewh) / max(float(vieww), 1.0f))), 1);

        gle::colorf(1, 1, 1);
        loopi(3)
        {
            int x = i * (tilew + gap);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, textures[i]);
            SETSHADER(hudrect);
            debugquad(x, statsheight, tilew, tileh, 0, 0, widths[i], heights[i]);
            draw_text(stagelabels[i], x, statsheight + tileh + FONTH/4);
        }
        draw_textf("geometry god rays debug %d: %s", 0, 0, debuggrg, modelabels[debuggrg - 1]);
        if(grgdebugms >= 0.0f) draw_textf("geometry god rays %.3f ms", 0, FONTH, grgdebugms);
        else draw_text("geometry god rays n/a", 0, FONTH);
        loopi(GRG_DEBUG_PASS_COUNT)
        {
            if(grgdebugpassms[i] >= 0.0f) draw_textf("%s %.3f ms", 0, (2 + i) * FONTH, grgdebugpassnames[i], grgdebugpassms[i]);
            else draw_textf("%s n/a", 0, (2 + i) * FONTH, grgdebugpassnames[i]);
        }
        return true;
    }
}
}
