// acoustics.cpp: baked environmental acoustics and source occlusion for sound.cpp

#include "engine.h"
#include "AL/efx-presets.h"
#include "acoustics.h"

namespace acoustics
{
    VARP(soundacoustics, 0, 0, 1);
    VAR(soundacousticsmooth, 0, 150, 2000);

    FVAR(soundacousticrange, 4.0f, 512.0f, 1024.0f);
    FVAR(soundacousticocclusion, 0.0f, 1.0f, 2.0f);
    FVAR(soundacousticblockgain, 0.05f, 0.15f, 1.0f);
    FVAR(soundacousticmufflegainhf, 0.02f, 0.05f, 1.0f);
    FVARP(soundacousticreverb, 0.0f, 1.2f, 2.0f);

    VARP(soundacousticgrid, 0, 1, 1);
    VARP(soundacousticcellsize, 16, 32, 128);
    VARP(soundacousticbakerays, 16, 64, 256);
    VARP(soundacousticthreads, 0, 0, 16);

    VARP(soundacousticastarrange, 0, 512, 4096);
    VARP(soundacousticastarbudget, 0, 1024, 100000);
    FVARP(soundacousticsourcereverbmix, 0.0f, 0.55f, 2.0f);
    FVARP(soundacousticlistenerreverbmix, 0.0f, 0.30f, 2.0f);
    FVARP(soundacousticpathreverbmix, 0.0f, 0.15f, 2.0f);

    VAR(debugsoundacoustics, 0, 0, 1);
    VARP(debugsoundacousticsradius, 32, 512, 1024);

    static const float SoundUnitsPerMeter = 5.0f;

    static float unitsToMeters(float units)
    {
        return max(units, 0.0f)/SoundUnitsPerMeter;
    }

    static float metersToUnits(float meters)
    {
        return max(meters, 0.0f)*SoundUnitsPerMeter;
    }

    static float rampfactor(float x, float low, float high)
    {
        if(high <= low) return x >= high ? 1.0f : 0.0f;
        return clamp((x - low)/(high - low), 0.0f, 1.0f);
    }

    static float smoothramp(float x, float low, float high)
    {
        float t = rampfactor(x, low, high);
        return t*t*(3.0f - 2.0f*t);
    }
    enum
    {
        AP_SMALLROOM = 0,
        AP_HALL,
        AP_CORRIDOR,
        AP_CAVE,
        AP_OPENOUTDOOR,
        AP_COURTYARD,
        AP_STREET,
        AP_CANYON,
        AP_NUM
    };

    enum
    {
        AS_NORTH = 0,
        AS_NORTHEAST,
        AS_EAST,
        AS_SOUTHEAST,
        AS_SOUTH,
        AS_SOUTHWEST,
        AS_WEST,
        AS_NORTHWEST,
        AS_NUM
    };

    struct AcousticPreset
    {
        const char *name;
        bool outdoor;
        EFXEAXREVERBPROPERTIES efx;
    };

    static const AcousticPreset acousticPresets[AP_NUM] =
    {
        { "small_room", false, EFX_REVERB_PRESET_SPACESTATION_SMALLROOM },
        { "hall", false, EFX_REVERB_PRESET_CASTLE_HALL },
        { "corridor", false, EFX_REVERB_PRESET_STONECORRIDOR },
        { "cave", false, EFX_REVERB_PRESET_CAVE },
        { "open_outdoor", true, EFX_REVERB_PRESET_OUTDOORS_ROLLINGPLAINS },
        { "courtyard", true, EFX_REVERB_PRESET_CASTLE_COURTYARD },
        { "street", true, EFX_REVERB_PRESET_CITY_STREETS },
        { "canyon", true, EFX_REVERB_PRESET_OUTDOORS_DEEPCANYON }
    };

    struct AcousticChoice
    {
        int first, second;
        float firstWeight, secondWeight;

        AcousticChoice() : first(0), second(0), firstWeight(1), secondWeight(0) {}
    };

    struct AcousticCell
    {
        ivec coord;
        vec origin;
        float airOccupancy, skyOpenness, openness, hitRatio, nearWallRatio, farWallRatio, nearDistance, medianDistance, farDistance, distanceVariance,
              horizontalHitRatio, horizontalOpenRatio, horizontalFarHitRatio, medianHorizontalDistance, downOpenness, ceilingDistance,
              corridorScore, sectorCorridorScore, cornerScore, courtyardScore, openPlainScore, boxRoomScore,
              pcaAnisotropy, pcaFlatness, pcaVerticality,
              verticalOpenness, clutterScore, irregularityScore, outdoorConnectivity, presetScores[AP_NUM], presetBlend, confidence,
              reverbGain, reverbDecay, reflection, muffleOpen, outdoorRatio;
        float sectorDistance[AS_NUM], sectorOpenness[AS_NUM], sectorNearRatio[AS_NUM];
        int primaryPreset, secondaryPreset, region, connections;
        bool valid, boundary;
        EFXEAXREVERBPROPERTIES reverbShape;
        AcousticChoice choice, indoorChoice, outdoorChoice;

        AcousticCell() : coord(0, 0, 0), origin(0, 0, 0), airOccupancy(0), skyOpenness(0), openness(0), hitRatio(0), nearWallRatio(0), farWallRatio(0),
            nearDistance(0), medianDistance(0), farDistance(0), distanceVariance(0), horizontalHitRatio(0), horizontalOpenRatio(0), horizontalFarHitRatio(0),
            medianHorizontalDistance(0), downOpenness(0), ceilingDistance(0), corridorScore(0), sectorCorridorScore(0), cornerScore(0),
            courtyardScore(0), openPlainScore(0), boxRoomScore(0), pcaAnisotropy(0), pcaFlatness(0), pcaVerticality(0),
            verticalOpenness(0), clutterScore(0), irregularityScore(0), outdoorConnectivity(0), presetBlend(0), confidence(0), reverbGain(0), reverbDecay(0.3f), reflection(0),
            muffleOpen(1), outdoorRatio(1), primaryPreset(AP_OPENOUTDOOR), secondaryPreset(AP_OPENOUTDOOR), region(-1), connections(0),
            valid(false), boundary(false)
        {
            loopi(AP_NUM) presetScores[i] = 0.0f;
            loopi(AS_NUM) sectorDistance[i] = sectorOpenness[i] = sectorNearRatio[i] = 0.0f;
            EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
            reverbShape = generic;
            choice.first = choice.second = AP_OPENOUTDOOR;
            indoorChoice.first = indoorChoice.second = AP_HALL;
            outdoorChoice.first = outdoorChoice.second = AP_OPENOUTDOOR;
        }
    };

    struct AcousticRegion
    {
        int id, firstCell, cellCount, firstPortal, portalCount, primaryPreset, secondaryPreset;
        vec center;
        float confidence, outdoorRatio, presetBlend, volume, openness, medianDistance, corridorScore, irregularityScore, reverbGain, reverbDecay, reflection, muffleOpen, presetScores[AP_NUM];
        EFXEAXREVERBPROPERTIES reverbShape;
        AcousticChoice choice;

        AcousticRegion() : id(-1), firstCell(-1), cellCount(0), firstPortal(-1), portalCount(0), primaryPreset(AP_OPENOUTDOOR), secondaryPreset(AP_OPENOUTDOOR), center(0, 0, 0),
            confidence(0), outdoorRatio(1), presetBlend(0), volume(0), openness(0), medianDistance(0), corridorScore(0), irregularityScore(0),
            reverbGain(0), reverbDecay(0.3f), reflection(0), muffleOpen(1)
        {
            loopi(AP_NUM) presetScores[i] = 0.0f;
            EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
            reverbShape = generic;
            choice.first = choice.second = AP_OPENOUTDOOR;
        }
    };

    struct AcousticPortal
    {
        int regionA, regionB, edgeCount;
        vec center, normal;
        float apertureSize, openingStrength, acousticCost, highFrequencyLoss, diffractionCost, traversalCost, visibility, thickness, transitionStrength;

        AcousticPortal() : regionA(-1), regionB(-1), edgeCount(0), center(0, 0, 0), normal(0, 0, 1), apertureSize(0), openingStrength(0), acousticCost(0),
            highFrequencyLoss(0), diffractionCost(0), traversalCost(0), visibility(0), thickness(0), transitionStrength(0) {}
    };

    struct AcousticAStarNode
    {
        int prev;
        float g, f;
        bool open, closed;

        AcousticAStarNode() : prev(-1), g(1e16f), f(1e16f), open(false), closed(false) {}
    };

    struct AcousticAStarQueueNode
    {
        int cell;
        float f;

        AcousticAStarQueueNode() {}
        AcousticAStarQueueNode(int cell, float f) : cell(cell), f(f) {}
    };

    static inline float heapscore(const AcousticAStarQueueNode &node) { return node.f; }

    struct AcousticAStarResult
    {
        vector<int> cells;
        vec virtualPosition;
        float pathCost, pathLength, sourceReverbGain, pathReverbGain, pathReverbDecay, pathReflection, muffleOpen, occlusion, diffraction, complexity;
        bool found;

        AcousticAStarResult() : virtualPosition(0, 0, 0), pathCost(0), pathLength(0), sourceReverbGain(0), pathReverbGain(0),
            pathReverbDecay(0.3f), pathReflection(0), muffleOpen(1), occlusion(0), diffraction(0), complexity(0), found(false) {}
    };

    static float acousticPercentile(vector<float> &values, float p, float fallback)
    {
        if(values.empty()) return fallback;
        values.sort();
        float pos = clamp(p, 0.0f, 1.0f)*(values.length() - 1),
              frac = pos - floorf(pos);
        int lo = clamp(int(pos), 0, values.length() - 1),
            hi = min(lo + 1, values.length() - 1);
        return values[lo] + (values[hi] - values[lo])*frac;
    }

    static EFXEAXREVERBPROPERTIES blendEfx(const EFXEAXREVERBPROPERTIES &a, const EFXEAXREVERBPROPERTIES &b, float t)
    {
        t = clamp(t, 0.0f, 1.0f);
        EFXEAXREVERBPROPERTIES out;
        #define BLEND_EFX_FIELD(name) out.name = a.name + (b.name - a.name)*t
        BLEND_EFX_FIELD(flDensity);
        BLEND_EFX_FIELD(flDiffusion);
        BLEND_EFX_FIELD(flGain);
        BLEND_EFX_FIELD(flGainHF);
        BLEND_EFX_FIELD(flGainLF);
        BLEND_EFX_FIELD(flDecayTime);
        BLEND_EFX_FIELD(flDecayHFRatio);
        BLEND_EFX_FIELD(flDecayLFRatio);
        BLEND_EFX_FIELD(flReflectionsGain);
        BLEND_EFX_FIELD(flReflectionsDelay);
        loopi(3) BLEND_EFX_FIELD(flReflectionsPan[i]);
        BLEND_EFX_FIELD(flLateReverbGain);
        BLEND_EFX_FIELD(flLateReverbDelay);
        loopi(3) BLEND_EFX_FIELD(flLateReverbPan[i]);
        BLEND_EFX_FIELD(flEchoTime);
        BLEND_EFX_FIELD(flEchoDepth);
        BLEND_EFX_FIELD(flModulationTime);
        BLEND_EFX_FIELD(flModulationDepth);
        BLEND_EFX_FIELD(flAirAbsorptionGainHF);
        BLEND_EFX_FIELD(flHFReference);
        BLEND_EFX_FIELD(flLFReference);
        BLEND_EFX_FIELD(flRoomRolloffFactor);
        #undef BLEND_EFX_FIELD
        out.iDecayHFLimit = t < 0.5f ? a.iDecayHFLimit : b.iDecayHFLimit;
        return out;
    }

    static AcousticChoice chooseAcousticPresets(const float *scores, bool outdoor, int fallback)
    {
        AcousticChoice choice;
        choice.first = fallback;
        choice.second = fallback;
        float best = -1.0f, next = -1.0f;
        loopi(AP_NUM) if(acousticPresets[i].outdoor == outdoor)
        {
            float score = scores[i];
            if(score > best)
            {
                next = best;
                choice.second = choice.first;
                best = score;
                choice.first = i;
            }
            else if(score > next)
            {
                next = score;
                choice.second = i;
            }
        }
        if(best <= 1e-4f)
        {
            choice.first = choice.second = fallback;
            choice.firstWeight = 1.0f;
            choice.secondWeight = 0.0f;
            return choice;
        }
        if(next <= 1e-4f || choice.second == choice.first)
        {
            choice.second = choice.first;
            choice.firstWeight = 1.0f;
            choice.secondWeight = 0.0f;
            return choice;
        }
        float total = best + next;
        choice.firstWeight = best/total;
        choice.secondWeight = next/total;
        return choice;
    }

    static AcousticChoice chooseTopAcousticPresets(const float *scores, int fallback)
    {
        AcousticChoice choice;
        choice.first = fallback;
        choice.second = fallback;
        float best = -1.0f, next = -1.0f;
        loopi(AP_NUM)
        {
            float score = scores[i];
            if(score > best)
            {
                next = best;
                choice.second = choice.first;
                best = score;
                choice.first = i;
            }
            else if(score > next)
            {
                next = score;
                choice.second = i;
            }
        }
        if(best <= 1e-4f)
        {
            choice.first = choice.second = fallback;
            choice.firstWeight = 1.0f;
            choice.secondWeight = 0.0f;
            return choice;
        }
        if(next <= 1e-4f || choice.second == choice.first)
        {
            choice.second = choice.first;
            choice.firstWeight = 1.0f;
            choice.secondWeight = 0.0f;
            return choice;
        }
        float total = best + next;
        choice.firstWeight = best/total;
        choice.secondWeight = next/total;
        return choice;
    }

    static void normalizeAcousticScores(float *scores)
    {
        float total = 0.0f;
        loopi(AP_NUM) total += max(scores[i], 0.0f);
        if(total > 1e-4f) loopi(AP_NUM) scores[i] = max(scores[i], 0.0f)/total;
    }

    static void updateCellPresetChoice(AcousticCell &cell)
    {
        normalizeAcousticScores(cell.presetScores);
        cell.indoorChoice = chooseAcousticPresets(cell.presetScores, false, AP_HALL);
        cell.outdoorChoice = chooseAcousticPresets(cell.presetScores, true, AP_OPENOUTDOOR);
        cell.choice = chooseTopAcousticPresets(cell.presetScores, cell.outdoorRatio >= 0.5f ? AP_OPENOUTDOOR : AP_HALL);
        cell.primaryPreset = cell.choice.first;
        cell.secondaryPreset = cell.choice.second;
        cell.presetBlend = cell.choice.secondWeight;
        cell.reverbShape = blendEfx(acousticPresets[cell.primaryPreset].efx, acousticPresets[cell.secondaryPreset].efx, cell.presetBlend);
        cell.reverbDecay = cell.reverbShape.flDecayTime;
    }

    static void updateRegionPresetChoice(AcousticRegion &region)
    {
        normalizeAcousticScores(region.presetScores);
        region.choice = chooseTopAcousticPresets(region.presetScores, region.outdoorRatio >= 0.5f ? AP_OPENOUTDOOR : AP_HALL);
        region.primaryPreset = region.choice.first;
        region.secondaryPreset = region.choice.second;
        region.presetBlend = region.choice.secondWeight;
        region.reverbShape = blendEfx(acousticPresets[region.primaryPreset].efx, acousticPresets[region.secondaryPreset].efx, region.presetBlend);
        region.reverbDecay = region.reverbShape.flDecayTime;
    }

    struct AcousticProbe
    {
        vec origin;
        int lastmillis, lastDebugMillis, cell, region;
        float openness, walldist, reverbGain, reverbDecay, reflection, outdoorRatio, muffleOpen, scores[AP_NUM];
        bool baked;
        EFXEAXREVERBPROPERTIES reverbShape;
        AcousticChoice indoorChoice, outdoorChoice;

        AcousticProbe() : origin(0, 0, 0), lastmillis(0), lastDebugMillis(0), cell(-1), region(-1), openness(1), walldist(0), reverbGain(0), reverbDecay(0.3f), reflection(0), outdoorRatio(1), muffleOpen(1), baked(false)
        {
            loopi(AP_NUM) scores[i] = 0.0f;
            EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
            reverbShape = generic;
            indoorChoice.first = indoorChoice.second = AP_HALL;
            outdoorChoice.first = outdoorChoice.second = AP_OPENOUTDOOR;
        }
    };

    static AcousticProbe acousticProbe;
    static vector<AcousticCell> acousticCells;
    static vector<AcousticRegion> acousticRegions;
    static vector<AcousticPortal> acousticPortals;
    static vector<int> acousticRegionPortalEdges;
    static hashtable<ivec, int> acousticCellLookup(1<<12);
    static vector<int> acousticDebugPath;
    static vec acousticDebugVirtualSource(0, 0, 0);
    static int acousticAStarFrame = -1, acousticAStarNodesThisFrame = 0, acousticDebugPathMillis = 0;

    struct AcousticBakeRequest
    {
        ivec coord;

        AcousticBakeRequest() {}
        AcousticBakeRequest(const ivec &coord) : coord(coord) {}
    };

    static SDL_mutex *acousticBakeMutex = NULL;
    static SDL_mutex *acousticRayMutex = NULL;
    static vector<AcousticBakeRequest> acousticBakeRequests;
    static vector<AcousticCell> acousticBakeCells;
    static volatile bool acousticBakeCanceled = false, checkAcousticBakeProgress = false;
    static int acousticBakeTotal = 0, acousticBakeProcessed = 0, acousticBakeValidCells = 0;
    static vec acousticBakeBounds[2];
    static bool acousticBakeBoundsSet[2] = { false, false };

    static Uint32 acousticBakeTimer(Uint32 interval, void *param)
    {
        checkAcousticBakeProgress = true;
        return interval;
    }

    static void showAcousticBakeProgress(int processed = acousticBakeProcessed, int valid = acousticBakeValidCells)
    {
        float bar = float(processed)/float(acousticBakeTotal > 0 ? acousticBakeTotal : 1);
        defformatstring(text, "%d%% - %d of %d acoustic grid cells (%d valid)", int(bar*100), processed, acousticBakeTotal, valid);
        renderprogress(bar, text);
        if(interceptkey(SDLK_ESCAPE)) acousticBakeCanceled = true;
        checkAcousticBakeProgress = false;
    }

    static float acousticRaycube(const vec &o, const vec &ray, float radius, int mode)
    {
        if(!acousticRayMutex) acousticRayMutex = SDL_CreateMutex();
        if(acousticRayMutex) SDL_LockMutex(acousticRayMutex);
        float dist = raycube(o, ray, radius, mode);
        if(acousticRayMutex) SDL_UnlockMutex(acousticRayMutex);
        return dist;
    }

    static ivec acousticCellCoord(const vec &o)
    {
        int size = max(soundacousticcellsize, 16);
        return ivec(int(floorf(o.x/size)), int(floorf(o.y/size)), int(floorf(o.z/size)));
    }

    static vec acousticCellCenter(const ivec &coord)
    {
        float size = max(soundacousticcellsize, 16);
        return vec((coord.x + 0.5f)*size, (coord.y + 0.5f)*size, (coord.z + 0.5f)*size);
    }

    static bool acousticBakeBoundsEnabled()
    {
        return acousticBakeBoundsSet[0] && acousticBakeBoundsSet[1];
    }

    static void acousticBakeBoundsMinMax(vec &bbmin, vec &bbmax)
    {
        loopi(3)
        {
            bbmin[i] = min(acousticBakeBounds[0][i], acousticBakeBounds[1][i]);
            bbmax[i] = max(acousticBakeBounds[0][i], acousticBakeBounds[1][i]);
        }
    }

    static bool acousticBakeCellBounds(int csize, int cellsperaxis, ivec &mincoord, ivec &maxcoord)
    {
        mincoord = ivec(0, 0, 0);
        maxcoord = ivec(cellsperaxis - 1, cellsperaxis - 1, cellsperaxis - 1);
        if(!acousticBakeBoundsEnabled()) return true;

        vec bbmin, bbmax;
        acousticBakeBoundsMinMax(bbmin, bbmax);
        loopi(3)
        {
            float lo = clamp(bbmin[i], 0.0f, float(worldsize)),
                  hi = clamp(bbmax[i], 0.0f, float(worldsize));
            if(hi <= lo) return false;
            mincoord[i] = clamp(int(floorf(lo/csize)), 0, cellsperaxis - 1);
            maxcoord[i] = clamp(int(ceilf(hi/csize)) - 1, 0, cellsperaxis - 1);
        }
        return mincoord.x <= maxcoord.x && mincoord.y <= maxcoord.y && mincoord.z <= maxcoord.z;
    }

    static int acousticCellIndex(const ivec &coord)
    {
        int *idx = acousticCellLookup.access(coord);
        return idx ? *idx : -1;
    }

    static AcousticCell *findAcousticCell(const vec &o)
    {
        if(!soundacousticgrid || acousticCells.empty()) return NULL;
        int idx = acousticCellIndex(acousticCellCoord(o));
        return acousticCells.inrange(idx) && acousticCells[idx].valid ? &acousticCells[idx] : NULL;
    }

    static bool acousticPointIsAir(const vec &p, float clearance)
    {
        if(!insideworld(p)) return false;
        static const vec dirs[6] =
        {
            vec(1, 0, 0), vec(-1, 0, 0), vec(0, 1, 0), vec(0, -1, 0), vec(0, 0, 1), vec(0, 0, -1)
        };
        loopi(6) if(acousticRaycube(p, dirs[i], clearance, RAY_POLY) >= clearance*0.95f) return true;
        return false;
    }

    static void scoreAcousticCell(AcousticCell &cell, float range)
    {
        float hits = cell.hitRatio,
              skyOpen = cell.skyOpenness,
              ceilingDist = cell.ceilingDistance,
              horizontalHitRatio = cell.horizontalHitRatio,
              horizontalNearRatio = cell.nearWallRatio,
              horizontalFarHitRatio = cell.horizontalFarHitRatio,
              horizontalOpenRatio = cell.horizontalOpenRatio,
              medianHorizontalDistance = cell.medianHorizontalDistance,
              horizontalVariance = cell.distanceVariance,
              nearPercentile = cell.nearDistance,
              medianHitDistance = cell.medianDistance,
              farPercentile = cell.farDistance,
              corridorScore = cell.corridorScore,
              downOpenRatio = cell.downOpenness;
        float varianceScore = clamp(sqrtf(horizontalVariance)/max(metersToUnits(medianHorizontalDistance), 1.0f), 0.0f, 1.0f),
              percentileSpread = clamp((farPercentile - nearPercentile)/max(farPercentile, 1.0f), 0.0f, 1.0f),
              irregularityScore = clamp(varianceScore*0.50f + percentileSpread*0.30f + cell.cornerScore*0.10f + cell.pcaAnisotropy*0.10f, 0.0f, 1.0f),
              skyOutdoor = smoothramp(skyOpen, 0.35f, 0.70f),
              floodOutdoor = clamp(cell.outdoorConnectivity, 0.0f, 1.0f),
              outdoorRaw = clamp(skyOutdoor*(0.35f + floodOutdoor*0.65f) + floodOutdoor*smoothramp(skyOpen, 0.15f, 0.45f)*0.25f, 0.0f, 1.0f),
              indoorRaw = 1.0f - outdoorRaw,
              ceilingOpen = skyOpen,
              fill = clamp(horizontalNearRatio + horizontalHitRatio - horizontalFarHitRatio, 0.0f, 1.0f),
              roomSizeScore = 1.0f - smoothramp(medianHorizontalDistance, 6.0f, 24.0f);

        cell.presetScores[AP_SMALLROOM] = indoorRaw*(0.35f + horizontalHitRatio*0.65f)*roomSizeScore*(1.0f - irregularityScore*0.45f)*(1.0f - horizontalFarHitRatio*0.35f)*(0.75f + cell.boxRoomScore*0.35f);
        cell.presetScores[AP_HALL] = indoorRaw*(0.30f + horizontalHitRatio*0.70f)*smoothramp(medianHorizontalDistance, 8.0f, 28.0f)*(0.40f + horizontalFarHitRatio*0.60f)*(1.0f - corridorScore*0.75f)*(1.0f - irregularityScore*0.35f)*(0.90f + cell.boxRoomScore*0.20f);

        cell.presetScores[AP_CORRIDOR] = indoorRaw
            *(0.20f + horizontalHitRatio*0.80f)
            *corridorScore
            *(0.55f + fill*0.45f) // slightly stronger fill reinforcement
            *(1.0f - ceilingOpen*0.40f) // penalise open/tall ceilings (halls have those)
            *(0.90f + cell.pcaAnisotropy*0.65f); // stronger directional reinforcement

        float lowCeiling = 1.0f - smoothramp(ceilingDist, metersToUnits(2.0f), metersToUnits(6.0f));
        cell.presetScores[AP_CAVE] = indoorRaw
            *(0.20f + hits*0.45f + horizontalFarHitRatio*0.20f)  // merged far hits in
            *smoothramp(irregularityScore, 0.25f, 0.70f)          // lower entry threshold
            *(0.40f + lowCeiling*0.60f)                           // explicit low-ceiling signal
            *(1.0f - corridorScore*0.55f)
            *(0.80f + cell.pcaVerticality*0.40f);                 // stronger verticality boost

        cell.presetScores[AP_OPENOUTDOOR] = outdoorRaw*skyOpen*(0.45f + horizontalOpenRatio*0.55f)*(1.0f - horizontalNearRatio)*(1.0f - corridorScore*0.50f)*(0.75f + cell.openPlainScore*0.45f);
        cell.presetScores[AP_COURTYARD] = outdoorRaw*skyOpen*(0.25f + horizontalNearRatio*0.75f)*(0.25f + horizontalHitRatio*0.75f)*(1.0f - corridorScore*0.40f)*(0.70f + cell.courtyardScore*0.60f);
        cell.presetScores[AP_STREET] = outdoorRaw*skyOpen*(0.25f + horizontalNearRatio*0.75f)*max(corridorScore, cell.sectorCorridorScore)*(0.85f + cell.pcaAnisotropy*0.35f);
        cell.presetScores[AP_CANYON] = outdoorRaw*skyOpen*(0.25f + horizontalFarHitRatio*0.75f)*(0.35f + irregularityScore*0.65f)*(0.55f + max(corridorScore, cell.pcaAnisotropy)*0.45f)*(0.70f + downOpenRatio*0.30f);

        cell.presetScores[AP_SMALLROOM] *= 0.72f;
        cell.presetScores[AP_HALL] *= 1.14f;
        cell.presetScores[AP_CORRIDOR] *= 1.28f;
        cell.presetScores[AP_CAVE] *= 1.22f;
        normalizeAcousticScores(cell.presetScores);

        if(indoorRaw > 0.2f && cell.presetScores[AP_SMALLROOM] + cell.presetScores[AP_HALL] + cell.presetScores[AP_CORRIDOR] + cell.presetScores[AP_CAVE] <= 1e-4f)
            cell.presetScores[medianHorizontalDistance < 10.0f ? AP_SMALLROOM : AP_HALL] = indoorRaw;
        if(outdoorRaw > 0.2f && cell.presetScores[AP_OPENOUTDOOR] + cell.presetScores[AP_COURTYARD] + cell.presetScores[AP_STREET] + cell.presetScores[AP_CANYON] <= 1e-4f)
            cell.presetScores[AP_OPENOUTDOOR] = outdoorRaw;
        cell.outdoorRatio = outdoorRaw;
        updateCellPresetChoice(cell);

        float indoorStrength = (1.0f - outdoorRaw)*clamp(0.25f + hits*0.35f + horizontalFarHitRatio*0.25f + fill*0.15f, 0.0f, 1.0f),
              outdoorStrength = outdoorRaw*clamp(0.07f + horizontalNearRatio*0.35f + horizontalFarHitRatio*0.30f + corridorScore*0.25f, 0.04f, 0.75f),
              indoorMuffleOpen = clamp(smoothramp(medianHorizontalDistance, 6.0f, 36.0f)*0.70f + horizontalOpenRatio*0.30f, 0.0f, 1.0f),
              outdoorMuffleOpen = clamp(skyOpen*0.55f + horizontalOpenRatio*0.35f + smoothramp(medianHorizontalDistance, 24.0f, 96.0f)*0.10f, 0.0f, 1.0f);

        cell.verticalOpenness = clamp(skyOpen*0.7f + downOpenRatio*0.3f, 0.0f, 1.0f);
        cell.clutterScore = fill;
        cell.irregularityScore = irregularityScore;
        cell.openness = clamp(1.0f - hits, 0.0f, 1.0f);
        cell.reverbGain = clamp(indoorStrength + outdoorStrength, 0.0f, 1.0f);
        cell.reflection = clamp(horizontalNearRatio*0.35f + horizontalFarHitRatio*0.25f + corridorScore*0.30f + hits*0.15f, 0.0f, 1.0f);
        cell.muffleOpen = clamp(indoorMuffleOpen*(1.0f - outdoorRaw) + outdoorMuffleOpen*outdoorRaw, 0.0f, 1.0f);
        cell.confidence = clamp(cell.airOccupancy*(1.0f - cell.boundary*0.35f)*(0.45f + hits*0.30f + min(range, ceilingDist + metersToUnits(medianHitDistance))/max(range, 1.0f)*0.25f), 0.05f, 1.0f);
    }

    static int acousticHorizontalSector(const vec &dir)
    {
        float angle = PI/2.0f - atan2f(dir.y, dir.x);
        if(angle < 0.0f) angle += 2.0f*PI;
        else if(angle >= 2.0f*PI) angle -= 2.0f*PI;
        return clamp(int(floorf((angle + PI/8.0f)*(AS_NUM/(2.0f*PI))))&7, 0, AS_NUM - 1);
    }

    static void bakeAcousticCellRays(AcousticCell &cell, int rays, float range)
    {
        rays = clamp(rays, 16, 256);
        const float golden = PI*(3.0f - sqrtf(5.0f)),
                    nearDist = metersToUnits(6.0f),
                    farDist = metersToUnits(24.0f),
                    maxHitDist = range*0.98f;
        float open = 0, hits = 0, skyOpen = 1.0f, ceilingDist = 0,
              skyCount = 0, skyOpenCount = 0, ceilingHits = 0,
              skyDiagCount = 0, skyDiagOpen = 0,
              horizontalCount = 0, horizontalHits = 0, horizontalNear = 0, horizontalFar = 0, horizontalFarHits = 0, horizontalDist = 0, horizontalDist2 = 0,
              downCount = 0, downOpen = 0,
              pcaCount = 0, pcaSumX = 0, pcaSumY = 0, pcaSumZ = 0,
              pcaXX = 0, pcaXY = 0, pcaYY = 0, pcaZZ = 0;
        vector<float> hitDistances, horizontalDistances;

        struct AcousticSector
        {
            int count, hits;
            float dist, nearHits;

            AcousticSector() : count(0), hits(0), dist(0), nearHits(0) {}
        } sectors[AS_NUM];

        loopi(rays)
        {
            float z = 1.0f - (2.0f*(i + 0.5f))/rays,
                  r = sqrtf(max(0.0f, 1.0f - z*z)),
                  a = golden*i;
            vec dir(cosf(a)*r, sinf(a)*r, z);
            float dist = acousticRaycube(cell.origin, dir, range, RAY_POLY);
            dist = clamp(dist, 0.0f, range);
            bool hit = dist < maxHitDist;
            vec sample = vec(dir).mul(dist);
            pcaCount += 1.0f;
            pcaSumX += sample.x;
            pcaSumY += sample.y;
            pcaSumZ += sample.z;
            pcaXX += sample.x*sample.x;
            pcaXY += sample.x*sample.y;
            pcaYY += sample.y*sample.y;
            pcaZZ += sample.z*sample.z;
            open += dist/range;
            if(hit)
            {
                hits += 1.0f;
                hitDistances.add(unitsToMeters(dist));
            }
            if(dir.z > 0.25f)
            {
                skyDiagCount += 1.0f;
                skyDiagOpen += hit ? smoothramp(dist/range, 0.78f, 0.98f) : 1.0f;
            }
            if(dir.z > 0.65f)
            {
                skyCount += 1.0f;
                if(!hit) skyOpenCount += 1.0f;
                else
                {
                    ceilingHits += 1.0f;
                    ceilingDist += dist;
                }
            }
            else if(fabs(dir.z) < 0.30f)
            {
                horizontalCount += 1.0f;
                horizontalDist += dist;
                horizontalDist2 += dist*dist;
                horizontalDistances.add(unitsToMeters(dist));
                if(hit)
                {
                    horizontalHits += 1.0f;
                    if(dist <= nearDist) horizontalNear += 1.0f;
                    if(dist >= farDist) horizontalFarHits += 1.0f;
                }
                if(dist >= farDist) horizontalFar += 1.0f;

                int sector = acousticHorizontalSector(dir);
                sectors[sector].count++;
                sectors[sector].dist += dist;
                if(hit)
                {
                    sectors[sector].hits++;
                    if(dist <= nearDist) sectors[sector].nearHits += 1.0f;
                }
            }
            else if(dir.z < -0.35f)
            {
                downCount += 1.0f;
                if(!hit) downOpen += 1.0f;
            }
        }

        open /= rays;
        hits /= rays;
        if(skyCount > 0) skyOpen = skyOpenCount/skyCount;
        float verticalSkyOpen = skyOpen,
              diagonalSkyOpen = skyDiagCount > 0 ? skyDiagOpen/skyDiagCount : verticalSkyOpen;
        skyOpen = clamp(max(verticalSkyOpen*0.65f + diagonalSkyOpen*0.35f, diagonalSkyOpen*0.55f), 0.0f, 1.0f);
        if(ceilingHits > 0) ceilingDist /= ceilingHits;

        float horizontalHitRatio = horizontalCount > 0 ? horizontalHits/horizontalCount : hits,
              horizontalNearRatio = horizontalCount > 0 ? horizontalNear/horizontalCount : 0.0f,
              horizontalFarRatio = horizontalCount > 0 ? horizontalFar/horizontalCount : open,
              horizontalFarHitRatio = horizontalCount > 0 ? horizontalFarHits/horizontalCount : 0.0f,
              horizontalOpenRatio = max(horizontalFarRatio - horizontalFarHitRatio, 0.0f),
              horizontalAvgDist = horizontalCount > 0 ? horizontalDist/horizontalCount : open*range,
              horizontalVariance = 0.0f,
              downOpenRatio = downCount > 0 ? downOpen/downCount : 0.0f;
        if(horizontalCount > 1)
        {
            float mean = horizontalAvgDist;
            horizontalVariance = max(horizontalDist2/horizontalCount - mean*mean, 0.0f);
        }

        float avgHitDistance = hitDistances.empty() ? unitsToMeters(horizontalAvgDist) : 0.0f;
        loopv(hitDistances) avgHitDistance += hitDistances[i];
        if(!hitDistances.empty()) avgHitDistance /= hitDistances.length();
        float nearPercentile = acousticPercentile(hitDistances, 0.25f, avgHitDistance),
              medianHitDistance = acousticPercentile(hitDistances, 0.50f, avgHitDistance),
              farPercentile = acousticPercentile(hitDistances, 0.75f, avgHitDistance),
              medianHorizontalDistance = acousticPercentile(horizontalDistances, 0.50f, unitsToMeters(horizontalAvgDist));

        float sectorOpen[AS_NUM], sectorNear[AS_NUM];
        loopi(AS_NUM)
        {
            sectorOpen[i] = sectors[i].count ? clamp(sectors[i].dist/(sectors[i].count*range), 0.0f, 1.0f) : open;
            sectorNear[i] = sectors[i].count ? clamp(sectors[i].nearHits/sectors[i].count, 0.0f, 1.0f) : horizontalNearRatio;
            cell.sectorDistance[i] = sectors[i].count ? unitsToMeters(sectors[i].dist/sectors[i].count) : unitsToMeters(horizontalAvgDist);
            cell.sectorOpenness[i] = sectorOpen[i];
            cell.sectorNearRatio[i] = sectorNear[i];
        }
        float sectorCorridor = 0.0f;
        loopi(4)
        {
            int a = i, b = i + 4, side1 = (i + 2)&7, side2 = (i + 6)&7;
            float alongFar = min(sectorOpen[a], sectorOpen[b]),
                  sideClosed = 1.0f - 0.5f*(sectorOpen[side1] + sectorOpen[side2]),
                  sideNear = 0.5f*(sectorNear[side1] + sectorNear[side2]);
            sectorCorridor = max(sectorCorridor, clamp(alongFar*(sideClosed*0.55f + sideNear*0.45f), 0.0f, 1.0f));
        }
        float sectorMean = 0.0f, sectorVar = 0.0f;
        loopi(AS_NUM)
        {
            sectorMean += sectorOpen[i];
        }
        sectorMean /= AS_NUM;
        loopi(AS_NUM) sectorVar += (sectorOpen[i] - sectorMean)*(sectorOpen[i] - sectorMean);
        sectorVar /= AS_NUM;
        float cornerScore = 0.0f;
        loopi(AS_NUM)
        {
            int next = (i + 1)&7, away = (i + 4)&7, awaynext = (i + 5)&7;
            cornerScore = max(cornerScore, min(sectorNear[i], sectorNear[next])*max(sectorOpen[away], sectorOpen[awaynext]));
        }
        float pcaAnisotropy = 0.0f, pcaFlatness = 0.0f, pcaVerticality = 0.0f;
        if(pcaCount > 1.0f)
        {
            float inv = 1.0f/pcaCount,
                  meanX = pcaSumX*inv, meanY = pcaSumY*inv, meanZ = pcaSumZ*inv,
                  cxx = max(pcaXX*inv - meanX*meanX, 0.0f),
                  cxy = pcaXY*inv - meanX*meanY,
                  cyy = max(pcaYY*inv - meanY*meanY, 0.0f),
                  czz = max(pcaZZ*inv - meanZ*meanZ, 0.0f),
                  trace = cxx + cyy,
                  delta = sqrtf(max((cxx - cyy)*(cxx - cyy) + 4.0f*cxy*cxy, 0.0f)),
                  major = max(0.5f*(trace + delta), 0.0f),
                  minor = max(0.5f*(trace - delta), 0.0f),
                  total = max(major + minor + czz, 1.0f);
            pcaAnisotropy = clamp((major - minor)/max(major + minor, 1.0f), 0.0f, 1.0f);
            pcaFlatness = clamp((major + minor - czz)/total, 0.0f, 1.0f);
            pcaVerticality = clamp(czz/total, 0.0f, 1.0f);
        }
        float varianceScore = clamp(sqrtf(horizontalVariance)/max(horizontalAvgDist, 1.0f), 0.0f, 1.0f),
              corridorScore = clamp(max(max(min(horizontalNearRatio, horizontalFarRatio)*max(varianceScore, 0.25f), smoothramp(sectorCorridor, 0.06f, 0.36f)), pcaAnisotropy*(0.25f + horizontalNearRatio*0.55f)), 0.0f, 1.0f),
              balancedSectors = 1.0f - clamp(sqrtf(sectorVar)*2.5f, 0.0f, 1.0f);

        cell.skyOpenness = skyOpen;
        cell.hitRatio = hits;
        cell.nearWallRatio = horizontalNearRatio;
        cell.farWallRatio = horizontalFarRatio;
        cell.horizontalHitRatio = horizontalHitRatio;
        cell.horizontalOpenRatio = horizontalOpenRatio;
        cell.horizontalFarHitRatio = horizontalFarHitRatio;
        cell.nearDistance = nearPercentile;
        cell.medianDistance = medianHitDistance;
        cell.farDistance = farPercentile;
        cell.medianHorizontalDistance = medianHorizontalDistance;
        cell.distanceVariance = horizontalVariance;
        cell.downOpenness = downOpenRatio;
        cell.ceilingDistance = ceilingDist;
        cell.sectorCorridorScore = smoothramp(sectorCorridor, 0.06f, 0.36f);
        cell.cornerScore = clamp(cornerScore, 0.0f, 1.0f);
        cell.courtyardScore = clamp(skyOpen*horizontalHitRatio*balancedSectors*(0.45f + horizontalNearRatio*0.55f), 0.0f, 1.0f);
        cell.openPlainScore = clamp(skyOpen*horizontalOpenRatio*(1.0f - horizontalNearRatio)*(1.0f - pcaAnisotropy*0.65f), 0.0f, 1.0f);
        cell.boxRoomScore = clamp((1.0f - skyOpen)*horizontalHitRatio*balancedSectors*(1.0f - varianceScore)*(1.0f - horizontalOpenRatio*0.45f), 0.0f, 1.0f);
        cell.pcaAnisotropy = pcaAnisotropy;
        cell.pcaFlatness = pcaFlatness;
        cell.pcaVerticality = pcaVerticality;
        cell.corridorScore = corridorScore;

        scoreAcousticCell(cell, range);
    }

    static bool bakeAcousticCell(AcousticCell &cell, const ivec &coord, int rays, float range)
    {
        int cellsize = max(soundacousticcellsize, 16);
        float half = cellsize*0.5f,
              sampleHalf = half*0.92f,
              clearance = clamp(cellsize*0.08f, 1.5f, 4.0f);
        vec center = acousticCellCenter(coord), centroid(0, 0, 0), nearest(center);
        float nearestdist = 1e16f;
        int samples = 0, air = 0;

        loopi(8)
        {
            vec p(center.x + (i&1 ? sampleHalf : -sampleHalf), center.y + (i&2 ? sampleHalf : -sampleHalf), center.z + (i&4 ? sampleHalf : -sampleHalf));
            samples++;
            if(acousticPointIsAir(p, clearance))
            {
                air++;
                centroid.add(p);
                float dist = p.squaredist(center);
                if(dist < nearestdist) { nearestdist = dist; nearest = p; }
            }
        }

        static const vec faces[6] =
        {
            vec(1, 0, 0), vec(-1, 0, 0), vec(0, 1, 0), vec(0, -1, 0), vec(0, 0, 1), vec(0, 0, -1)
        };
        samples++;
        if(acousticPointIsAir(center, clearance))
        {
            air++;
            centroid.add(center);
            nearest = center;
            nearestdist = 0;
        }
        loopi(6)
        {
            vec p = vec(faces[i]).mul(sampleHalf).add(center);
            samples++;
            if(acousticPointIsAir(p, clearance))
            {
                air++;
                centroid.add(p);
                float dist = p.squaredist(center);
                if(dist < nearestdist) { nearestdist = dist; nearest = p; }
            }
        }

        if(!air) return false;

        cell = AcousticCell();
        cell.coord = coord;
        cell.airOccupancy = air/float(samples);
        cell.boundary = air < samples;
        cell.origin = air > 1 ? centroid.div(float(air)) : nearest;
        cell.valid = true;
        bakeAcousticCellRays(cell, rays, range);
        return true;
    }

    struct AcousticBakeWorker
    {
        SDL_Thread *thread;
        int rays;
        float range;

        AcousticBakeWorker(int rays, float range) : thread(NULL), rays(rays), range(range) {}

        bool bakeNext()
        {
            SDL_LockMutex(acousticBakeMutex);
            if(acousticBakeCanceled || acousticBakeRequests.empty())
            {
                SDL_UnlockMutex(acousticBakeMutex);
                return false;
            }
            AcousticBakeRequest req = acousticBakeRequests.pop();
            SDL_UnlockMutex(acousticBakeMutex);

            AcousticCell cell;
            bool valid = bakeAcousticCell(cell, req.coord, rays, range);
            SDL_LockMutex(acousticBakeMutex);
            if(valid)
            {
                acousticBakeCells.add(cell);
                acousticBakeValidCells++;
            }
            acousticBakeProcessed++;
            SDL_UnlockMutex(acousticBakeMutex);
            return true;
        }

        static int run(void *data)
        {
            AcousticBakeWorker *w = (AcousticBakeWorker *)data;
            while(w->bakeNext());
            return 0;
        }
    };

    static bool acousticCellsCompatible(const AcousticCell &a, const AcousticCell &b)
    {
        if(!a.valid || !b.valid) return false;
        if(fabs(a.outdoorRatio - b.outdoorRatio) > 0.35f) return false;
        if(a.primaryPreset == b.primaryPreset) return true;
        float overlap = 0.0f;
        loopi(AP_NUM) overlap += min(a.presetScores[i], b.presetScores[i]);
        return overlap > 0.28f || max(a.presetScores[b.primaryPreset], b.presetScores[a.primaryPreset]) > 0.20f;
    }

    struct AcousticSmoothUpdate
    {
        int cell;
        float blend, outdoorRatio, openness, medianDistance, corridorScore, irregularityScore, reverbGain, reflection, muffleOpen, scores[AP_NUM];

        AcousticSmoothUpdate() : cell(-1), blend(0), outdoorRatio(0), openness(0), medianDistance(0), corridorScore(0), irregularityScore(0),
            reverbGain(0), reflection(0), muffleOpen(1)
        {
            loopi(AP_NUM) scores[i] = 0.0f;
        }
    };

    static void smoothAcousticCellOutliers()
    {
        static const ivec dirs[6] =
        {
            ivec(1, 0, 0), ivec(-1, 0, 0), ivec(0, 1, 0), ivec(0, -1, 0), ivec(0, 0, 1), ivec(0, 0, -1)
        };

        vector<AcousticSmoothUpdate> updates;
        loopv(acousticCells)
        {
            AcousticCell &cell = acousticCells[i];
            if(!cell.valid || cell.confidence >= 0.58f) continue;
            AcousticSmoothUpdate update;
            update.cell = i;
            float total = 0.0f;
            int compatible = 0;
            loopj(6)
            {
                int nextidx = acousticCellIndex(ivec(cell.coord).add(dirs[j]));
                if(!acousticCells.inrange(nextidx)) continue;
                const AcousticCell &next = acousticCells[nextidx];
                if(!next.valid || fabs(cell.outdoorRatio - next.outdoorRatio) > 0.35f || !acousticCellsCompatible(cell, next)) continue;
                float weight = clamp(next.confidence, 0.10f, 1.0f);
                total += weight;
                compatible++;
                loopk(AP_NUM) update.scores[k] += next.presetScores[k]*weight;
                update.outdoorRatio += next.outdoorRatio*weight;
                update.openness += next.openness*weight;
                update.medianDistance += next.medianDistance*weight;
                update.corridorScore += next.corridorScore*weight;
                update.irregularityScore += next.irregularityScore*weight;
                update.reverbGain += next.reverbGain*weight;
                update.reflection += next.reflection*weight;
                update.muffleOpen += next.muffleOpen*weight;
            }
            if(compatible < 2 || total <= 1e-4f) continue;
            update.blend = clamp((0.65f - cell.confidence)*1.35f, 0.10f, 0.65f);
            loopj(AP_NUM) update.scores[j] /= total;
            update.outdoorRatio /= total;
            update.openness /= total;
            update.medianDistance /= total;
            update.corridorScore /= total;
            update.irregularityScore /= total;
            update.reverbGain /= total;
            update.reflection /= total;
            update.muffleOpen /= total;
            updates.add(update);
        }

        loopv(updates)
        {
            AcousticSmoothUpdate &update = updates[i];
            if(!acousticCells.inrange(update.cell)) continue;
            AcousticCell &cell = acousticCells[update.cell];
            float k = update.blend;
            loopj(AP_NUM) cell.presetScores[j] += (update.scores[j] - cell.presetScores[j])*k;
            cell.outdoorRatio += (update.outdoorRatio - cell.outdoorRatio)*k;
            cell.openness += (update.openness - cell.openness)*k;
            cell.medianDistance += (update.medianDistance - cell.medianDistance)*k;
            cell.corridorScore += (update.corridorScore - cell.corridorScore)*k;
            cell.irregularityScore += (update.irregularityScore - cell.irregularityScore)*k;
            cell.reverbGain += (update.reverbGain - cell.reverbGain)*k;
            cell.reflection += (update.reflection - cell.reflection)*k;
            cell.muffleOpen += (update.muffleOpen - cell.muffleOpen)*k;
            cell.confidence = clamp(cell.confidence + k*0.20f, 0.0f, 1.0f);
            updateCellPresetChoice(cell);
        }
    }

    static void floodFillOutdoorAcousticCells(float range)
    {
        static const ivec dirs[6] =
        {
            ivec(1, 0, 0), ivec(-1, 0, 0), ivec(0, 1, 0), ivec(0, -1, 0), ivec(0, 0, 1), ivec(0, 0, -1)
        };

        int csize = max(soundacousticcellsize, 16),
            cellsperaxis = (worldsize + csize - 1)/csize;
        vector<int> pending;
        loopv(acousticCells)
        {
            AcousticCell &cell = acousticCells[i];
            cell.outdoorConnectivity = 0.0f;
            if(!cell.valid) continue;
            bool boundaryAir = cell.coord.x <= 0 || cell.coord.y <= 0 || cell.coord.z <= 0 ||
                               cell.coord.x >= cellsperaxis - 1 || cell.coord.y >= cellsperaxis - 1 || cell.coord.z >= cellsperaxis - 1,
                 topAir = cell.coord.z >= cellsperaxis - 1 || cell.origin.z >= worldsize - csize;
            if(boundaryAir || topAir)
            {
                cell.outdoorConnectivity = 1.0f;
                pending.add(i);
            }
        }

        while(!pending.empty())
        {
            int curidx = pending.pop();
            AcousticCell &cur = acousticCells[curidx];
            loopi(6)
            {
                int nextidx = acousticCellIndex(ivec(cur.coord).add(dirs[i]));
                if(!acousticCells.inrange(nextidx)) continue;
                AcousticCell &next = acousticCells[nextidx];
                if(!next.valid || next.outdoorConnectivity > 0.0f) continue;
                next.outdoorConnectivity = 1.0f;
                pending.add(nextidx);
            }
        }

        loopv(acousticCells) if(acousticCells[i].valid)
        {
            scoreAcousticCell(acousticCells[i], range);
            if(acousticCells[i].boundary) acousticCells[i].confidence *= 0.80f;
        }
    }

    static void finalizeAcousticGrid()
    {
        static const ivec dirs[6] =
        {
            ivec(1, 0, 0), ivec(-1, 0, 0), ivec(0, 1, 0), ivec(0, -1, 0), ivec(0, 0, 1), ivec(0, 0, -1)
        };

        loopv(acousticCells)
        {
            AcousticCell &cell = acousticCells[i];
            if(!cell.valid) continue;
            cell.connections = 0;
            loopj(6)
            {
                int idx = acousticCellIndex(ivec(cell.coord).add(dirs[j]));
                if(acousticCells.inrange(idx) && acousticCells[idx].valid) cell.connections++;
            }
            if(!cell.connections && cell.airOccupancy <= 0.0f) cell.valid = false;
            if(cell.boundary) cell.confidence *= 0.80f;
        }

        floodFillOutdoorAcousticCells(metersToUnits(soundacousticrange));
        smoothAcousticCellOutliers();

        acousticRegions.setsize(0);
        acousticPortals.setsize(0);
        acousticRegionPortalEdges.setsize(0);
        vector<int> pending;
        loopv(acousticCells) acousticCells[i].region = -1;
        float cellvolume = float(max(soundacousticcellsize, 16))*float(max(soundacousticcellsize, 16))*float(max(soundacousticcellsize, 16));
        loopv(acousticCells)
        {
            AcousticCell &start = acousticCells[i];
            if(!start.valid || start.region >= 0) continue;
            AcousticRegion &region = acousticRegions.add();
            region.id = acousticRegions.length() - 1;
            region.firstCell = i;
            region.primaryPreset = start.primaryPreset;
            region.secondaryPreset = start.secondaryPreset;
            pending.add(i);
            start.region = region.id;
            while(!pending.empty())
            {
                int curidx = pending.pop();
                AcousticCell &cur = acousticCells[curidx];
                region.cellCount++;
                region.center.add(cur.origin);
                region.confidence += cur.confidence;
                region.outdoorRatio += cur.outdoorRatio;
                region.volume += cellvolume*cur.airOccupancy;
                region.openness += cur.openness;
                region.medianDistance += cur.medianDistance;
                region.corridorScore += cur.corridorScore;
                region.irregularityScore += cur.irregularityScore;
                region.reverbGain += cur.reverbGain;
                region.reflection += cur.reflection;
                region.muffleOpen += cur.muffleOpen;
                loopk(AP_NUM) region.presetScores[k] += cur.presetScores[k];
                loopj(6)
                {
                    int nextidx = acousticCellIndex(ivec(cur.coord).add(dirs[j]));
                    if(!acousticCells.inrange(nextidx)) continue;
                    AcousticCell &next = acousticCells[nextidx];
                    if(!next.valid || next.region >= 0 || !acousticCellsCompatible(cur, next)) continue;
                    next.region = region.id;
                    pending.add(nextidx);
                }
            }
            if(region.cellCount > 0)
            {
                region.center.div(float(region.cellCount));
                region.confidence = clamp(region.confidence/region.cellCount, 0.0f, 1.0f);
                region.outdoorRatio = clamp(region.outdoorRatio/region.cellCount, 0.0f, 1.0f);
                region.openness = clamp(region.openness/region.cellCount, 0.0f, 1.0f);
                region.medianDistance /= region.cellCount;
                region.corridorScore = clamp(region.corridorScore/region.cellCount, 0.0f, 1.0f);
                region.irregularityScore = clamp(region.irregularityScore/region.cellCount, 0.0f, 1.0f);
                region.reverbGain = clamp(region.reverbGain/region.cellCount, 0.0f, 1.0f);
                region.reflection = clamp(region.reflection/region.cellCount, 0.0f, 1.0f);
                region.muffleOpen = clamp(region.muffleOpen/region.cellCount, 0.0f, 1.0f);
                loopj(AP_NUM) region.presetScores[j] /= region.cellCount;
                updateRegionPresetChoice(region);
            }
        }

        loopv(acousticCells)
        {
            AcousticCell &cell = acousticCells[i];
            if(!cell.valid || !acousticRegions.inrange(cell.region)) continue;
            const AcousticRegion &region = acousticRegions[cell.region];
            if(region.cellCount <= 1) continue;
            float k = clamp((0.72f - cell.confidence)*0.65f, 0.0f, 0.35f);
            if(k <= 0.0f) continue;
            loopj(AP_NUM) cell.presetScores[j] += (region.presetScores[j] - cell.presetScores[j])*k;
            cell.outdoorRatio += (region.outdoorRatio - cell.outdoorRatio)*k;
            cell.openness += (region.openness - cell.openness)*k;
            cell.medianDistance += (region.medianDistance - cell.medianDistance)*k;
            cell.corridorScore += (region.corridorScore - cell.corridorScore)*k;
            cell.irregularityScore += (region.irregularityScore - cell.irregularityScore)*k;
            cell.reverbGain += (region.reverbGain - cell.reverbGain)*k;
            cell.reflection += (region.reflection - cell.reflection)*k;
            cell.muffleOpen += (region.muffleOpen - cell.muffleOpen)*k;
            updateCellPresetChoice(cell);
        }

        loopv(acousticCells)
        {
            AcousticCell &cell = acousticCells[i];
            if(!cell.valid || cell.region < 0) continue;
            loopj(6)
            {
                int nextidx = acousticCellIndex(ivec(cell.coord).add(dirs[j]));
                if(!acousticCells.inrange(nextidx)) continue;
                AcousticCell &next = acousticCells[nextidx];
                if(!next.valid || next.region <= cell.region) continue;
                int regionA = min(cell.region, next.region),
                    regionB = max(cell.region, next.region),
                    portalidx = -1;
                loopk(acousticPortals.length())
                {
                    if(acousticPortals[k].regionA == regionA && acousticPortals[k].regionB == regionB)
                    {
                        portalidx = k;
                        break;
                    }
                }
                AcousticPortal &portal = portalidx >= 0 ? acousticPortals[portalidx] : acousticPortals.add();
                if(portalidx < 0)
                {
                    portal.regionA = regionA;
                    portal.regionB = regionB;
                }
                vec edgecenter = vec(cell.origin).add(next.origin).mul(0.5f),
                    edgenormal = cell.region == regionA ? vec(next.origin).sub(cell.origin) : vec(cell.origin).sub(next.origin),
                    raydir = vec(next.origin).sub(cell.origin);
                float edgeThickness = max(edgenormal.magnitude(), 1.0f);
                if(!edgenormal.iszero()) edgenormal.normalize();
                raydir.safenormalize();
                float visibility = acousticRaycube(cell.origin, raydir, edgeThickness, RAY_POLY) >= edgeThickness*0.90f ? 1.0f : 0.35f,
                      edgeArea = float(max(soundacousticcellsize, 16))*float(max(soundacousticcellsize, 16))*min(cell.airOccupancy, next.airOccupancy),
                      opening = clamp((cell.airOccupancy + next.airOccupancy)*0.5f*visibility, 0.0f, 1.0f),
                      transition = clamp(fabs(cell.outdoorRatio - next.outdoorRatio) + (cell.primaryPreset == next.primaryPreset ? 0.0f : 0.25f), 0.0f, 1.0f);
                portal.edgeCount++;
                portal.center.add(edgecenter);
                portal.normal.add(edgenormal);
                portal.apertureSize += edgeArea;
                portal.openingStrength += opening;
                portal.visibility += visibility;
                portal.thickness += edgeThickness;
                portal.transitionStrength += transition;
            }
        }

        loopv(acousticPortals)
        {
            AcousticPortal &portal = acousticPortals[i];
            if(portal.edgeCount <= 0) continue;
            portal.center.div(float(portal.edgeCount));
            if(!portal.normal.iszero()) portal.normal.normalize();
            else portal.normal = vec(0, 0, 1);
            portal.openingStrength = clamp(portal.openingStrength/portal.edgeCount, 0.0f, 1.0f);
            portal.visibility = clamp(portal.visibility/portal.edgeCount, 0.0f, 1.0f);
            portal.thickness /= portal.edgeCount;
            portal.transitionStrength = clamp(portal.transitionStrength/portal.edgeCount, 0.0f, 1.0f);
            float apertureFactor = smoothramp(sqrtf(max(portal.apertureSize, 0.0f)), float(max(soundacousticcellsize, 16))*0.65f, float(max(soundacousticcellsize, 16))*2.5f),
                  narrowness = 1.0f - apertureFactor,
                  distanceCost = acousticRegions.inrange(portal.regionA) && acousticRegions.inrange(portal.regionB) ?
                      acousticRegions[portal.regionA].center.dist(acousticRegions[portal.regionB].center)/max(metersToUnits(soundacousticrange), 1.0f) : 0.0f;
            portal.diffractionCost = clamp(narrowness*0.45f + (1.0f - portal.visibility)*0.25f + portal.transitionStrength*0.20f + smoothramp(portal.thickness, max(soundacousticcellsize, 16)*0.8f, max(soundacousticcellsize, 16)*2.5f)*0.10f, 0.0f, 1.0f);
            portal.highFrequencyLoss = clamp((1.0f - portal.openingStrength)*0.35f + narrowness*0.25f + portal.diffractionCost*0.30f + portal.transitionStrength*0.10f, 0.0f, 1.0f);
            portal.traversalCost = clamp(distanceCost*0.35f + (1.0f - apertureFactor)*0.25f + portal.diffractionCost*0.25f + portal.highFrequencyLoss*0.15f, 0.0f, 2.0f);
            portal.acousticCost = portal.traversalCost;
        }

        loopv(acousticRegions)
        {
            AcousticRegion &region = acousticRegions[i];
            region.firstPortal = acousticRegionPortalEdges.length();
            loopj(acousticPortals.length()) if(acousticPortals[j].regionA == region.id || acousticPortals[j].regionB == region.id)
                acousticRegionPortalEdges.add(j);
            region.portalCount = acousticRegionPortalEdges.length() - region.firstPortal;
        }
    }

    void clearAcousticGrid()
    {
        acousticDebugPath.setsize(0);
        acousticCells.setsize(0);
        acousticRegions.setsize(0);
        acousticPortals.setsize(0);
        acousticRegionPortalEdges.setsize(0);
        acousticCellLookup.clear();
        acousticProbe.baked = false;
        acousticProbe.cell = -1;
        acousticProbe.region = -1;
    }

    int numAcousticCells() { return acousticCells.length(); }
    int numAcousticRegions() { return acousticRegions.length(); }
    int numAcousticPortals() { return acousticPortals.length(); }

    void setAcousticBakeCorner(int corner, const vec &pos)
    {
        if(corner < 0 || corner > 1) return;
        acousticBakeBounds[corner] = pos;
        acousticBakeBoundsSet[corner] = true;
        conoutf(CON_DEBUG, "sound acoustics bake corner %d set to %.1f %.1f %.1f", corner + 1, pos.x, pos.y, pos.z);
    }

    void bakeAcousticGrid(int cellsize, int rays)
    {
        if(worldsize <= 0)
        {
            conoutf(CON_WARN, "sound acoustics: no world for acoustic bake");
            return;
        }
        renderbackground("baking sound acoustics (esc to abort)");
        if(cellsize > 0) soundacousticcellsize = clamp(cellsize, 16, 128);
        rays = clamp(rays > 0 ? rays : soundacousticbakerays, 16, 256);
        soundacousticbakerays = rays;

        clearAcousticGrid();
        renderprogress(0, "finding acoustic grid cells");
        Uint32 start = SDL_GetTicks();
        int csize = max(soundacousticcellsize, 16),
            cellsperaxis = (worldsize + csize - 1)/csize;
        float range = metersToUnits(soundacousticrange);
        ivec mincoord, maxcoord;
        if(!acousticBakeCellBounds(csize, cellsperaxis, mincoord, maxcoord))
        {
            conoutf(CON_WARN, "sound acoustics: invalid acoustic bake bounds");
            return;
        }

        acousticBakeRequests.setsize(0);
        acousticBakeCells.setsize(0);
        acousticBakeTotal = (maxcoord.x - mincoord.x + 1)*(maxcoord.y - mincoord.y + 1)*(maxcoord.z - mincoord.z + 1);
        acousticBakeProcessed = acousticBakeValidCells = 0;
        acousticBakeCanceled = false;
        checkAcousticBakeProgress = false;
        for(int x = mincoord.x; x <= maxcoord.x; x++) for(int y = mincoord.y; y <= maxcoord.y; y++) for(int z = mincoord.z; z <= maxcoord.z; z++)
        {
            ivec coord(x, y, z);
            acousticBakeRequests.add(AcousticBakeRequest(coord));
        }
        if(acousticBakeBoundsEnabled())
        {
            vec bbmin, bbmax;
            acousticBakeBoundsMinMax(bbmin, bbmax);
            conoutf(CON_DEBUG, "sound acoustics: baking bounded cells %d,%d,%d to %d,%d,%d (%.1f %.1f %.1f -> %.1f %.1f %.1f)",
                mincoord.x, mincoord.y, mincoord.z, maxcoord.x, maxcoord.y, maxcoord.z,
                bbmin.x, bbmin.y, bbmin.z, bbmax.x, bbmax.y, bbmax.z);
        }

        if(!acousticBakeMutex) acousticBakeMutex = SDL_CreateMutex();
        if(!acousticRayMutex) acousticRayMutex = SDL_CreateMutex();
        int numthreads = soundacousticthreads > 0 ? soundacousticthreads : numcpus;
        SDL_TimerID timer = SDL_AddTimer(500, acousticBakeTimer, NULL);
        if(numthreads <= 1)
        {
            AcousticBakeWorker worker(rays, range);
            while(!acousticBakeCanceled && worker.bakeNext())
            {
                if(checkAcousticBakeProgress) showAcousticBakeProgress();
            }
        }
        else
        {
            vector<AcousticBakeWorker *> workers;
            renderprogress(0, "creating acoustic bake threads");
            loopi(numthreads)
            {
                AcousticBakeWorker *w = workers.add(new AcousticBakeWorker(rays, range));
                w->thread = SDL_CreateThread(AcousticBakeWorker::run, "acoustic bake", w);
            }
            showAcousticBakeProgress(0, 0);
            while(!acousticBakeCanceled)
            {
                SDL_Delay(500);
                SDL_LockMutex(acousticBakeMutex);
                int processed = acousticBakeProcessed, valid = acousticBakeValidCells, remaining = acousticBakeRequests.length();
                SDL_UnlockMutex(acousticBakeMutex);
                showAcousticBakeProgress(processed, valid);
                if(!remaining && processed >= acousticBakeTotal) break;
            }
            SDL_LockMutex(acousticBakeMutex);
            acousticBakeRequests.setsize(0);
            SDL_UnlockMutex(acousticBakeMutex);
            loopv(workers) SDL_WaitThread(workers[i]->thread, NULL);
            workers.deletecontents();
        }
        if(timer) SDL_RemoveTimer(timer);

        if(acousticBakeCanceled)
        {
            acousticBakeRequests.setsize(0);
            acousticBakeCells.setsize(0);
            clearAcousticGrid();
            conoutf("sound acoustics bake aborted");
            return;
        }

        loopv(acousticBakeCells)
        {
            int idx = acousticCells.length();
            acousticCells.add(acousticBakeCells[i]);
            acousticCellLookup[acousticCells[idx].coord] = idx;
        }
        acousticBakeRequests.setsize(0);
        acousticBakeCells.setsize(0);
        renderprogress(1, "finalizing acoustic grid");
        finalizeAcousticGrid();
        Uint32 end = SDL_GetTicks();
        conoutf(CON_DEBUG, "sound acoustics: baked whole map: %d cells, %d regions, %d portals (%d unit cells, %d rays)",
            acousticCells.length(), acousticRegions.length(), acousticPortals.length(), csize, rays);
        conoutf("baked sound acoustics in %.1f seconds", (end - start)/1000.0f);
    }

    static float smoothstepfactor(int elapsed)
    {
        if(soundacousticsmooth <= 0) return 1.0f;
        return clamp(elapsed/float(soundacousticsmooth), 0.0f, 1.0f);
    }

    static void applyAcousticCell(const AcousticCell &cell, int elapsed)
    {
        float k = acousticProbe.walldist <= 0 ? 1.0f : smoothstepfactor(elapsed);
        acousticProbe.baked = true;
        acousticProbe.cell = acousticCellIndex(cell.coord);
        acousticProbe.region = cell.region;
        loopi(AP_NUM) acousticProbe.scores[i] += (cell.presetScores[i] - acousticProbe.scores[i])*k;
        acousticProbe.indoorChoice = cell.indoorChoice;
        acousticProbe.outdoorChoice = cell.outdoorChoice;
        acousticProbe.outdoorRatio += (cell.outdoorRatio - acousticProbe.outdoorRatio)*k;
        acousticProbe.openness += (cell.openness - acousticProbe.openness)*k;
        acousticProbe.walldist += (metersToUnits(cell.medianDistance) - acousticProbe.walldist)*k;
        acousticProbe.reverbGain += (cell.reverbGain - acousticProbe.reverbGain)*k;
        acousticProbe.reverbDecay += (cell.reverbDecay - acousticProbe.reverbDecay)*k;
        acousticProbe.reflection += (cell.reflection - acousticProbe.reflection)*k;
        acousticProbe.muffleOpen += (cell.muffleOpen - acousticProbe.muffleOpen)*k;
        acousticProbe.reverbShape = cell.reverbShape;

        if(debugsoundacoustics && totalmillis - acousticProbe.lastDebugMillis >= 1000)
        {
            acousticProbe.lastDebugMillis = totalmillis;
            conoutf(CON_DEBUG, "sound acoustics baked: region %d, %s %d%% / %s %d%%, occupancy %d%%, confidence %d%%, sky %d%%, exterior %d%%, median %.1fm, irregular %d%%, corridor %d%%, pca %d%%",
                cell.region, acousticPresets[cell.primaryPreset].name, int((1.0f - cell.presetBlend)*100.0f + 0.5f),
                acousticPresets[cell.secondaryPreset].name, int(cell.presetBlend*100.0f + 0.5f),
                int(cell.airOccupancy*100.0f + 0.5f), int(cell.confidence*100.0f + 0.5f), int(cell.skyOpenness*100.0f + 0.5f),
                int(cell.outdoorConnectivity*100.0f + 0.5f), cell.medianDistance, int(cell.irregularityScore*100.0f + 0.5f),
                int(cell.corridorScore*100.0f + 0.5f), int(cell.pcaAnisotropy*100.0f + 0.5f));
        }
    }

    static void updateEfxReverb()
    {
        bool usebaked = soundacoustics && acousticProbe.baked;
        sound::updateAcousticReverb(usebaked ? &acousticProbe.reverbShape : NULL,
            acousticProbe.reverbGain*soundacousticreverb, acousticProbe.reverbDecay, acousticProbe.reflection);
    }

    void updateAcoustics()
    {
        if(!soundacoustics || !camera1)
        {
            acousticProbe.baked = false;
            acousticProbe.cell = -1;
            acousticProbe.region = -1;
            updateEfxReverb();
            return;
        }

        int now = totalmillis;
        if(!acousticProbe.lastmillis) acousticProbe.lastmillis = now;
        int elapsed = max(now - acousticProbe.lastmillis, 1);
        acousticProbe.lastmillis = now;
        acousticProbe.origin = camera1->o;
        acousticAStarFrame = now;
        acousticAStarNodesThisFrame = 0;
        AcousticCell *cell = findAcousticCell(acousticProbe.origin);
        if(cell)
        {
            applyAcousticCell(*cell, elapsed);
            updateEfxReverb();
            return;
        }

        acousticProbe.baked = false;
        acousticProbe.cell = -1;
        acousticProbe.region = -1;
        updateEfxReverb();
    }

    static bool acousticAStarBudgetAvailable()
    {
        if(acousticAStarFrame != totalmillis)
        {
            acousticAStarFrame = totalmillis;
            acousticAStarNodesThisFrame = 0;
        }
        return soundacousticastarbudget > 0 && acousticAStarNodesThisFrame < soundacousticastarbudget;
    }

    static float acousticAStarHeuristic(const AcousticCell &cell, const AcousticCell &target, float cellsize)
    {
        return cell.origin.dist(target.origin)/max(cellsize, 1.0f);
    }

    static bool acousticAStarPassable(int cellidx, int targetidx)
    {
        if(cellidx == targetidx) return true;
        return acousticCells.inrange(cellidx) && acousticCells[cellidx].valid && acousticCells[cellidx].airOccupancy > 0.5f;
    }

    static void buildAcousticAStarResult(int sourceidx, int targetidx, const vector<AcousticAStarNode> &nodes, AcousticAStarResult &result)
    {
        result.cells.setsize(0);
        for(int cur = targetidx; acousticCells.inrange(cur); cur = nodes[cur].prev)
        {
            result.cells.add(cur);
            if(cur == sourceidx) break;
        }
        result.cells.reverse();
        if(result.cells.empty() || result.cells[0] != sourceidx || result.cells.last() != targetidx)
        {
            result.cells.setsize(0);
            return;
        }

        result.found = true;
        result.pathCost = nodes[targetidx].g;
        result.pathLength = 0.0f;
        result.sourceReverbGain = acousticCells[sourceidx].reverbGain;
        float reverbGain = 0.0f, reverbDecay = 0.0f, reflection = 0.0f, muffleOpen = 0.0f, air = 0.0f;
        loopv(result.cells)
        {
            const AcousticCell &cell = acousticCells[result.cells[i]];
            if(i) result.pathLength += acousticCells[result.cells[i-1]].origin.dist(cell.origin);
            reverbGain += cell.reverbGain;
            reverbDecay += cell.reverbDecay;
            reflection += cell.reflection;
            muffleOpen += cell.muffleOpen;
            air += cell.airOccupancy;
        }
        float count = max(float(result.cells.length()), 1.0f),
              direct = max(acousticCells[sourceidx].origin.dist(acousticProbe.origin), 1.0f);
        result.pathReverbGain = clamp(reverbGain/count, 0.0f, 1.0f);
        result.pathReverbDecay = reverbDecay/count;
        result.pathReflection = clamp(reflection/count, 0.0f, 1.0f);
        result.muffleOpen = clamp(muffleOpen/count, 0.0f, 1.0f);
        result.complexity = clamp((result.pathLength/direct - 1.0f)*0.65f + float(max(result.cells.length() - 2, 0))*0.045f, 0.0f, 1.0f);
        result.diffraction = clamp((1.0f - air/count)*0.60f + result.complexity*0.40f, 0.0f, 1.0f);
        result.occlusion = clamp((result.diffraction*0.55f + result.complexity*0.45f)*soundacousticocclusion, 0.0f, 1.0f);

        vec portal = acousticCells[result.cells.length() >= 2 ? result.cells[result.cells.length() - 2] : targetidx].origin,
            listenerToPortal = vec(portal).sub(acousticProbe.origin);
        if(!listenerToPortal.iszero()) listenerToPortal.safenormalize();
        float portalBias = clamp(0.35f + result.occlusion*0.35f + result.complexity*0.20f, 0.20f, 0.82f),
              behind = max(soundacousticcellsize, 16)*(0.20f + result.occlusion*0.35f);
        result.virtualPosition = vec(acousticCells[sourceidx].origin).mul(1.0f - portalBias).add(vec(portal).mul(portalBias));
        result.virtualPosition.add(vec(listenerToPortal).mul(behind));
    }

    static bool findAcousticAStarPath(int sourceidx, int targetidx, float dist, AcousticAStarResult &result)
    {
        if(!acousticCells.inrange(sourceidx) || !acousticCells.inrange(targetidx) || sourceidx == targetidx) return false;
        if(soundacousticastarrange > 0 && dist > soundacousticastarrange) return false;
        if(!acousticAStarBudgetAvailable()) return false;

        vector<AcousticAStarNode> nodes;
        loopv(acousticCells) nodes.add(AcousticAStarNode());
        vector<AcousticAStarQueueNode> queue;
        float cellsize = max(soundacousticcellsize, 16);
        nodes[sourceidx].g = 0.0f;
        nodes[sourceidx].f = acousticAStarHeuristic(acousticCells[sourceidx], acousticCells[targetidx], cellsize);
        nodes[sourceidx].open = true;
        queue.addheap(AcousticAStarQueueNode(sourceidx, nodes[sourceidx].f));

        static const ivec dirs[6] = { ivec(1, 0, 0), ivec(-1, 0, 0), ivec(0, 1, 0), ivec(0, -1, 0), ivec(0, 0, 1), ivec(0, 0, -1) };
        while(!queue.empty())
        {
            if(!acousticAStarBudgetAvailable()) return false;
            AcousticAStarQueueNode q = queue.removeheap();
            if(!nodes.inrange(q.cell) || nodes[q.cell].closed || q.f > nodes[q.cell].f + 1e-4f) continue;
            acousticAStarNodesThisFrame++;
            if(q.cell == targetidx)
            {
                buildAcousticAStarResult(sourceidx, targetidx, nodes, result);
                return result.found;
            }

            AcousticAStarNode &curNode = nodes[q.cell];
            curNode.closed = true;
            const AcousticCell &curCell = acousticCells[q.cell];
            loopi(6)
            {
                int nextidx = acousticCellIndex(ivec(curCell.coord).add(dirs[i]));
                if(!acousticAStarPassable(nextidx, targetidx) || nodes[nextidx].closed) continue;
                const AcousticCell &nextCell = acousticCells[nextidx];
                float airPenalty = 1.0f + (1.0f - clamp(nextCell.airOccupancy, 0.0f, 1.0f))*1.25f,
                      wallPenalty = 1.0f + nextCell.nearWallRatio*0.35f,
                      g = curNode.g + airPenalty*wallPenalty;
                if(nodes[nextidx].open && g >= nodes[nextidx].g - 1e-4f) continue;
                nodes[nextidx].prev = q.cell;
                nodes[nextidx].g = g;
                nodes[nextidx].f = g + acousticAStarHeuristic(nextCell, acousticCells[targetidx], cellsize);
                nodes[nextidx].open = true;
                queue.addheap(AcousticAStarQueueNode(nextidx, nodes[nextidx].f));
            }
        }
        return false;
    }

    static float acousticTravelReverbSend(float sourceReverbGain, float listenerReverbGain, float pathReverbGain, float occlusion, float pathComplexity, float diffraction)
    {
        float directness = clamp(1.0f - occlusion*0.75f - diffraction*0.35f - pathComplexity*0.25f, 0.0f, 1.0f),
              sourceWeight = max(soundacousticsourcereverbmix*(0.75f + directness*0.25f - occlusion*0.25f), 0.0f),
              listenerWeight = max(soundacousticlistenerreverbmix*(0.85f + listenerReverbGain*0.25f + occlusion*0.35f), 0.0f),
              pathWeight = max(soundacousticpathreverbmix*(0.80f + pathComplexity*0.60f + diffraction*0.40f), 0.0f),
              totalWeight = max(sourceWeight + listenerWeight + pathWeight, 1e-4f),
              mixedReverb = (sourceReverbGain*sourceWeight + listenerReverbGain*listenerWeight + pathReverbGain*pathWeight)/totalWeight,
              travelWetness = clamp(0.30f + occlusion*0.45f + pathComplexity*0.25f + diffraction*0.15f, 0.0f, 1.0f);
        return clamp(mixedReverb*travelWetness*soundacousticreverb, 0.0f, 1.0f);
    }

    static float acousticDirectOcclusion(const vec &from, const vec &to)
    {
        vec dir = vec(to).sub(from);
        float len = dir.magnitude();
        if(len <= 1.0f) return 0.0f;

        dir.div(len);

        vec side(-dir.y, dir.x, 0);
        if(side.iszero()) side = vec(1, 0, 0);
        side.safenormalize();

        vec up(0, 0, 1);

        const float spread = 2.0f,
                    endpointTolerance = min(max(len*0.02f, 1.0f), 4.0f);
        int blocked = 0;

        vec offsets[4] =
        {
            vec(0, 0, 0),
            vec(side).mul(spread),
            vec(side).mul(-spread),
            vec(up).mul(spread)
        };

        loopi(sizeof(offsets)/sizeof(offsets[0]))
        {
            vec start = vec(from).add(offsets[i]),
                end = vec(to).add(offsets[i]),
                ray = vec(end).sub(start);
            float raylen = ray.magnitude();
            if(raylen <= 1.0f) continue;

            ray.div(raylen);

            float hit = acousticRaycube(start, ray, raylen, RAY_POLY);

            if(hit < raylen - endpointTolerance) blocked++;
        }

        if(blocked <= 1) return 0.0f;
        return clamp((blocked - 1)/3.0f, 0.0f, 1.0f);
    }

    static void applySameCellOcclusion(float occlusion, float &volf, float &gainhf)
    {
        float occ = clamp(occlusion*soundacousticocclusion, 0.0f, 0.65f),
              sameCellBlockGain = max(soundacousticblockgain, 0.55f),
              sameCellMuffleGainHF = max(clamp(soundacousticmufflegainhf, 0.02f, 1.0f), 0.35f),
              occVol = powf(occ, 0.85f),
              occHF = powf(occ, 0.60f);
        volf *= 1.0f - occVol*(1.0f - sameCellBlockGain);
        gainhf *= 1.0f - occHF*(1.0f - sameCellMuffleGainHF);
    }

    void acousticSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend, AcousticSourceInfo *info)
    {
        if(info)
        {
            info->apparent = loc;
            info->occlusion = info->virtualGain = 0.0f;
            info->virtualGainHF = 1.0f;
            info->path = false;
        }

        if(!soundacoustics || !acousticProbe.baked || dist <= 1.0f) return;

        float directOcclusion = acousticDirectOcclusion(loc, acousticProbe.origin);
        if(directOcclusion <= 0.0f) return; // Cheap direct test first

        AcousticCell *sourceCell = findAcousticCell(loc);
        if(!sourceCell || !acousticCells.inrange(acousticProbe.cell)) return;
        int sourceidx = acousticCellIndex(sourceCell->coord),
            targetidx = acousticProbe.cell;
        if(sourceidx == targetidx)
        {
            applySameCellOcclusion(directOcclusion, volf, gainhf);
            if(info) info->occlusion = clamp(directOcclusion*soundacousticocclusion, 0.0f, 0.65f);
            return;
        }

        AcousticAStarResult path;
        if(!findAcousticAStarPath(sourceidx, targetidx, dist, path)) return;

        acousticDebugPath = path.cells;
        acousticDebugVirtualSource = path.virtualPosition;
        acousticDebugPathMillis = totalmillis;

        float closedGainHF = clamp(soundacousticmufflegainhf, 0.02f, 1.0f);
        float openGainHF = max(closedGainHF, 0.5f);

        float occ = clamp(path.occlusion, 0.0f, 1.0f);

        // Normal environmental openness
        float routeHF = closedGainHF + (openGainHF - closedGainHF)*path.muffleOpen;

        // But hard obstruction should cap brightness
        float blockedHFMax = 1.0f - powf(occ, 0.45f)*0.92f;
        blockedHFMax = clamp(blockedHFMax, closedGainHF, 1.0f);

        float effectiveMuffleGainHF = min(routeHF, blockedHFMax);

        // Stronger non-linear occlusion, HF dies faster than volume
        float occVol = powf(occ, 0.70f);
        float occHF  = powf(occ, 0.35f);

        volf *= 1.0f - occVol*(1.0f - soundacousticblockgain);
        gainhf *= 1.0f - occHF*(1.0f - effectiveMuffleGainHF);

        reverbSend = max(reverbSend, acousticTravelReverbSend(path.sourceReverbGain, acousticProbe.reverbGain, path.pathReverbGain, occ, path.complexity, path.diffraction));

        if(info)
        {
            info->apparent = path.virtualPosition;
            info->occlusion = path.occlusion;
            info->virtualGain = clamp(path.occlusion*(0.35f + path.complexity*0.45f), 0.0f, 0.75f);
            info->virtualGainHF = clamp(effectiveMuffleGainHF + (1.0f - effectiveMuffleGainHF)*0.45f, 0.02f, 1.0f);
            info->path = true;
        }
    }

    void acousticHudSource(float &reverbSend)
    {
        if(!soundacoustics || !acousticProbe.baked) return;
        reverbSend = max(reverbSend, clamp(acousticProbe.reverbGain*soundacousticreverb, 0.0f, 1.0f));
    }

    static bvec acousticCellDebugColor(const AcousticCell &cell)
    {
        int r = int((cell.boundary ? 255.0f : 80.0f) + cell.nearWallRatio*80.0f),
            g = int(90.0f + cell.outdoorRatio*150.0f),
            b = int(220.0f - cell.outdoorRatio*130.0f);
        return bvec(clamp(r, 0, 255), clamp(g, 0, 255), clamp(b, 0, 255));
    }

    static void drawAcousticCellLine(const vec *v, int a, int b, const bvec &color, uchar alpha)
    {
        gle::attrib(v[a]); gle::attrib(color, alpha);
        gle::attrib(v[b]); gle::attrib(color, alpha);
    }

    static void drawAcousticDebugLine(const vec &a, const vec &b, uchar alpha)
    {
        bvec white(255, 255, 255);
        gle::attrib(a); gle::attrib(white, alpha);
        gle::attrib(b); gle::attrib(white, alpha);
    }

    static void drawAcousticBakeBounds()
    {
        if(!acousticBakeBoundsEnabled()) return;

        vec bbmin, bbmax;
        acousticBakeBoundsMinMax(bbmin, bbmax);
        vec v[8] =
        {
            vec(bbmin.x, bbmin.y, bbmin.z),
            vec(bbmax.x, bbmin.y, bbmin.z),
            vec(bbmax.x, bbmax.y, bbmin.z),
            vec(bbmin.x, bbmax.y, bbmin.z),
            vec(bbmin.x, bbmin.y, bbmax.z),
            vec(bbmax.x, bbmin.y, bbmax.z),
            vec(bbmax.x, bbmax.y, bbmax.z),
            vec(bbmin.x, bbmax.y, bbmax.z)
        };
        bvec red(255, 0, 0);

        GLfloat oldwidth = 1.0f;
        glGetFloatv(GL_LINE_WIDTH, &oldwidth);
        glLineWidth(4.0f);
        gle::begin(GL_LINES, 24);
        drawAcousticCellLine(v, 0, 1, red, 255);
        drawAcousticCellLine(v, 1, 2, red, 255);
        drawAcousticCellLine(v, 2, 3, red, 255);
        drawAcousticCellLine(v, 3, 0, red, 255);
        drawAcousticCellLine(v, 4, 5, red, 255);
        drawAcousticCellLine(v, 5, 6, red, 255);
        drawAcousticCellLine(v, 6, 7, red, 255);
        drawAcousticCellLine(v, 7, 4, red, 255);
        drawAcousticCellLine(v, 0, 4, red, 255);
        drawAcousticCellLine(v, 1, 5, red, 255);
        drawAcousticCellLine(v, 2, 6, red, 255);
        drawAcousticCellLine(v, 3, 7, red, 255);
        xtraverts += gle::end();
        glLineWidth(oldwidth);
    }

    static void drawAcousticAStarDebug(int maxradius)
    {
        if(acousticDebugPath.empty() || !camera1 || totalmillis - acousticDebugPathMillis > 500)
            return;

        int lines = 3;
        loopv(acousticDebugPath) if(acousticCells.inrange(acousticDebugPath[i]) && acousticCells[acousticDebugPath[i]].origin.dist(camera1->o) <= maxradius)
            lines += 3 + (i ? 1 : 0);
        if(lines <= 0) return;

        GLfloat oldwidth = 1.0f;
        glGetFloatv(GL_LINE_WIDTH, &oldwidth);
        glLineWidth(3.0f);
        gle::begin(GL_LINES, lines*2);

        float marker = max(soundacousticcellsize, 16)*0.35f;
        drawAcousticDebugLine(vec(acousticDebugVirtualSource.x - marker, acousticDebugVirtualSource.y, acousticDebugVirtualSource.z), vec(acousticDebugVirtualSource.x + marker, acousticDebugVirtualSource.y, acousticDebugVirtualSource.z), 255);
        drawAcousticDebugLine(vec(acousticDebugVirtualSource.x, acousticDebugVirtualSource.y - marker, acousticDebugVirtualSource.z), vec(acousticDebugVirtualSource.x, acousticDebugVirtualSource.y + marker, acousticDebugVirtualSource.z), 255);
        drawAcousticDebugLine(vec(acousticDebugVirtualSource.x, acousticDebugVirtualSource.y, acousticDebugVirtualSource.z - marker), vec(acousticDebugVirtualSource.x, acousticDebugVirtualSource.y, acousticDebugVirtualSource.z + marker), 255);

        loopv(acousticDebugPath)
        {
            if(!acousticCells.inrange(acousticDebugPath[i])) continue;
            const vec &cur = acousticCells[acousticDebugPath[i]].origin;
            if(cur.dist(camera1->o) > maxradius) continue;
            drawAcousticDebugLine(vec(cur.x - marker*0.45f, cur.y, cur.z), vec(cur.x + marker*0.45f, cur.y, cur.z), 210);
            drawAcousticDebugLine(vec(cur.x, cur.y - marker*0.45f, cur.z), vec(cur.x, cur.y + marker*0.45f, cur.z), 210);
            drawAcousticDebugLine(vec(cur.x, cur.y, cur.z - marker*0.45f), vec(cur.x, cur.y, cur.z + marker*0.45f), 210);
            if(i && acousticCells.inrange(acousticDebugPath[i-1]))
                drawAcousticDebugLine(acousticCells[acousticDebugPath[i-1]].origin, cur, 235);
        }

        xtraverts += gle::end();
        glLineWidth(oldwidth);
    }

    void drawAcousticsDebug()
    {
        if(!debugsoundacoustics) return;
        bool drawbounds = acousticBakeBoundsEnabled(),
             drawgrid = soundacoustics && !acousticCells.empty();
        if(!drawbounds && !drawgrid) return;
        ldrnotextureshader->set();
        GLboolean cull = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        gle::defvertex();
        gle::defcolor(4, GL_UNSIGNED_BYTE);

        if(drawgrid && camera1 && soundacousticgrid)
        {
            int visible = 0,
                maxradius = min(debugsoundacousticsradius, 256);
            loopv(acousticCells) if(acousticCells[i].valid && acousticCells[i].origin.dist(camera1->o) <= maxradius) visible++;
            if(visible > 0)
            {
                float half = max(soundacousticcellsize, 16)*0.5f;
                gle::begin(GL_LINES, visible*24);
                loopv(acousticCells)
                {
                    const AcousticCell &cell = acousticCells[i];
                    if(!cell.valid || cell.origin.dist(camera1->o) > maxradius) continue;
                    vec c = acousticCellCenter(cell.coord),
                        v[8] =
                        {
                            vec(c.x - half, c.y - half, c.z - half),
                            vec(c.x + half, c.y - half, c.z - half),
                            vec(c.x + half, c.y + half, c.z - half),
                            vec(c.x - half, c.y + half, c.z - half),
                            vec(c.x - half, c.y - half, c.z + half),
                            vec(c.x + half, c.y - half, c.z + half),
                            vec(c.x + half, c.y + half, c.z + half),
                            vec(c.x - half, c.y + half, c.z + half)
                        };
                    bvec color = acousticCellDebugColor(cell);
                    uchar alpha = uchar(clamp(45 + int(cell.confidence*150.0f), 45, 195));
                    drawAcousticCellLine(v, 0, 1, color, alpha);
                    drawAcousticCellLine(v, 1, 2, color, alpha);
                    drawAcousticCellLine(v, 2, 3, color, alpha);
                    drawAcousticCellLine(v, 3, 0, color, alpha);
                    drawAcousticCellLine(v, 4, 5, color, alpha);
                    drawAcousticCellLine(v, 5, 6, color, alpha);
                    drawAcousticCellLine(v, 6, 7, color, alpha);
                    drawAcousticCellLine(v, 7, 4, color, alpha);
                    drawAcousticCellLine(v, 0, 4, color, alpha);
                    drawAcousticCellLine(v, 1, 5, color, alpha);
                    drawAcousticCellLine(v, 2, 6, color, alpha);
                    drawAcousticCellLine(v, 3, 7, color, alpha);
                }
                xtraverts += gle::end();
            }
        }
        if(drawgrid) drawAcousticAStarDebug(min(max(debugsoundacousticsradius, 32), 256));
        if(drawbounds) drawAcousticBakeBounds();
        glDepthMask(GL_TRUE);
        if(cull) glEnable(GL_CULL_FACE);
    }

}
