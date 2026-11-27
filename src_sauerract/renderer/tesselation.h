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
        bool conforming();
        bool displacing();
        bool shadowsenabled();
        bool eligible(const VSlot &vslot, const VSlot *layer, int layermask, const ivec &bbmin, const ivec &bbmax);
        Shader *worldshader(Slot &slot);
        Shader *shadowshader(VSlot &vslot, VSlot *layer, int layermask, bool transparent = false);
        bool hasshadowcasters();
        float shadowmargin();
        float displacementmargin();
        void beginworld();
        void endworld();
        void beginshadow();
        void endshadow();
        void setparams(Slot &slot, VSlot &vslot, VSlot *layer, int layermask);
        bool beginwireframe();
        void endwireframe();
        void beginremesh();
        void finishseams();
        bool remesh(VSlot &vslot, VSlot *layer, int layermask, vector<vertex> &verts, vector<ushort> &indices, int maxindices);
        void endremesh(vector<vertex> &verts, vtxarray *va);
        void invalidateseams(vtxarray *va);
        void marktjoint(const vec &position);
        void invalidateblendseams();
        void invalidatematerialseams();
        void resetremeshstats();
        void printremeshstats();
    }
}

#endif
