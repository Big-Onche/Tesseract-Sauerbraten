// acoustics.cpp: baked environmental acoustics and source occlusion for sound.cpp

#include "engine.h"
#include "AL/efx-presets.h"
#include "acoustics.h"

namespace acoustics
{
    VARP(soundacoustics, 0, 0, 1);
    VAR(soundacousticsmooth, 0, 150, 2000);
    VAR(debugsoundacoustics, 0, 0, 1);
    FVAR(soundacousticrange, 4.0f, 512.0f, 1024.0f);
    FVAR(soundacousticocclusion, 0.0f, 1.0f, 2.0f);
    FVAR(soundacousticblockgain, 0.05f, 0.7f, 1.0f);
    FVAR(soundacousticmufflegainhf, 0.02f, 0.1f, 1.0f);
    FVARP(soundacousticreverb, 0.0f, 1.2f, 2.0f);
    VARP(soundacousticgrid, 0, 1, 1);
    VARP(soundacousticcellsize, 16, 32, 128);
    VARP(soundacousticbakerays, 16, 64, 256);
    VARP(soundacousticgridradius, 32, 256, 256);
    VARP(soundacousticthreads, 0, 0, 16);

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

    struct AcousticPreset
    {
        const char *name;
        bool outdoor;
        EFXEAXREVERBPROPERTIES efx;
    };

    static const AcousticPreset acousticPresets[AP_NUM] =
    {
        { "small_room", false, EFX_REVERB_PRESET_CASTLE_SMALLROOM },
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
              corridorScore, verticalOpenness, clutterScore, irregularityScore, outdoorConnectivity, presetScores[AP_NUM], presetBlend, confidence,
              reverbGain, reverbDecay, reflection, muffleOpen, outdoorRatio;
        int primaryPreset, secondaryPreset, region, connections;
        bool valid, boundary;
        EFXEAXREVERBPROPERTIES reverbShape;
        AcousticChoice indoorChoice, outdoorChoice;

        AcousticCell() : coord(0, 0, 0), origin(0, 0, 0), airOccupancy(0), skyOpenness(0), openness(0), hitRatio(0), nearWallRatio(0), farWallRatio(0),
            nearDistance(0), medianDistance(0), farDistance(0), distanceVariance(0), corridorScore(0), verticalOpenness(0), clutterScore(0),
            irregularityScore(0), outdoorConnectivity(0), presetBlend(0), confidence(0), reverbGain(0), reverbDecay(0.3f), reflection(0),
            muffleOpen(1), outdoorRatio(1), primaryPreset(AP_OPENOUTDOOR), secondaryPreset(AP_OPENOUTDOOR), region(-1), connections(0),
            valid(false), boundary(false)
        {
            loopi(AP_NUM) presetScores[i] = 0.0f;
            EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
            reverbShape = generic;
            indoorChoice.first = indoorChoice.second = AP_HALL;
            outdoorChoice.first = outdoorChoice.second = AP_OPENOUTDOOR;
        }
    };

    struct AcousticRegion
    {
        int id, firstCell, cellCount, primaryPreset, secondaryPreset;
        vec center;
        float confidence, outdoorRatio, presetBlend;

        AcousticRegion() : id(-1), firstCell(-1), cellCount(0), primaryPreset(AP_OPENOUTDOOR), secondaryPreset(AP_OPENOUTDOOR), center(0, 0, 0),
            confidence(0), outdoorRatio(1), presetBlend(0) {}
    };

    struct AcousticPortal
    {
        int regionA, regionB;
        vec center, normal;
        float apertureSize, openingStrength, acousticCost, highFrequencyLoss, diffractionCost;

        AcousticPortal() : regionA(-1), regionB(-1), center(0, 0, 0), normal(0, 0, 1), apertureSize(0), openingStrength(0), acousticCost(0),
            highFrequencyLoss(0), diffractionCost(0) {}
    };

    struct AcousticPath
    {
        vector<int> regions, portals;
        float acousticDistance, pathCost, diffractionAmount, occlusionAmount;
        int bestPortal;

        AcousticPath() : acousticDistance(0), pathCost(0), diffractionAmount(0), occlusionAmount(0), bestPortal(-1) {}
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

    struct AcousticProbe
    {
        vec origin;
        int lastmillis, lastDebugMillis, cell;
        float openness, walldist, reverbGain, reverbDecay, reflection, outdoorRatio, muffleOpen, scores[AP_NUM];
        bool baked;
        EFXEAXREVERBPROPERTIES reverbShape;
        AcousticChoice indoorChoice, outdoorChoice;

        AcousticProbe() : origin(0, 0, 0), lastmillis(0), lastDebugMillis(0), cell(-1), openness(1), walldist(0), reverbGain(0), reverbDecay(0.3f), reflection(0), outdoorRatio(1), muffleOpen(1), baked(false)
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
    static hashtable<ivec, int> acousticCellLookup(1<<12);

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
        loopi(6) if(acousticRaycube(p, dirs[i], clearance, RAY_CLIPMAT|RAY_POLY) >= clearance*0.95f) return true;
        return false;
    }

    static void scoreAcousticCell(AcousticCell &cell, float hits, float skyOpen, float ceilingDist, float horizontalHitRatio, float horizontalNearRatio,
        float horizontalFarRatio, float horizontalFarHitRatio, float horizontalOpenRatio, float medianHorizontalDistance, float horizontalVariance,
        float nearPercentile, float medianHitDistance, float farPercentile, float corridorScore, float downOpenRatio, float range)
    {
        float varianceScore = clamp(sqrtf(horizontalVariance)/max(metersToUnits(medianHorizontalDistance), 1.0f), 0.0f, 1.0f),
              percentileSpread = clamp((farPercentile - nearPercentile)/max(farPercentile, 1.0f), 0.0f, 1.0f),
              irregularityScore = clamp(varianceScore*0.65f + percentileSpread*0.35f, 0.0f, 1.0f),
              outdoorRaw = smoothramp(skyOpen, 0.35f, 0.70f),
              indoorRaw = 1.0f - outdoorRaw,
              ceilingOpen = skyOpen,
              ceilingBlocked = 1.0f - ceilingOpen,
              fill = clamp(horizontalNearRatio + horizontalHitRatio - horizontalFarHitRatio, 0.0f, 1.0f),
              roomSizeScore = 1.0f - smoothramp(medianHorizontalDistance, 6.0f, 24.0f);

        cell.presetScores[AP_SMALLROOM] = indoorRaw*(0.35f + horizontalHitRatio*0.65f)*roomSizeScore*(1.0f - irregularityScore*0.45f)*(1.0f - horizontalFarHitRatio*0.35f)*0.80f;
        cell.presetScores[AP_HALL] = indoorRaw*(0.30f + horizontalHitRatio*0.70f)*smoothramp(medianHorizontalDistance, 8.0f, 28.0f)*(0.40f + horizontalFarHitRatio*0.60f)*(1.0f - corridorScore*0.75f)*(1.0f - irregularityScore*0.35f)*1.05f;
        cell.presetScores[AP_CORRIDOR] = indoorRaw*(0.25f + horizontalHitRatio*0.75f)*corridorScore*(0.60f + fill*0.40f)*1.55f;
        cell.presetScores[AP_CAVE] = indoorRaw*(0.25f + hits*0.50f)*smoothramp(irregularityScore, 0.35f, 0.85f)*(0.25f + horizontalFarHitRatio*0.45f)*(0.35f + ceilingBlocked*0.35f)*(1.0f - corridorScore*0.70f)*0.85f;
        cell.presetScores[AP_OPENOUTDOOR] = outdoorRaw*skyOpen*(0.45f + horizontalOpenRatio*0.55f)*(1.0f - horizontalNearRatio)*(1.0f - corridorScore*0.50f);
        cell.presetScores[AP_COURTYARD] = outdoorRaw*skyOpen*(0.25f + horizontalNearRatio*0.75f)*(0.25f + horizontalHitRatio*0.75f)*(1.0f - corridorScore*0.40f);
        cell.presetScores[AP_STREET] = outdoorRaw*skyOpen*(0.25f + horizontalNearRatio*0.75f)*corridorScore;
        cell.presetScores[AP_CANYON] = outdoorRaw*skyOpen*(0.25f + horizontalFarHitRatio*0.75f)*(0.35f + irregularityScore*0.65f)*(0.70f + downOpenRatio*0.30f);

        if(indoorRaw > 0.2f && cell.presetScores[AP_SMALLROOM] + cell.presetScores[AP_HALL] + cell.presetScores[AP_CORRIDOR] + cell.presetScores[AP_CAVE] <= 1e-4f)
            cell.presetScores[medianHorizontalDistance < 10.0f ? AP_SMALLROOM : AP_HALL] = indoorRaw;
        if(outdoorRaw > 0.2f && cell.presetScores[AP_OPENOUTDOOR] + cell.presetScores[AP_COURTYARD] + cell.presetScores[AP_STREET] + cell.presetScores[AP_CANYON] <= 1e-4f)
            cell.presetScores[AP_OPENOUTDOOR] = outdoorRaw;

        cell.indoorChoice = chooseAcousticPresets(cell.presetScores, false, AP_HALL);
        cell.outdoorChoice = chooseAcousticPresets(cell.presetScores, true, AP_OPENOUTDOOR);

        EFXEAXREVERBPROPERTIES indoorShape = blendEfx(acousticPresets[cell.indoorChoice.first].efx, acousticPresets[cell.indoorChoice.second].efx, cell.indoorChoice.secondWeight),
                                outdoorShape = blendEfx(acousticPresets[cell.outdoorChoice.first].efx, acousticPresets[cell.outdoorChoice.second].efx, cell.outdoorChoice.secondWeight);
        cell.reverbShape = blendEfx(indoorShape, outdoorShape, outdoorRaw);

        float best = -1.0f, next = -1.0f;
        cell.primaryPreset = AP_OPENOUTDOOR;
        cell.secondaryPreset = AP_OPENOUTDOOR;
        loopi(AP_NUM)
        {
            float score = cell.presetScores[i];
            if(score > best)
            {
                next = best;
                cell.secondaryPreset = cell.primaryPreset;
                best = score;
                cell.primaryPreset = i;
            }
            else if(score > next)
            {
                next = score;
                cell.secondaryPreset = i;
            }
        }
        float total = max(best + max(next, 0.0f), 1e-4f);
        cell.presetBlend = clamp(max(next, 0.0f)/total, 0.0f, 1.0f);

        float indoorStrength = (1.0f - outdoorRaw)*clamp(0.25f + hits*0.35f + horizontalFarHitRatio*0.25f + fill*0.15f, 0.0f, 1.0f),
              outdoorStrength = outdoorRaw*clamp(0.07f + horizontalNearRatio*0.35f + horizontalFarHitRatio*0.30f + corridorScore*0.25f, 0.04f, 0.75f),
              indoorMuffleOpen = clamp(smoothramp(medianHorizontalDistance, 6.0f, 36.0f)*0.70f + horizontalOpenRatio*0.30f, 0.0f, 1.0f),
              outdoorMuffleOpen = clamp(skyOpen*0.55f + horizontalOpenRatio*0.35f + smoothramp(medianHorizontalDistance, 24.0f, 96.0f)*0.10f, 0.0f, 1.0f);

        cell.skyOpenness = skyOpen;
        cell.hitRatio = hits;
        cell.nearWallRatio = horizontalNearRatio;
        cell.farWallRatio = horizontalFarRatio;
        cell.nearDistance = nearPercentile;
        cell.medianDistance = medianHitDistance;
        cell.farDistance = farPercentile;
        cell.distanceVariance = horizontalVariance;
        cell.corridorScore = corridorScore;
        cell.verticalOpenness = clamp(skyOpen*0.7f + downOpenRatio*0.3f, 0.0f, 1.0f);
        cell.clutterScore = fill;
        cell.irregularityScore = irregularityScore;
        cell.outdoorConnectivity = outdoorRaw;
        cell.outdoorRatio = outdoorRaw;
        cell.openness = clamp(1.0f - hits, 0.0f, 1.0f);
        cell.reverbGain = clamp(indoorStrength + outdoorStrength, 0.0f, 1.0f);
        cell.reverbDecay = cell.reverbShape.flDecayTime;
        cell.reflection = clamp(horizontalNearRatio*0.35f + horizontalFarHitRatio*0.25f + corridorScore*0.30f + hits*0.15f, 0.0f, 1.0f);
        cell.muffleOpen = clamp(indoorMuffleOpen*(1.0f - outdoorRaw) + outdoorMuffleOpen*outdoorRaw, 0.0f, 1.0f);
        cell.confidence = clamp(cell.airOccupancy*(1.0f - cell.boundary*0.35f)*(0.45f + hits*0.30f + min(range, ceilingDist + metersToUnits(medianHitDistance))/max(range, 1.0f)*0.25f), 0.05f, 1.0f);
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
              downCount = 0, downOpen = 0;
        vector<float> hitDistances, horizontalDistances;

        struct AcousticSector
        {
            int count, hits;
            float dist, nearHits;

            AcousticSector() : count(0), hits(0), dist(0), nearHits(0) {}
        } sectors[8];

        loopi(rays)
        {
            float z = 1.0f - (2.0f*(i + 0.5f))/rays,
                  r = sqrtf(max(0.0f, 1.0f - z*z)),
                  a = golden*i;
            vec dir(cosf(a)*r, sinf(a)*r, z);
            float dist = acousticRaycube(cell.origin, dir, range, RAY_CLIPMAT|RAY_POLY|RAY_SKIPFIRST);
            dist = clamp(dist, 0.0f, range);
            bool hit = dist < maxHitDist;
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

                float yaw = atan2f(dir.y, dir.x);
                int sector = clamp(int(floorf((yaw + PI)*(8.0f/(2.0f*PI)))), 0, 7);
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

        float sectorOpen[8], sectorNear[8];
        loopi(8)
        {
            sectorOpen[i] = sectors[i].count ? clamp(sectors[i].dist/(sectors[i].count*range), 0.0f, 1.0f) : open;
            sectorNear[i] = sectors[i].count ? clamp(sectors[i].nearHits/sectors[i].count, 0.0f, 1.0f) : horizontalNearRatio;
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
        float varianceScore = clamp(sqrtf(horizontalVariance)/max(horizontalAvgDist, 1.0f), 0.0f, 1.0f),
              corridorScore = clamp(max(min(horizontalNearRatio, horizontalFarRatio)*max(varianceScore, 0.25f), smoothramp(sectorCorridor, 0.06f, 0.36f)), 0.0f, 1.0f);

        scoreAcousticCell(cell, hits, skyOpen, ceilingDist, horizontalHitRatio, horizontalNearRatio, horizontalFarRatio, horizontalFarHitRatio,
            horizontalOpenRatio, medianHorizontalDistance, horizontalVariance, nearPercentile, medianHitDistance, farPercentile, corridorScore, downOpenRatio, range);
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
        if(a.primaryPreset == b.primaryPreset) return true;
        if(fabs(a.outdoorRatio - b.outdoorRatio) > 0.35f) return false;
        return max(a.presetScores[b.primaryPreset], b.presetScores[a.primaryPreset]) > 0.20f;
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

        acousticRegions.setsize(0);
        acousticPortals.setsize(0);
        vector<int> pending;
        loopv(acousticCells) acousticCells[i].region = -1;
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
                loopj(6)
                {
                    int nextidx = acousticCellIndex(ivec(cur.coord).add(dirs[j]));
                    if(!acousticCells.inrange(nextidx)) continue;
                    AcousticCell &next = acousticCells[nextidx];
                    if(!next.valid || next.region >= 0 || !acousticCellsCompatible(start, next)) continue;
                    next.region = region.id;
                    pending.add(nextidx);
                }
            }
            if(region.cellCount > 0)
            {
                region.center.div(float(region.cellCount));
                region.confidence = clamp(region.confidence/region.cellCount, 0.0f, 1.0f);
                region.outdoorRatio = clamp(region.outdoorRatio/region.cellCount, 0.0f, 1.0f);
            }
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
                AcousticPortal &portal = acousticPortals.add();
                portal.regionA = cell.region;
                portal.regionB = next.region;
                portal.center = vec(cell.origin).add(next.origin).mul(0.5f);
                portal.normal = vec(next.origin).sub(cell.origin);
                if(!portal.normal.iszero()) portal.normal.normalize();
                portal.apertureSize = max(soundacousticcellsize, 16)*min(cell.airOccupancy, next.airOccupancy);
                portal.openingStrength = clamp((cell.airOccupancy + next.airOccupancy)*0.5f, 0.0f, 1.0f);
                portal.acousticCost = 1.0f - portal.openingStrength;
                portal.highFrequencyLoss = clamp((cell.boundary || next.boundary ? 0.35f : 0.10f) + portal.acousticCost*0.40f, 0.0f, 1.0f);
                portal.diffractionCost = clamp((1.0f - portal.openingStrength)*0.75f + max(cell.nearWallRatio, next.nearWallRatio)*0.25f, 0.0f, 1.0f);
            }
        }
    }

    void clearAcousticGrid()
    {
        acousticCells.setsize(0);
        acousticRegions.setsize(0);
        acousticPortals.setsize(0);
        acousticCellLookup.clear();
        acousticProbe.baked = false;
        acousticProbe.cell = -1;
    }

    int numAcousticCells() { return acousticCells.length(); }
    int numAcousticRegions() { return acousticRegions.length(); }
    int numAcousticPortals() { return acousticPortals.length(); }

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

        acousticBakeRequests.setsize(0);
        acousticBakeCells.setsize(0);
        acousticBakeTotal = cellsperaxis*cellsperaxis*cellsperaxis;
        acousticBakeProcessed = acousticBakeValidCells = 0;
        acousticBakeCanceled = false;
        checkAcousticBakeProgress = false;
        loop(x, cellsperaxis) loop(y, cellsperaxis) loop(z, cellsperaxis)
        {
            ivec coord(x, y, z);
            acousticBakeRequests.add(AcousticBakeRequest(coord));
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
            conoutf(CON_DEBUG, "sound acoustics baked: region %d, %s %d%% / %s %d%%, occupancy %d%%, confidence %d%%, sky %d%%, median %.1fm, irregular %d%%, corridor %d%%",
                cell.region, acousticPresets[cell.primaryPreset].name, int((1.0f - cell.presetBlend)*100.0f + 0.5f),
                acousticPresets[cell.secondaryPreset].name, int(cell.presetBlend*100.0f + 0.5f),
                int(cell.airOccupancy*100.0f + 0.5f), int(cell.confidence*100.0f + 0.5f), int(cell.skyOpenness*100.0f + 0.5f),
                cell.medianDistance, int(cell.irregularityScore*100.0f + 0.5f), int(cell.corridorScore*100.0f + 0.5f));
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
            updateEfxReverb();
            return;
        }

        int now = totalmillis;
        if(!acousticProbe.lastmillis) acousticProbe.lastmillis = now;
        int elapsed = max(now - acousticProbe.lastmillis, 1);
        acousticProbe.lastmillis = now;
        acousticProbe.origin = camera1->o;
        AcousticCell *cell = findAcousticCell(acousticProbe.origin);
        if(cell)
        {
            applyAcousticCell(*cell, elapsed);
            updateEfxReverb();
            return;
        }

        acousticProbe.baked = false;
        acousticProbe.cell = -1;
        updateEfxReverb();
    }

    void acousticSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend)
    {
        if(!soundacoustics || !acousticProbe.baked || dist <= 1.0f) return;
        vec dir = vec(loc).sub(acousticProbe.origin);
        if(dir.iszero()) return;
        dir.normalize();
        float clear = acousticRaycube(acousticProbe.origin, dir, dist, RAY_CLIPMAT|RAY_POLY|RAY_SKIPFIRST),
              pathOpen = clear + 2.0f < dist ? clamp(clear/max(dist, 1.0f), 0.0f, 1.0f) : 1.0f,
              occlusion = clamp(powf(1.0f - pathOpen, 0.75f)*soundacousticocclusion, 0.0f, 1.0f),
              muffleOpen = clamp(pathOpen*0.65f + acousticProbe.muffleOpen*0.35f, 0.0f, 1.0f),
              closedGainHF = clamp(soundacousticmufflegainhf, 0.02f, 1.0f),
              openGainHF = max(closedGainHF, 0.5f),
              effectiveMuffleGainHF = closedGainHF + (openGainHF - closedGainHF)*muffleOpen;
        volf *= 1.0f - occlusion*(1.0f - soundacousticblockgain);
        gainhf *= 1.0f - occlusion*(1.0f - effectiveMuffleGainHF);
        reverbSend = max(reverbSend, clamp((acousticProbe.reverbGain*(0.35f + 0.65f*occlusion))*soundacousticreverb, 0.0f, 1.0f));
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

    void drawAcousticsDebug()
    {
        if(!soundacoustics || !debugsoundacoustics || acousticCells.empty()) return;
        ldrnotextureshader->set();
        GLboolean cull = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        gle::defvertex();
        gle::defcolor(4, GL_UNSIGNED_BYTE);

        if(camera1 && soundacousticgrid && !acousticCells.empty())
        {
            int visible = 0,
                maxradius = min(soundacousticgridradius, 256);
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
        glDepthMask(GL_TRUE);
        if(cull) glEnable(GL_CULL_FACE);
    }

}
