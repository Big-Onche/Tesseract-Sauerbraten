#ifndef ENGINE_ACOUSTICS_H
#define ENGINE_ACOUSTICS_H

#include "AL/efx-presets.h"

namespace acoustics
{
    struct AcousticSourceInfo
    {
        vec apparent;
        float occlusion, virtualGain, virtualGainHF;
        bool path;

        AcousticSourceInfo() : apparent(0, 0, 0), occlusion(0), virtualGain(0), virtualGainHF(1), path(false) {}
    };

    void updateAcoustics();
    void acousticSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend, AcousticSourceInfo *info = NULL);
    void acousticHudSource(float &reverbSend);
    void drawAcousticsDebug();

    void clearAcousticGrid();
    void bakeAcousticGrid(int cellsize, int rays);
    int numAcousticCells();
    int numAcousticRegions();
    int numAcousticPortals();
}

namespace sound
{
    void updateAcousticReverb(const EFXEAXREVERBPROPERTIES *acousticShape, float reverbGain, float reverbDecay, float reflectionAmount);
}

#endif
