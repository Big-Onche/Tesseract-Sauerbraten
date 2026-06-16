#ifndef ENGINE_ACOUSTICS_H
#define ENGINE_ACOUSTICS_H

#include "AL/efx-presets.h"

namespace acoustics
{
    void updateAcoustics();
    void acousticSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend);
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
