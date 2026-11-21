#ifndef TESSELATE_RENDERER_VOLUMETRICS_H
#define TESSELATE_RENDERER_VOLUMETRICS_H

namespace renderer
{
    namespace volumetrics
    {
        extern int volumetricbackend, cvollocallights;

        bool candelaactive();
        bool render();
        void resethistory();
        void cleanup();
    }
}

#endif
