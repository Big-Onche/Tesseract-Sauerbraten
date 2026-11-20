#include "hdr.h"
#include "reflections.h"

namespace renderer
{
    VARP(bloom, 0, 1, 1);
    FVARP(bloomstrength, 0.0f, 2.0f, 8.0f);
    FVARP(bloomthreshold, 0.0f, 1.0f, 64.0f);
    FVARP(bloomsoftknee, 0.0f, 0.5f, 1.0f);
    VARP(tonemap, 0, 2, 7);
    FVARP(exposure, 0.01f, 1.0f, 16.0f);
    VARFP(eyeadaptation, 0, 1, 1, reseteyeadaptation());
    FVARP(eyeadaptsensitivity, 0.001f, 0.18f, 4.0f);
    FVARP(eyeadaptbrightenspeed, 0.01f, 1.0f, 20.0f);
    FVARP(eyeadaptdarkenspeed, 0.01f, 3.0f, 20.0f);
    FVARP(eyeadaptminexposure, 0.01f, 0.25f, 16.0f);
    FVARP(eyeadaptmaxexposure, 0.01f, 4.0f, 64.0f);
    VARP(eyeadaptationdebug, 0, 0, 1);
    // 0 final, 1 raw HDR, 2 mask, 3-7 bloom mips, 8 reconstructed bloom.
    VARP(debug, 0, 0, 8);

    bool processhdr(GLuint hdrtex, int width, int height, GLuint outfbo, int aatype)
    {
        if(width <= 0 || height <= 0 || !preparehdr(width, height, bloom || (debug >= 2 && debug <= 8))) return false;

        if(eyeadaptation && !updateeyeadaptation(hdrtex, width, height)) return false;

        if(bloom || (debug >= 2 && debug <= 8))
        {
            if(!renderbloom(hdrtex, width, height)) return false;
        }

        if(!combinehdr(hdrtex, width, height)) return false;
        return tonemaphdr(outfbo, width, height, aatype);
    }

    void cleanup()
    {
        reflections::cleanup();
        cleanupbloom();
        cleanupeyeadaptation();
        cleanuptonemap();
    }
}
