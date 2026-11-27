/*
 * Candela-derived volumetric scattering backend for Tesseract.
 *
 * The ray integration, phase-function, temporal accumulation and edge-aware
 * reconstruction are adapted from Candela by Samuel Rasquinha (@swr06).
 * Candela is distributed under GPL-3.0; see candela/LICENSE. This port was
 * rewritten for Tesseract's Z-up world, G-buffer, CSM atlas, Radiance Hints
 * and HDR pipeline and does not include Candela's renderer or probe system.
 */

#include "engine.h"
#include "volumetrics.h"

namespace renderer
{
    namespace volumetrics
    {
        extern int _sunvolcolour;
        extern bvec sunvolcolour;
        extern float volstrength, voldensity, volextinction, voldirectstrength, volindirectstrength, vollocalstrength, volmaxdistance,
                     volheight, volthickness, volskydistance;
        extern int volsteps, voluniformdensity, volresolution, voltemporal, volspatial, voldebug;

        namespace
        {
            GLuint volumetricfbo = 0, rawtexture = 0, rawdebugtexture = 0, historytexture[2] = { 0, 0 },
                   historydepth[2] = { 0, 0 }, historyvalidity[2] = { 0, 0 }, filteredtexture = 0, compositetexture = 0;
            int resourcewidth = -1, resourceheight = -1, volumetricwidth = 0, volumetricheight = 0, historyindex = 0,
                previouscsmcount = -1, previousrhcount = -1;
            bool historyvalid = false, pipelineavailable = true;
            uint frameindex = 0;
            matrix4 previousviewprojection, previousworldmatrix;
            vec previouscamera(0, 0, 0), previoussun(0, 0, 0), previoussuncolor(0, 0, 0), previousambient(0, 0, 0);
            locallight previousLocalLights[16];
            int previousLocalLightCount = -1;
            float previousfovy = -1.0f;

            void settingschanged()
            {
                historyvalid = false;
            }

            int resolutionshift()
            {
                return clamp(volresolution, 0, 2);
            }

            void allocaterectangle(GLuint &texture, int width, int height, GLenum format, bool linear)
            {
                if(!texture) glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_RECTANGLE, texture);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                GLenum component = format == GL_R16F || format == GL_R32F ? GL_RED : GL_RGBA;
                glTexImage2D(GL_TEXTURE_RECTANGLE, 0, format, width, height, 0, component, GL_FLOAT, NULL);
            }

            bool bindtarget(GLuint color, int width, int height, const char *label)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, volumetricfbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, color, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_RECTANGLE, 0, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_RECTANGLE, 0, 0);
                GLenum drawbuffer = GL_COLOR_ATTACHMENT0;
                glDrawBuffers_(1, &drawbuffer);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                {
                    conoutf(CON_ERROR, "volumetrics: incomplete %s framebuffer", label);
                    return false;
                }
                glViewport(0, 0, width, height);
                return true;
            }

            bool bindrawtargets()
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, volumetricfbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rawtexture, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_RECTANGLE, rawdebugtexture, 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_RECTANGLE, 0, 0);
                const GLenum drawbuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
                glDrawBuffers_(2, drawbuffers);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                {
                    conoutf(CON_ERROR, "volumetrics: incomplete raymarch framebuffer");
                    return false;
                }
                glViewport(0, 0, volumetricwidth, volumetricheight);
                return true;
            }

            bool bindhistorytargets(int destination)
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, volumetricfbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, historytexture[destination], 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_RECTANGLE, historydepth[destination], 0);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_RECTANGLE, historyvalidity[destination], 0);
                const GLenum drawbuffers[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
                glDrawBuffers_(3, drawbuffers);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                {
                    conoutf(CON_ERROR, "volumetrics: incomplete temporal framebuffer");
                    return false;
                }
                glViewport(0, 0, volumetricwidth, volumetricheight);
                return true;
            }

            void bindrectangle(GLenum unit, GLuint texture)
            {
                glActiveTexture_(unit);
                glBindTexture(GL_TEXTURE_RECTANGLE, texture);
            }

            void restorehdrtarget()
            {
                glBindFramebuffer_(GL_FRAMEBUFFER, msaalight ? mshdrfbo : hdrfbo);
                glViewport(0, 0, vieww, viewh);
                glActiveTexture_(GL_TEXTURE0);
            }

            bool prepareresources()
            {
                int shift = resolutionshift(), width = max((vieww + (1<<shift) - 1)>>shift, 1),
                    height = max((viewh + (1<<shift) - 1)>>shift, 1);
                if(volumetricfbo && resourcewidth == vieww && resourceheight == viewh && volumetricwidth == width && volumetricheight == height)
                    return true;

                cleanup();
                resourcewidth = vieww;
                resourceheight = viewh;
                volumetricwidth = width;
                volumetricheight = height;
                GLenum volumeformat = hasAFBO && hasTF ? GL_RGBA16F : GL_RGBA8;
                GLenum depthformat = hasAFBO && hasTF && hasTRG ? GL_R32F : volumeformat;
                glGenFramebuffers_(1, &volumetricfbo);
                allocaterectangle(rawtexture, width, height, volumeformat, false);
                allocaterectangle(rawdebugtexture, width, height, volumeformat, false);
                loopi(2)
                {
                    allocaterectangle(historytexture[i], width, height, volumeformat, true);
                    allocaterectangle(historydepth[i], width, height, depthformat, false);
                    allocaterectangle(historyvalidity[i], width, height, volumeformat, false);
                }
                allocaterectangle(filteredtexture, width, height, volumeformat, true);
                allocaterectangle(compositetexture, vieww, viewh, hdrformat, false);
                if(!bindrawtargets() || !bindhistorytargets(0) || !bindtarget(filteredtexture, width, height, "spatial") ||
                   !bindtarget(compositetexture, vieww, viewh, "HDR composition"))
                {
                    cleanup();
                    return false;
                }
                glBindTexture(GL_TEXTURE_RECTANGLE, 0);
                glBindFramebuffer_(GL_FRAMEBUFFER, 0);
                historyindex = 0;
                historyvalid = false;
                previousviewprojection = camprojmatrix;
                previousworldmatrix = linearworldmatrix;
                previouscamera = camera1->o;
                previousfovy = fovy;
                return true;
            }

            void binddepth(GLenum unit)
            {
                // Tesseract has no late transparent-depth attachment. The
                // explicit fallback is opaque G-buffer depth: transparent
                // color is fogged to the opaque surface behind it, never by
                // sampling a nonexistent texture.
                glActiveTexture_(unit);
                glBindTexture(msaalight ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_RECTANGLE, msaalight ? msdepthtex : gdepthtex);
            }

            bool renderraymarch(int csmcount, int rhcount, const locallight *localLights, int localLightCount)
            {
                if(!bindrawtargets()) return false;
                Shader *shader = useshaderbyname(msaalight ? (shadowatlastarget == GL_TEXTURE_2D ? "cvolraymarchms2d" : "cvolraymarchmsrect") :
                                                             (shadowatlastarget == GL_TEXTURE_2D ? "cvolraymarch2d" : "cvolraymarchrect"));
                if(!shader || shader->isnull()) return false;
                timer *timer = begintimer("volumetric rendering");
                shader->set();
                LOCALPARAM(worldmatrix, worldmatrix);
                LOCALPARAM(camera, camera1->o);
                binddepth(GL_TEXTURE0);
                glActiveTexture_(GL_TEXTURE1);
                glBindTexture(msaalight ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_RECTANGLE, msaalight ? msnormaltex : gnormaltex);
                bindvolumetriclighting(2, 3, 7, csmcount, rhcount);
                LOCALPARAMF(cvolFullSize, float(vieww), float(viewh), 1.0f/max(vieww, 1), 1.0f/max(viewh, 1));
                LOCALPARAMF(cvolLowSize, float(volumetricwidth), float(volumetricheight), float(vieww)/volumetricwidth,
                            float(viewh)/volumetricheight);
                LOCALPARAMF(cvolMedium, voldensity, volextinction, volstrength, voluniformdensity ? 1.0f : 0.0f);
                LOCALPARAMF(cvolLayer, volheight, max(volthickness, 1.0f), volmaxdistance, float(volsteps));
                LOCALPARAMF(cvolSky, volskydistance, 0.0f, 0.0f, 0.0f);
                LOCALPARAMF(cvolLighting, voldirectstrength, volindirectstrength, float(csmcount), float(rhcount));
                LOCALPARAMF(cvolLocalParams, vollocalstrength, 0.0f, 0.0f, 0.0f);
                LOCALPARAMI(cvolLocalLightCount, localLightCount);
                if(localLightCount)
                {
                    vec4 positionRadius[16], color[16], secondary[16], directionCone[16], attenuation[16], shadowParams[16],
                         shadowOffsetFlags[16];
                    loopi(localLightCount)
                    {
                        positionRadius[i] = localLights[i].positionRadius;
                        color[i] = localLights[i].color;
                        secondary[i] = localLights[i].secondary;
                        directionCone[i] = localLights[i].directionCone;
                        attenuation[i] = localLights[i].attenuation;
                        shadowParams[i] = localLights[i].shadowParams;
                        shadowOffsetFlags[i] = localLights[i].shadowOffsetFlags;
                    }
                    LOCALPARAMV(cvolLocalLightPositionRadius, positionRadius, localLightCount);
                    LOCALPARAMV(cvolLocalLightColor, color, localLightCount);
                    LOCALPARAMV(cvolLocalLightSecondary, secondary, localLightCount);
                    LOCALPARAMV(cvolLocalLightDirectionCone, directionCone, localLightCount);
                    LOCALPARAMV(cvolLocalLightAttenuation, attenuation, localLightCount);
                    LOCALPARAMV(cvolLocalLightShadowParams, shadowParams, localLightCount);
                    LOCALPARAMV(cvolLocalLightShadowOffsetFlags, shadowOffsetFlags, localLightCount);
                }
                LOCALPARAMI(cvolFrame, frameindex++);
                LOCALPARAM(cvolSunDirection, getdirectionallightdir());
                const bvec &sun = _sunvolcolour ? sunvolcolour : getdirectionallightcolor();
                float sunscale = 2.0f*ldrscaleb*getdirectionallightscale()*getdirectionallightvisibility();
                LOCALPARAMF(cvolSunColor, sun.x*sunscale, sun.y*sunscale, sun.z*sunscale);
                screenquad(vieww, viewh);
                endtimer(timer);
                return true;
            }

            bool temporalaccumulation()
            {
                int destination = 1 - historyindex;
                if(!bindhistorytargets(destination)) return false;
                Shader *shader = useshaderbyname("cvoltemporal");
                if(!shader || shader->isnull()) return false;
                timer *timer = begintimer("volumetric temporal");
                shader->set();
                LOCALPARAM(linearworldmatrix, linearworldmatrix);
                LOCALPARAM(camera, camera1->o);
                LOCALPARAM(cvolPreviousViewProjection, previousviewprojection);
                LOCALPARAM(cvolPreviousWorldMatrix, previousworldmatrix);
                LOCALPARAMF(cvolLowSize, float(volumetricwidth), float(volumetricheight), float(vieww)/volumetricwidth,
                            float(viewh)/volumetricheight);
                LOCALPARAMF(cvolTemporalParams, historyvalid && voltemporal ? 1.0f : 0.0f, 0.90f, float(volumetricwidth),
                            float(volumetricheight));
                bindrectangle(GL_TEXTURE0, rawtexture);
                bindrectangle(GL_TEXTURE1, rawdebugtexture);
                bindrectangle(GL_TEXTURE2, historytexture[historyindex]);
                bindrectangle(GL_TEXTURE3, historydepth[historyindex]);
                glActiveTexture_(GL_TEXTURE0);
                screenquad();
                endtimer(timer);
                historyindex = destination;
                return true;
            }

            GLuint spatialfilter()
            {
                if(!volspatial) return historytexture[historyindex];
                if(!bindtarget(filteredtexture, volumetricwidth, volumetricheight, "spatial filter")) return 0;
                Shader *shader = useshaderbyname("cvolspatial");
                if(!shader || shader->isnull()) return 0;
                timer *timer = begintimer("volumetric filtering");
                shader->set();
                LOCALPARAMF(cvolFilterParams, float(volumetricwidth), float(volumetricheight), 0.02f, 8.0f);
                bindrectangle(GL_TEXTURE0, historytexture[historyindex]);
                bindrectangle(GL_TEXTURE1, rawdebugtexture);
                glActiveTexture_(GL_TEXTURE0);
                screenquad();
                endtimer(timer);
                return filteredtexture;
            }

            bool compose(GLuint volume)
            {
                if(!bindtarget(compositetexture, vieww, viewh, "HDR composition")) return false;
                Shader *shader = useshaderbyname(msaalight ? "cvolcompositems" : "cvolcomposite");
                Shader *copyshader = useshaderbyname("cvolcopy");
                if(!shader || shader->isnull() || !copyshader || copyshader->isnull()) return false;
                timer *timer = begintimer("volumetric composition");
                shader->set();
                binddepth(GL_TEXTURE0);
                glActiveTexture_(GL_TEXTURE1);
                glBindTexture(msaalight ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_RECTANGLE, msaalight ? mshdrtex : hdrtex);
                bindrectangle(GL_TEXTURE2, volume);
                bindrectangle(GL_TEXTURE3, rawtexture);
                bindrectangle(GL_TEXTURE4, rawdebugtexture);
                bindrectangle(GL_TEXTURE5, historyvalidity[historyindex]);
                LOCALPARAMF(cvolCompositeScale, float(volumetricwidth), float(volumetricheight), float(volumetricwidth)/vieww,
                            float(volumetricheight)/viewh);
                LOCALPARAMF(cvolCompositeParams, volmaxdistance, 0.0f, 0.0f, 0.0f);
                LOCALPARAMI(cvolDebug, voldebug);
                glActiveTexture_(GL_TEXTURE0);
                screenquad();

                restorehdrtarget();
                copyshader->set();
                bindrectangle(GL_TEXTURE0, compositetexture);
                screenquad();
                endtimer(timer);
                return true;
            }

            bool historydiscontinuity(const vec &sun, const vec &suncolor, const vec &ambientcolor, int csmcount, int rhcount)
            {
                float teleportdistance = max(64.0f, volmaxdistance*0.25f);
                return previousfovy < 0.0f || camera1->o.dist(previouscamera) > teleportdistance || fabsf(fovy - previousfovy) > 0.01f ||
                       (!previoussun.iszero() && sun.dot(previoussun) < 0.9995f) || suncolor.dist(previoussuncolor) > 0.05f ||
                       ambientcolor.dist(previousambient) > 0.05f || csmcount != previouscsmcount || rhcount != previousrhcount;
            }
        }

        FVARFP(volstrength, 0.0f, 1.0f, 8.0f, settingschanged());
        FVARFP(voldensity, 0.0f, 0.35f, 8.0f, settingschanged());
        FVARFP(volextinction, 0.0f, 0.0025f, 0.1f, settingschanged());
        FVARFP(voldirectstrength, 0.0f, 2.0f, 32.0f, settingschanged());
        FVARFP(volindirectstrength, 0.0f, 1.0f, 32.0f, settingschanged());
        FVARFP(vollocalstrength, 0.0f, 8.0f, 32.0f, settingschanged());
        VARFP(volsteps, 4, 8, 128, settingschanged());
        FVARFP(volmaxdistance, 1.0f, 1536.0f, 65536.0f, settingschanged());
        FVARFP(volheight, -65536.0f, 256.0f, 65536.0f, settingschanged());
        FVARFP(volthickness, 1.0f, 512.0f, 65536.0f, settingschanged());
        // Fraction of volmaxdistance marched through background pixels. This is
        // map-persistent so outdoor maps can retain strong ground-level shafts
        // without washing out their sky, atmosphere or cloud layers.
        FVARFR(volskydistance, 0.0f, 0.35f, 1.0f, settingschanged());
        VARFP(voluniformdensity, 0, 0, 1, settingschanged());
        CVARFR(sunvolcolour, 0, settingschanged());
        // 0 = full, 1 = half on each axis, 2 = quarter on each axis.
        VARFP(volresolution, 0, 0, 2, cleanupvolumetric());
        VARFP(voltemporal, 0, 1, 1, settingschanged());
        VARFP(volspatial, 0, 1, 1, settingschanged());
        // 0 final; 1 filtered volume; 2 raw scattering; 3 transmittance;
        // 4 density; 5 sun visibility; 6 CSM cascade; 7 temporal rejection.
        VARFP(voldebug, 0, 0, 7, settingschanged());

        bool render()
        {
            if(!pipelineavailable || drawtex || vieww <= 0 || viewh <= 0 || volstrength <= 0.0f || voldensity <= 0.0f ||
               volmaxdistance <= 0.0f || volsteps <= 0)
                return true;
            if(!prepareresources())
            {
                cleanup();
                pipelineavailable = false;
                restorehdrtarget();
                conoutf(CON_ERROR, "volumetrics unavailable");
                return false;
            }

            vec sun = getdirectionallightdir();
            const bvec &sunbytes = _sunvolcolour ? sunvolcolour : getdirectionallightcolor();
            vec suncolor = sunbytes.tocolor().mul(getdirectionallightscale()*getdirectionallightvisibility());
            vec ambientcolor = skylight.tocolor().mul(skylightscale);
            int csmcount = 0, rhcount = 0;
            static const int maximumLocalLights = 16;
            locallight localLights[maximumLocalLights];
            int localLightCount = vollocalstrength > 0.0f ? collectlocallights(localLights, maximumLocalLights) : 0;
            bindvolumetriclighting(2, 3, 7, csmcount, rhcount);
            if(historydiscontinuity(sun, suncolor, ambientcolor, csmcount, rhcount)) historyvalid = false;
            if(localLightCount != previousLocalLightCount ||
               (localLightCount && memcmp(localLights, previousLocalLights, localLightCount*sizeof(locallight))))
                historyvalid = false;

            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glDepthMask(GL_FALSE);
            if(!renderraymarch(csmcount, rhcount, localLights, localLightCount) || !temporalaccumulation())
            {
                cleanup();
                pipelineavailable = false;
                restorehdrtarget();
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
                conoutf(CON_ERROR, "volumetric shader unavailable");
                return false;
            }
            GLuint filtered = spatialfilter();
            if(!filtered || !compose(filtered))
            {
                cleanup();
                pipelineavailable = false;
                restorehdrtarget();
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
                conoutf(CON_ERROR, "volumetric composition failed");
                return false;
            }

            previousviewprojection = camprojmatrix;
            previousworldmatrix = linearworldmatrix;
            previouscamera = camera1->o;
            previousfovy = fovy;
            previoussun = sun;
            previoussuncolor = suncolor;
            previousambient = ambientcolor;
            previouscsmcount = csmcount;
            previousrhcount = rhcount;
            previousLocalLightCount = localLightCount;
            if(localLightCount) memcpy(previousLocalLights, localLights, localLightCount*sizeof(locallight));
            historyvalid = true;
            restorehdrtarget();
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            return true;
        }

        void resethistory()
        {
            historyvalid = false;
        }

        void cleanup()
        {
            GLuint textures[] = { rawtexture, rawdebugtexture, historytexture[0], historytexture[1], historydepth[0], historydepth[1],
                                  historyvalidity[0], historyvalidity[1], filteredtexture, compositetexture };
            loopi(int(sizeof(textures)/sizeof(textures[0]))) if(textures[i]) glDeleteTextures(1, &textures[i]);
            rawtexture = rawdebugtexture = historytexture[0] = historytexture[1] = historydepth[0] = historydepth[1] = 0;
            historyvalidity[0] = historyvalidity[1] = filteredtexture = compositetexture = 0;
            if(volumetricfbo) { glDeleteFramebuffers_(1, &volumetricfbo); volumetricfbo = 0; }
            resourcewidth = resourceheight = -1;
            volumetricwidth = volumetricheight = 0;
            historyindex = 0;
            historyvalid = false;
            previousfovy = -1.0f;
            previouscsmcount = previousrhcount = -1;
            previousLocalLightCount = -1;
            pipelineavailable = true;
        }
    }
}
