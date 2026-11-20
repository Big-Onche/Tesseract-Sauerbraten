#ifndef TESSELATE_RENDERER_HDR_H
#define TESSELATE_RENDERER_HDR_H

#include "renderer.h"

namespace renderer
{
    extern int bloom, tonemap, debug;
    extern float bloomstrength, bloomthreshold, bloomsoftknee, exposure;

    bool preparehdr(int width, int height, bool needbloom);
    bool renderbloom(GLuint hdrtex, int width, int height);
    bool combinehdr(GLuint hdrtex, int width, int height);
    bool tonemaphdr(GLuint outfbo, int width, int height, int aatype);
    void cleanupbloom();
    void cleanuptonemap();

    GLuint bloombright();
    GLuint bloommip(int level);
    int bloomwidth(int level);
    int bloomheight(int level);
    int brightwidth();
    int brightheight();
}

#endif
