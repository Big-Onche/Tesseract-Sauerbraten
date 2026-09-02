// lensflare.cpp: procedural sun flares and shafts
// good ol' OG particles ones are still in header

#include "engine.h"

extern GLuint hdrfbo, mshdrfbo;

namespace lensFlares
{
    static const float referenceFov = 100.0f;
    static const float occlusionRadius = 10.0f;
    static const int occlusionSegments = 64;

    struct queuedFlare
    {
        vec o;
        bvec color;
        int maxDistance;
        bool unlimitedDistance;
        bool lensGhosts;
    };

    static vector<queuedFlare> queuedFlares;

    enum flareRejectStage
    {
        FLARE_REJECT_DISTANCE = 0,
        FLARE_REJECT_MAX_DISTANCE,
        FLARE_REJECT_CLIP,
        FLARE_REJECT_SCREEN,
        FLARE_REJECT_EDGE,
        FLARE_REJECT_COLOR,
        FLARE_REJECT_VISIBILITY,
        FLARE_REJECT_RAYCAST,
        FLARE_REJECT_HARD_CENTER,
        FLARE_REJECT_COUNT
    };

    struct flareProfile
    {
        Uint64 renderStart;
        float totalMillis, averageMillis;
        int queued, rendered, rejected;
        int queriesIssued, queriesResolved, queriesPending, fallbackHits;
        int visibilityPrecomputed, visibilityPerPixel, visibilityFallback;
        int rejects[FLARE_REJECT_COUNT];
        bool sunQueued, sunRendered;

        flareProfile() : renderStart(0), totalMillis(0.0f), averageMillis(-1.0f), queued(0), rendered(0), rejected(0),
                         queriesIssued(0), queriesResolved(0), queriesPending(0), fallbackHits(0), visibilityPrecomputed(0), visibilityPerPixel(0),
                         visibilityFallback(0), sunQueued(false), sunRendered(false)
        {
            memset(rejects, 0, sizeof(rejects));
        }
    };

    static flareProfile profile;
    static int pendingQueryCount();

    // Settings
    VARP(flares, 0, 1, 1);
    VARP(flareghosts, 0, 1, 1);
    VARP(sunflares, 0, 1, 1);
    VAR(debugflares, 0, 0, 1);
    FVARP(lensflareocclusionradians, 0.001f, 0.05236f, 0.2f);
    VARP(lensflareocclusionlerp, 0, 25, 5000);
    FVARP(lensflarecloudocclusionthreshold, 0.05f, 0.60f, 1.0f);
    // Map vars
    VARR(sunflareshaftsize, 1, 75, 400);
    VARR(sunflarestrength, 0, 25, 200);

    static void beginProfile()
    {
        if(!debugflares) return;

        profile.renderStart = SDL_GetPerformanceCounter();
        profile.queued = queuedFlares.length();
        profile.rendered = profile.rejected = 0;
        profile.queriesIssued = profile.queriesResolved = profile.queriesPending = profile.fallbackHits = 0;
        profile.visibilityPrecomputed = profile.visibilityPerPixel = profile.visibilityFallback = 0;
        memset(profile.rejects, 0, sizeof(profile.rejects));
        profile.sunQueued = profile.sunRendered = false;
    }

    static void endProfile()
    {
        if(!debugflares || !profile.renderStart) return;

        Uint64 frequency = SDL_GetPerformanceFrequency();
        profile.totalMillis = frequency ? float((SDL_GetPerformanceCounter() - profile.renderStart) * 1000.0 / frequency) : 0.0f;
        profile.averageMillis = profile.averageMillis < 0.0f ? profile.totalMillis :
                                profile.averageMillis + (profile.totalMillis - profile.averageMillis) * 0.1f;
        profile.rejected = max(profile.queued - profile.rendered, 0);
        profile.queriesPending = pendingQueryCount();
        profile.renderStart = 0;
    }

    static void recordReject(flareRejectStage stage)
    {
        if(debugflares) profile.rejects[stage]++;
    }

    struct occlusionQuery
    {
        GLuint id;
        int issuedFrame;
        float total;
        bool pending;

        occlusionQuery() : id(0), issuedFrame(-1), total(1.0f), pending(false) {}
    };

    static const int SUN_QUERY_COUNT = 3;
    static const int CLOUD_QUERY_COUNT = 3;
    static const int CLOUD_QUERY_INTERVAL = 2;
    static const int LOCAL_QUERY_COUNT = 16;
    static const int SOURCE_VISIBILITY_QUERY_COUNT = 16;
    static const int LOCAL_CACHE_COUNT = 32;
    static const int LOCAL_VISIBILITY_MILLIS = 350;
    static const int SOURCE_VISIBILITY_REFRESH_MILLIS = 200;
    static occlusionQuery sunOcclusionQueries[SUN_QUERY_COUNT];
    static occlusionQuery cloudOcclusionQueries[CLOUD_QUERY_COUNT];
    struct localOcclusionQuery : occlusionQuery
    {
        vec source;
        bool area, sun;

        localOcclusionQuery() : source(0, 0, 0), area(false), sun(false) {}
    };

    struct localVisibility
    {
        vec source;
        float center, area;
        int centerMillis, areaMillis, lastUsedMillis;
        float layerVisibility[4];
        int layerVisibilityMillis[4];
        bool sun;

        localVisibility(const vec &source, bool sun) : source(source), center(1.0f), area(1.0f), centerMillis(0), areaMillis(0),
                                                        lastUsedMillis(0), sun(sun)
        {
            loopi(4)
            {
                layerVisibility[i] = 1.0f;
                layerVisibilityMillis[i] = 0;
            }
        }
    };

    struct sourceVisibilityQuery : occlusionQuery
    {
        vec source;
        int layer;

        sourceVisibilityQuery() : source(0, 0, 0), layer(0) {}
    };

    static localOcclusionQuery localOcclusionQueries[LOCAL_QUERY_COUNT];
    static sourceVisibilityQuery sourceVisibilityQueries[SOURCE_VISIBILITY_QUERY_COUNT];
    static vector<localVisibility> localVisibilityCache;
    static int flareQueryFrame = 0, sunLastResolvedFrame = -1;
    static int cloudLastIssuedFrame = -CLOUD_QUERY_INTERVAL, cloudLastResolvedFrame = -1;
    static float cloudOcclusionLast = 0.0f;
    static float sunGeometryVisibilityTarget = 1.0f, sunOcclusionTarget = 1.0f, sunOcclusionSmoothed = 1.0f;
    static int sunOcclusionMillis = 0, sunOcclusionDebugMillis = 0;
    static vec lastCameraPos(0, 0, 0);
    static int lastCameraMillis = 0;
    static float cameraVelocityBias = 0.0f;

    static int pendingQueryCount()
    {
        int pending = 0;
        loopi(SUN_QUERY_COUNT) if(sunOcclusionQueries[i].pending) pending++;
        loopi(CLOUD_QUERY_COUNT) if(cloudOcclusionQueries[i].pending) pending++;
        loopi(LOCAL_QUERY_COUNT) if(localOcclusionQueries[i].pending) pending++;
        loopi(SOURCE_VISIBILITY_QUERY_COUNT) if(sourceVisibilityQueries[i].pending) pending++;
        return pending;
    }

    static occlusionQuery *availableQuery(occlusionQuery *queries, int count)
    {
        loopi(count) if(!queries[i].pending)
        {
            if(!queries[i].id) glGenQueries_(1, &queries[i].id);
            if(queries[i].id) return &queries[i];
        }
        return NULL;
    }

    static void pollSunOcclusionQueries()
    {
        loopi(SUN_QUERY_COUNT)
        {
            occlusionQuery &query = sunOcclusionQueries[i];
            if(!query.pending || query.issuedFrame >= flareQueryFrame) continue;

            GLint available = 0;
            glGetQueryObjectiv_(query.id, GL_QUERY_RESULT_AVAILABLE, &available);
            if(!available) continue;

            GLuint samples = 0;
            glGetQueryObjectuiv_(query.id, GL_QUERY_RESULT, &samples);
            if(query.issuedFrame > sunLastResolvedFrame)
            {
                sunGeometryVisibilityTarget = clamp(samples / max(query.total, 1.0f), 0.0f, 1.0f);
                sunLastResolvedFrame = query.issuedFrame;
            }
            query.pending = false;
            if(debugflares) profile.queriesResolved++;
        }
    }

    static void pollCloudOcclusionQueries()
    {
        loopi(CLOUD_QUERY_COUNT)
        {
            occlusionQuery &query = cloudOcclusionQueries[i];
            if(!query.pending || query.issuedFrame >= flareQueryFrame) continue;

            GLint available = 0;
            glGetQueryObjectiv_(query.id, GL_QUERY_RESULT_AVAILABLE, &available);
            if(!available) continue;

            GLuint samples = 0;
            glGetQueryObjectuiv_(query.id, GL_QUERY_RESULT, &samples);
            if(query.issuedFrame > cloudLastResolvedFrame)
            {
                cloudOcclusionLast = clamp(samples / max(query.total, 1.0f), 0.0f, 1.0f);
                cloudLastResolvedFrame = query.issuedFrame;
            }
            query.pending = false;
            if(debugflares) profile.queriesResolved++;
        }
    }

    static bool sameSource(const vec &a, const vec &b)
    {
        return a.squaredist(b) <= 1.0e-6f;
    }

    static localVisibility *findLocalVisibility(const vec &source, bool sun, bool create = false)
    {
        loopv(localVisibilityCache) if(localVisibilityCache[i].sun == sun && sameSource(localVisibilityCache[i].source, source))
            return &localVisibilityCache[i];
        if(!create) return NULL;

        if(localVisibilityCache.length() < LOCAL_CACHE_COUNT) return &localVisibilityCache.add(localVisibility(source, sun));

        int oldest = 0;
        loopv(localVisibilityCache) if(localVisibilityCache[i].lastUsedMillis < localVisibilityCache[oldest].lastUsedMillis) oldest = i;
        localVisibilityCache[oldest] = localVisibility(source, sun);
        return &localVisibilityCache[oldest];
    }

    static bool localQueryPending(const vec &source, bool sun, bool area)
    {
        loopi(LOCAL_QUERY_COUNT)
        {
            const localOcclusionQuery &query = localOcclusionQueries[i];
            if(query.pending && query.sun == sun && query.area == area && sameSource(query.source, source)) return true;
        }
        return false;
    }

    static localOcclusionQuery *availableLocalQuery()
    {
        loopi(LOCAL_QUERY_COUNT) if(!localOcclusionQueries[i].pending)
        {
            localOcclusionQuery &query = localOcclusionQueries[i];
            if(!query.id) glGenQueries_(1, &query.id);
            if(query.id) return &query;
        }
        return NULL;
    }

    static void pollLocalOcclusionQueries()
    {
        int millis = totalmillis ? totalmillis : lastmillis;
        loopi(LOCAL_QUERY_COUNT)
        {
            localOcclusionQuery &query = localOcclusionQueries[i];
            if(!query.pending || query.issuedFrame >= flareQueryFrame) continue;

            GLint available = 0;
            glGetQueryObjectiv_(query.id, GL_QUERY_RESULT_AVAILABLE, &available);
            if(!available) continue;

            GLuint samples = 0;
            glGetQueryObjectuiv_(query.id, GL_QUERY_RESULT, &samples);
            localVisibility *visibility = findLocalVisibility(query.source, query.sun, true);
            float result = clamp(samples / max(query.total, 1.0f), 0.0f, 1.0f);
            if(query.area)
            {
                visibility->area = result;
                visibility->areaMillis = millis;
            }
            else
            {
                visibility->center = result;
                visibility->centerMillis = millis;
            }
            query.pending = false;
            if(debugflares) profile.queriesResolved++;
        }
    }

    static bool sourceVisibilityQueryPending(const vec &source, int layer)
    {
        loopi(SOURCE_VISIBILITY_QUERY_COUNT)
        {
            const sourceVisibilityQuery &query = sourceVisibilityQueries[i];
            if(query.pending && query.layer == layer && sameSource(query.source, source)) return true;
        }
        return false;
    }

    static sourceVisibilityQuery *availableSourceVisibilityQuery()
    {
        loopi(SOURCE_VISIBILITY_QUERY_COUNT) if(!sourceVisibilityQueries[i].pending)
        {
            sourceVisibilityQuery &query = sourceVisibilityQueries[i];
            if(!query.id) glGenQueries_(1, &query.id);
            if(query.id) return &query;
        }
        return NULL;
    }

    static void pollSourceVisibilityQueries()
    {
        int millis = totalmillis ? totalmillis : lastmillis;
        loopi(SOURCE_VISIBILITY_QUERY_COUNT)
        {
            sourceVisibilityQuery &query = sourceVisibilityQueries[i];
            if(!query.pending || query.issuedFrame >= flareQueryFrame) continue;

            GLint available = 0;
            glGetQueryObjectiv_(query.id, GL_QUERY_RESULT_AVAILABLE, &available);
            if(!available) continue;

            GLuint samples = 0;
            glGetQueryObjectuiv_(query.id, GL_QUERY_RESULT, &samples);
            localVisibility *visibility = findLocalVisibility(query.source, false, true);
            float result = clamp(samples / max(query.total, 1.0f), 0.0f, 1.0f);
            visibility->layerVisibility[query.layer] = result*result*(3.0f - 2.0f*result);
            visibility->layerVisibilityMillis[query.layer] = millis;
            query.pending = false;
            if(debugflares) profile.queriesResolved++;
        }
    }

    static bool shouldRender(bool sun = false)
    {
        if(!flares || (sun && sunflarestrength <= 0)) return false;
        return !sun || (sunflares && !sunlight.iszero() && sunlightscale > 1.0e-4f);
    }

    static float getFovScale()
    {
        return clamp(tanf(0.5f * referenceFov * RAD) / max(tanf(0.5f * curfov * RAD), 1.0e-4f), 0.25f, 8.0f);
    }

    static float occlusionRadiusPixels()
    {
        float angle = clamp(lensflareocclusionradians, 0.001f, 0.2f);
        return max(0.5f * viewh * tanf(angle) / max(tanf(0.5f * fovy * RAD), 1.0e-4f), 1.0f);
    }

    static void updateCameraVelocityBias()
    {
        int millis = totalmillis ? totalmillis : lastmillis;
        if(lastCameraMillis)
        {
            int elapsed = max(millis - lastCameraMillis, 1);
            float speed = camera1->o.dist(lastCameraPos) * 1000.0f / elapsed;
            cameraVelocityBias = clamp(speed * 0.025f, 0.0f, 12.0f);
        }
        lastCameraPos = camera1->o;
        lastCameraMillis = millis;
    }

    static void drawOcclusionCircle(const vec4 &screen, float radiusPixels, bool filled, float ndcDepth = 1.0f)
    {
        float x = screen.x * 2.0f - 1.0f, y = screen.y * 2.0f - 1.0f,
              rx = 2.0f * radiusPixels / max(float(vieww), 1.0f),
              ry = 2.0f * radiusPixels / max(float(viewh), 1.0f);

        gle::defvertex(3);
        gle::begin(filled ? GL_TRIANGLE_FAN : GL_LINE_LOOP);
        if(filled) gle::attribf(x, y, ndcDepth);
        loopi(filled ? occlusionSegments + 1 : occlusionSegments)
        {
            float a = (2.0f * M_PI * (i % occlusionSegments)) / occlusionSegments;
            gle::attribf(x + cosf(a) * rx, y + sinf(a) * ry, ndcDepth);
        }
        gle::end();
    }

    static float drawSourceVisibilityPattern(const vec4 &screen, float sampleRadius)
    {
        static const float offsets[9][2] =
        {
            { 0.0f, 0.0f }, { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f },
            { 1.0f, 1.0f }, { -1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f }
        };
        static const float weights[9] = { 4.0f, 2.0f, 2.0f, 2.0f, 2.0f, 1.5f, 1.5f, 1.5f, 1.5f };

        float total = 0.0f;
        loopi(9)
        {
            vec4 sample(screen);
            float pixelX = clamp(screen.x * vieww + offsets[i][0] * sampleRadius, 0.5f, max(float(vieww) - 0.5f, 0.5f));
            float pixelY = clamp(screen.y * viewh + offsets[i][1] * sampleRadius, 0.5f, max(float(viewh) - 0.5f, 0.5f));
            sample.x = pixelX / max(float(vieww), 1.0f);
            sample.y = pixelY / max(float(viewh), 1.0f);
            float pointRadius = sqrtf(weights[i]) * 0.75f;
            drawOcclusionCircle(sample, pointRadius, true);
            total += float(M_PI) * pointRadius * pointRadius;
        }
        return total * max(msaalight ? msaasamples : 1, 1);
    }

    static void issueSourceVisibilityQuery(const vec &source, int layer, const vec4 &screen, float sourceDepth, float sampleRadius,
                                           Shader *visibilityShader)
    {
        if(!visibilityShader || !glGenQueries_ || !glBeginQuery_ || sourceVisibilityQueryPending(source, layer)) return;
        sourceVisibilityQuery *query = availableSourceVisibilityQuery();
        if(!query) return;

        bool hadDepth = glIsEnabled(GL_DEPTH_TEST) != 0, hadBlend = glIsEnabled(GL_BLEND) != 0;
        GLboolean oldDepthMask = GL_TRUE, oldColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

        glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        glViewport(0, 0, vieww, viewh);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_FALSE);
        if(hadDepth) glDisable(GL_DEPTH_TEST);
        if(hadBlend) glDisable(GL_BLEND);

        glActiveTexture_(GL_TEXTURE0);
        if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
        else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
        glActiveTexture_(GL_TEXTURE1);
        if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msnormaltex);
        else glBindTexture(GL_TEXTURE_RECTANGLE, gnormaltex);
        glActiveTexture_(GL_TEXTURE2);
        bool hasCloudComposite = volumetricClouds::bindcomposite(2);
        vec4 cloudParams = hasCloudComposite ? volumetricClouds::compositetexparams() : vec4(0, 0, 0, 0);
        if(!hasCloudComposite) glBindTexture(GL_TEXTURE_RECTANGLE, 0);
        glActiveTexture_(GL_TEXTURE0);

        GLOBALPARAMF(sourceVisibilityDepth, sourceDepth);
        GLOBALPARAMF(sunFlareCloudTex, cloudParams.x, cloudParams.y, cloudParams.z, cloudParams.w);
        visibilityShader->set();
        glBeginQuery_(GL_SAMPLES_PASSED, query->id);
        float total = drawSourceVisibilityPattern(screen, sampleRadius);
        glEndQuery_(GL_SAMPLES_PASSED);

        glDepthMask(oldDepthMask);
        glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
        if(hadDepth) glEnable(GL_DEPTH_TEST);
        if(hadBlend) glEnable(GL_BLEND);
        glActiveTexture_(GL_TEXTURE0);

        query->source = source;
        query->layer = layer;
        query->total = max(total, 1.0f);
        query->issuedFrame = flareQueryFrame;
        query->pending = true;
        if(debugflares) profile.queriesIssued++;
    }

    static void prepareLocalFlareVisibilities(const vec &source, const vec4 &screen, const vec4 &layerWeights, const vec4 &visibilityOverride,
                                               Shader *visibilityShader, vec4 &visibilities, bool &usedPrecomputed, bool &usedFallback)
    {
        static const float radiusScales[4] = { 0.55f, 1.10f, 0.85f, 1.45f };
        int millis = totalmillis ? totalmillis : lastmillis;
        localVisibility *visibility = findLocalVisibility(source, false, true);
        visibilities = vec4(1.0f, 1.0f, 1.0f, 1.0f);
        usedPrecomputed = usedFallback = false;

        loopi(4)
        {
            if(layerWeights[i] <= 1.0e-4f)
            {
                visibilities[i] = 0.0f;
                continue;
            }

            int age = visibility->layerVisibilityMillis[i] ? millis - visibility->layerVisibilityMillis[i] : LOCAL_VISIBILITY_MILLIS + 1;
            bool valid = age <= LOCAL_VISIBILITY_MILLIS;
            if(valid)
            {
                visibilities[i] = visibility->layerVisibility[i];
                usedPrecomputed = true;
            }
            else usedFallback = true;

            if(!valid || age >= SOURCE_VISIBILITY_REFRESH_MILLIS)
                issueSourceVisibilityQuery(source, i, screen, visibilityOverride.z, visibilityOverride.w * radiusScales[i], visibilityShader);
        }
    }

    static void issueLocalOcclusionQuery(const vec &source, bool sun, bool area, const vec4 &screen, float radiusPixels, float ndcDepth)
    {
        if(!glGenQueries_ || !glBeginQuery_ || localQueryPending(source, sun, area)) return;
        localOcclusionQuery *query = availableLocalQuery();
        if(!query) return;

        bool hadDepth = glIsEnabled(GL_DEPTH_TEST) != 0, hadBlend = glIsEnabled(GL_BLEND) != 0;
        GLboolean oldDepthMask = GL_TRUE, oldColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        GLint oldDepthFunc;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);
        glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);

        glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        glViewport(0, 0, vieww, viewh);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        if(hadBlend) glDisable(GL_BLEND);

        nullshader->set();
        glBeginQuery_(GL_SAMPLES_PASSED, query->id);
        drawOcclusionCircle(screen, radiusPixels, true, ndcDepth);
        glEndQuery_(GL_SAMPLES_PASSED);

        glDepthFunc(oldDepthFunc);
        glDepthMask(oldDepthMask);
        glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
        if(!hadDepth) glDisable(GL_DEPTH_TEST);
        if(hadBlend) glEnable(GL_BLEND);

        query->source = source;
        query->sun = sun;
        query->area = area;
        query->total = float(M_PI) * radiusPixels * radiusPixels * max(msaalight ? msaasamples : 1, 1);
        query->issuedFrame = flareQueryFrame;
        query->pending = true;
        if(debugflares) profile.queriesIssued++;
    }

    static bool hardCenterVisible(const vec &source, bool sun, const vec4 &screen, float ndcDepth)
    {
        if(screen.x < 0.0f || screen.x > 1.0f || screen.y < 0.0f || screen.y > 1.0f) return false;

        int millis = totalmillis ? totalmillis : lastmillis;
        localVisibility *visibility = findLocalVisibility(source, sun, true);
        visibility->lastUsedMillis = millis;
        float biasedDepth = clamp(ndcDepth + min(0.0025f + cameraVelocityBias * 0.00035f, 0.02f), -1.0f, 1.0f);
        issueLocalOcclusionQuery(source, sun, false, screen, 0.75f, biasedDepth);

        float conservativeRadius = 1.5f + cameraVelocityBias;
        bool needsArea = conservativeRadius > 1.5f;
        if(needsArea) issueLocalOcclusionQuery(source, sun, true, screen, conservativeRadius, biasedDepth);

        bool centerValid = visibility->centerMillis && millis - visibility->centerMillis <= LOCAL_VISIBILITY_MILLIS;
        bool areaValid = visibility->areaMillis && millis - visibility->areaMillis <= LOCAL_VISIBILITY_MILLIS;
        if(centerValid && visibility->center <= 0.0f) return false;
        if(needsArea && areaValid && visibility->area < 0.35f) return false;
        if(debugflares && (!centerValid || (needsArea && !areaValid))) profile.fallbackHits++;

        return true;
    }

    static void drawDebugCircle(Shader *debugShader, const vec4 &screen, float radiusPixels)
    {
        if(!debugflares || !debugShader) return;

        bool hadDepth = glIsEnabled(GL_DEPTH_TEST) != 0, hadBlend = glIsEnabled(GL_BLEND) != 0;
        GLint oldBlendSrcRGB, oldBlendDstRGB, oldBlendSrcAlpha, oldBlendDstAlpha;
        glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDstAlpha);

        if(hadDepth) glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        if(glBlendFuncSeparate_) glBlendFuncSeparate_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        gle::colorf(1.0f, 0.0f, 0.0f, 1.0f);
        debugShader->set();
        drawOcclusionCircle(screen, radiusPixels, false);
        gle::colorf(1.0f, 1.0f, 1.0f, 1.0f);

        if(glBlendFuncSeparate_) glBlendFuncSeparate_(oldBlendSrcRGB, oldBlendDstRGB, oldBlendSrcAlpha, oldBlendDstAlpha);
        else glBlendFunc(oldBlendSrcRGB, oldBlendDstRGB);
        if(!hadBlend) glDisable(GL_BLEND);
        if(hadDepth) glEnable(GL_DEPTH_TEST);
    }

    static float queryCloudCircleOcclusion(const vec4 &screen, float radiusPixels, Shader *cloudOcclusionShader)
    {
        if(!cloudOcclusionShader || !glGenQueries_ || !glBeginQuery_) return cloudOcclusionLast;
        if(!volumetricClouds::bindcomposite(0))
        {
            cloudOcclusionLast = 0.0f;
            return cloudOcclusionLast;
        }
        if(flareQueryFrame - cloudLastIssuedFrame < CLOUD_QUERY_INTERVAL) return cloudOcclusionLast;

        occlusionQuery *query = availableQuery(cloudOcclusionQueries, CLOUD_QUERY_COUNT);
        if(!query) return cloudOcclusionLast;

        const vec4 &cloudParams = volumetricClouds::compositetexparams();
        bool hadDepth = glIsEnabled(GL_DEPTH_TEST) != 0, hadBlend = glIsEnabled(GL_BLEND) != 0;
        GLboolean oldDepthMask = GL_TRUE, oldColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

        glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        glViewport(0, 0, vieww, viewh);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_FALSE);
        if(hadDepth) glDisable(GL_DEPTH_TEST);
        if(hadBlend) glDisable(GL_BLEND);

        GLOBALPARAMF(sunFlareCloudTex, cloudParams.x, cloudParams.y, cloudParams.z, cloudParams.w);
        cloudOcclusionShader->set();
        glBeginQuery_(GL_SAMPLES_PASSED, query->id);
        drawOcclusionCircle(screen, radiusPixels, true);
        glEndQuery_(GL_SAMPLES_PASSED);

        glDepthMask(oldDepthMask);
        glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
        if(hadDepth) glEnable(GL_DEPTH_TEST);
        if(hadBlend) glEnable(GL_BLEND);
        glActiveTexture_(GL_TEXTURE0);

        query->total = float(M_PI) * radiusPixels * radiusPixels * max(msaalight ? msaasamples : 1, 1);
        query->issuedFrame = flareQueryFrame;
        query->pending = true;
        cloudLastIssuedFrame = flareQueryFrame;
        if(debugflares) profile.queriesIssued++;
        return cloudOcclusionLast;
    }

    static void reportDebugOcclusion(float occlusion, float geometryOcclusion = -1.0f, float cloudOcclusion = -1.0f)
    {
        if(!debugflares) return;
        int millis = totalmillis ? totalmillis : lastmillis;
        if(millis - sunOcclusionDebugMillis < 1000) return;
        sunOcclusionDebugMillis = millis;
        if(geometryOcclusion >= 0.0f || cloudOcclusion >= 0.0f)
            conoutf(CON_INFO, "lens flare occlusion: %.1f%% (geometry %.1f%%, clouds %.1f%%)",
                100.0f * clamp(occlusion, 0.0f, 1.0f),
                100.0f * clamp(max(geometryOcclusion, 0.0f), 0.0f, 1.0f),
                100.0f * clamp(max(cloudOcclusion, 0.0f), 0.0f, 1.0f));
        else conoutf(CON_INFO, "lens flare occlusion: %.1f%%", 100.0f * clamp(occlusion, 0.0f, 1.0f));
    }

    static float updateSunOcclusion(const vec4 &screen, Shader *debugShader, Shader *cloudOcclusionShader)
    {
        float radiusPixels = occlusionRadiusPixels();

        float cloudOcclusion = queryCloudCircleOcclusion(screen, radiusPixels, cloudOcclusionShader);
        float cloudVisibility = 1.0f - clamp(cloudOcclusion / max(lensflarecloudocclusionthreshold, 1.0e-4f), 0.0f, 1.0f);
        sunOcclusionTarget = clamp(sunGeometryVisibilityTarget * cloudVisibility, 0.0f, 1.0f);

        int millis = totalmillis ? totalmillis : lastmillis;
        if(!sunOcclusionMillis)
        {
            sunOcclusionSmoothed = sunOcclusionTarget;
            sunOcclusionMillis = millis;
        }
        else
        {
            int elapsed = max(millis - sunOcclusionMillis, 0);
            sunOcclusionMillis = millis;
            if(sunOcclusionTarget < sunOcclusionSmoothed) sunOcclusionSmoothed = sunOcclusionTarget;
            else
            {
                float lerp = lensflareocclusionlerp <= 0 ? 1.0f : clamp(float(elapsed) / max(float(lensflareocclusionlerp), 1.0f), 0.0f, 1.0f);
                sunOcclusionSmoothed += (sunOcclusionTarget - sunOcclusionSmoothed) * lerp;
            }
        }

        reportDebugOcclusion(1.0f - sunOcclusionTarget, 1.0f - sunGeometryVisibilityTarget, cloudOcclusion);

        if(glGenQueries_ && glBeginQuery_)
        {
            occlusionQuery *query = availableQuery(sunOcclusionQueries, SUN_QUERY_COUNT);
            if(query)
            {
                bool hadDepth = glIsEnabled(GL_DEPTH_TEST) != 0, hadBlend = glIsEnabled(GL_BLEND) != 0;
                GLboolean oldDepthMask = GL_TRUE, oldColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
                GLint oldDepthFunc;
                glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
                glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);
                glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);

                glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
                glViewport(0, 0, vieww, viewh);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthMask(GL_FALSE);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                if(hadBlend) glDisable(GL_BLEND);

                nullshader->set();
                glBeginQuery_(GL_SAMPLES_PASSED, query->id);
                drawOcclusionCircle(screen, radiusPixels, true);
                glEndQuery_(GL_SAMPLES_PASSED);

                glDepthFunc(oldDepthFunc);
                glDepthMask(oldDepthMask);
                glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
                if(!hadDepth) glDisable(GL_DEPTH_TEST);
                if(hadBlend) glEnable(GL_BLEND);

                int samples = max(msaalight ? msaasamples : 1, 1);
                query->total = float(M_PI) * radiusPixels * radiusPixels * samples;
                query->issuedFrame = flareQueryFrame;
                query->pending = true;
                if(debugflares) profile.queriesIssued++;
            }
        }

        drawDebugCircle(debugShader, screen, radiusPixels);

        return clamp(sunOcclusionSmoothed, 0.0f, 1.0f);
    }

    void addFlares(const vec &o, int r, int g, int b, bool unlimitedDistance, bool lensGhosts, int maxDistance)
    {
        if(!shouldRender()) return;
        queuedFlare &f = queuedFlares.add();
        f.o = o;
        f.color = bvec(uchar(clamp(r, 0, 255)), uchar(clamp(g, 0, 255)), uchar(clamp(b, 0, 255)));
        f.maxDistance = maxDistance;
        f.unlimitedDistance = unlimitedDistance;
        f.lensGhosts = lensGhosts;
    }

    void cleanup()
    {
        queuedFlares.setsize(0);
        profile = flareProfile();
        loopi(SUN_QUERY_COUNT) if(sunOcclusionQueries[i].id) glDeleteQueries_(1, &sunOcclusionQueries[i].id);
        loopi(SUN_QUERY_COUNT) sunOcclusionQueries[i] = occlusionQuery();
        loopi(CLOUD_QUERY_COUNT) if(cloudOcclusionQueries[i].id) glDeleteQueries_(1, &cloudOcclusionQueries[i].id);
        loopi(CLOUD_QUERY_COUNT) cloudOcclusionQueries[i] = occlusionQuery();
        loopi(LOCAL_QUERY_COUNT) if(localOcclusionQueries[i].id) glDeleteQueries_(1, &localOcclusionQueries[i].id);
        loopi(LOCAL_QUERY_COUNT) localOcclusionQueries[i] = localOcclusionQuery();
        loopi(SOURCE_VISIBILITY_QUERY_COUNT) if(sourceVisibilityQueries[i].id) glDeleteQueries_(1, &sourceVisibilityQueries[i].id);
        loopi(SOURCE_VISIBILITY_QUERY_COUNT) sourceVisibilityQueries[i] = sourceVisibilityQuery();
        localVisibilityCache.setsize(0);
        flareQueryFrame = 0;
        sunLastResolvedFrame = -1;
        cloudLastIssuedFrame = -CLOUD_QUERY_INTERVAL;
        cloudLastResolvedFrame = -1;
        cloudOcclusionLast = 0.0f;
    }

    static void drawFlare(Shader *flareShader, const vec4 &screen, const vec4 &params, const vec &color, float ghostStrength,
                          const vec4 &layerWeights, const vec4 &visibilities, const vec4 &visibilityOverride)
    {
        GLOBALPARAMF(sunFlareScreen, screen.x, screen.y, screen.z, screen.w);
        GLOBALPARAMF(sunFlareParams, params.x, params.y, params.z, params.w);
        GLOBALPARAMF(sunFlareGhostStrength, ghostStrength);
        GLOBALPARAMF(sunFlareLayerWeights, layerWeights.x, layerWeights.y, layerWeights.z, layerWeights.w);
        GLOBALPARAMF(sunFlareVisibilities, visibilities.x, visibilities.y, visibilities.z, visibilities.w);
        GLOBALPARAMF(sunFlareVisibilityOverride, visibilityOverride.x, visibilityOverride.y, visibilityOverride.z, visibilityOverride.w);
        GLOBALPARAM(sunFlareColor, color);
        flareShader->set();
        screenquad(vieww, viewh);
    }

    static float projectedRadiusPixels(const vec &center, const vec2 &centerNdc, float worldRadius)
    {
        float radiusPixels = 0.0f;

        vec sample(center);
        sample.madd(camright, worldRadius);
        vec4 sampleClip;
        camprojmatrix.transform(sample, sampleClip);
        if(sampleClip.w > 1.0e-4f)
        {
            vec2 sampleNdc(sampleClip.x / sampleClip.w, sampleClip.y / sampleClip.w);
            vec2 delta(sampleNdc.x - centerNdc.x, sampleNdc.y - centerNdc.y);
            radiusPixels = max(radiusPixels, 0.5f * sqrtf(delta.x*delta.x*vieww*vieww + delta.y*delta.y*viewh*viewh));
        }

        sample = center;
        sample.madd(camup, worldRadius);
        camprojmatrix.transform(sample, sampleClip);
        if(sampleClip.w > 1.0e-4f)
        {
            vec2 sampleNdc(sampleClip.x / sampleClip.w, sampleClip.y / sampleClip.w);
            vec2 delta(sampleNdc.x - centerNdc.x, sampleNdc.y - centerNdc.y);
            radiusPixels = max(radiusPixels, 0.5f * sqrtf(delta.x*delta.x*vieww*vieww + delta.y*delta.y*viewh*viewh));
        }

        return max(radiusPixels, 1.0f);
    }

    static bool initSun(vec4 &sunScreen, vec4 &sunParams, vec &sunColor, float &ghostStrength, vec4 &layerWeights, vec4 &visibilityOverride)
    {
        if(!shouldRender(true)) return false;

        vec sunPoint(camera1->o);
        sunPoint.madd(sunlightdir, max(nearplane*4.0f, 1.0f));

        vec4 sunClip;
        camprojmatrix.transform(sunPoint, sunClip);
        if(sunClip.w <= 1.0e-4f || sunClip.z < -sunClip.w) return false;

        vec2 sunNdc(sunClip.x / sunClip.w, sunClip.y / sunClip.w);
        if(fabsf(sunNdc.x) > 1.35f || fabsf(sunNdc.y) > 1.35f) return false;

        float screenEdge = max(fabsf(sunNdc.x), fabsf(sunNdc.y));
        float edgeFade = clamp(1.0f - max(screenEdge - 0.90f, 0.0f) / 0.40f, 0.0f, 1.0f);
        float horizonFade = clamp((sunlightdir.z - 0.02f) / 0.10f, 0.0f, 1.0f);
        float screenFade = edgeFade * horizonFade;
        if(screenFade <= 1.0e-4f) return false;

        float shaftScale = max(sunflareshaftsize / 100.0f, 0.01f);
        sunScreen = vec4(sunNdc.x * 0.5f + 0.5f, sunNdc.y * 0.5f + 0.5f, screenFade, shaftScale);

        vec baseColor = sunlight.tocolor();
        float colorMax = max(max(baseColor.x, baseColor.y), baseColor.z);
        if(colorMax <= 1.0e-4f) return false;

        float sunScale = max(sunlightscale, 0.0f);
        float sunLuma = (0.2126f * baseColor.x + 0.7152f * baseColor.y + 0.0722f * baseColor.z) * sunScale;
        if(sunLuma <= 1.0e-4f) return false;

        sunColor = vec(baseColor).mul(1.0f / colorMax);

        float sourceBoost = clamp(0.5f + sqrtf(sunLuma), 0.5f, 4.0f);
        float strength = (sunflarestrength / 50.0f) * sourceBoost;
        sunParams = vec4(strength, 0.0f, lastmillis / 1000.0f, sourceBoost);
        ghostStrength = flareghosts ? 0.5f : 0.0f;
        layerWeights = vec4(1.0f, 1.0f, 1.0f, flareghosts ? 1.0f : 0.0f);
        visibilityOverride = vec4(1.0f, 1.0f, 0.0f, 0.0f);
        return true;
    }

    static bool initFlare(const queuedFlare &source, vec4 &flareScreen, vec4 &flareParams, vec &flareColor, float &ghostStrength, vec4 &layerWeights, vec4 &visibilityOverride, float &centerDepth)
    {
        // Note: shouldRender() is intentionally not rechecked here.
        // addFlares() already gates on it, so queuedFlares is never populated when rendering is disabled.
        vec flaredir(source.o);
        flaredir.sub(camera1->o);
        float flareDistance = flaredir.magnitude();
        if(!(flareDistance > 1.0e-4f) || flareDistance >= FLT_MAX)
        {
            recordReject(FLARE_REJECT_DISTANCE);
            return false;
        }

        float distanceFade = 1.0f;
        if(!source.unlimitedDistance)
        {
            float maxDistance = max(float(source.maxDistance), 1.0f);
            distanceFade = clamp(1.0f - flareDistance / maxDistance, 0.0f, 1.0f);
            if(distanceFade <= 1.0e-4f)
            {
                recordReject(FLARE_REJECT_MAX_DISTANCE);
                return false;
            }
        }

        vec4 flareClip;
        camprojmatrix.transform(source.o, flareClip);
        if(flareClip.w <= 1.0e-4f || flareClip.z < -flareClip.w)
        {
            recordReject(FLARE_REJECT_CLIP);
            return false;
        }
        centerDepth = clamp(flareClip.z / flareClip.w, -1.0f, 1.0f);

        vec2 flareNdc(flareClip.x / flareClip.w, flareClip.y / flareClip.w);
        if(fabsf(flareNdc.x) > 1.35f || fabsf(flareNdc.y) > 1.35f)
        {
            recordReject(FLARE_REJECT_SCREEN);
            return false;
        }

        float screenEdge = max(fabsf(flareNdc.x), fabsf(flareNdc.y));
        float edgeFade = clamp(1.0f - max(screenEdge - 0.90f, 0.0f) / 0.40f, 0.0f, 1.0f);
        float screenFade = edgeFade * distanceFade;
        if(screenFade <= 1.0e-4f)
        {
            recordReject(FLARE_REJECT_EDGE);
            return false;
        }

        vec baseColor(source.color.r / 255.0f, source.color.g / 255.0f, source.color.b / 255.0f);
        float colorMax = max(max(baseColor.x, baseColor.y), baseColor.z);
        if(colorMax <= 1.0e-4f)
        {
            recordReject(FLARE_REJECT_COLOR);
            return false;
        }

        if(isvisiblesphere(0.0f, source.o) > (source.unlimitedDistance ? VFC_FOGGED : VFC_FULL_VISIBLE))
        {
            recordReject(FLARE_REJECT_VISIBILITY);
            return false;
        }

        flaredir.mul(1.0f / flareDistance);
        if(raycube(camera1->o, flaredir, flareDistance, RAY_CLIPMAT | RAY_POLY) < flareDistance - 0.25f)
        {
            recordReject(FLARE_REJECT_RAYCAST);
            return false;
        }

        flareScreen = vec4(flareNdc.x * 0.5f + 0.5f, flareNdc.y * 0.5f + 0.5f, screenFade, max(sunflareshaftsize / 100.0f, 0.01f));
        flareColor = vec(baseColor).mul(1.0f / colorMax);

        float flareLuma = 0.2126f * baseColor.x + 0.7152f * baseColor.y + 0.0722f * baseColor.z;
        float sourceBoost = clamp(0.5f + sqrtf(flareLuma), 0.5f, 2.5f);
        float strength = (sunflarestrength / 50.0f) * sourceBoost;
        vec2 linearDepthScale = projmatrix.lineardepthscale();
        float sourceDepth = linearDepthScale.x*flareClip.z + linearDepthScale.y*flareClip.w;
        float occlusionRadiusPixels = projectedRadiusPixels(source.o, flareNdc, occlusionRadius);
        float ghostLayer = flareghosts && source.lensGhosts ? 1.0f : 0.0f;
        flareParams = vec4(strength, 0.0f, lastmillis / 1000.0f, sourceBoost);
        ghostStrength = ghostLayer ? 0.5f : 0.0f;
        layerWeights = vec4(0.0f, 0.0f, 1.0f, ghostLayer);
        visibilityOverride = vec4(-1.0f, -1.0f, sourceDepth, occlusionRadiusPixels);
        return true;
    }

    void render()
    {
        beginProfile();
        flareQueryFrame++;
        pollSunOcclusionQueries();
        pollCloudOcclusionQueries();
        pollLocalOcclusionQueries();
        pollSourceVisibilityQueries();

        vec4 sunScreen, sunParams, sunLayerWeights, sunVisibilityOverride;
        vec sunColor;
        float sunGhostStrength = 0.0f;
        bool renderSun = initSun(sunScreen, sunParams, sunColor, sunGhostStrength, sunLayerWeights, sunVisibilityOverride);
        if(debugflares) profile.sunQueued = renderSun;
        updateCameraVelocityBias();

        if(!renderSun && queuedFlares.empty())
        {
            queuedFlares.setsize(0);
            endProfile();
            return;
        }

        Shader *flareShader = useshaderbyname("lensflare");
        Shader *debugShader = debugflares ? useshaderbyname("lensflaredebug") : NULL;
        Shader *cloudOcclusionShader = useshaderbyname("lensflarecloudocclusion");
        Shader *sourceVisibilityShader = useshaderbyname("lensflarevisibility");
        if(!flareShader)
        {
            queuedFlares.setsize(0);
            endProfile();
            return;
        }

        if(renderSun)
        {
            if(!hardCenterVisible(sunlightdir, true, sunScreen, 1.0f))
            {
                sunGeometryVisibilityTarget = sunOcclusionTarget = sunOcclusionSmoothed = 0.0f;
                reportDebugOcclusion(1.0f, 1.0f, 0.0f);
                drawDebugCircle(debugShader, sunScreen, occlusionRadiusPixels());
                renderSun = false;
            }
            else
            {
                float sunVisibility = updateSunOcclusion(sunScreen, debugShader, cloudOcclusionShader);
                sunVisibilityOverride = vec4(sunVisibility, 1.0f, 0.0f, 0.0f);
                if(sunVisibility <= 1.0e-4f) renderSun = false;
            }
        }

        bool hadScissor = glIsEnabled(GL_SCISSOR_TEST) != 0;
        bool hadDepth = glIsEnabled(GL_DEPTH_TEST) != 0;
        bool hadBlend = glIsEnabled(GL_BLEND) != 0;
        GLint oldBlendSrcRGB, oldBlendDstRGB, oldBlendSrcAlpha, oldBlendDstAlpha;
        glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDstAlpha);

        if(hadScissor) glDisable(GL_SCISSOR_TEST);
        if(hadDepth) glDisable(GL_DEPTH_TEST);

        glActiveTexture_(GL_TEXTURE0);
        if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
        else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
        glActiveTexture_(GL_TEXTURE1);
        if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msnormaltex);
        else glBindTexture(GL_TEXTURE_RECTANGLE, gnormaltex);
        glActiveTexture_(GL_TEXTURE2);
        bool hasCloudComposite = volumetricClouds::bindcomposite(2);
        vec4 cloudCompositeParams = hasCloudComposite ? volumetricClouds::compositetexparams() : vec4(0, 0, 0, 0);
        if(!hasCloudComposite) glBindTexture(GL_TEXTURE_RECTANGLE, 0);
        glActiveTexture_(GL_TEXTURE0);

        glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
        glViewport(0, 0, vieww, viewh);
        glEnable(GL_BLEND);
        if(glBlendFuncSeparate_) glBlendFuncSeparate_(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        else glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        GLOBALPARAMF(sunFlareFovScale, getFovScale());
        GLOBALPARAMF(sunFlareCloudTex, cloudCompositeParams.x, cloudCompositeParams.y, cloudCompositeParams.z, cloudCompositeParams.w);
        if(renderSun)
        {
            vec4 sunVisibilities(1.0f, 1.0f, 1.0f, sunVisibilityOverride.x);
            drawFlare(flareShader, sunScreen, sunParams, sunColor, sunGhostStrength, sunLayerWeights, sunVisibilities, sunVisibilityOverride);
            if(debugflares)
            {
                profile.sunRendered = true;
                if(sunLastResolvedFrame >= 0) profile.visibilityPrecomputed++;
                else profile.visibilityFallback++;
            }
        }
        loopv(queuedFlares)
        {
            vec4 flareScreen, flareParams, layerWeights, visibilityOverride;
            vec flareColor;
            float ghostStrength = 0.0f;
            float centerDepth = 1.0f;
            if(!initFlare(queuedFlares[i], flareScreen, flareParams, flareColor, ghostStrength, layerWeights, visibilityOverride,
                          centerDepth)) continue;
            if(!hardCenterVisible(queuedFlares[i].o, false, flareScreen, centerDepth))
            {
                recordReject(FLARE_REJECT_HARD_CENTER);
                continue;
            }
            vec4 visibilities;
            bool usedPrecomputed = false, usedFallback = false;
            prepareLocalFlareVisibilities(queuedFlares[i].o, flareScreen, layerWeights, visibilityOverride, sourceVisibilityShader, visibilities,
                                          usedPrecomputed, usedFallback);
            drawFlare(flareShader, flareScreen, flareParams, flareColor, ghostStrength, layerWeights, visibilities, visibilityOverride);
            if(debugflares) profile.rendered++;
            if(debugflares)
            {
                if(usedPrecomputed) profile.visibilityPrecomputed++;
                profile.visibilityPerPixel++;
                if(usedFallback) profile.visibilityFallback++;
            }
        }

        if(glBlendFuncSeparate_) glBlendFuncSeparate_(oldBlendSrcRGB, oldBlendDstRGB, oldBlendSrcAlpha, oldBlendDstAlpha);
        else glBlendFunc(oldBlendSrcRGB, oldBlendDstRGB);
        if(!hadBlend) glDisable(GL_BLEND);
        if(hadDepth) glEnable(GL_DEPTH_TEST);
        if(hadScissor) glEnable(GL_SCISSOR_TEST);
        queuedFlares.setsize(0);
        endProfile();
    }

    void debugview()
    {
        if(!debugflares) return;

        int y = 0;
        draw_text("lens flares", 0, y);
        y += FONTH;
        draw_textf("render total: %.3f ms", 0, y, profile.totalMillis);
        y += FONTH;
        draw_textf("render average: %.3f ms", 0, y, max(profile.averageMillis, 0.0f));
        y += FONTH;
        draw_textf("queued: %d", 0, y, profile.queued);
        y += FONTH;
        draw_textf("rendered: %d", 0, y, profile.rendered);
        y += FONTH;
        draw_textf("rejected: %d", 0, y, profile.rejected);
        y += FONTH;
        draw_textf("sun: %s", 0, y, profile.sunQueued ? (profile.sunRendered ? "rendered" : "rejected") : "inactive");
        y += FONTH;
        draw_textf("queries issued/resolved: %d / %d", 0, y, profile.queriesIssued, profile.queriesResolved);
        y += FONTH;
        draw_textf("queries pending/fallbacks: %d / %d", 0, y, profile.queriesPending, profile.fallbackHits);
        y += FONTH;
        draw_textf("visibility precomputed/per-pixel/fallback: %d / %d / %d", 0, y, profile.visibilityPrecomputed, profile.visibilityPerPixel,
                   profile.visibilityFallback);
        y += FONTH;
        draw_textf("reject distance/max: %d / %d", 0, y, profile.rejects[FLARE_REJECT_DISTANCE], profile.rejects[FLARE_REJECT_MAX_DISTANCE]);
        y += FONTH;
        draw_textf("reject clip/screen/edge: %d / %d / %d", 0, y, profile.rejects[FLARE_REJECT_CLIP], profile.rejects[FLARE_REJECT_SCREEN],
                   profile.rejects[FLARE_REJECT_EDGE]);
        y += FONTH;
        draw_textf("reject color/pvs/ray: %d / %d / %d", 0, y, profile.rejects[FLARE_REJECT_COLOR], profile.rejects[FLARE_REJECT_VISIBILITY],
                   profile.rejects[FLARE_REJECT_RAYCAST]);
        y += FONTH;
        draw_textf("reject hard center: %d", 0, y, profile.rejects[FLARE_REJECT_HARD_CENTER]);
    }
}
