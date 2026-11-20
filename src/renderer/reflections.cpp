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

        namespace
        {
            GLuint reflectionfbo = 0, rawtexture = 0, auxiliarytexture = 0, hitnormaltexture = 0, historytexture[2] = { 0, 0 },
                   historysurface[2] = { 0, 0 }, historyvalidation[2] = { 0, 0 }, filteredtexture = 0, upscaledtexture = 0;
            int resourcewidth = -1, resourceheight = -1, tracewidth = 0, traceheight = 0, historyindex = 0;
            bool historyvalid = false;
            bool pipelineavailable = true;
            uint historygeneration = 0;
            matrix4 previousviewprojection, previousworldmatrix;

            void settingschanged()
            {
                historyvalid = false;
                pipelineavailable = true;
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

            bool bindtracetargets(int width, int height)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, reflectionfbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rawtexture, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_RECTANGLE, auxiliarytexture, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_RECTANGLE, hitnormaltexture, 0);
                const GLenum drawbuffers[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
                glDrawBuffers_(3, drawbuffers);
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
                if(!bindtracetargets(tracewidth, traceheight) ||
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
                if(!bindtracetargets(tracewidth, traceheight)) return false;
                Shader *shader = useshaderbyname(msaalight ? "rtreflectiontracems" : "rtreflectiontrace");
                if(!shader || shader->isnull()) return false;
                timer *reflectiontimer = begintimer("RT reflection tracing");
                shader->set();
                setcommonparams();
                LOCALPARAMF(rtTraceParams, rtmaxdistance, 1.0f/65536.0f, float(worldbvh::root()), 0.0f);
                LOCALPARAMF(rtTraceOptions, float(rtadaptive), float(worldbvh::materialcount()), hastexturedskybox() ? 1.0f : 0.0f,
                            getskyboxrotation());
                LOCALPARAMF(rtAdaptiveParams, rtadaptivehigh, min(rtadaptivemedium, rtadaptivehigh),
                            min(rtadaptivelow, min(rtadaptivemedium, rtadaptivehigh)), 0.0f);
                vec direction = getdirectionallightdir();
                const bvec &sun = getdirectionallightcolor();
                float sunscale = 2.0f*ldrscaleb*getdirectionallightscale()*getdirectionallightvisibility();
                LOCALPARAM(rtSunDirection, direction);
                LOCALPARAMF(rtSunColor, sun.x*sunscale, sun.y*sunscale, sun.z*sunscale);
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
                LOCALPARAMF(rtTemporalParams, historyvalid && rtdenoise ? 1.0f : 0.0f, float(rtspecularhistory), float(rtreconstruction), 0.0f);
                bindgbuffer();
                bindrectangle(GL_TEXTURE3, rawtexture);
                bindrectangle(GL_TEXTURE4, auxiliarytexture);
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
                    LOCALPARAMF(rtSpatialParams, float(1<<i), 4.5f, 5.0f, 0.0f);
                    bindgbuffer();
                    bindrectangle(GL_TEXTURE3, source);
                    bindrectangle(GL_TEXTURE4, historyvalidation[historyindex]);
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

        VARFP(rtreflections, 0, 0, 1, settingschanged());
        FVARFP(rtresolutionscale, 0.25f, 1.0f, 1.0f, cleanup());
        VARFP(rtadaptive, 0, 1, 1, settingschanged());
        FVARFP(rtadaptivehigh, 0.05f, 0.45f, 1.0f, settingschanged());
        FVARFP(rtadaptivemedium, 0.005f, 0.025f, 1.0f, settingschanged());
        FVARFP(rtadaptivelow, 0.0f, 0.008f, 1.0f, settingschanged());
        FVARFP(rtmaxdistance, 1.0f, 4096.0f, 8192.0f, settingschanged());
        FVARP(rtreflectionstrength, 0.0f, 2.0f, 8.0f);
        FVARP(rtreflectionroughnesspower, 0.25f, 3.0f, 8.0f);
        VARFP(rtdenoise, 0, 1, 1, settingschanged());
        VARFP(rtspatialfilter, 0, 1, 1, settingschanged());
        VARFP(rtspecularhistory, 1, 24, 64, settingschanged());
        VARFP(rtspecularspatial, 0, 2, 3, settingschanged());
        VARFP(rtreconstruction, 0, 2, 2, settingschanged());
        // 0 composite; 1 raw; 2 temporal; 3 spatial; 4 validity; 5 hit distance; 6 ray direction; 7 hit normal;
        // 8 traversal cost; 9 roughness; 10 allocation; 11 final contribution; 12 history length; 13 Fresnel weight.
        VARP(rtdebug, 0, 0, 13);

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
            conoutf("RT debug continued: 8 traversal, 9 roughness, 10 allocation, 11 final, 12 history length, 13 Fresnel");
            conoutf("RT reflection geometry: opaque world BVH; alpha/refract use material fallback; mapmodels are absent from the M03 BVH");
        }
        COMMANDN(rtreflectionstats, printstats, "");

        bool active()
        {
            return rtreflections == 1 && pipelineavailable && glversion >= 430 && worldbvh::ready() && !drawtex && !mainmenu;
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
            }
            if(!tracereflections() || !temporalaccumulation())
            {
                historyvalid = false;
                pipelineavailable = false;
                restorehdrtarget(width, height);
                return;
            }
            GLuint filtered = spatialfilter();
            if(!filtered || !compose(filtered))
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
        }
    }
}
