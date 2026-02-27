#include "engine.h"

extern GLuint shadowatlastex;
extern GLenum shadowatlastarget;
extern int csmshadowmap, csmsplits;

namespace godrays
{
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;

    // graphic settings
    VARP(godrays, 0, 1, 1);
    VARP(godraysteps, 2, 32, 128);
    FVARP(godrayscale, 0.f, 0.25f, 2.f);
    FVARP(godrayhorizonboost, 0.0f, 0.0f, 1.0f);
    FVARP(godrayclamp, 0.0f, 0.5f, 8.0f);
    FVARP(godrayskyclamp, 0.0f, 0.3f, 1.0f);
    FVARP(godrayskymax, 0.0f, 0.32f, 1.0f);
    FVARP(godraygeomshadow, 0.0f, 1.0f, 1.0f);

    // map settings
    FVARR(godraystrength, 0.0f, 4.0f, 8.0f);
    FVARR(godraydensity, 0.0f, 4.0f, 8.0f);
    FVARR(godraydist, 0.1f, 1.25f, 8.0f);

    static void cleanupbuffer()
    {
        if(fbo)
        {
            glDeleteFramebuffers_(1, &fbo);
            fbo = 0;
        }
        if(tex)
        {
            glDeleteTextures(1, &tex);
            tex = 0;
        }
        w = h = 0;
    }

    void init()
    {
        useshaderbyname("godrays");
    }

    float scaledstrength(int cloudalpha, int cloudamount)
    {
        float cloudamountk = clamp(float(cloudamount) / 100.0f, 0.0f, 1.0f);
        // Keep a subtle floor at clear skies, then ramp toward full shafts as overcast increases.
        float godrayamountk = 0.20f + 0.80f * cloudamountk;
        return godraystrength * (cloudalpha * 0.01f) * godrayamountk;
    }

    bool shouldrender(Shader *shader, float strengthscaled)
    {
        return shader && strengthscaled > 1e-4f;
    }

    void render(Shader *shader, Shader *upscaleshader, GLuint shadowtex, float worldscale, float maxclouddist, float bilateraledge, float strengthscaled)
    {
        if(!shouldrender(shader, strengthscaled) || !shadowtex) return;

        float godraymaxdist = min(max(godraydist, 0.1f) * worldscale, maxclouddist);
        float godrayext = max(godraydensity, 0.0f) / max(worldscale, 1.0f);
        float godrayhboost = max(godrayhorizonboost, 0.0f);
        float godrayclampcfg = max(godrayclamp, 0.0f);
        float godrayskyclampcfg = max(godrayskyclamp, 0.0f);
        float godrayskymaxcfg = max(godrayskymax, 0.0f);
        int godraycsmsplits = (!sunlight.iszero() && csmshadowmap && csmsplits > 0 && shadowatlastex && shadowatlastarget == GL_TEXTURE_RECTANGLE) ? csmsplits : 0;
        float godraygeom = godraycsmsplits > 0 ? clamp(godraygeomshadow, 0.0f, 1.0f) : 0.0f;
        float godrayscalefactor = godrayscale > 1e-4f ? godrayscale : 1.0f;
        int targetw = max(int(ceilf(vieww * godrayscalefactor)), 1);
        int targeth = max(int(ceilf(viewh * godrayscalefactor)), 1);
        bool rescaledgodrays = targetw != vieww || targeth != viewh;

        GLOBALPARAMF(godrayparams2, godrayhboost, godrayclampcfg, godrayskyclampcfg, godrayskymaxcfg);
        GLOBALPARAMI(vccsmsplits, godraycsmsplits);
        GLOBALPARAMF(vcgeomshadow, godraygeom);
        if(godraycsmsplits > 0)
        {
            glActiveTexture_(GL_TEXTURE1);
            glBindTexture(shadowatlastarget, shadowatlastex);
            glTexParameteri(shadowatlastarget, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(shadowatlastarget, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            glActiveTexture_(GL_TEXTURE0);
        }

        if(rescaledgodrays)
        {
            if(targetw != w || targeth != h) cleanupbuffer();
            if(!tex)
            {
                w = targetw;
                h = targeth;
                glGenTextures(1, &tex);
                createtexture(tex, w, h, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
            }
            if(!fbo)
            {
                glGenFramebuffers_(1, &fbo);
                glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, tex, 0);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud godrays buffer!");
            }

            glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, w, h);
            glDisable(GL_BLEND);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);

            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, shadowtex);
            GLOBALPARAMF(vcscale, float(vieww)/w, float(viewh)/h, float(w)/vieww, float(h)/viewh);
            GLOBALPARAMF(godrayparams, strengthscaled, godraymaxdist, godrayext, float(godraysteps));
            shader->set();
            screenquad(w, h);

            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
            glViewport(0, 0, vieww, viewh);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, tex);
            GLOBALPARAMF(vcscale, float(vieww)/w, float(viewh)/h, float(w)/vieww, float(h)/viewh);

            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            if(upscaleshader)
            {
                GLOBALPARAMF(vcbilateraldepthscale, 1.0f / max(float(farplane) * bilateraledge, 1e-4f));
                upscaleshader->set();
                screenquad(vieww, viewh);
            }
            else
            {
                SETSHADER(scalelinear);
                screenquad(w, h);
            }
            glDisable(GL_BLEND);
        }
        else
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
            glViewport(0, 0, vieww, viewh);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, shadowtex);
            GLOBALPARAMF(vcscale, 1.0f, 1.0f, 1.0f, 1.0f);
            GLOBALPARAMF(godrayparams, strengthscaled, godraymaxdist, godrayext, float(godraysteps));

            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            shader->set();
            screenquad(vieww, viewh);
            glDisable(GL_BLEND);
        }
    }

    void cleanup()
    {
        cleanupbuffer();
    }
}

namespace vclouds
{
    GLuint vctex = 0, vcfbo = 0;
    GLuint vcbilateraltex = 0, vcbilateralfbo = 0;
    GLuint vcbilateraltemptex = 0, vcbilateraltempfbo = 0;
    GLuint vcshadowtex = 0, vcshadowfbo = 0;
    int vcw = 0, vch = 0, vcfullw = 0, vcfullh = 0;
    int vcshadowsz = 0;

    // graphic settings
    VARP(volumetricclouds, 0, 1, 1);
    VARP(vcblur, 0, 1, 1);
    VARP(vcblurscale, 1, 1, 4);
    FVARP(vcscale, 0.1f, 0.25f, 2.0f);
    FVARP(vcbilateraledge, 1e-5f, 0.02f, 1.0f);
    VARP(vcsteps, 4, 16, 128);
    VARP(vcsunsteps, 2, 4, 64);
    VARP(vcstreuse, 1, 4, 16);
    FVARP(vcstrecalc, 0.0f, 8e-4f, 0.1f);
    VARP(vcshadow, 0, 1, 1);
    VARP(vcshadowmapsize, 64, 512, 2048);
    VARP(vcshadowsamples, 1, 4, 8);
    VARP(vcshadowpcf, 0, 1, 2);
    FVAR(vcphaseg, -0.95f, 0.55f, 0.95f);
    FVARP(vcphaseg2, -0.95f, -0.30f, 0.95f);
    FVARP(vcphasew, 0.0f, 0.22f, 1.0f);
    VARP(vcmsoctaves, 1, 2, 6);
    FVARP(vcmsscatter, 0.0f, 0.65f, 1.0f);
    FVARP(vcmsextinction, 0.0f, 0.82f, 1.0f);
    FVARP(vcmsphase, 0.25f, 1.12f, 4.0f);
    FVARP(vcscattereps, 1e-7f, 1e-4f, 1e-2f);

    // map settings
    VARR(vcdensity, 0, 70, 100);
    VARR(vcalpha, 0, 80, 100);
    VARR(vcheight, 0, 80, 100);
    VARR(vcthickness, 0, 20, 100);
    VARR(vcdome, -100, 0, 100);
    VARR(vcscrollx, -1000, 0, 1000);
    VARR(vcscrolly, -1000, 0, 1000);
    VARR(vcskyinherit, 0, 80, 100);
    VARR(vcshadowinherit, 0, 30, 100);
    VARR(vcamount, 0, 50, 100);
    FVARR(vcshadowstrength, 0.0f, 0.65f, 1.0f);
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

    void init()
    {
        if(!volumetricclouds) return;
        useshaderbyname("volumetricclouds");
        useshaderbyname("volumetriccloudsupscale");
        useshaderbyname("volumetriccloudsbilateral");
        useshaderbyname("volumetriccloudshadowmap");
        useshaderbyname("volumetriccloudshadowapply");
        godrays::init();
        useshaderbyname("scalelinear");
    }

    void render()
    {
        if(!volumetricclouds) return;

        Shader *cloudshader = useshaderbyname("volumetricclouds");
        Shader *upscaleshader = useshaderbyname("volumetriccloudsupscale");
        Shader *bilateralshader = useshaderbyname("volumetriccloudsbilateral");
        Shader *shadowmapshader = vcshadow ? useshaderbyname("volumetriccloudshadowmap") : NULL;
        Shader *shadowapplyshader = vcshadow ? useshaderbyname("volumetriccloudshadowapply") : NULL;
        Shader *godraysshader = (vcshadow && godrays::godrays) ? useshaderbyname("godrays") : NULL;
        float shadowstrength = vcshadowstrength * (vcalpha * 0.01f);
        float godraystrengthscaled = godrays::scaledstrength(vcalpha, vcamount);
        bool doshadowapply = shadowapplyshader && shadowstrength > 1e-4f;
        bool dogodrays = godrays::shouldrender(godraysshader, godraystrengthscaled);
        bool needshadowmap = shadowmapshader && (doshadowapply || dogodrays);
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

        if(!needshadowmap && (vcshadowtex || vcshadowfbo))
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
        if(!vcbilateraltemptex)
        {
            glGenTextures(1, &vcbilateraltemptex);
            createtexture(vcbilateraltemptex, vieww, viewh, NULL, 3, 1, GL_RGBA8, GL_TEXTURE_RECTANGLE);
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
        if(!vcbilateraltempfbo)
        {
            glGenFramebuffers_(1, &vcbilateraltempfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateraltempfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vcbilateraltemptex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating volumetric cloud bilateral temp buffer!");
            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        }
        if(needshadowmap)
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

        float maxclouddist = max(float(farplane), ws);
        // Dome coefficient is in world-z units per squared world-xy distance.
        // Positive vcdome bends the layer downward toward the horizon.
        float domek = -float(vcdome) * (ws / max(maxclouddist * maxclouddist, 1.0f)) / 100.0f;

        GLOBALPARAMF(vcbounds, base, top, maxclouddist, lastmillis / 1000.0f);
        GLOBALPARAMF(vcdome, domek, camera1->o.x, camera1->o.y, 0.0f);
        GLOBALPARAMF(vcnoise, 1.0f / max(ws * 0.18f, 1.0f), 1.0f / max(ws * 0.06f, 1.0f), 0.50f, 0.95f);
        GLOBALPARAMF(vcscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
        GLOBALPARAMF(vcdensity, float(vcamount) / 50.0f); // clearly need to update the shader VARS!
        GLOBALPARAMF(vcalpha, float(vcalpha) / 100.0f);
        GLOBALPARAMF(vcthickness, float(vcdensity) / 100.0f);
        GLOBALPARAMF(vcphaseg, vcphaseg);
        GLOBALPARAMF(vcphaseg2, vcphaseg2);
        GLOBALPARAMF(vcphasew, vcphasew);
        float msscatter = clamp(vcmsscatter, 0.0f, 1.0f);
        // Keep a <= b for the multi-scatter approximation to stay energy stable.
        float msextinction = max(clamp(vcmsextinction, 0.0f, 1.0f), msscatter);
        GLOBALPARAMF(vcmultiscat, float(vcmsoctaves), msscatter, msextinction, vcmsphase);
        GLOBALPARAMF(vcscattereps, vcscattereps);
        GLOBALPARAMF(vcsteps, float(vcsteps));
        GLOBALPARAMF(vcsunsteps, float(vcsunsteps));
        GLOBALPARAMF(vcsunreuse, float(vcstreuse));
        GLOBALPARAMF(vcsunrecalc, vcstrecalc);
        GLOBALPARAMF(vcscroll, float(vcscrollx), float(vcscrolly));
        vec skycube[6];
        vec2 skycubefront;
        getskycubetints(skycube, skycubefront);
        GLOBALPARAM(vcskycubelf, skycube[0]);
        GLOBALPARAM(vcskycubert, skycube[1]);
        GLOBALPARAM(vcskycubebk, skycube[2]);
        GLOBALPARAM(vcskycubeft, skycube[3]);
        GLOBALPARAM(vcskycubedn, skycube[4]);
        GLOBALPARAM(vcskycubeup, skycube[5]);
        GLOBALPARAMF(vcskycubefront, skycubefront.x, skycubefront.y);
        GLOBALPARAMF(vcskyinherit, clamp(float(vcskyinherit) / 100.0f, 0.0f, 1.0f));
        GLOBALPARAMF(vcshadowinherit, clamp(float(vcshadowinherit) / 100.0f, 0.0f, 1.0f));
        vec cloudsuncolor = sunlight.tocolor().mul(sunlightscale);
        GLOBALPARAM(vcsunlightcolor, cloudsuncolor);
        GLOBALPARAM(vccolour, vccolour.tocolor());
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
            GLOBALPARAMF(tvbilateraldepthscale, 1.0f / max(float(farplane) * vcbilateraledge, 1e-4f));
            GLOBALPARAMF(vcblurscale, float(vcblurscale));

            // Pass 1: horizontal bilateral blur + upscale from low-res cloud buffer.
            glBindFramebuffer_(GL_FRAMEBUFFER, vcbilateraltempfbo);
            glViewport(0, 0, vieww, viewh);
            glDisable(GL_BLEND);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);

            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_RECTANGLE, vctex);
            GLOBALPARAMF(vcscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
            GLOBALPARAMF(vcblurdir, 1.0f, 0.0f);
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
            GLOBALPARAMF(vcscale, 1.0f, 1.0f, 1.0f, 1.0f);
            GLOBALPARAMF(vcblurdir, 0.0f, 1.0f);
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
            glBindTexture(GL_TEXTURE_RECTANGLE, vctex);
            GLOBALPARAMF(vcscale, float(vieww)/vcw, float(viewh)/vch, float(vcw)/vieww, float(vch)/viewh);
            upscaleshader->set();
            screenquad(vieww, viewh);

            compositetex = vcbilateraltex;
            compositetexw = vieww;
            compositetexh = viewh;
        }

        if(needshadowmap && vcshadowtex && vcshadowfbo)
        {
            float shadowworld = max(float(worldsize) * 2.0f, 1.0f);
            float worldpertexel = shadowworld / max(float(vcshadowsz), 1.0f);
            float snappedx = floorf(camera1->o.x / worldpertexel) * worldpertexel;
            float snappedy = floorf(camera1->o.y / worldpertexel) * worldpertexel;
            float minx = snappedx - shadowworld * 0.5f;
            float miny = snappedy - shadowworld * 0.5f;
            float cloudmidz = (base + top) * 0.5f;

            GLOBALPARAMF(vcshadowmapworld, minx, miny, worldpertexel, float(vcshadowsz));
            GLOBALPARAMF(vcshadowsamples, float(vcshadowsamples));

            glBindFramebuffer_(GL_FRAMEBUFFER, vcshadowfbo);
            glViewport(0, 0, vcshadowsz, vcshadowsz);
            glDisable(GL_BLEND);
            glClearColor(1, 1, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            shadowmapshader->set();
            screenquad(vcshadowsz, vcshadowsz);

            glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
            glViewport(0, 0, vieww, viewh);
            GLOBALPARAMF(vcshadowparams, shadowstrength, cloudmidz, 0.08f, 0.20f);
            GLOBALPARAMF(vcshadowpcf, float(vcshadowpcf));

            if(doshadowapply)
            {
                glActiveTexture_(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_RECTANGLE, vcshadowtex);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ZERO, GL_SRC_COLOR);
                shadowapplyshader->set();
                screenquad(vieww, viewh);
                glDisable(GL_BLEND);
            }

            if(dogodrays)
            {
                godrays::render(godraysshader, upscaleshader, vcshadowtex, ws, maxclouddist, vcbilateraledge, godraystrengthscaled);
            }
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
        godrays::cleanup();
        cleanupshadowmap();
        vcw = vch = vcfullw = vcfullh = 0;
    }
}
