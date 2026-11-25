#ifndef TESSELATE_RENDERER_VOLUMETRICS_H
#define TESSELATE_RENDERER_VOLUMETRICS_H

namespace renderer
{
    namespace volumetrics
    {
        struct locallight
        {
            vec4 positionRadius, color, secondary, directionCone, attenuation, shadowParams, shadowOffsetFlags;
        };

        int collectlocallights(locallight *destination, int maximum);
        bool render();
        void resethistory();
        void cleanup();
    }
}

#endif
