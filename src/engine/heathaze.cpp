// heathaze.cpp: heat haze effects (/doc/Heat Haze.md)

#include "engine.h"

extern GLuint hdrfbo, refractfbo, refracttex;
extern GLuint mshdrfbo, msrefractfbo, msrefracttex;

namespace heatHaze
{
    VARP(heathaze, 0, 1, 1);
    FVARP(heathazestrength, 0, 32.0f, 64.0f);
    FVARP(heathazenearstrength, 0, 1.0f, 16.0f);
    FVARP(heathazefarstrength, 0, 1.0f, 16.0f);
    FVARP(heathazenear, 0, 64.0f, 1e4f);
    FVARP(heathazefar, 1, 512.0f, 2e4f);
    FVARP(heathazedepthscale, 1e-3f, 0.1f, 1e3f);
    FVARP(heathazescrollx, -10.0f, 0.1f, 10.0f);
    FVARP(heathazescrolly, -10.0f, 0.5f, 10.0f);

    bool shouldRender()
    {
        return heathaze && heathazestrength > 0 && (heathazenearstrength > 0 || heathazefarstrength > 0);
    }

    bool bindSceneTexture()
    {
        if(!shouldRender()) return false;

        if(msaalight)
        {
            if(!mshdrfbo || !msrefractfbo || !msrefracttex) return false;
        }
        else
        {
            if(!hdrfbo || !refractfbo || !refracttex) return false;
        }

        // The transparent pass can leave scissor enabled for particle layers.
        // Ensure we always copy the full resolved scene into the haze source.
        bool hadScissor = glIsEnabled(GL_SCISSOR_TEST) != 0;
        if(hadScissor) glDisable(GL_SCISSOR_TEST);

        if(msaalight)
        {
            glBindFramebuffer_(GL_READ_FRAMEBUFFER, mshdrfbo);
            glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, msrefractfbo);
            glBlitFramebuffer_(0, 0, vieww, viewh, 0, 0, vieww, viewh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer_(GL_FRAMEBUFFER, mshdrfbo);
            glActiveTexture_(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msrefracttex);
        }
        else
        {
            glBindFramebuffer_(GL_READ_FRAMEBUFFER, hdrfbo);
            glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, refractfbo);
            glBlitFramebuffer_(0, 0, vieww, viewh, 0, 0, vieww, viewh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer_(GL_FRAMEBUFFER, hdrfbo);
            glActiveTexture_(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_RECTANGLE, refracttex);
        }

        glActiveTexture_(GL_TEXTURE0);
        if(hadScissor) glEnable(GL_SCISSOR_TEST);
        return true;
    }

    void setShaderParams(bool scroll, bool fade)
    {
        float distSpan = max(heathazefar - heathazenear, 1e-3f);
        LOCALPARAMF(heatHazeParams, max(heathazestrength, 0.0f), max(heathazedepthscale, 1e-3f), heathazenear, 1.0f/distSpan);
        LOCALPARAMF(heatHazeStrengths, max(heathazenearstrength, 0.0f), max(heathazefarstrength, 0.0f));
        float scrollTime = lastmillis*0.001f;
        float scrollX = scroll ? fmodf(heathazescrollx*scrollTime, 1.0f) : 0.0f;
        float scrollY = scroll ? fmodf(heathazescrolly*scrollTime, 1.0f) : 0.0f;
        LOCALPARAMF(heatHazeScroll, scrollX, scrollY);
        LOCALPARAMF(heatHazeOptions, fade ? 1.0f : 0.0f, 0, 0, 0);
    }
}
