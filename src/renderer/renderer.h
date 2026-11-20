#ifndef TESSELATE_RENDERER_H
#define TESSELATE_RENDERER_H

#include "engine.h"

namespace renderer
{
    bool processhdr(GLuint hdrtex, int width, int height, GLuint outfbo, int aatype);
    void cleanup();
}

#endif
