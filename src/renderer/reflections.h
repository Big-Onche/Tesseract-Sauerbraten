#ifndef TESSELATE_RENDERER_REFLECTIONS_H
#define TESSELATE_RENDERER_REFLECTIONS_H

namespace renderer
{
    namespace reflections
    {
        extern int rtreflections;

        bool active();
        void render(GLuint hdrtexture, int width, int height);
        void resethistory();
        void cleanup();
    }
}

#endif
