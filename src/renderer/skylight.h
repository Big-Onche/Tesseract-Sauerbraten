#ifndef TESSELATE_RENDERER_SKYLIGHT_H
#define TESSELATE_RENDERER_SKYLIGHT_H

namespace renderer
{
    namespace skylight
    {
        void setup();
        void cleanup();
        void invalidate();
        void invalidate(const ivec &bbmin, const ivec &bbmax);
        void bindtexture(int unit);
        void bindocclusionparams();
        void bindambientparams();
    }
}

#endif
