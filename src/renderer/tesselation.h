#ifndef TESSELATE_RENDERER_TESSELATION_H
#define TESSELATE_RENDERER_TESSELATION_H

#include "engine.h"

namespace renderer
{
    namespace tesselation
    {
        void init();
        void cleanup();
        bool enabled();
        Shader *worldshader(Shader *base);
        void beginworld();
        void endworld();
        void setfactor();
        bool beginwireframe();
        void endwireframe();
    }
}

#endif
