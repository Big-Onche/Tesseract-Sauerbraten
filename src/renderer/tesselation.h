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
        bool shadowsenabled();
        Shader *worldshader(Slot &slot);
        Shader *shadowshader(Slot &slot, bool transparent = false);
        bool hasshadowcasters();
        float shadowmargin();
        void beginworld();
        void endworld();
        void beginshadow();
        void endshadow();
        void setparams(Slot &slot, VSlot &vslot);
        bool beginwireframe();
        void endwireframe();
    }
}

#endif
