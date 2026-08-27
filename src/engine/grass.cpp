#include "engine.h"

namespace grass
{

static void settingsChanged();
static void clearBurnField();

static const int patchSize = 32, maxInstances = 1<<20, impulseMaxEvents = 64, impulseMaxPatch = 4,
                 burnFieldTexelSize = 8, burnFieldMaxSize = 2048;
static const int IMPULSE_PLAYER = 2;

VARP(grass, 0, 1, 1);
VARP(grassdist, 0, 768, 10000);
FVARP(grasstaper, 0, 0.3f, 1);
FVARFP(grassstep, 0.5f, 3.5, 8, settingsChanged());
FVARFP(grassdensity, 0.05f, 0.4, 4, settingsChanged());
VAR(grassheight, 1, 7, 64);
FVARP(grassmargin, 0, 0.25f, 32);
FVAR(grassmarginfade, 0, 0, 1);
VARR(grassscale, 1, 2, 64);
CVAR0R(grasscolour, 0xFFFFFF);
FVAR(grassblendthreshold, 0, 0.7f, 1);

VARP(grassloddist, 8, 192, 10000);
VAR(grasslodtransition, 0, 64, 256);
FVAR(grassfardensity, 0.05f, 1, 1);

FVAR(grassrootdarkness, 0, 1.5f, 2.0f);
FVAR(grassrootdarknesspower, 0.25f, 5.0f, 16.0f);

VARP(grassshadowcascades, 0, 2, 2);
FVARF(grassshadowdensitynear, 0.01f, 0.75f, 1, settingsChanged());
FVARF(grassshadowdensity, 0.01f, 0.5f, 1, settingsChanged());
FVAR(grassshadowtransition, 0, 1.0f, 1.0f);
VAR(grassshadowlodnear, 0, 1, 1);
VAR(grassshadowlodfar, 0, 1, 1);
VAR(grassshadowwindcascades, 0, 1, 2);
VAR(grassstats, 0, 0, 1);

VAR(grassanimmillis, 1, 3000, 60000);
FVAR(grassanimscale, 0, 0.2f, 1);
FVAR(grasswindangle, 0, 0, 360);
FVAR(grasswindscale, 0, 0.015f, 1);
FVAR(grasswindnoisescale, 0.0001f, 0.05f, 0.1f);
FVAR(grasswindscrollspeed, 0, 0.1f, 1);
FVAR(grasswindguststrength, 0, 0.55f, 2);
FVAR(grasswinddirectionvariation, 0, 0.2f, 1);
FVAR(grasswinddetailscale, 1, 5, 32);
FVAR(grasswinddetailstrength, 0, 0.2f, 1);

VARP(grassimpulses, 0, 1, 1);
FVARP(grassimpulsebulletdist, 0, 256, 10000);
FVARP(grassimpulsedist, 0, 512, 10000);
FVAR(grassimpulsestrength, 0, 1, 5);
FVAR(grassimpulsewobble, 0, 0.6f, 1);
FVAR(grassimpulsewobblespeed, 0, 32, 64);
FVAR(grassimpulseafterwind, 0, 4, 5);
VAR(grassimpulseafterwindmillis, 0, 1500, 2000);
FVAR(grassimpulseafterwindwobblespeed, 0, 6, 32);
FVAR(grassimpulseminstrength, 0, 0.01f, 10);
VAR(grassimpulsebulletmergemillis, 0, 250, 500);
FVAR(grassimpulsebulletmergedist, 0, 16, 64);

VARP(grassplayerflatten, 0, 1, 1);
FVARP(grassplayerflattendist, 0, 256, 10000);
FVAR(grassplayerflattenradius, 1, 8, 64);
FVAR(grassplayerflattenstrength, 0, 3, 5);
VAR(grassplayerflattenmillis, 100, 600, 3000);
FVAR(grassplayerflattenstep, 0.5f, 2, 32);

VARP(grassimpactparticles, 0, 1, 1);
VAR(grassplayerparticlemillis, 16, 80, 1000);
FVAR(grassparticlesize, 0.1f, 0.5f, 8);
FVAR(grassparticlebrightness, 0, 0.8f, 1);

VAR(grassburnholdmillis, 0, 25000, 300000);
VAR(grassburnfademillis, 100, 15000, 300000);
VAR(grassburnpropagatemillis, 1, 500, 10000);
VAR(grassburnfadeinmillis, 1, 150, 5000);
VAR(grassburnmillis, 1, 500, 5000);
CVAR0R(grassburncolour, 0x4A3838);
CVAR0R(grassburnglowcolour, 0xFF2B00);
FVAR(grassburnpadding, -100, 10, 100);
FVAR(grassburnvariation, 0, 0.4f, 1);
FVAR(grassburnscrollx, -2, 0.03f, 2);
FVAR(grassburnscrolly, -2, 0.75f, 2);
FVAR(grassburnglowscale, 0, 6, 10);
VAR(grassburnparticles, 0, 1, 1);
VAR(grassburnparticlemillis, 16, 500, 1000);
VAR(grassburnparticlemax, 1, 32, 64);
VAR(grassburnparticleminmillis, 0, 3000, 10000);
VAR(grassburnparticlemaxmillis, 0, 6000, 10000);
FVAR(grassburnsparkspread, 0, 16, 512);
FVAR(grassburnflamethrowerradius, 0, 12, 256);
VAR(grassburncarrierinterval, 16, 500, 1000);
FVAR(grassburncarrierstep, 0, 48, 64);

enum { MAX_IMPULSES_PER_PATCH = 8, IMPULSE_CELL_SIZE = 64, MAX_BURN_EVENTS = 4096, BURN_FIELD_TILE_SIZE = 16,
       MAX_BURN_EVENT_PARTICLE_POSITIONS = 64, BURN_TIME_QUANTUM = 16, BURN_TIME_PERIOD = 65535 };

struct DebugStats
{
    int patches, drawCalls, instances, sourceTris;
    int impulseCandidateChecks, impulseRelevantChecks, impulsePatchesAffected;
    int burnCandidateChecks, burnRelevantChecks, burnPatchesAffected, burnFieldDirtyTiles, burnFieldTexelsUpdated;

    DebugStats() : patches(0), drawCalls(0), instances(0), sourceTris(0), impulseCandidateChecks(0), impulseRelevantChecks(0),
                   impulsePatchesAffected(0), burnCandidateChecks(0), burnRelevantChecks(0), burnPatchesAffected(0), burnFieldDirtyTiles(0),
                   burnFieldTexelsUpdated(0)
    {
    }
};

static uint hash(uint n);
static uint damageSeed = 0;

struct Impulse
{
    vec position, direction;
    float radius, strength, propagationSpeed, falloff, radial;
    int startTime, lastRefresh, lastParticleTime, lifetime, type;
    const dynent *owner;
    uint queryVersion;
};

static vector<Impulse> impulseList;
static hashtable<uint, vector<int> > impulseGrid(1<<10);
static uint impulseQueryVersion = 0;

struct GrassParticleEvent
{
    vec position, direction;
    float radius, strength;
    int type, spawnTime;
};

static vector<GrassParticleEvent> grassParticleEvents;

static void queueGrassParticleEvent(const vec &position, const vec &direction, float radius, float strength, int type)
{
    if(!grassimpactparticles || (position.x < 0 && position.y < 0 && position.z < 0) || radius <= 0 || strength <= 0) return;
    if(grassParticleEvents.length() >= impulseMaxEvents) grassParticleEvents.remove(0);

    GrassParticleEvent &event = grassParticleEvents.add();
    event.position = position;
    event.direction = direction;
    event.radius = radius;
    event.strength = strength;
    event.type = type;
    event.spawnTime = lastmillis;
}

static float impulseRemainingStrength(const Impulse &impulse)
{
    int referenceTime = impulse.type == IMPULSE_PLAYER ? impulse.lastRefresh : impulse.startTime;
    float age = clamp((lastmillis - referenceTime)/float(max(impulse.lifetime, 1)), 0.0f, 1.0f);
    return impulse.strength*(1.0f - age);
}

static void pruneImpulses()
{
    loopvrev(impulseList)
    {
        const Impulse &impulse = impulseList[i];
        int referenceTime = impulse.type == IMPULSE_PLAYER ? impulse.lastRefresh : impulse.startTime;
        if(lastmillis - referenceTime >= impulse.lifetime) impulseList.removeunordered(i);
    }

    while(impulseList.length() > impulseMaxEvents)
    {
        int weakest = 0;
        loopv(impulseList)
            if(impulseRemainingStrength(impulseList[i]) < impulseRemainingStrength(impulseList[weakest])) weakest = i;
        impulseList.removeunordered(weakest);
    }
}

void addImpulse(const vec &position, const vec &direction, float radius, float strength, int lifetime, int type, float propagationSpeed,
                float falloff, float radial)
{
    if(!grassimpulses || (position.x < 0 && position.y < 0 && position.z < 0) || radius <= 0 || strength <= 0 || lifetime <= 0 ||
       (type != IMPULSE_BULLET && type != IMPULSE_EXPLOSION)) return;

    queueGrassParticleEvent(position, direction, radius, strength, type);
    pruneImpulses();
    int effectLifetime = lifetime + (grassimpulseafterwind > 0 ? grassimpulseafterwindmillis : 0);
    vec normalizedDir = vec(direction).safenormalize();

    if(type == IMPULSE_BULLET && grassimpulsebulletmergemillis > 0 && grassimpulsebulletmergedist > 0)
    {
        float mergeDist = max(grassimpulsebulletmergedist, radius*0.5f), mergeDistSquared = mergeDist*mergeDist;
        loopv(impulseList)
        {
            Impulse &impulse = impulseList[i];

            if(impulse.type != type || lastmillis - impulse.startTime > grassimpulsebulletmergemillis ||
               impulse.position.squaredist(position) > mergeDistSquared) continue;

            impulse.position.add(position).mul(0.5f);
            impulse.direction.add(normalizedDir).safenormalize();
            impulse.radius = max(impulse.radius, radius);
            impulse.strength = max(impulse.strength, strength);
            impulse.lifetime = max(impulse.lifetime, effectLifetime);
            impulse.startTime = lastmillis;
            impulse.lastRefresh = lastmillis;
            impulse.lastParticleTime = 0;
            impulse.falloff = max(impulse.falloff, falloff);
            impulse.radial = max(impulse.radial, radial);
            return;
        }
    }

    int target = impulseList.length();

    if(target >= impulseMaxEvents)
    {
        int weakest = 0;

        loopv(impulseList)
        {
            if(impulseRemainingStrength(impulseList[i]) < impulseRemainingStrength(impulseList[weakest])) weakest = i;
        }

        if(strength <= impulseRemainingStrength(impulseList[weakest])) return;
        target = weakest;
    }
    else impulseList.add();

    Impulse &impulse = impulseList[target];
    impulse.position = position;
    impulse.direction = normalizedDir;
    impulse.radius = radius;
    impulse.strength = strength;
    impulse.propagationSpeed = max(propagationSpeed, 0.0f);
    impulse.falloff = clamp(falloff, 0.0f, 1.0f);
    impulse.radial = max(radial, 0.0f);
    impulse.startTime = lastmillis;
    impulse.lastRefresh = lastmillis;
    impulse.lastParticleTime = 0;
    impulse.lifetime = effectLifetime;
    impulse.type = type;
    impulse.owner = NULL;
    impulse.queryVersion = 0;
}

static Impulse &addPlayerImpulse(const dynent *owner, const vec &position, const vec &direction, float radius)
{
    int target = impulseList.length();

    if(target >= impulseMaxEvents)
    {
        target = 0;
        loopv(impulseList) if(impulseRemainingStrength(impulseList[i]) < impulseRemainingStrength(impulseList[target])) target = i;
    }
    else impulseList.add();

    Impulse &impulse = impulseList[target];
    impulse.position = position;
    impulse.direction = vec(direction.x, direction.y, 0).safenormalize();
    impulse.radius = radius;
    impulse.strength = grassplayerflattenstrength;
    impulse.propagationSpeed = 0;
    impulse.falloff = 1;
    impulse.radial = 1;
    impulse.startTime = impulse.lastRefresh = lastmillis;
    impulse.lastParticleTime = lastmillis;
    impulse.lifetime = grassplayerflattenmillis;
    impulse.type = IMPULSE_PLAYER;
    impulse.owner = owner;
    impulse.queryVersion = 0;
    return impulse;
}

static void updatePlayerImpulses()
{
    if(!grassimpulses || !grassplayerflatten || grassplayerflattenstrength <= 0 || grassplayerflattendist <= 0) return;

    loopi(game::numdynents())
    {
        dynent *d = game::iterdynents(i);
        if(!d || d->type != ENT_PLAYER || d->state != CS_ALIVE || d->physstate <= PHYS_FALL || d->inwater) continue;

        vec position = d->feetpos(0.2f);
        if(camera1->o.squaredist(position) > grassplayerflattendist*grassplayerflattendist) continue;

        vec direction(d->vel.x, d->vel.y, 0);
        float radius = max(grassplayerflattenradius, d->radius), step = min(grassplayerflattenstep, radius);
        Impulse *contact = NULL;

        loopv(impulseList) if(impulseList[i].type == IMPULSE_PLAYER && impulseList[i].owner == d)
        {
            contact = &impulseList[i];
            break;
        }

        if(contact && contact->position.squaredist(position) >= step*step)
        {
            int lastParticleTime = contact->lastParticleTime;
            if(lastmillis - lastParticleTime >= grassplayerparticlemillis)
            {
                queueGrassParticleEvent(position, direction, radius, grassplayerflattenstrength, IMPULSE_PLAYER);
                lastParticleTime = lastmillis;
            }
            contact->owner = NULL;
            contact = &addPlayerImpulse(d, position, direction, radius);
            contact->lastParticleTime = lastParticleTime;
        }
        if(!contact) contact = &addPlayerImpulse(d, position, direction, radius);
        else
        {
            if(direction.squaredlen() > 1e-4f) contact->direction = direction.safenormalize();
            contact->radius = radius;
            contact->strength = grassplayerflattenstrength;
            contact->lifetime = grassplayerflattenmillis;
            contact->lastRefresh = lastmillis;
        }
    }
}

void clearImpulses()
{
    impulseList.setsize(0);
    grassParticleEvents.setsize(0);
    impulseGrid.clear();
    impulseQueryVersion = 0;
}

struct BurnParticlePosition
{
    vec position;
    int ignition;
};

struct BurnFieldUpdate
{
    int index, ignition, recoverStart, expiry;
    ushort mask;
};

struct BurnEvent
{
    vec center;
    float radius, seed;
    int burnStart, recoverStart, particleEndTime, lastEmitMillis, numParticlePositions;
    bool needsRaster;
    BurnParticlePosition particlePositions[MAX_BURN_EVENT_PARTICLE_POSITIONS];
};

struct BurnCarrier
{
    size_t owner;
    vec center;
    float radius;
    int lastSpawnMillis;
};

static vector<BurnEvent> burnEvents;
static vector<BurnCarrier> burnCarriers;

void addBurnEvent(const vec &center, float radius, int lifetime)
{
    if((center.x < 0 && center.y < 0 && center.z < 0) || radius <= 0) return;
    if(burnEvents.length() >= MAX_BURN_EVENTS)
        burnEvents.remove(0);

    BurnEvent &event = burnEvents.add();
    event.center = center;
    event.radius = radius;
    event.seed = (hash(++damageSeed) & 0xFFFFFFu)/float(0x1000000u);
    event.burnStart = lastmillis;
    event.recoverStart = lastmillis + grassburnpropagatemillis + max(grassburnfadeinmillis, grassburnmillis) + (lifetime >= 0 ? lifetime : grassburnholdmillis);

    int minParticleMillis = min(grassburnparticleminmillis, grassburnparticlemaxmillis),
        maxParticleMillis = max(grassburnparticleminmillis, grassburnparticlemaxmillis);

    event.particleEndTime = lastmillis + minParticleMillis + int((maxParticleMillis - minParticleMillis)*event.seed + 0.5f);
    event.lastEmitMillis = lastmillis - grassburnparticlemillis;
    event.numParticlePositions = -1;
    event.needsRaster = true;
}

void carryBurnEvent(size_t owner, const vec &center, float radius)
{
    if((center.x < 0 && center.y < 0 && center.z < 0) || radius <= 0) return;

    loopv(burnCarriers) if(burnCarriers[i].owner == owner)
    {
        BurnCarrier &carrier = burnCarriers[i];
        float movedDist = carrier.center.squaredist(center),
              mergeStep = max(min(grassburncarrierstep, max(radius, carrier.radius)), 0.5f*burnFieldTexelSize);

        if(movedDist <= 1e-4f || lastmillis - carrier.lastSpawnMillis < grassburncarrierinterval || movedDist < mergeStep*mergeStep) return;
        carrier.center = center;
        carrier.radius = radius;
        carrier.lastSpawnMillis = lastmillis;
        addBurnEvent(center, radius);
        return;
    }

    BurnCarrier &carrier = burnCarriers.add();
    carrier.owner = owner;
    carrier.center = center;
    carrier.radius = radius;
    carrier.lastSpawnMillis = lastmillis;
    addBurnEvent(center, radius);
}

void removeBurnEvent(size_t owner)
{
    loopvrev(burnCarriers) if(burnCarriers[i].owner == owner) burnCarriers.removeunordered(i);
}

void clearBurnEvents()
{
    burnEvents.setsize(0);
    burnCarriers.setsize(0);
    damageSeed = 0;
    clearBurnField();
}

struct Instance
{
    // originAngle: world-space position xyz, blade angle w
    // variation: blade scale x, stable hash y, edge distance z, surface normal z w
    vec4 originAngle, variation;
};

struct MeshVert
{
    vec pos;
    vec2 tc;
};

struct Mesh
{
    int offset, count, verts;
};

static GLuint meshVbo = 0, meshEbo = 0;
static Mesh meshes[2];
static Shader *shader = NULL, *impulseShader = NULL, *burnShader = NULL, *burnImpulseShader = NULL, *shadowShader = NULL;
static Texture *burnTexture = NULL, *windTexture = NULL;
static GLuint burnFieldTexture = 0;
static int burnFieldDimension = 0, burnFieldWorldSize = 0, burnFieldTiles = 0;
static float burnFieldCellSize = 0;
static vector<ushort> burnFieldState;
static vector<int> burnFieldIgnition, burnFieldRecover, burnFieldExpiry, burnFieldTileExpiry;
static vector<BurnFieldUpdate> burnFieldPending;
static vector<uchar> burnFieldUploadDirty;
static int burnFieldNextExpiry = 0;

static uint hash(uint n)
{
    n ^= n >> 16;
    n *= 0x7FEB352Du;
    n ^= n >> 15;
    n *= 0x846CA68Bu;
    return n ^ (n >> 16);
}

static uint hashCombine(uint seed, uint value)
{
    return hash(seed ^ (value + 0x9E3779B9u + (seed << 6) + (seed >> 2)));
}

static inline uint impulseCellKey(int x, int y)
{
    return (uint(x) << 16) | uint(y);
}

static bool impulseIsRenderable(const Impulse &impulse)
{
    float strength = impulseRemainingStrength(impulse);

    if(strength <= grassimpulseminstrength || grassimpulsedist <= 0) return false;

    float centerDist = camera1->o.dist(impulse.position),
          dist = impulse.type == IMPULSE_PLAYER ? centerDist : max(centerDist - impulse.radius, 0.0f), limit;

    if(impulse.type == IMPULSE_BULLET) limit = min(grassimpulsebulletdist, grassimpulsedist);
    else if(impulse.type == IMPULSE_PLAYER) limit = min(grassplayerflattendist, grassimpulsedist);
    else limit = grassimpulsedist;

    return dist <= limit && isvisiblesphere(impulse.radius + grassheight, impulse.position) != VFC_NOT_VISIBLE;
}

static void buildImpulseGrid()
{
    enumerate(impulseGrid, vector<int>, bucket, bucket.setsize(0));
    impulseGrid.recycle();
    impulseQueryVersion = 0;

    if(!grassimpulses || impulseList.empty()) return;

    int cells = max((worldsize + IMPULSE_CELL_SIZE - 1)/IMPULSE_CELL_SIZE, 1);

    loopv(impulseList)
    {
        Impulse &impulse = impulseList[i];

        if(!impulseIsRenderable(impulse)) continue;

        int x1 = clamp(int(floorf((impulse.position.x - impulse.radius)/IMPULSE_CELL_SIZE)), 0, cells - 1),
            x2 = clamp(int(floorf((impulse.position.x + impulse.radius)/IMPULSE_CELL_SIZE)), 0, cells - 1),
            y1 = clamp(int(floorf((impulse.position.y - impulse.radius)/IMPULSE_CELL_SIZE)), 0, cells - 1),
            y2 = clamp(int(floorf((impulse.position.y + impulse.radius)/IMPULSE_CELL_SIZE)), 0, cells - 1);

        for(int y = y1; y <= y2; ++y) for(int x = x1; x <= x2; ++x) impulseGrid[impulseCellKey(x, y)].add(i);
    }
}

static void collectImpulseCandidates(float x1, float y1, float x2, float y2, vector<int> &candidates)
{
    candidates.setsize(0);

    if(!impulseGrid.numelems) return;

    int cells = max((worldsize + IMPULSE_CELL_SIZE - 1)/IMPULSE_CELL_SIZE, 1),
        cx1 = clamp(int(floorf(x1/IMPULSE_CELL_SIZE)), 0, cells - 1),
        cx2 = clamp(int(floorf(x2/IMPULSE_CELL_SIZE)), 0, cells - 1),
        cy1 = clamp(int(floorf(y1/IMPULSE_CELL_SIZE)), 0, cells - 1),
        cy2 = clamp(int(floorf(y2/IMPULSE_CELL_SIZE)), 0, cells - 1);

    uint queryVersion = ++impulseQueryVersion;

    if(!queryVersion)
    {
        loopv(impulseList) impulseList[i].queryVersion = 0;
        queryVersion = ++impulseQueryVersion;
    }

    for(int y = cy1; y <= cy2; ++y) for(int x = cx1; x <= cx2; ++x)
    {
        vector<int> *bucket = impulseGrid.access(impulseCellKey(x, y));

        if(!bucket) continue;

        loopv(*bucket)
        {
            Impulse &impulse = impulseList[(*bucket)[i]];
            if(impulse.queryVersion == queryVersion) continue;
            impulse.queryVersion = queryVersion;
            candidates.add((*bucket)[i]);
        }
    }
}

static float burnEventFieldRadius(const BurnEvent &event)
{
    return max(event.radius*(1 + grassburnvariation) + grassburnpadding, 0.0f);
}

static ushort encodeBurnTime(int millis)
{
    return ushort((millis/BURN_TIME_QUANTUM)%BURN_TIME_PERIOD + 1);
}

static void clearBurnField()
{
    if(burnFieldTexture)
    {
        glDeleteTextures(1, &burnFieldTexture);
        burnFieldTexture = 0;
    }

    burnFieldDimension = burnFieldWorldSize = burnFieldTiles = 0;
    burnFieldCellSize = 0;
    burnFieldState.setsize(0);
    burnFieldIgnition.setsize(0);
    burnFieldRecover.setsize(0);
    burnFieldExpiry.setsize(0);
    burnFieldTileExpiry.setsize(0);
    burnFieldPending.setsize(0);
    burnFieldUploadDirty.setsize(0);
    burnFieldNextExpiry = 0;

    loopv(burnEvents) burnEvents[i].needsRaster = true;
}

static bool setupBurnField()
{
    int limit = max(1, min(burnFieldMaxSize, hwtexsize)),
        wanted = max(1, (worldsize + burnFieldTexelSize - 1)/burnFieldTexelSize),
        dimension = min(wanted, limit);

    if(burnFieldTexture && burnFieldDimension == dimension && burnFieldWorldSize == worldsize) return true;

    clearBurnField();
    burnFieldDimension = dimension;
    burnFieldWorldSize = worldsize;
    burnFieldCellSize = worldsize/float(burnFieldDimension);
    burnFieldTiles = (burnFieldDimension + BURN_FIELD_TILE_SIZE - 1)/BURN_FIELD_TILE_SIZE;
    int texels = burnFieldDimension*burnFieldDimension, tiles = burnFieldTiles*burnFieldTiles;

    ushort *state = burnFieldState.pad(texels*3);

    int *ignition = burnFieldIgnition.pad(texels), *recover = burnFieldRecover.pad(texels),
        *expiry = burnFieldExpiry.pad(texels), *tileexpiry = burnFieldTileExpiry.pad(tiles);

    uchar *uploadDirty = burnFieldUploadDirty.pad(tiles);
    memset(state, 0, texels*3*sizeof(ushort));
    memset(ignition, 0, texels*sizeof(int));
    memset(recover, 0, texels*sizeof(int));
    memset(expiry, 0, texels*sizeof(int));
    memset(tileexpiry, 0, tiles*sizeof(int));
    memset(uploadDirty, 0, tiles*sizeof(uchar));

    glGenTextures(1, &burnFieldTexture);
    glActiveTexture_(GL_TEXTURE3);
    createtexture(burnFieldTexture, burnFieldDimension, burnFieldDimension, NULL, 3, 0, GL_RGB16, GL_TEXTURE_2D, 0, 0, 0, false, GL_RGB);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, burnFieldDimension, burnFieldDimension, GL_RGB, GL_UNSIGNED_SHORT, state);
    glActiveTexture_(GL_TEXTURE0);

    return true;
}

static void uploadBurnField(int x1, int y1, int width, int height, vector<ushort> &pixels, DebugStats *stats)
{
    pixels.setsize(0);
    pixels.pad(width*height*3);
    loopi(height) memcpy(&pixels[i*width*3], &burnFieldState[((y1 + i)*burnFieldDimension + x1)*3], width*3*sizeof(ushort));
    glTexSubImage2D(GL_TEXTURE_2D, 0, x1, y1, width, height, GL_RGB, GL_UNSIGNED_SHORT, pixels.getbuf());

    if(stats) stats->burnFieldTexelsUpdated += width*height;
}

static bool applyBurnFieldUpdate(const BurnFieldUpdate &update)
{
    ushort mask = update.mask;
    int ignition = update.ignition, recoverStart = update.recoverStart, expiry = update.expiry;

    if(burnFieldExpiry[update.index] > lastmillis && burnFieldState[update.index*3 + 2])
    {
        ushort oldMask = burnFieldState[update.index*3 + 2];
        uint remaining = uint(65535 - oldMask)*uint(65535 - mask);
        mask = ushort(65535 - (remaining + 32767)/65535);
        ignition = max(ignition, burnFieldIgnition[update.index]);
        recoverStart = max(recoverStart, burnFieldRecover[update.index]);
        expiry = max(expiry, burnFieldExpiry[update.index]);
    }

    burnFieldState[update.index*3] = encodeBurnTime(ignition);
    burnFieldState[update.index*3 + 1] = encodeBurnTime(recoverStart);
    burnFieldState[update.index*3 + 2] = mask;
    burnFieldIgnition[update.index] = ignition;
    burnFieldRecover[update.index] = recoverStart;
    burnFieldExpiry[update.index] = expiry;

    int x = update.index%burnFieldDimension, y = update.index/burnFieldDimension,
        tileIndex = (y/BURN_FIELD_TILE_SIZE)*burnFieldTiles + x/BURN_FIELD_TILE_SIZE;
    burnFieldTileExpiry[tileIndex] = max(burnFieldTileExpiry[tileIndex], expiry);

    if(!burnFieldNextExpiry || expiry < burnFieldNextExpiry) burnFieldNextExpiry = expiry;

    return true;
}

static void applyPendingBurnField(vector<ushort> &pixels, DebugStats *stats)
{
    for(int i = 0; i < burnFieldPending.length();)
    {
        if(burnFieldPending[i].ignition > lastmillis)
        {
            ++i;
            continue;
        }
        BurnFieldUpdate update = burnFieldPending.removeunordered(i);

        if(!applyBurnFieldUpdate(update)) continue;

        int x = update.index%burnFieldDimension, y = update.index/burnFieldDimension;
        burnFieldUploadDirty[(y/BURN_FIELD_TILE_SIZE)*burnFieldTiles + x/BURN_FIELD_TILE_SIZE] = 1;
    }

    loopv(burnFieldUploadDirty) if(burnFieldUploadDirty[i])
    {
        int tx = i%burnFieldTiles, ty = i/burnFieldTiles,
            x1 = tx*BURN_FIELD_TILE_SIZE, y1 = ty*BURN_FIELD_TILE_SIZE,
            width = min(int(BURN_FIELD_TILE_SIZE), burnFieldDimension - x1),
            height = min(int(BURN_FIELD_TILE_SIZE), burnFieldDimension - y1);

        uploadBurnField(x1, y1, width, height, pixels, stats);
        burnFieldUploadDirty[i] = 0;

        if(stats) stats->burnFieldDirtyTiles++;
    }
}

static void rasterizeBurnEvent(BurnEvent &event, vector<ushort> &pixels, DebugStats *stats)
{
    event.needsRaster = false;
    float maxRadius = burnEventFieldRadius(event), reach = maxRadius + 2*burnFieldCellSize;

    if(maxRadius <= 0) return;

    int x1 = clamp(int(floorf((event.center.x - reach)/burnFieldCellSize)), 0, burnFieldDimension),
        x2 = clamp(int(ceilf((event.center.x + reach)/burnFieldCellSize)), 0, burnFieldDimension),
        y1 = clamp(int(floorf((event.center.y - reach)/burnFieldCellSize)), 0, burnFieldDimension),
        y2 = clamp(int(ceilf((event.center.y + reach)/burnFieldCellSize)), 0, burnFieldDimension);

    if(x1 >= x2 || y1 >= y2) return;

    float feather = max(burnFieldCellSize*1.5f, 0.001f);
    int expiry = event.recoverStart + grassburnfademillis;
    bool affected = false;

    for(int y = y1; y < y2; ++y) for(int x = x1; x < x2; ++x)
    {
        float worldX = (x + 0.5f)*burnFieldCellSize, worldY = (y + 0.5f)*burnFieldCellSize,
              offsetX = worldX - event.center.x, offsetY = worldY - event.center.y,
              angle = atan2f(offsetY, offsetX),
              jitter = (hashCombine(hashCombine(uint(event.seed*0x1000000u), uint(x)), uint(y)) & 0xFFFFFFu)/float(0x7FFFFFu) - 1.0f,
              contour = 0.55f*sinf(angle*5.0f + event.seed*2.0f*M_PI) + 0.30f*sinf(angle*9.0f - event.seed*4.976f) + 0.15f*jitter,
              radius = max(event.radius*(1.0f + grassburnvariation*contour) + grassburnpadding, 0.0f),
              distance = sqrtf(offsetX*offsetX + offsetY*offsetY),
              edge = clamp((radius - distance)/feather + 0.5f, 0.0f, 1.0f);

        if(radius <= 0 || edge <= 0) continue;

        BurnFieldUpdate update;
        update.index = y*burnFieldDimension + x;
        update.ignition = event.burnStart + int(min(distance/radius, 1.0f)*grassburnpropagatemillis);
        update.recoverStart = event.recoverStart;
        update.expiry = expiry;
        update.mask = ushort(edge*65535 + 0.5f);

        if(update.ignition > lastmillis) burnFieldPending.add(update);
        else if(applyBurnFieldUpdate(update)) affected = true;
    }
    if(affected)
    {
        if(!burnFieldNextExpiry || expiry < burnFieldNextExpiry) burnFieldNextExpiry = expiry;
        uploadBurnField(x1, y1, x2 - x1, y2 - y1, pixels, stats);
    }
}

static void updateBurnField(DebugStats *stats)
{
    if(!setupBurnField()) return;

    vector<ushort> pixels;
    glActiveTexture_(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, burnFieldTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    applyPendingBurnField(pixels, stats);
    loopv(burnEvents) if(burnEvents[i].needsRaster) rasterizeBurnEvent(burnEvents[i], pixels, stats);

    if(burnFieldNextExpiry && lastmillis >= burnFieldNextExpiry)
    {
        burnFieldNextExpiry = 0;
        for(int ty = 0; ty < burnFieldTiles; ++ty) for(int tx = 0; tx < burnFieldTiles; ++tx)
        {
            int tileIndex = ty*burnFieldTiles + tx;
            if(!burnFieldTileExpiry[tileIndex]) continue;
            if(burnFieldTileExpiry[tileIndex] > lastmillis)
            {
                if(!burnFieldNextExpiry || burnFieldTileExpiry[tileIndex] < burnFieldNextExpiry) burnFieldNextExpiry = burnFieldTileExpiry[tileIndex];
                continue;
            }

            int x1 = tx*BURN_FIELD_TILE_SIZE, y1 = ty*BURN_FIELD_TILE_SIZE,
                width = min(int(BURN_FIELD_TILE_SIZE), burnFieldDimension - x1),
                height = min(int(BURN_FIELD_TILE_SIZE), burnFieldDimension - y1), nextExpiry = 0;

            bool changed = false;

            for(int y = y1; y < y1 + height; ++y) for(int x = x1; x < x1 + width; ++x)
            {
                int index = y*burnFieldDimension + x;

                if(burnFieldExpiry[index] && burnFieldExpiry[index] <= lastmillis)
                {
                    burnFieldState[index*3] = burnFieldState[index*3 + 1] = burnFieldState[index*3 + 2] = 0;
                    burnFieldIgnition[index] = burnFieldRecover[index] = 0;
                    burnFieldExpiry[index] = 0;
                    changed = true;
                }
                nextExpiry = max(nextExpiry, burnFieldExpiry[index]);
            }

            burnFieldTileExpiry[tileIndex] = nextExpiry;

            if(nextExpiry > lastmillis)
            {
                if(!burnFieldNextExpiry || nextExpiry < burnFieldNextExpiry) burnFieldNextExpiry = nextExpiry;
            }
            if(changed)
            {
                uploadBurnField(x1, y1, width, height, pixels, stats);
                if(stats) stats->burnFieldDirtyTiles++;
            }
        }
    }
    glActiveTexture_(GL_TEXTURE0);

    loopvrev(burnEvents)
    {
        if(!burnEvents[i].needsRaster && lastmillis >= burnEvents[i].particleEndTime) burnEvents.removeunordered(i);
    }
}

struct PatchKey
{
    int x, y;
    ushort texture, blend;

    PatchKey() {}
    PatchKey(int x, int y, ushort texture, ushort blend) : x(x), y(y), texture(texture), blend(blend) {}
};

static inline bool htcmp(const PatchKey &x, const PatchKey &y)
{
    return x.x == y.x && x.y == y.y && x.texture == y.texture && x.blend == y.blend;
}

static inline uint hthash(const PatchKey &key)
{
    uint hash = hashCombine(uint(key.x), uint(key.y));
    hash = hashCombine(hash, uint(key.texture));
    return hashCombine(hash, uint(key.blend));
}

struct CellKey
{
    int patch, x, y;
    uint seed;

    CellKey() {}
    CellKey(int patch, int x, int y, uint seed) : patch(patch), x(x), y(y), seed(seed) {}
};

static inline bool htcmp(const CellKey &x, const CellKey &y)
{
    return x.patch == y.patch && x.x == y.x && x.y == y.y && x.seed == y.seed;
}

static inline uint hthash(const CellKey &key)
{
    uint hash = hashCombine(uint(key.patch), uint(key.x));
    hash = hashCombine(hash, uint(key.y));
    return hashCombine(hash, key.seed);
}

struct BuildPatch
{
    PatchKey key;
    Slot *slot;
    ivec blendPos;
    vector<Instance> instances;
    vec bbMin, bbMax;
    vec4 particlePositions[MAX_PATCH_PARTICLE_POSITIONS];
    int numParticleCandidates, sourceTris;

    BuildPatch(const PatchKey &key, Slot *slot, const ivec &blendPos) :
        key(key), slot(slot), blendPos(blendPos), bbMin(1e16f, 1e16f, 1e16f), bbMax(-1e16f, -1e16f, -1e16f), numParticleCandidates(0), sourceTris(0)
    {
    }
};

static float hashUnit(uint n)
{
    return (hash(n) & 0xFFFFFFu) / float(0x1000000u);
}

static uint surfaceSeed(const vtxarray &va, const Triangle &g)
{
    uint seed = 0xA341316Cu;
    seed = hashCombine(seed, uint(va.o.x));
    seed = hashCombine(seed, uint(va.o.y));
    seed = hashCombine(seed, uint(va.o.z));
    seed = hashCombine(seed, uint(va.size));
    seed = hashCombine(seed, uint(g.texture));
    seed = hashCombine(seed, uint(int(g.surface.x*65536.0f)));
    seed = hashCombine(seed, uint(int(g.surface.y*65536.0f)));
    seed = hashCombine(seed, uint(int(g.surface.z*65536.0f)));
    return hashCombine(seed, uint(int(g.surface.offset*8.0f)));
}

static bool insideTriangle(const Triangle &g, float x, float y, float &edgeDist)
{
    bool positive = false, negative = false;
    edgeDist = 1e16f;
    loopi(g.numVerts)
    {
        const vec &a = g.v[i], &b = g.v[(i + 1)%g.numVerts];
        float dx = b.x - a.x, dy = b.y - a.y,
              side = dx*(y - a.y) - dy*(x - a.x),
              edgeLength = sqrtf(dx*dx + dy*dy);
        if(side > 1e-4f) positive = true;
        else if(side < -1e-4f) negative = true;
        if(positive && negative) return false;
        if(edgeLength > 1e-6f) edgeDist = min(edgeDist, fabsf(side)/edgeLength);
    }
    return true;
}

void destroy(vtxarray *va)
{
    if(va->grassBuf)
    {
        glDeleteBuffers_(1, &va->grassBuf);
        va->grassBuf = 0;
    }
    va->grassPatches.setsize(0);
}

void build(vtxarray *va)
{
    destroy(va);
    if(glversion < 400 || !glDrawElementsInstanced_ || !glVertexAttribDivisor_ || va->grassTris.empty()) return;

    vector<Instance> instances;
    vector<BuildPatch *> buildPatches;
    hashtable<PatchKey, int> patchIndices;
    hashset<CellKey> generatedCells(1<<16);
    const float spacing = grassstep/sqrtf(grassdensity), patchExtent = float(patchSize);
    int numInstances = 0;
    bool full = false;
    BlendMapCache *blendCache = NULL;

    loopv(va->grassTris) if(va->grassTris[i].blend)
    {
        blendCache = newblendmapcache();
        setblendmaporigin(blendCache, va->o, va->size);
        break;
    }

    loopv(va->grassTris)
    {
        const Triangle &g = va->grassTris[i];
        Slot *slot = lookupvslot(g.texture, false).slot;
        float minX = g.v[0].x, maxX = minX, minY = g.v[0].y, maxY = minY;
        loopj(g.numVerts)
        {
            minX = min(minX, g.v[j].x);
            maxX = max(maxX, g.v[j].x);
            minY = min(minY, g.v[j].y);
            maxY = max(maxY, g.v[j].y);
        }

        int minPatchX = int(floorf(minX/patchExtent)), maxPatchX = int(floorf(maxX/patchExtent)),
            minPatchY = int(floorf(minY/patchExtent)), maxPatchY = int(floorf(maxY/patchExtent));

        uint seed = surfaceSeed(*va, g);

        for(int py = minPatchY; py <= maxPatchY && !full; ++py) for(int px = minPatchX; px <= maxPatchX && !full; ++px)
        {
            float patchMinX = max(minX, px*patchExtent), patchMaxX = min(maxX, (px + 1)*patchExtent),
                  patchMinY = max(minY, py*patchExtent), patchMaxY = min(maxY, (py + 1)*patchExtent);
            if(patchMinX >= patchMaxX || patchMinY >= patchMaxY) continue;

            PatchKey key(px, py, g.texture, g.blend);
            int *patchIndex = patchIndices.access(key);
            if(!patchIndex)
            {
                int index = buildPatches.length();
                patchIndex = &patchIndices.access(key, index);
                buildPatches.add(new BuildPatch(key, slot, ivec(g.center)));
            }
            BuildPatch &patch = *buildPatches[*patchIndex];
            int start = patch.instances.length(),
                minGridX = int(floorf(patchMinX/spacing)) - 1, maxGridX = int(floorf(patchMaxX/spacing)),
                minGridY = int(floorf(patchMinY/spacing)) - 1, maxGridY = int(floorf(patchMaxY/spacing));

            for(int gy = minGridY; gy <= maxGridY && !full; ++gy) for(int gx = minGridX; gx <= maxGridX; ++gx)
            {
                uint cellSeed = hashCombine(hashCombine(seed, uint(gx)), uint(gy));
                float x = (gx + hashUnit(cellSeed))*spacing,
                      y = (gy + hashUnit(cellSeed ^ 0xB5297A4Du))*spacing;
                if(x < patchMinX || x >= patchMaxX || y < patchMinY || y >= patchMaxY) continue;

                float edgeDist;
                if(!insideTriangle(g, x, y, edgeDist)) continue;

                CellKey cell(*patchIndex, gx, gy, seed);
                if(generatedCells.access(cell)) continue;
                generatedCells.add(cell);

                float z = g.surface.zintersect(vec(x, y, 0));
                Instance &inst = patch.instances.add();
                float variation = hashUnit(cellSeed ^ 0x68E31DA4u);
                inst.originAngle = vec4(x, y, z, hashUnit(cellSeed ^ 0x1B56C4E9u)*2*M_PI);
                inst.variation = vec4(0.8f + 0.4f*variation, hashUnit(cellSeed ^ 0xC2B2AE35u), edgeDist, g.surface.z);
                patch.bbMin.min(vec(x, y, z));
                patch.bbMax.max(vec(x, y, z));
                numInstances++;
                if(numInstances >= maxInstances) full = true;
            }

            int count = patch.instances.length() - start;

            if(!count) continue;

            patch.sourceTris++;

            loopj(count)
            {
                const Instance &inst = patch.instances[start + j];
                const vec4 &candidate = inst.originAngle;

                if(g.blend && blendCache && lookupblendmap(blendCache, vec(candidate))/255.0f <= grassblendthreshold) continue;

                int selected = patch.numParticleCandidates < MAX_PATCH_PARTICLE_POSITIONS ? patch.numParticleCandidates :
                               int(hashCombine(seed, uint(j))%uint(patch.numParticleCandidates + 1));

                patch.numParticleCandidates++;
                if(selected < MAX_PATCH_PARTICLE_POSITIONS)
                    patch.particlePositions[selected] = vec4(vec(candidate), inst.variation.y);
            }
        }
        if(full) break;
    }

    freeblendmapcache(blendCache);
    loopv(buildPatches)
    {
        BuildPatch &build = *buildPatches[i];
        if(build.instances.empty()) continue;

        Patch &patch = va->grassPatches.add();
        patch.center = vec(build.bbMin).add(build.bbMax).mul(0.5f);
        patch.radius = patch.center.dist(build.bbMax);
        patch.offset = instances.length();
        patch.count = build.instances.length();
        patch.sourceTris = build.sourceTris;
        patch.texture = build.key.texture;
        patch.blend = build.key.blend;
        patch.slot = build.slot;
        patch.blendPos = build.blendPos;
        patch.numParticlePositions = min(build.numParticleCandidates, int(MAX_PATCH_PARTICLE_POSITIONS));
        loopj(patch.numParticlePositions) patch.particlePositions[j] = build.particlePositions[j];
        patch.shadowCount[0] = patch.shadowCount[1] = 0;
        loopvj(build.instances)
        {
            float instanceHash = build.instances[j].variation.y;
            if(instanceHash <= grassshadowdensitynear) patch.shadowCount[0]++;
            if(instanceHash <= grassshadowdensity) patch.shadowCount[1]++;
        }
        instances.move(build.instances);
    }
    buildPatches.deletecontents();
    if(instances.empty()) return;
    glGenBuffers_(1, &va->grassBuf);
    gle::bindvbo(va->grassBuf);
    glBufferData_(GL_ARRAY_BUFFER, instances.length()*sizeof(Instance), instances.getbuf(), GL_STATIC_DRAW);
    gle::clearvbo();
}

static void settingsChanged()
{
    loopv(valist) if(!valist[i]->grassTris.empty()) build(valist[i]);
}

static void addPlane(vector<MeshVert> &verts, vector<ushort> &indices, float angle, int segments)
{
    int base = verts.length();
    float dx = cosf(angle), dy = sinf(angle);
    for(int row = 0; row <= segments; ++row)
    {
        float z = row/float(segments);
        MeshVert &left = verts.add(), &right = verts.add();
        left.pos = vec(-0.5f*dx, -0.5f*dy, z);
        left.tc = vec2(0, 1 - z);
        right.pos = vec(0.5f*dx, 0.5f*dy, z);
        right.tc = vec2(1, 1 - z);
    }
    loopi(segments)
    {
        int row = base + 2*i;
        indices.add(ushort(row));
        indices.add(ushort(row + 1));
        indices.add(ushort(row + 3));
        indices.add(ushort(row));
        indices.add(ushort(row + 3));
        indices.add(ushort(row + 2));
    }
}

static void addMesh(vector<MeshVert> &verts, vector<ushort> &indices, int lod, int planes, int segments)
{
    Mesh &mesh = meshes[lod];
    mesh.offset = indices.length();
    int startVerts = verts.length();
    loopi(planes) addPlane(verts, indices, M_PI*i/float(planes), segments);
    mesh.count = indices.length() - mesh.offset;
    mesh.verts = verts.length() - startVerts;
}

static void initMeshes()
{
    if(meshVbo) return;
    vector<MeshVert> verts;
    vector<ushort> indices;
    addMesh(verts, indices, 0, 3, 2);
    addMesh(verts, indices, 1, 2, 1);

    glGenBuffers_(1, &meshVbo);
    gle::bindvbo(meshVbo);
    glBufferData_(GL_ARRAY_BUFFER, verts.length()*sizeof(MeshVert), verts.getbuf(), GL_STATIC_DRAW);
    glGenBuffers_(1, &meshEbo);
    gle::bindebo(meshEbo);
    glBufferData_(GL_ELEMENT_ARRAY_BUFFER, indices.length()*sizeof(ushort), indices.getbuf(), GL_STATIC_DRAW);
    gle::clearvbo();
    gle::clearebo();
}

static Shader *loadShader()
{
    return generateshader("grass", "grassShader");
}

void loadShaders()
{
    if(glversion < 400) return;
    shader = loadShader();
    impulseShader = generateshader("grassimpulse", "grassImpulseShader");
    burnShader = generateshader("grassburn", "grassBurnShader");
    burnImpulseShader = generateshader("grassburnimpulse", "grassBurnImpulseShader");
    shadowShader = generateshader("grassshadow", "grassShadowShader");
}

void clearShaders()
{
    shader = impulseShader = burnShader = burnImpulseShader = shadowShader = NULL;
}

static void setupAttribs()
{
    gle::bindvbo(meshVbo);
    const MeshVert *vert = 0;
    glVertexAttribPointer_(gle::ATTRIB_VERTEX, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVert), vert->pos.v);
    glVertexAttribPointer_(gle::ATTRIB_TEXCOORD0, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVert), vert->tc.v);
    glEnableVertexAttribArray_(gle::ATTRIB_VERTEX);
    glEnableVertexAttribArray_(gle::ATTRIB_TEXCOORD0);
    glVertexAttribDivisor_(gle::ATTRIB_VERTEX, 0);
    glVertexAttribDivisor_(gle::ATTRIB_TEXCOORD0, 0);
    gle::bindebo(meshEbo);
}

static void bindInstances(vtxarray *va, int offset)
{
    gle::bindvbo(va->grassBuf);
    const Instance *inst = (const Instance *)(size_t(offset)*sizeof(Instance));
    glVertexAttribPointer_(gle::ATTRIB_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), inst->originAngle.v);
    glVertexAttribPointer_(gle::ATTRIB_TEXCOORD1, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), inst->variation.v);
    glEnableVertexAttribArray_(gle::ATTRIB_COLOR);
    glEnableVertexAttribArray_(gle::ATTRIB_TEXCOORD1);
    glVertexAttribDivisor_(gle::ATTRIB_COLOR, 1);
    glVertexAttribDivisor_(gle::ATTRIB_TEXCOORD1, 1);
}

static void cleanupAttribs()
{
    glVertexAttribDivisor_(gle::ATTRIB_COLOR, 0);
    glVertexAttribDivisor_(gle::ATTRIB_TEXCOORD1, 0);
    glDisableVertexAttribArray_(gle::ATTRIB_VERTEX);
    glDisableVertexAttribArray_(gle::ATTRIB_COLOR);
    glDisableVertexAttribArray_(gle::ATTRIB_TEXCOORD0);
    glDisableVertexAttribArray_(gle::ATTRIB_TEXCOORD1);
    gle::clearvbo();
    gle::clearebo();
}

static void setFrameParams()
{
    float angle = grasswindangle*RAD,
          windPhase = fmodf(float(lastmillis), float(max(grassanimmillis, 1)))/float(max(grassanimmillis, 1));

    GLOBALPARAMF(grassWindParams, windPhase, grassanimscale, cosf(angle), sinf(angle));
    GLOBALPARAMF(grassWindSpatial, grasswindscale, grassmargin, grassmarginfade, 0.0f);
    GLOBALPARAMF(grassWindNoiseParams, grasswindnoisescale, lastmillis/1000.0f*grasswindscrollspeed,
                 grasswindguststrength, grasswinddirectionvariation);
    GLOBALPARAMF(grassWindDetailParams, grasswinddetailscale, grasswinddetailstrength, 0.0f, 0.0f);
    GLOBALPARAMF(grassImpulseControl, grassimpulsestrength, grassimpulsewobble, grassimpulsewobblespeed, grassimpulseafterwind);
    GLOBALPARAMF(grassImpulseAfterWindParams, grassimpulseafterwindmillis/1000.0f, grassimpulseafterwindwobblespeed, 0.0f, 0.0f);
    bvec color(grasscolour);
    GLOBALPARAMF(grassColourParams, color.x/255.0f, color.y/255.0f, color.z/255.0f, grassrootdarkness);
    GLOBALPARAMF(grassRootDarknessPower, grassrootdarknesspower);
    GLOBALPARAMF(grassBlendThreshold, grassblendthreshold);
}

static void setBurnFrameParams()
{
    bvec burnColor(grassburncolour);
    GLOBALPARAMF(grassBurnColourParams, burnColor.x/255.0f, burnColor.y/255.0f, burnColor.z/255.0f);
    bvec burnGlowColor(grassburnglowcolour);
    GLOBALPARAMF(grassBurnGlowColourParams, burnGlowColor.x/255.0f, burnGlowColor.y/255.0f, burnGlowColor.z/255.0f);
    float burnTime = lastmillis/1000.0f;
    GLOBALPARAMF(grassBurnAnimParams, fmodf(burnTime*grassburnscrollx, 1.0f), fmodf(burnTime*grassburnscrolly, 1.0f), grassburnglowscale);
    GLOBALPARAMF(grassBurnFieldParams, 1.0f/float(max(worldsize, 1)), 1.0f/float(max(worldsize, 1)), 1.0f/float(max(burnFieldDimension, 1)), float((lastmillis/BURN_TIME_QUANTUM)%BURN_TIME_PERIOD));
    GLOBALPARAMF(grassBurnTimeParams, float(BURN_TIME_QUANTUM), float(grassburnfadeinmillis), float(grassburnmillis), float(grassburnfademillis));
}

static void setDrawParams(Texture *tex, float density, float fade, float windScale)
{
    float width = grassheight*tex->xs/float(max(grassscale*tex->ys, 1)),
          taperStart = grassdist*grasstaper;

    LOCALPARAMF(grassMeshParams, width, float(grassheight), windScale, 0.0f);
    LOCALPARAMF(grassDrawParams, density, fade, float(grassdist), taperStart);
}

static int burnParticleFrame = -1, burnParticleEmits = 0;

static int burnParticleIgnition(const vec4 &candidate, const BurnEvent &event)
{
    vec offset = vec(candidate).sub(event.center);
    float angle = atan2f(offset.y, offset.x),
          jitter = sinf(candidate.w*91.713f + event.seed*37.119f)*43758.5453f;

    jitter = (jitter - floorf(jitter))*2.0f - 1.0f;

    float contour = 0.55f*sinf(angle*5.0f + event.seed*2.0f*M_PI) + 0.30f*sinf(angle*9.0f - event.seed*4.976f) + 0.15f*jitter,
          radius = max(event.radius*(1.0f + grassburnvariation*contour) + grassburnpadding, 0.0f),
          distance = offset.magnitude();

    if(radius <= 0 || distance > radius) return -1;

    return event.burnStart + int(distance/radius*grassburnpropagatemillis);
}

static void cacheBurnParticlePositions(vtxarray *vas, BurnEvent &event)
{
    event.numParticlePositions = 0;
    int matches = 0;
    float maxRadius = burnEventFieldRadius(event);

    for(vtxarray *va = vas; va; va = va->next)
    {
        if(!va->grassBuf || va->grassPatches.empty() || va->occluded >= OCCLUDE_GEOM || va->distance > grassdist) continue;

        loopv(va->grassPatches)
        {
            const Patch &patch = va->grassPatches[i];
            float radius = patch.radius + grassheight,
                  dist = max(camera1->o.dist(patch.center) - patch.radius, 0.0f),
                  reach = patch.radius + maxRadius;

            if(dist > grassdist || isfoggedsphere(radius, patch.center) || patch.center.squaredist(event.center) > reach*reach) continue;

            loopk(patch.numParticlePositions)
            {
                const vec4 &candidate = patch.particlePositions[k];
                int ignition = burnParticleIgnition(candidate, event);

                if(ignition < 0) continue;

                int selected = matches < MAX_BURN_EVENT_PARTICLE_POSITIONS ? matches : rnd(matches + 1);
                matches++;

                if(selected >= MAX_BURN_EVENT_PARTICLE_POSITIONS) continue;

                event.numParticlePositions = max(event.numParticlePositions, selected + 1);
                BurnParticlePosition &cached = event.particlePositions[selected];
                cached.position = vec(candidate);
                cached.ignition = ignition;
            }
        }
    }
}

static bool findBurnParticlePosition(vtxarray *vas, BurnEvent &event, vec &result)
{
    if(event.numParticlePositions < 0) cacheBurnParticlePositions(vas, event);
    int matches = 0;
    loopi(event.numParticlePositions) if(lastmillis >= event.particlePositions[i].ignition)
    {
        if(!rnd(++matches)) result = event.particlePositions[i].position;
    }
    return matches > 0;
}

static bool findGrassParticlePosition(vtxarray *vas, const GrassParticleEvent &event, vec &result)
{
    float searchRadius = max(event.radius, grassstep*2), searchRadiusSquared = searchRadius*searchRadius,
          closestDistance = searchRadiusSquared;
    bool found = false;

    for(vtxarray *va = vas; va; va = va->next)
    {
        if(!va->grassBuf || va->grassPatches.empty() || va->occluded >= OCCLUDE_GEOM || va->distance > grassdist) continue;

        loopv(va->grassPatches)
        {
            const Patch &patch = va->grassPatches[i];
            float reach = patch.radius + searchRadius + grassheight;
            if(patch.center.squaredist(event.position) > reach*reach) continue;

            loopk(patch.numParticlePositions)
            {
                vec candidate(patch.particlePositions[k]);
                float distance = candidate.squaredist(event.position);
                if(distance > closestDistance) continue;
                closestDistance = distance;
                result = candidate;
                found = true;
            }
        }
    }
    return found;
}

static void emitGrassParticleBurst(const GrassParticleEvent &event, const vec &position)
{
    int count = event.type == IMPULSE_EXPLOSION ? 32 : (event.type == IMPULSE_BULLET ? 8 : 3);
    int fade = event.type == IMPULSE_EXPLOSION ? 1100 : 650;
    float speed = event.type == IMPULSE_EXPLOSION ? 400.0f : 120.0f,
          verticalSpeed = event.type == IMPULSE_EXPLOSION ? 300.0f : 160.0f;

    bvec particleColor(grasscolour);
    particleColor.scale(int(grassparticlebrightness*255), 255);
    vec travel(event.direction.x, event.direction.y, 0);
    travel.safenormalize();

    loopi(count)
    {
        float angle = rndscale(2*M_PI), radialSpeed = speed*(0.45f + rndscale(0.75f));
        vec velocity(cosf(angle)*radialSpeed, sinf(angle)*radialSpeed, verticalSpeed*(0.55f + rndscale(0.75f)));

        if(travel.squaredlen() > 1e-4f)
            velocity.add(vec(travel).mul(speed*(event.type == IMPULSE_BULLET ? 0.45f : 0.18f)));

        vec origin(position);
        origin.x += rndscale(2.0f) - 1.0f;
        origin.y += rndscale(2.0f) - 1.0f;
        origin.z += 0.5f;
        particle_flying_flare(origin, velocity, fade + rnd(fade/2), PART_GRASS, particleColor.tohexcolor(),
                              grassparticlesize*(0.75f + rndscale(0.5f)), 2 + rnd(2));
    }
}

static void emitGrassParticles(vtxarray *vas)
{
    if(!grassimpactparticles)
    {
        grassParticleEvents.setsize(0);
        return;
    }

    loopvrev(grassParticleEvents)
    {
        GrassParticleEvent &event = grassParticleEvents[i];
        float limit = event.type == IMPULSE_BULLET ? min(grassimpulsebulletdist, grassimpulsedist) :
                      event.type == IMPULSE_PLAYER ? min(grassplayerflattendist, grassimpulsedist) : grassimpulsedist;

        if(lastmillis - event.spawnTime > 500 || camera1->o.squaredist(event.position) > limit*limit)
        {
            grassParticleEvents.removeunordered(i);
            continue;
        }
        if(!canemitparticles()) continue;

        vec position;
        if(findGrassParticlePosition(vas, event, position)) emitGrassParticleBurst(event, position);
        grassParticleEvents.removeunordered(i);
    }
}

static void emitBurnParticles(vtxarray *vas)
{
    if(!grassburnparticles) return;
    if(burnParticleFrame != lastmillis)
    {
        burnParticleFrame = lastmillis;
        burnParticleEmits = 0;
    }

    loopv(burnEvents)
    {
        if(burnParticleEmits >= grassburnparticlemax) return;
        BurnEvent &event = burnEvents[i];
        if(lastmillis < event.burnStart || lastmillis >= event.recoverStart || lastmillis >= event.particleEndTime ||
           lastmillis - event.lastEmitMillis < grassburnparticlemillis) continue;

        float maxRadius = burnEventFieldRadius(event);
        if(maxRadius <= 0) continue;

        event.lastEmitMillis = lastmillis;
        vec particlePosition;
        if(!findBurnParticlePosition(vas, event, particlePosition)) continue;
        vec origin = particlePosition;
        origin.z += 1.25f;

        particle_splash(PART_SMOKE, 1, 1200, origin, 0x20202088, 1.0f, 1, -150);
        particle_splash(PART_HAZE_NOISE, 1, 800, origin, 0x3C3C3C, 8.0f, 1, 0);
        float sparkSpread = min(grassburnsparkspread, maxRadius);
        loopk(2)
        {
            float angle = rndscale(2*M_PI), radius = sqrtf(rndscale(1))*sparkSpread;
            vec sparkOrigin = particlePosition;
            sparkOrigin.x += cosf(angle)*radius;
            sparkOrigin.y += sinf(angle)*radius;
            sparkOrigin.z += 1.25f;
            particle_splash(PART_SPARK, 1, 1000, sparkOrigin, 0xFF7020, 0.55f, 1, -60);
        }


        burnParticleEmits++;
    }
}

static bool patchUsesBurnField(const Patch &patch, DebugStats *stats)
{
    if(burnFieldTileExpiry.empty()) return false;

    int x1 = clamp(int(floorf((patch.center.x - patch.radius)/burnFieldCellSize)), 0, burnFieldDimension - 1),
        x2 = clamp(int(floorf((patch.center.x + patch.radius)/burnFieldCellSize)), 0, burnFieldDimension - 1),
        y1 = clamp(int(floorf((patch.center.y - patch.radius)/burnFieldCellSize)), 0, burnFieldDimension - 1),
        y2 = clamp(int(floorf((patch.center.y + patch.radius)/burnFieldCellSize)), 0, burnFieldDimension - 1);

    int tx1 = x1/BURN_FIELD_TILE_SIZE, tx2 = x2/BURN_FIELD_TILE_SIZE,
        ty1 = y1/BURN_FIELD_TILE_SIZE, ty2 = y2/BURN_FIELD_TILE_SIZE;

    for(int ty = ty1; ty <= ty2; ++ty) for(int tx = tx1; tx <= tx2; ++tx)
    {
        if(stats) stats->burnCandidateChecks++;
        if(burnFieldTileExpiry[ty*burnFieldTiles + tx] > lastmillis)
        {
            if(stats) stats->burnRelevantChecks++;
            return true;
        }
    }
    return false;
}

struct PatchImpulses
{
    vec4 positions[MAX_IMPULSES_PER_PATCH], directions[MAX_IMPULSES_PER_PATCH], params[MAX_IMPULSES_PER_PATCH];
    int count;

    PatchImpulses() : count(0) {}
};

static void collectPatchImpulses(const Patch &patch, PatchImpulses &selected, DebugStats *stats)
{
    selected.count = 0;
    if(!grassimpulses || grassimpulsestrength <= 0 || impulseList.empty() || !impulseGrid.numelems || !impulseMaxPatch) return;
    static vector<int> candidates;
    collectImpulseCandidates(patch.center.x - patch.radius, patch.center.y - patch.radius,
                             patch.center.x + patch.radius, patch.center.y + patch.radius, candidates);

    int indices[MAX_IMPULSES_PER_PATCH], limit = min(impulseMaxPatch, int(MAX_IMPULSES_PER_PATCH));
    float scores[MAX_IMPULSES_PER_PATCH];

    loopv(candidates)
    {
        const Impulse &impulse = impulseList[candidates[i]];

        if(stats) stats->impulseCandidateChecks++;

        float reach = patch.radius + grassheight + impulse.radius,
              centerDist = vec2(patch.center).dist(vec2(impulse.position));

        if(centerDist > reach) continue;

        float distance = max(centerDist - patch.radius, 0.0f), relevance = max(1.0f - distance/max(impulse.radius, 0.001f), 0.0f),
              age = max(lastmillis - impulse.startTime, 0)/1000.0f;

        if(impulse.type == IMPULSE_EXPLOSION)
        {
            float wave = impulse.propagationSpeed > 0 ? age*impulse.propagationSpeed : impulse.radius*age*1000.0f/max(impulse.lifetime, 1),
                  frontWidth = max(impulse.radius*impulse.falloff, 2.0f),
                  arrivalTime = distance/max(impulse.propagationSpeed, 0.001f);
            relevance = max(1.0f - fabsf(distance - wave)/(patch.radius + frontWidth), 0.0f);
            if(age >= arrivalTime)
            {
                float edge = max(1.0f - distance/max(impulse.radius, 0.001f), 0.0f);
                relevance = max(relevance, 0.5f*edge);
            }
        }

        float score = impulseRemainingStrength(impulse)*relevance;

        if(score <= grassimpulseminstrength) continue;
        if(stats) stats->impulseRelevantChecks++;

        int insert = selected.count;

        if(selected.count < limit) selected.count++;
        else
        {
            if(score <= scores[limit - 1]) continue;
            insert = limit - 1;
        }
        while(insert > 0 && score > scores[insert - 1])
        {
            if(insert < limit)
            {
                scores[insert] = scores[insert - 1];
                indices[insert] = indices[insert - 1];
            }
            --insert;
        }
        scores[insert] = score;
        indices[insert] = candidates[i];
    }

    loopi(selected.count)
    {
        const Impulse &impulse = impulseList[indices[i]];
        float ageSeconds = max(lastmillis - impulse.startTime, 0)/1000.0f,
              releaseSeconds = impulse.type == IMPULSE_PLAYER ? max(lastmillis - impulse.lastRefresh, 0)/1000.0f : impulse.direction.z,
              behavior = impulse.type == IMPULSE_EXPLOSION ? max(impulse.propagationSpeed, 0.001f) : impulse.radial;
        selected.positions[i] = vec4(impulse.position, impulse.radius);
        selected.directions[i] = vec4(impulse.direction.x, impulse.direction.y, releaseSeconds, impulse.strength);
        selected.params[i] = vec4(float(impulse.type), ageSeconds, behavior, impulse.lifetime/1000.0f);
    }
}

static void setImpulseParams(const PatchImpulses &impulses)
{
    LOCALPARAMI(grassImpulseCount, impulses.count);
    LOCALPARAMV(grassImpulsePositions, impulses.positions, impulses.count);
    LOCALPARAMV(grassImpulseDirections, impulses.directions, impulses.count);
    LOCALPARAMV(grassImpulseParams, impulses.params, impulses.count);
}

static void drawLod(const Patch &patch, Texture *tex, int lod, float density, float fade, float windScale, DebugStats *stats, int statInstances = -1)
{
    if(density <= 0 || fade <= 0) return;
    setDrawParams(tex, density, fade, windScale);
    const Mesh &mesh = meshes[lod];
    glDrawElementsInstanced_(GL_TRIANGLES, mesh.count, GL_UNSIGNED_SHORT, (const void *)(size_t(mesh.offset)*sizeof(ushort)), patch.count);
    xtravertsva += mesh.verts*patch.count;
    glde++;

    if(stats)
    {
        stats->drawCalls++;
        stats->instances += statInstances >= 0 ? statInstances : patch.count;
    }
}

static void drawPatchLod(const Patch &patch, Texture *tex, float dist, bool shadow, int cascade, DebugStats *stats)
{
    if(shadow)
    {
        int index = clamp(cascade, 0, 1), lod = index ? grassshadowlodfar : grassshadowlodnear;
        float density = index ? grassshadowdensity : grassshadowdensitynear,
              windScale = cascade < grassshadowwindcascades ? 1.0f : 0.0f;

        if(!patch.shadowCount[index]) return;

        drawLod(patch, tex, lod, density, 1, windScale, stats, patch.shadowCount[index]);
        return;
    }

    float transition = min(float(grasslodtransition), float(grassloddist));
    if(transition > 0 && dist >= grassloddist - transition && dist <= grassloddist + transition)
    {
        float blend = clamp((dist - (grassloddist - transition))/(2*transition), 0.0f, 1.0f);
        drawLod(patch, tex, 0, 1, 1 - blend, 1, stats);
        drawLod(patch, tex, 1, grassfardensity, blend, 1, stats);
        return;
    }

    int lod = dist < grassloddist ? 0 : 1;
    drawLod(patch, tex, lod, lod ? grassfardensity : 1.0f, 1, 1, stats);
}

static void renderPatches(vtxarray *vas, bool shadow, int cascade, DebugStats *stats)
{
    Shader *baseShader = shadow ? shadowShader : shader;
    if(!baseShader) return;

    setFrameParams();
    float nextShadowDensity = cascade + 1 < grassshadowcascades ? grassshadowdensity : 0.0f;
    GLOBALPARAMF(grassShadowParams, shadow ? grassshadowtransition : 0.0f, nextShadowDensity, 0.0f, 0.0f);
    if(!shadow) setBurnFrameParams();

    initMeshes();
    setupAttribs();
    glDisable(GL_CULL_FACE);
    if(!windTexture) windTexture = textureload("packages/noise/grass_wind.jpg", 0, true, false);
    glActiveTexture_(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, windTexture->id);
    glActiveTexture_(GL_TEXTURE0);

    GLuint textureId = 0;
    Shader *boundShader = NULL;
    bool textureBound = false, burnTextureBound = false;
    int blend = -1;

    for(vtxarray *va = vas; va; va = shadow ? va->rnext : va->next)
    {
        if(!va->grassBuf || va->grassPatches.empty()) continue;
        if(shadow)
        {
            if(!(va->shadowmask&(1<<cascade))) continue;
        }
        else if(va->occluded >= OCCLUDE_GEOM || va->distance > grassdist) continue;

        loopv(va->grassPatches)
        {
            const Patch &patch = va->grassPatches[i];
            float radius = patch.radius + grassheight,
                  dist = max(camera1->o.dist(patch.center) - patch.radius, 0.0f);

            if(dist > grassdist) continue;

            if(shadow)
            {
                if(!(calcspherecsmsplits(patch.center, radius)&(1<<cascade))) continue;
                if(!patch.shadowCount[cascade > 0 ? 1 : 0]) continue;
            }
            else if(isvisiblesphere(radius, patch.center) >= VFC_FOGGED) continue;

            PatchImpulses patchImpulses;

            if(!shadow) collectPatchImpulses(patch, patchImpulses, stats);

            bool interacting = !shadow && patchImpulses.count > 0 && impulseShader,
                 burning = !shadow && burnShader && burnFieldTexture && burnFieldNextExpiry && patchUsesBurnField(patch, stats);

            // Effect selection only changes shader state; patches keep the same instance buffer and draw ranges.
            Shader *patchShader = burning ? (interacting && burnImpulseShader ? burnImpulseShader : burnShader) : interacting ? impulseShader : baseShader;

            Slot &slot = *patch.slot;
            if(!slot.grasstex)
            {
                if(!slot.grass) continue;
                slot.grasstex = textureload(slot.grass, 2);
            }

            Texture *tex = slot.grasstex;

            if(!textureBound || textureId != tex->id)
            {
                glBindTexture(GL_TEXTURE_2D, tex->id);
                textureId = tex->id;
                textureBound = true;
            }

            if(burning && !burnTextureBound)
            {
                if(!burnTexture) burnTexture = textureload("packages/noise/burning_grass.jpg", 0, true, false);
                glActiveTexture_(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, burnTexture->id);
                glActiveTexture_(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, burnFieldTexture);
                glActiveTexture_(GL_TEXTURE0);
                burnTextureBound = true;
            }

            if(boundShader != patchShader || blend != patch.blend)
            {
                if(patch.blend)
                {
                    glActiveTexture_(GL_TEXTURE1);
                    bindblendtexture(patch.blendPos);
                    glActiveTexture_(GL_TEXTURE0);
                    patchShader->setvariant(0, 0);
                }
                else patchShader->set();
                boundShader = patchShader;
                blend = patch.blend;
            }

            if(interacting)
            {
                setImpulseParams(patchImpulses);
                if(stats) stats->impulsePatchesAffected++;
            }

            if(stats)
            {
                if(burning) stats->burnPatchesAffected++;
                stats->patches++;
                stats->sourceTris += patch.sourceTris;
            }
            bindInstances(va, patch.offset);
            drawPatchLod(patch, tex, dist, shadow, cascade, stats);
        }
    }

    cleanupAttribs();
    glEnable(GL_CULL_FACE);
}

static int lastStatPrint = 0;

void render()
{
    pruneImpulses();
    if(!grass || !grassdist || glversion < 400 || !glDrawElementsInstanced_ || !visibleva) return;

    DebugStats stats, *statsPtr = grassstats ? &stats : NULL;
    updatePlayerImpulses();
    buildImpulseGrid();
    updateBurnField(statsPtr);

    if(!burnTexture) burnTexture = textureload("packages/noise/burning_grass.jpg", 0, true, false);

    emitGrassParticles(visibleva);
    emitBurnParticles(visibleva);
    timer *cpuTimer = begintimer("grass", false), *gpuTimer = begintimer("grass");
    renderPatches(visibleva, false, 0, statsPtr);
    endtimer(gpuTimer);
    endtimer(cpuTimer);

    if(grassstats && totalmillis - lastStatPrint >= 1000)
    {
        lastStatPrint = totalmillis - totalmillis%1000;
        conoutf(CON_INFO, "grass: %d visible patches, %d draw calls, %d instances, %d source triangle contributions, "
                         "%d impulses, %d impulse candidate checks, %d impulse relevant checks, %d impulse patches, "
                         "%d burn events, %d burn candidate checks, %d burn relevant checks, %d fire patches, "
                         "%d burn field dirty tiles, %d burn field texels updated",
                stats.patches, stats.drawCalls, stats.instances, stats.sourceTris,
                impulseList.length(), stats.impulseCandidateChecks, stats.impulseRelevantChecks, stats.impulsePatchesAffected,
                burnEvents.length(), stats.burnCandidateChecks, stats.burnRelevantChecks, stats.burnPatchesAffected,
                stats.burnFieldDirtyTiles, stats.burnFieldTexelsUpdated);
    }
}

void renderShadow(int cascade)
{
    if(!grass || !grassdist || cascade < 0 || cascade >= grassshadowcascades || glversion < 400 || !glDrawElementsInstanced_ || !shadowva) return;
    renderPatches(shadowva, true, cascade, NULL);
}

void cleanup()
{
    clearImpulses();
    clearBurnEvents();
    if(meshVbo)
    {
        glDeleteBuffers_(1, &meshVbo);
        meshVbo = 0;
    }
    if(meshEbo)
    {
        glDeleteBuffers_(1, &meshEbo);
        meshEbo = 0;
    }
    clearShaders();
}

} // namespace grass
