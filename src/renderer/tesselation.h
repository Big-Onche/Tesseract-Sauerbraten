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
        bool displacing();
        Shader *worldshader(Slot &slot);
        void beginworld();
        void endworld();
        void setparams(Slot &slot, VSlot &vslot);
        bool beginwireframe();
        void endwireframe();
    }
}

#endif
