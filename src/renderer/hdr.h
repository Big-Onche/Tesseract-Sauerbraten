#ifndef TESSELATE_RENDERER_HDR_H
#define TESSELATE_RENDERER_HDR_H

#include "renderer.h"

namespace renderer
{
    extern int bloom, tonemap, debug, eyeadaptation, eyeadaptationdebug;
    extern float bloomstrength, bloomthreshold, bloomsoftknee, exposure, eyeadaptsensitivity, eyeadaptbrightenspeed,
                 eyeadaptdarkenspeed, eyeadaptminexposure, eyeadaptmaxexposure;

    bool preparehdr(int width, int height, bool needbloom);
    bool renderbloom(GLuint hdrtex, int width, int height);
    bool combinehdr(GLuint hdrtex, int width, int height);
    bool tonemaphdr(GLuint outfbo, int width, int height, int aatype);
    bool updateeyeadaptation(GLuint hdrtex, int width, int height);
    void cleanupbloom();
    void cleanupeyeadaptation();
    void cleanuptonemap();
    void reseteyeadaptation();

    GLuint bloombright();
    GLuint bloommip(int level);
    GLuint eyeadaptationtexture();
    bool eyeadaptationready();
    int bloomwidth(int level);
    int bloomheight(int level);
    int brightwidth();
    int brightheight();
}

#endif
