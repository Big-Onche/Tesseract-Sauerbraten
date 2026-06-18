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
    FVARP(godrayclamp, 0.0f, 0.3f, 1.0f);
    FVARP(godrayskyclamp, 0.0f, 0.15f, 1.0f);
    FVARP(godrayskymax, 0.0f, 0.25f, 1.0f);
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
                GLenum godrayformat = hasTF ? GL_RGBA16F : GL_RGBA8;
                createtexture(tex, w, h, NULL, 3, 1, godrayformat, GL_TEXTURE_RECTANGLE);
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
