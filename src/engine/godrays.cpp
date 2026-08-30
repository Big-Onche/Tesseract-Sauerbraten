// godrays.cpp: dedicated screen-space cloud crepuscular rays, separate from world volumetric lights

#include "engine.h"

extern GLuint hdrfbo, mshdrfbo;

namespace godrays
{
namespace crepuscularrays
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
