#ifndef TESSELATE_RENDERER_REFLECTIONS_H
#define TESSELATE_RENDERER_REFLECTIONS_H

namespace renderer
{
    namespace reflections
    {
        struct tracelight
        {
            vec4 positionRadius, color, directionCone;
        };

        extern int rtreflections;

        int collectlights(tracelight *destination, int maximum);
        bool active();
        void render(GLuint hdrtexture, int width, int height);
        void resethistory();
        void cleanup();
    }
}

#endif
