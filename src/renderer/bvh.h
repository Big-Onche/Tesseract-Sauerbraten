#ifndef TESSELATE_RENDERER_BVH_H
#define TESSELATE_RENDERER_BVH_H

struct vtxarray;

namespace renderer
{
    namespace worldbvh
    {
        enum MaterialFlags
        {
            MATERIAL_OPAQUE      = 0,
            MATERIAL_BLEND       = 1<<0,
            MATERIAL_ALPHA_BACK  = 1<<1,
            MATERIAL_ALPHA_FRONT = 1<<2,
            MATERIAL_REFRACT     = 1<<3,
            MATERIAL_DOUBLE_SIDED = 1<<4,
            MATERIAL_WATER       = 1<<5,
            MATERIAL_LAVA        = 1<<6,
            MATERIAL_GLASS       = 1<<7
        };

        enum BufferBinding
        {
            NODE_BINDING = 8,
            TRIANGLE_BINDING,
            VERTEX_BINDING,
            BLAS_BINDING,
            INSTANCE_BINDING,
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
        unsigned int materialcount();
    }
}

#endif
