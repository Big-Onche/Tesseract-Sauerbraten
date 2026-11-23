#include "engine.h"
#include "reflections.h"
#include "bvh/bvh.h"

extern GLuint gcolortex, gnormaltex, gdepthtex, mscolortex, msnormaltex, msdepthtex, hdrfbo;
extern bvec skyboxcolour;

namespace renderer
{
    extern float pbrroughnessscale, pbrspecularscale;

    namespace reflections
    {
        extern float rtresolutionscale, rtmaxdistance, rtreflectionstrength, rtreflectionroughnesspower;
        extern float rtadaptivehigh, rtadaptivemedium, rtadaptivelow;
        extern int rtadaptive, rtdenoise, rtspatialfilter, rtspecularhistory, rtspecularspatial, rtreconstruction, rtdebug;
        extern int profilert, profilertdetail, profilertwindow;

        namespace
        {
            GLuint reflectionfbo = 0, rawtexture = 0, auxiliarytexture = 0, hitnormaltexture = 0, historytexture[2] = { 0, 0 },
                   historysurface[2] = { 0, 0 }, historyvalidation[2] = { 0, 0 }, filteredtexture = 0, upscaledtexture = 0;
            int resourcewidth = -1, resourceheight = -1, tracewidth = 0, traceheight = 0, historyindex = 0;
            bool historyvalid = false;
            bool pipelineavailable = true;
            uint historygeneration = 0;
            matrix4 previousviewprojection, previousworldmatrix;

            enum ProfilePass
            {
                PROFILE_TRACE = 0,
                PROFILE_TEMPORAL,
                PROFILE_SPATIAL,
                PROFILE_COMPOSITE,
                PROFILE_TOTAL,
                PROFILE_PASS_COUNT
            };
            enum
            {
                PROFILE_QUERY_COUNT = 4,
                PROFILE_TIMESTAMPS = 10,
                PROFILE_COUNTER_COUNT = 16,
                PROFILE_COUNTER_BINDING = 14,
                PROFILE_MAX_SAMPLES = 240
            };

            struct profilestat
            {
                float samples[PROFILE_MAX_SAMPLES], latest, average, minimum, maximum;
                int count, next;

                profilestat() : latest(-1), average(-1), minimum(-1), maximum(-1), count(0), next(0) {}

                void reset()
                {
                    latest = average = minimum = maximum = -1;
                    count = next = 0;
                }

                void add(float value)
                {
                    int window = clamp(profilertwindow, 8, int(PROFILE_MAX_SAMPLES));
                    samples[next] = value;
                    next = (next + 1)%window;
                    count = min(count + 1, window);
                    latest = value;
                    average = 0.0f;
                    minimum = maximum = samples[0];
                    loopi(count)
                    {
                        average += samples[i];
                        minimum = min(minimum, samples[i]);
                        maximum = max(maximum, samples[i]);
                    }
                    average /= count;
                }
            };

            struct profileframe
            {
                uint counters[PROFILE_COUNTER_COUNT];
                int fullwidth, fullheight, tracewidth, traceheight, msaa, passmask;
                uint bvhgeneration;
                bool detailed, multisampled;

                profileframe() : fullwidth(0), fullheight(0), tracewidth(0), traceheight(0), msaa(0), passmask(0), bvhgeneration(0), detailed(false),
                                 multisampled(false)
                {
                    memset(counters, 0, sizeof(counters));
                }
            };

            GLuint profilequeries[PROFILE_QUERY_COUNT][PROFILE_TIMESTAMPS] = { { 0 } }, profilebuffers[PROFILE_QUERY_COUNT] = { 0 };
            int profilequerycycle = 0, profilequerywaiting = 0, profilequeryactive = -1, profilepassmask = 0;
            profilestat profilestats[PROFILE_PASS_COUNT];
            profileframe profileframes[PROFILE_QUERY_COUNT], latestprofileframe;
            uint profilesamples = 0;
            bool profiletimersupported = false;

            void resetprofilestats()
            {
                loopi(PROFILE_PASS_COUNT) profilestats[i].reset();
                latestprofileframe = profileframe();
                profilesamples = 0;
            }

            void initprofilerresources(bool counters)
            {
                if(!profilequeries[0][0]) glGenQueries_(PROFILE_QUERY_COUNT*PROFILE_TIMESTAMPS, &profilequeries[0][0]);
                if(counters && !profilebuffers[0])
                {
                    glGenBuffers_(PROFILE_QUERY_COUNT, profilebuffers);
                    loopi(PROFILE_QUERY_COUNT)
                    {
                        glBindBuffer_(GL_SHADER_STORAGE_BUFFER, profilebuffers[i]);
                        glBufferData_(GL_SHADER_STORAGE_BUFFER, PROFILE_COUNTER_COUNT*sizeof(uint), NULL, GL_STREAM_READ);
                    }
                    glBindBuffer_(GL_SHADER_STORAGE_BUFFER, 0);
                }
            }

            void pollprofiler()
            {
                if(!profilequeries[0][0]) return;
                loopi(PROFILE_QUERY_COUNT) if(profilequerywaiting&(1<<i))
                {
                    GLint available = 0;
                    glGetQueryObjectiv_(profilequeries[i][1], GL_QUERY_RESULT_AVAILABLE, &available);
                    if(!available) continue;

                    GLuint64EXT timestamp[PROFILE_TIMESTAMPS] = { 0 };
                    glGetQueryObjectui64v_(profilequeries[i][0], GL_QUERY_RESULT, &timestamp[0]);
                    glGetQueryObjectui64v_(profilequeries[i][1], GL_QUERY_RESULT, &timestamp[1]);
                    profileframe &frame = profileframes[i];
                    loopj(4) if(frame.passmask&(1<<j))
                    {
                        glGetQueryObjectui64v_(profilequeries[i][2 + 2*j], GL_QUERY_RESULT, &timestamp[2 + 2*j]);
                        glGetQueryObjectui64v_(profilequeries[i][3 + 2*j], GL_QUERY_RESULT, &timestamp[3 + 2*j]);
                        float elapsed = timestamp[3 + 2*j] >= timestamp[2 + 2*j] ?
                                        float(timestamp[3 + 2*j] - timestamp[2 + 2*j])*1.0e-6f : 0.0f;
                        profilestats[j].add(elapsed);
                    }
                    float total = timestamp[1] >= timestamp[0] ? float(timestamp[1] - timestamp[0])*1.0e-6f : 0.0f;
                    profilestats[PROFILE_TOTAL].add(total);
                    if(frame.detailed)
                    {
                        glBindBuffer_(GL_SHADER_STORAGE_BUFFER, profilebuffers[i]);
                        glGetBufferSubData_(GL_SHADER_STORAGE_BUFFER, 0, sizeof(frame.counters), frame.counters);
                        glBindBuffer_(GL_SHADER_STORAGE_BUFFER, 0);
                    }
                    latestprofileframe = frame;
                    ++profilesamples;
                    profilequerywaiting &= ~(1<<i);
                }
            }

            void beginprofiler(int width, int height)
            {
                profilequeryactive = -1;
                profilepassmask = 0;
                if(!profilert) return;
                pollprofiler();
                profiletimersupported = hasTQ && glQueryCounter_ && (!profilertdetail || (glBindBufferBase_ && glGetBufferSubData_));
                if(!profiletimersupported || (profilequerywaiting&(1<<profilequerycycle))) return;
                initprofilerresources(profilertdetail != 0);
                profilequeryactive = profilequerycycle;
                profileframe &frame = profileframes[profilequeryactive];
                frame = profileframe();
                frame.fullwidth = width;
                frame.fullheight = height;
                frame.tracewidth = tracewidth;
                frame.traceheight = traceheight;
                frame.msaa = msaasamples;
                frame.bvhgeneration = worldbvh::generation();
                frame.detailed = profilertdetail != 0;
                frame.multisampled = msaalight != 0;
                if(frame.detailed)
                {
                    uint zero[PROFILE_COUNTER_COUNT] = { 0 };
                    glBindBuffer_(GL_SHADER_STORAGE_BUFFER, profilebuffers[profilequeryactive]);
                    glBufferData_(GL_SHADER_STORAGE_BUFFER, sizeof(zero), zero, GL_STREAM_READ);
                    glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, PROFILE_COUNTER_BINDING, profilebuffers[profilequeryactive]);
                    glBindBuffer_(GL_SHADER_STORAGE_BUFFER, 0);
                }
                glQueryCounter_(profilequeries[profilequeryactive][0], GL_TIMESTAMP);
            }

            void beginprofilerpass(ProfilePass pass)
            {
                if(profilequeryactive < 0 || pass >= PROFILE_TOTAL) return;
                profilepassmask |= 1<<pass;
                glQueryCounter_(profilequeries[profilequeryactive][2 + 2*pass], GL_TIMESTAMP);
            }

            void endprofilerpass(ProfilePass pass)
            {
                if(profilequeryactive < 0 || pass >= PROFILE_TOTAL) return;
                glQueryCounter_(profilequeries[profilequeryactive][3 + 2*pass], GL_TIMESTAMP);
                if(pass == PROFILE_TRACE && glMemoryBarrier_)
                    glMemoryBarrier_(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            }

            void endprofiler()
            {
                if(profilequeryactive < 0) return;
                bool detailed = profileframes[profilequeryactive].detailed;
                profileframes[profilequeryactive].passmask = profilepassmask;
                glQueryCounter_(profilequeries[profilequeryactive][1], GL_TIMESTAMP);
                profilequerywaiting |= 1<<profilequeryactive;
                profilequerycycle = (profilequeryactive + 1)%PROFILE_QUERY_COUNT;
                profilequeryactive = -1;
                if(detailed) glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, PROFILE_COUNTER_BINDING, 0);
            }

            void cleanupprofiler()
            {
                if(profilequeries[0][0]) glDeleteQueries_(PROFILE_QUERY_COUNT*PROFILE_TIMESTAMPS, &profilequeries[0][0]);
                if(profilebuffers[0]) glDeleteBuffers_(PROFILE_QUERY_COUNT, profilebuffers);
                memset(profilequeries, 0, sizeof(profilequeries));
                memset(profilebuffers, 0, sizeof(profilebuffers));
                profilequerycycle = profilequerywaiting = 0;
                profilequeryactive = -1;
                resetprofilestats();
            }

            void settingschanged()
            {
                historyvalid = false;
                pipelineavailable = true;
                cleanupprofiler();
            }

            float resolutionscale()
            {
                return clamp(rtresolutionscale, 0.25f, 1.0f);
            }

            void allocaterectangle(GLuint &texture, int width, int height, bool linear)
            {
                if(!texture) glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_RECTANGLE, texture);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
            }

            bool bindtarget(GLuint texture, int width, int height, const char *label)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, reflectionfbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, texture, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_RECTANGLE, 0, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_RECTANGLE, 0, 0);
                GLenum drawbuffer = GL_COLOR_ATTACHMENT0;
                glDrawBuffers_(1, &drawbuffer);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                {
                    conoutf(CON_ERROR, "RT reflections: incomplete %s framebuffer", label);
                    return false;
                }
                glViewport(0, 0, width, height);
                return true;
            }

            bool bindhistorytargets(GLuint color, GLuint feature, GLuint length, int width, int height, const char *label)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, reflectionfbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, color, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_RECTANGLE, feature, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_RECTANGLE, length, 0);
                const GLenum drawbuffers[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
                glDrawBuffers_(3, drawbuffers);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                {
                    conoutf(CON_ERROR, "RT reflections: incomplete %s framebuffer", label);
                    return false;
                }
                glViewport(0, 0, width, height);
                return true;
            }

            bool bindtracetargets(int width, int height, bool diagnostic)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, reflectionfbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rawtexture, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_RECTANGLE, diagnostic ? auxiliarytexture : 0, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_RECTANGLE, diagnostic ? hitnormaltexture : 0, 0);
                const GLenum drawbuffers[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
                glDrawBuffers_(diagnostic ? 3 : 1, drawbuffers);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                {
                    conoutf(CON_ERROR, "RT reflections: incomplete trace framebuffer");
                    return false;
                }
                glViewport(0, 0, width, height);
                return true;
            }

            void bindrectangle(GLenum unit, GLuint texture)
            {
                glActiveTexture_(unit);
                glBindTexture(GL_TEXTURE_RECTANGLE, texture);
            }

            void restorehdrtarget(int width, int height)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, hdrfbo);
                glViewport(0, 0, width, height);
                glActiveTexture_(GL_TEXTURE0);
            }

            void bindgbuffer()
            {
                GLenum target = msaalight ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_RECTANGLE;
                glActiveTexture_(GL_TEXTURE0);
                glBindTexture(target, msaalight ? mscolortex : gcolortex);
                glActiveTexture_(GL_TEXTURE1);
                glBindTexture(target, msaalight ? msnormaltex : gnormaltex);
                glActiveTexture_(GL_TEXTURE2);
                glBindTexture(target, msaalight ? msdepthtex : gdepthtex);
            }

            void setcommonparams()
            {
                LOCALPARAM(rtWorldMatrix, worldmatrix);
                LOCALPARAM(rtPreviousWorldMatrix, previousworldmatrix);
                LOCALPARAM(rtPreviousViewProjection, previousviewprojection);
                LOCALPARAMF(rtFullSize, float(resourcewidth), float(resourceheight), 1.0f/max(resourcewidth, 1),
                            1.0f/max(resourceheight, 1));
                LOCALPARAMF(rtTraceSize, float(tracewidth), float(traceheight), 1.0f/max(tracewidth, 1), 1.0f/max(traceheight, 1));
                LOCALPARAMF(rtMaterialParams, pbrroughnessscale, pbrspecularscale, rtreflectionroughnesspower, rtreflectionstrength);
            }

            bool prepareresources(int width, int height)
            {
                float scale = resolutionscale();
                int tw = max(int(ceilf(width*scale)), 1), th = max(int(ceilf(height*scale)), 1);
                if(reflectionfbo && resourcewidth == width && resourceheight == height && tracewidth == tw && traceheight == th) return true;
                cleanup();
                resourcewidth = width;
                resourceheight = height;
                tracewidth = tw;
                traceheight = th;
                glGenFramebuffers_(1, &reflectionfbo);
                allocaterectangle(rawtexture, tracewidth, traceheight, false);
                allocaterectangle(auxiliarytexture, tracewidth, traceheight, false);
                allocaterectangle(hitnormaltexture, tracewidth, traceheight, false);
                loopi(2)
                {
                    allocaterectangle(historytexture[i], resourcewidth, resourceheight, true);
                    allocaterectangle(historysurface[i], resourcewidth, resourceheight, false);
                    allocaterectangle(historyvalidation[i], resourcewidth, resourceheight, false);
                }
                allocaterectangle(filteredtexture, resourcewidth, resourceheight, true);
                allocaterectangle(upscaledtexture, resourcewidth, resourceheight, true);
                if(!bindtracetargets(tracewidth, traceheight, false) || !bindtracetargets(tracewidth, traceheight, true) ||
                   !bindhistorytargets(historytexture[0], historysurface[0], historyvalidation[0], resourcewidth, resourceheight, "history") ||
                   !bindtarget(filteredtexture, resourcewidth, resourceheight, "filter") ||
                   !bindtarget(upscaledtexture, resourcewidth, resourceheight, "filter scratch"))
                {
                    cleanup();
                    return false;
                }
                glBindTexture(GL_TEXTURE_RECTANGLE, 0);
                glBindFramebuffer_(GL_FRAMEBUFFER, 0);
                historyvalid = false;
                historyindex = 0;
                previousviewprojection = camprojmatrix;
                previousworldmatrix = worldmatrix;
                return true;
            }

            bool tracereflections()
            {
                bool diagnostic = rtdebug != 0;
                if(!bindtracetargets(tracewidth, traceheight, diagnostic)) return false;
                const char *shadername;
                if(diagnostic) shadername = profilertdetail && profilequeryactive >= 0 ?
                                            (msaalight ? "rtreflectiontracemsdebugprofile" : "rtreflectiontracedebugprofile") :
                                            (msaalight ? "rtreflectiontracemsdebug" : "rtreflectiontracedebug");
                else shadername = profilertdetail && profilequeryactive >= 0 ?
                                  (msaalight ? "rtreflectiontracemsprofile" : "rtreflectiontraceprofile") :
                                  (msaalight ? "rtreflectiontracems" : "rtreflectiontrace");
                Shader *shader = useshaderbyname(shadername);
                if(!shader || shader->isnull()) return false;
                timer *reflectiontimer = begintimer("RT reflection tracing");
                shader->set();
                setcommonparams();
                LOCALPARAMF(rtTraceParams, rtmaxdistance, 1.0f/65536.0f, float(worldbvh::root()), rtdebug == 14 ? 1.0f : 0.0f);
                LOCALPARAMF(rtTraceOptions, float(rtadaptive), float(worldbvh::materialcount()), hastexturedskybox() ? 1.0f : 0.0f,
                            getskyboxrotation());
                LOCALPARAMF(rtAdaptiveParams, rtadaptivehigh, min(rtadaptivemedium, rtadaptivehigh),
                            min(rtadaptivelow, min(rtadaptivemedium, rtadaptivehigh)), 0.0f);
                vec direction = getdirectionallightdir();
                const bvec &sun = getdirectionallightcolor();
                float sunscale = 2.0f*ldrscaleb*getdirectionallightscale()*getdirectionallightvisibility();
                LOCALPARAM(rtSunDirection, direction);
                LOCALPARAMF(rtSunColor, sun.x*sunscale, sun.y*sunscale, sun.z*sunscale);
                static const int maximumTraceLights = 32;
                tracelight traceLights[maximumTraceLights];
                vec4 lightPositionRadius[maximumTraceLights], lightColor[maximumTraceLights], lightDirectionCone[maximumTraceLights];
                int lightCount = collectlights(traceLights, maximumTraceLights);
                loopi(lightCount)
                {
                    lightPositionRadius[i] = traceLights[i].positionRadius;
                    lightColor[i] = traceLights[i].color;
                    lightDirectionCone[i] = traceLights[i].directionCone;
                }
                LOCALPARAMI(rtLightCount, lightCount);
                if(lightCount)
                {
                    LOCALPARAMV(rtLightPositionRadius, lightPositionRadius, lightCount);
                    LOCALPARAMV(rtLightColor, lightColor, lightCount);
                    LOCALPARAMV(rtLightDirectionCone, lightDirectionCone, lightCount);
                }
                vec skycolors[6];
                vec2 skyfront;
                getskycubetints(skycolors, skyfront);
                LOCALPARAM(rtSkyLeft, skycolors[0]);
                LOCALPARAM(rtSkyRight, skycolors[1]);
                LOCALPARAM(rtSkyBack, skycolors[2]);
                LOCALPARAM(rtSkyFront, skycolors[3]);
                LOCALPARAM(rtSkyDown, skycolors[4]);
                LOCALPARAM(rtSkyUp, skycolors[5]);
                vec skytint = skyboxcolour.tocolor().mul(ldrscale);
                LOCALPARAM(rtSkyTint, skytint);
                bindgbuffer();
                bindskyboxtextures(4);
                worldbvh::bind();
                glActiveTexture_(GL_TEXTURE0);
                screenquad(tracewidth, traceheight);
                endtimer(reflectiontimer);
                return true;
            }

            bool temporalaccumulation()
            {
                int destination = 1 - historyindex;
                if(!bindhistorytargets(historytexture[destination], historysurface[destination], historyvalidation[destination], resourcewidth,
                                       resourceheight, "temporal")) return false;
                Shader *shader = useshaderbyname(msaalight ? "rtreflectiontemporalms" : "rtreflectiontemporal");
                if(!shader || shader->isnull()) return false;
                timer *temporaltimer = begintimer("RT reflection temporal");
                shader->set();
                setcommonparams();
                LOCALPARAMF(rtTemporalParams, historyvalid && rtdenoise ? 1.0f : 0.0f, float(rtspecularhistory), float(rtreconstruction),
                            rtdebug == 14 ? 1.0f : 0.0f);
                bindgbuffer();
                bindrectangle(GL_TEXTURE3, rawtexture);
                bindrectangle(GL_TEXTURE5, historytexture[historyindex]);
                bindrectangle(GL_TEXTURE6, historysurface[historyindex]);
                bindrectangle(GL_TEXTURE7, historyvalidation[historyindex]);
                glActiveTexture_(GL_TEXTURE0);
                screenquad(resourcewidth, resourceheight);
                endtimer(temporaltimer);
                historyindex = destination;
                return true;
            }

            GLuint spatialfilter()
            {
                if(!rtdenoise || !rtspatialfilter || rtspecularspatial <= 0) return historytexture[historyindex];
                Shader *shader = useshaderbyname(msaalight ? "rtreflectionspatialms" : "rtreflectionspatial");
                if(!shader || shader->isnull()) return 0;
                timer *spatialtimer = begintimer("RT reflection spatial");
                static const int passcounts[] = { 0, 1, 3, 5 };
                int passes = passcounts[clamp(rtspecularspatial, 0, 3)];
                GLuint source = historytexture[historyindex];
                loopi(passes)
                {
                    GLuint destination = (i&1) ? upscaledtexture : filteredtexture;
                    if(!bindtarget(destination, resourcewidth, resourceheight, "spatial filter")) return 0;
                    shader->set();
                    setcommonparams();
                    LOCALPARAMF(rtSpatialParams, float(1<<i), 4.5f, 5.0f, rtdebug == 14 ? 1.0f : 0.0f);
                    bindgbuffer();
                    bindrectangle(GL_TEXTURE3, source);
                    glActiveTexture_(GL_TEXTURE0);
                    screenquad(resourcewidth, resourceheight);
                    source = destination;
                }
                endtimer(spatialtimer);
                return source;
            }

            bool compose(GLuint filtered)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, hdrfbo);
                glViewport(0, 0, resourcewidth, resourceheight);
                Shader *shader = useshaderbyname(msaalight ? "rtreflectioncompositems" : "rtreflectioncomposite");
                if(!shader || shader->isnull()) return false;
                timer *compositetimer = begintimer("RT reflection composition");
                shader->set();
                setcommonparams();
                LOCALPARAMF(rtDebugParams, float(rtdebug), rtmaxdistance, float(rtspecularhistory), 0.0f);
                bindgbuffer();
                bindrectangle(GL_TEXTURE3, filtered);
                bindrectangle(GL_TEXTURE4, rawtexture);
                bindrectangle(GL_TEXTURE5, auxiliarytexture);
                bindrectangle(GL_TEXTURE6, historytexture[historyindex]);
                bindrectangle(GL_TEXTURE7, historysurface[historyindex]);
                bindrectangle(GL_TEXTURE8, hitnormaltexture);
                bindrectangle(GL_TEXTURE9, historyvalidation[historyindex]);
                if(!rtdebug)
                {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ONE);
                }
                glActiveTexture_(GL_TEXTURE0);
                screenquad(resourcewidth, resourceheight);
                if(!rtdebug) glDisable(GL_BLEND);
                endtimer(compositetimer);
                return true;
            }
        }

        VARFP(profilert, 0, 0, 1, cleanupprofiler());
        VARFP(profilertdetail, 0, 0, 1, cleanupprofiler());
        VARFP(profilertwindow, 8, 120, PROFILE_MAX_SAMPLES, cleanupprofiler());
        VARFP(rtreflections, 0, 0, 1, settingschanged());
        FVARFP(rtresolutionscale, 0.25f, 0.5f, 1.0f, cleanup());
        VARFP(rtadaptive, 0, 1, 1, settingschanged());
        FVARFP(rtadaptivehigh, 0.05f, 0.45f, 1.0f, settingschanged());
        FVARFP(rtadaptivemedium, 0.005f, 0.025f, 1.0f, settingschanged());
        FVARFP(rtadaptivelow, 0.0f, 0.008f, 1.0f, settingschanged());
        FVARFP(rtmaxdistance, 1.0f, 2048.0f, 8192.0f, settingschanged());
        FVARFP(rtreflectionstrength, 0.0f, 2.0f, 8.0f, cleanupprofiler());
        FVARFP(rtreflectionroughnesspower, 0.25f, 3.0f, 8.0f, cleanupprofiler());
        VARFP(rtdenoise, 0, 1, 1, settingschanged());
        VARFP(rtspatialfilter, 0, 1, 1, settingschanged());
        VARFP(rtspecularhistory, 1, 24, 64, settingschanged());
        VARFP(rtspecularspatial, 0, 2, 3, settingschanged());
        VARFP(rtreconstruction, 0, 2, 2, settingschanged());
        // 0 composite; 1 raw; 2 temporal; 3 spatial; 4 validity; 5 hit distance; 6 ray direction; 7 hit normal;
        // 8 traversal cost; 9 roughness; 10 allocation; 11 final contribution; 12 history length; 13 Fresnel weight; 14 perfect mirrors.
        VARFP(rtdebug, 0, 0, 14, settingschanged());

        static void printstats()
        {
            conoutf("RT reflections: backend %d, requested scale %.2f, active scale %.2f, trace %dx%d, output %dx%d",
                    rtreflections, rtresolutionscale, resolutionscale(), tracewidth, traceheight, resourcewidth, resourceheight);
            conoutf("RT reflections: BVH generation %u, history %s, denoise %s, spatial filter %s, adaptive %s",
                    worldbvh::generation(), historyvalid ? "valid" : "invalid", rtdenoise ? "on" : "off",
                    rtspatialfilter ? "on" : "off", rtadaptive ? "on" : "off");
            conoutf("RT reflection sampling: deterministic sharp ray, reconstruction %d, history %d, spatial quality %d", rtreconstruction,
                    rtspecularhistory, rtspecularspatial);
            conoutf("RT debug %d (0 composite, 1 raw, 2 temporal, 3 spatial, 4 validity, 5 distance, 6 direction, 7 hit normal)", rtdebug);
            conoutf("RT debug continued: 8 traversal, 9 roughness, 10 allocation, 11 final, 12 history length, 13 Fresnel, 14 perfect mirrors");
            conoutf("RT reflection geometry: opaque world BVH; alpha/refract use material fallback; mapmodels are absent from the M03 BVH");
            conoutf("RT reflection materials: bindless texture sampling %s, up to 32 visible colored local lights",
                    hasBindlessTexture ? "available" : "unavailable");
        }
        COMMANDN(rtreflectionstats, printstats, "");

        static void printprofilebaseline()
        {
            pollprofiler();
            const char *gpu = (const char *)glGetString(GL_RENDERER), *vendor = (const char *)glGetString(GL_VENDOR);
            string msaadescription;
            if(latestprofileframe.msaa) formatstring(msaadescription, "%dx", latestprofileframe.msaa);
            else copystring(msaadescription, "off");
            float effectivescale = latestprofileframe.fullwidth > 0 ? float(latestprofileframe.tracewidth)/latestprofileframe.fullwidth : 0.0f;
            conoutf("RT PROFILE BASELINE: GPU %s (%s)", gpu ? gpu : "unknown", vendor ? vendor : "unknown");
            conoutf("RT PROFILE CONFIG: full %dx%d, trace %dx%d, requested/effective scale %.3f/%.3f, MSAA %s (%s RT input), BVH generation %u",
                    latestprofileframe.fullwidth, latestprofileframe.fullheight, latestprofileframe.tracewidth, latestprofileframe.traceheight,
                    resolutionscale(), effectivescale, msaadescription, latestprofileframe.multisampled ? "multisample" : "resolved",
                    latestprofileframe.bvhgeneration);
            conoutf("RT PROFILE SETTINGS: adaptive %s, denoise %s, spatial %s/%d, reconstruction %d, history %d", rtadaptive ? "on" : "off",
                    rtdenoise ? "on" : "off", rtspatialfilter ? "on" : "off", rtspecularspatial, rtreconstruction, rtspecularhistory);
            static const char *names[PROFILE_PASS_COUNT] = { "trace", "temporal", "spatial", "composite", "total" };
            loopi(PROFILE_PASS_COUNT)
            {
                const profilestat &stat = profilestats[i];
                if(stat.count) conoutf("RT PROFILE GPU %s: latest %.3f ms, average %.3f ms, minimum %.3f ms, maximum %.3f ms, samples %d",
                                      names[i], stat.latest, stat.average, stat.minimum, stat.maximum, stat.count);
                else conoutf("RT PROFILE GPU %s: n/a", names[i]);
            }
            const uint *counter = latestprofileframe.counters;
            double traversal = double(counter[12]) + double(counter[15])*4294967296.0;
            if(latestprofileframe.detailed)
            {
                conoutf("RT PROFILE BVH: rays %u, hits %u, misses %u, invalid %u, material-invalid %u, visited nodes %.0f, max nodes %u", counter[8],
                        counter[9], counter[10], counter[11], counter[14], traversal, counter[13]);
                conoutf("RT PROFILE SAMPLING: no-surface %u, material-rejected %u, classes 0/1/2/3 %u/%u/%u/%u, "
                        "class-skipped %u, schedule-skipped %u",
                        counter[0], counter[1], counter[2], counter[3], counter[4], counter[5], counter[6], counter[7]);
            }
            else conoutf("RT PROFILE WORKLOAD: unavailable; use /profilertdetail 1 for instrumented shader counters");
            conoutf("RT PROFILE SAMPLES: %u completed timing frames; counters use 32-bit atomics with a carry word for total node visits",
                    profilesamples);
        }
        COMMANDN(profilertbaseline, printprofilebaseline, "");

        void drawprofiler()
        {
            if(!profilert) return;
            pollprofiler();

            string lines[24];
            int linecount = 0, pending = 0;
            loopi(PROFILE_QUERY_COUNT) if(profilequerywaiting&(1<<i)) ++pending;
            formatstring(lines[linecount++], "Penteract RT profiler  samples %u  pending %d  window %d", profilesamples, pending, profilertwindow);
            bool supported = hasTQ && glQueryCounter_ && (!profilertdetail || (glBindBufferBase_ && glGetBufferSubData_));
            if(!supported)
                formatstring(lines[linecount++], "GPU timestamps/counter readback unavailable on this backend");
            else if(!profilesamples) formatstring(lines[linecount++], "Waiting for an active RT reflections frame");
            else formatstring(lines[linecount++], "GPU ms                 latest     avg     min     max    %%total");
            static const char *names[PROFILE_PASS_COUNT] = { "Trace", "Temporal", "Spatial", "Composite", "Total" };
            float total = profilestats[PROFILE_TOTAL].latest;
            loopi(PROFILE_PASS_COUNT)
            {
                const profilestat &stat = profilestats[i];
                if(stat.count)
                {
                    float percent = i == PROFILE_TOTAL ? 100.0f : (total > 0.0f ? 100.0f*stat.latest/total : 0.0f);
                    formatstring(lines[linecount++], "  %-18s %7.3f %7.3f %7.3f %7.3f %6.1f", names[i], stat.latest, stat.average,
                                 stat.minimum, stat.maximum, percent);
                }
                else formatstring(lines[linecount++], "  %-18s     n/a", names[i]);
            }

            const uint *counter = latestprofileframe.counters;
            uint totalpixels = uint(max(latestprofileframe.tracewidth, 0))*uint(max(latestprofileframe.traceheight, 0));
            uint eligible = counter[2] + counter[3] + counter[4] + counter[5];
            float effectivescale = latestprofileframe.fullwidth > 0 ? float(latestprofileframe.tracewidth)/latestprofileframe.fullwidth : 0.0f;
            double traversal = double(counter[12]) + double(counter[15])*4294967296.0;
            float hitratio = counter[8] ? 100.0f*counter[9]/counter[8] : 0.0f;
            float missratio = counter[8] ? 100.0f*counter[10]/counter[8] : 0.0f;
            float averageTraversal = counter[8] ? float(traversal/counter[8]) : 0.0f;
            float avoided = totalpixels ? 100.0f*(totalpixels - min(counter[8], totalpixels))/totalpixels : 0.0f;
            string msaadescription;
            if(latestprofileframe.msaa) formatstring(msaadescription, "%dx", latestprofileframe.msaa);
            else copystring(msaadescription, "off");
            if(latestprofileframe.detailed)
            {
                formatstring(lines[linecount++], "BVH workload");
                formatstring(lines[linecount++], "  rays %u  hits %u (%.1f%%)  misses %u (%.1f%%)  invalid %u", counter[8], counter[9], hitratio,
                             counter[10], missratio, counter[11]);
                formatstring(lines[linecount++], "  visited nodes avg %.1f  max %u  total %.0f  invalid material %u", averageTraversal, counter[13],
                             traversal, counter[14]);
                formatstring(lines[linecount++], "Adaptive sampling");
                formatstring(lines[linecount++], "  pixels %u  eligible %u  no surface %u  material rejected %u", totalpixels, eligible, counter[0],
                             counter[1]);
                formatstring(lines[linecount++], "  classes 0/1/2/3  %u / %u / %u / %u", counter[2], counter[3], counter[4], counter[5]);
                formatstring(lines[linecount++], "  class skipped %u  schedule skipped %u  traced %u  avoided %.1f%%", counter[6], counter[7],
                             counter[8], avoided);
            }
            else formatstring(lines[linecount++], "Workload counters off  (/profilertdetail 1 enables the instrumented trace shader)");
            formatstring(lines[linecount++], "Configuration");
            formatstring(lines[linecount++], "  full %dx%d  trace %dx%d  scale requested/effective %.3f/%.3f", latestprofileframe.fullwidth,
                         latestprofileframe.fullheight, latestprofileframe.tracewidth, latestprofileframe.traceheight, resolutionscale(),
                         effectivescale);
            formatstring(lines[linecount++], "  MSAA %s  RT input %s", msaadescription, latestprofileframe.multisampled ? "multisample" : "resolved");
            formatstring(lines[linecount++], "  adaptive %s  denoise %s  spatial %s/%d  reconstruction %d  history %d", rtadaptive ? "on" : "off",
                         rtdenoise ? "on" : "off", rtspatialfilter ? "on" : "off", rtspecularspatial, rtreconstruction, rtspecularhistory);
            formatstring(lines[linecount++], "  BVH generation %u  OpenGL backend  overhead: %s", latestprofileframe.bvhgeneration,
                         latestprofileframe.detailed ? "timestamps + shader atomics" : "timestamps only");

            float maximumwidth = 1.0f;
            loopi(linecount) maximumwidth = max(maximumwidth, text_widthf(lines[i]));
            float resolutionscale = min(hudw/1920.0f, hudh/1080.0f);
            float fitscale = min((hudw - 16.0f)/maximumwidth, (hudh - 16.0f)/(linecount*FONTH));
            float textscale = max(min(min(resolutionscale, fitscale), 2.0f), 0.1f);
            pushhudtranslate(8.0f, 8.0f, textscale);
            loopi(linecount) draw_text(lines[i], 0, i*FONTH);
            pophudmatrix();
        }

        bool active()
        {
            return rtreflections == 1 && pipelineavailable && glversion >= 430 && hasBindlessTexture && worldbvh::ready() && !drawtex && !mainmenu;
        }

        void render(GLuint hdrtexture, int width, int height)
        {
            (void)hdrtexture;
            if(!active() || width <= 0 || height <= 0)
            {
                historyvalid = false;
                return;
            }
            if(!prepareresources(width, height))
            {
                pipelineavailable = false;
                restorehdrtarget(width, height);
                return;
            }
            uint generation = worldbvh::generation();
            if(historygeneration != generation)
            {
                historyvalid = false;
                historygeneration = generation;
                cleanupprofiler();
            }
            beginprofiler(width, height);
            beginprofilerpass(PROFILE_TRACE);
            bool traced = tracereflections();
            endprofilerpass(PROFILE_TRACE);
            beginprofilerpass(PROFILE_TEMPORAL);
            bool accumulated = traced && temporalaccumulation();
            endprofilerpass(PROFILE_TEMPORAL);
            if(!traced || !accumulated)
            {
                endprofiler();
                historyvalid = false;
                pipelineavailable = false;
                restorehdrtarget(width, height);
                return;
            }
            bool hasspatial = rtdenoise && rtspatialfilter && rtspecularspatial > 0;
            if(hasspatial) beginprofilerpass(PROFILE_SPATIAL);
            GLuint filtered = spatialfilter();
            if(hasspatial) endprofilerpass(PROFILE_SPATIAL);
            beginprofilerpass(PROFILE_COMPOSITE);
            bool composed = filtered && compose(filtered);
            endprofilerpass(PROFILE_COMPOSITE);
            endprofiler();
            if(!filtered || !composed)
            {
                historyvalid = false;
                pipelineavailable = false;
                restorehdrtarget(width, height);
                return;
            }
            previousviewprojection = camprojmatrix;
            previousworldmatrix = worldmatrix;
            historyvalid = true;
            restorehdrtarget(width, height);
        }

        void resethistory()
        {
            historyvalid = false;
        }

        void cleanup()
        {
            GLuint textures[] = { rawtexture, auxiliarytexture, hitnormaltexture, historytexture[0], historytexture[1], historysurface[0],
                                  historysurface[1], historyvalidation[0], historyvalidation[1], filteredtexture, upscaledtexture };
            loopi(int(sizeof(textures)/sizeof(textures[0]))) if(textures[i]) glDeleteTextures(1, &textures[i]);
            rawtexture = auxiliarytexture = hitnormaltexture = historytexture[0] = historytexture[1] = historysurface[0] = historysurface[1] = 0;
            historyvalidation[0] = historyvalidation[1] = 0;
            filteredtexture = upscaledtexture = 0;
            if(reflectionfbo) { glDeleteFramebuffers_(1, &reflectionfbo); reflectionfbo = 0; }
            resourcewidth = resourceheight = -1;
            tracewidth = traceheight = 0;
            historyindex = 0;
            historyvalid = false;
            historygeneration = 0;
            pipelineavailable = true;
            cleanupprofiler();
        }
    }
}
