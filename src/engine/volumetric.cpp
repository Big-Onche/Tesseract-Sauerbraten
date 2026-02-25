#include "engine.h"

namespace vclouds
{
    GLuint vctex = 0, vcfbo = 0;
    GLuint vcbilateraltex = 0, vcbilateralfbo = 0;
    GLuint vcshadowtex = 0, vcshadowfbo = 0;
    int vcw = 0, vch = 0, vcfullw = 0, vcfullh = 0;
    int vcshadowsz = 0;

    VARP(volumetricclouds, 0, 1, 1);
    VARP(vcblur, 0, 1, 1);
    VARP(vcblurscale, 1, 1, 4);
    FVARR(vcscale, 0.25f, 0.5f, 2.0f);
    FVARR(vcbilateraledge, 1e-5f, 0.02f, 1.0f);
    VARR(vcdensity, 0, 100, 200);
    FVARR(vcalpha, 0.0f, 0.75f, 1.0f);
    VARR(vcheight, 0, 80, 100);
    VARR(vcthickness, 0, 20, 100);
    FVARR(vcdarkness, 0.0f, 4.0f, 8.0f);
    VARR(vcshadow, 0, 1, 1);
    VARP(vcshadowmapsize, 64, 512, 2048);
    FVARR(vcshadowstrength, 0.0f, 0.65f, 1.0f);
    VARR(vcshadowsamples, 1, 4, 8);
    VARR(vcshadowpcf, 0, 1, 2);
    VARR(vcshadowdebug, 0, 0, 1);
    CVARR(vccolour, 0xFFFFFF);

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
    }

    static void viewshadowmap()
    {
        if(!vcshadowdebug || !vcshadowtex || !vcshadowsz) return;

        int w = max(min(vieww, viewh)/3, 64), h = w;
        int x = 0, y = 0;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        SETSHADER(hudrect);
        gle::colorf(1, 1, 1);
        glBindTexture(GL_TEXTURE_RECTANGLE, vcshadowtex);
        debugquad(x, y, w, h, 0, 0, vcshadowsz, vcshadowsz);

        SETSHADER(hudnotexture);
        gle::colorf(1.0f, 0.2f, 0.2f);
        float cx = x + w*0.5f, cy = y + h*0.5f;
        float t = max(w / 256.0f, 1.0f);
        debugquad(cx - 0.5f*t, y, t, h, 0, 0, 1, 1);
        debugquad(x, cy - 0.5f*t, w, t, 0, 0, 1, 1);

        gle::colorf(1, 1, 1);
        glDisable(GL_BLEND);
    }

    void init()
    {
        if(!volumetricclouds) return;
        useshaderbyname("volumetricclouds");
        useshaderbyname("volumetriccloudsbilateral");
        useshaderbyname("volumetriccloudshadowmap");
        useshaderbyname("volumetriccloudshadowapply");
        useshaderbyname("scalelinear");
    }

    void render()
    {
        if(!volumetricclouds) return;

        Shader *cloudshader = useshaderbyname("volumetricclouds");
        Shader *bilateralshader = useshaderbyname("volumetriccloudsbilateral");
        Shader *shadowmapshader = vcshadow ? useshaderbyname("volumetriccloudshadowmap") : NULL;
        Shader *shadowapplyshader = vcshadow ? useshaderbyname("volumetriccloudshadowapply") : NULL;
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

        if((!vcshadow || !shadowmapshader || !shadowapplyshader || shadowstrength <= 1e-4f) && (vcshadowtex || vcshadowfbo))
            cleanupshadowmap();

        if(!vctex)
        {
            glGenTextures(1, &vctex);
            createtexture(vctex, vcw, vch, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }
        if(!vcbilateraltex)
        {
            glGenTextures(1, &vcbilateraltex);
            createtexture(vcbilateraltex, vieww, viewh, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
        }

        if(!vcfbo)
        {
            glGenFramebuffers_(1, &vcfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vctex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud buffer!");
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

        float ws = max(float(worldsize), 1.0f);
        float cloudmid = ws * (vcheight / 100.0f);
        float halfthickness = 0.5f * ws * (vcthickness / 100.0f);
        float base = max(cloudmid - halfthickness, 0.0f);
        float top = min(cloudmid + halfthickness, ws);
        if(top <= base + 1.0f) top = base + 1.0f;
        if(top > ws)
        {
            top = ws;
            base = max(top - 1.0f, 0.0f);
        }

        GLOBALPARAMF(tvcloudbounds, base, top, max(float(farplane), ws), lastmillis / 1000.0f);
        GLOBALPARAMF(tvcloudnoise, 1.0f / max(ws * 0.18f, 1.0f), 1.0f / max(ws * 0.06f, 1.0f), 0.50f, 0.95f);
        GLOBALPARAMF(tvcloudscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
        GLOBALPARAMF(vclouddensity, float(vcdensity) / 100.0f);
        GLOBALPARAMF(vcloudalpha, vcalpha);
        GLOBALPARAMF(vcloudthickness, vcdarkness);
        GLOBALPARAM(vcloudcolour, vccolour.tocolor());
        GLOBALPARAM(sunlightdir, sunlightdir);

        glBindFramebuffer_(GL_FRAMEBUFFER, vcfbo);
        glViewport(0, 0, vcw, vch);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        cloudshader->set();
        screenquad(vcw, vch);

        GLuint compositetex = vctex;
        int compositetexw = vcw, compositetexh = vch;

        if(vcblur && bilateralshader)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateralfbo);
            glViewport(0, 0, vieww, viewh);
            glDisable(GL_BLEND);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);

            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, vctex);
            GLOBALPARAMF(tvbilateraldepthscale, 1.0f / max(float(farplane) * vcbilateraledge, 1e-4f));
            GLOBALPARAMF(vcblurscale, float(vcblurscale));
            bilateralshader->set();
            screenquad(vieww, viewh);

            compositetex = vcbilateraltex;
            compositetexw = vieww;
            compositetexh = viewh;
        }

        if(vcshadow && vcshadowtex && vcshadowfbo && shadowmapshader && shadowapplyshader && shadowstrength > 1e-4f)
        {
            float shadowworld = max(float(worldsize) * 2.0f, 1.0f);
            float worldpertexel = shadowworld / max(float(vcshadowsz), 1.0f);
            float snappedx = floorf(camera1->o.x / worldpertexel) * worldpertexel;
            float snappedy = floorf(camera1->o.y / worldpertexel) * worldpertexel;
            float minx = snappedx - shadowworld * 0.5f;
            float miny = snappedy - shadowworld * 0.5f;
            float cloudmidz = (base + top) * 0.5f;

            GLOBALPARAMF(tvshadowmapworld, minx, miny, worldpertexel, float(vcshadowsz));
            GLOBALPARAMF(tvcloudshadowsamples, float(vcshadowsamples));

            glBindFramebuffer_(GL_FRAMEBUFFER, vcshadowfbo);
            glViewport(0, 0, vcshadowsz, vcshadowsz);
            glDisable(GL_BLEND);
            glClearColor(1, 1, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
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

        viewshadowmap();

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
        cleanupshadowmap();
        vcw = vch = vcfullw = vcfullh = 0;
    }
}

