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
        bool eligible(const Slot &slot, const ivec &bbmin, const ivec &bbmax);
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
        void beginremesh(vector<vertex> &verts);
        bool remesh(VSlot &vslot, vector<vertex> &verts, vector<ushort> &indices, int maxindices);
        void endremesh(vector<vertex> &verts);
        void resetremeshstats();
        void printremeshstats();
    }
}

#endif
