#ifndef TESSELATE_RENDERER_BVH_H
#define TESSELATE_RENDERER_BVH_H

struct vtxarray;

namespace renderer
{
    namespace worldbvh
    {
        enum BufferBinding
        {
            NODE_BINDING = 8,
            TRIANGLE_BINDING,
            TRIANGLE_UV_BINDING,
            MATERIAL_BINDING
        };

        void invalidate(vtxarray *va);
        void sync(bool force = false);
        void clear();
        void cleanup();
        void bind();
        void debugdraw();
        bool ready();
        unsigned int generation();
        unsigned int root();
        unsigned int stacksize();
        unsigned int materialcount();
    }
}

#endif
