#include "tesselation.h"

PFNGLPATCHPARAMETERIPROC glPatchParameteri_ = NULL;

VAR(tesselationsupported, 1, 0, 0);
VAR(tesselationmaxlevel, 1, 0, 0);
VAR(tesselationmaxpatchvertices, 1, 0, 0);
VARP(tesselation, 0, 0, 1);
VARFP(tesselationfixed, 1, 4, 64,
{
    if(tesselationmaxlevel > 0) tesselationfixed = min(tesselationfixed, tesselationmaxlevel);
});
FVARP(tesselationscale, 0.0f, 1.0f, 16.0f);
FVARP(tesselationheightlod, 0.0f, 0.0f, 12.0f);
VARP(tesselationdisplacement, 0, 1, 1);
// 0 = off, 1 = wireframe, 2 = sampled height, 3 = normalized displacement magnitude
VARP(tesselationdebug, 0, 0, 3);

extern void *getprocaddress(const char *name);
extern bool hasext(const char *ext);

namespace renderer
{
    namespace tesselation
    {
        static Shader *lastworldbase = NULL, *lastworldshader = NULL;
        static GLint previouspatchvertices = 3, previouspolygonmode[2] = { GL_FILL, GL_FILL };
        static bool worldactive = false, wireframeactive = false;

        void init()
        {
            tesselationsupported = 0;
            tesselationmaxlevel = 0;
            tesselationmaxpatchvertices = 0;
            glPatchParameteri_ = NULL;

            const bool advertised = glversion >= 400 || (glslversion >= 150 && hasext("GL_ARB_tessellation_shader"));
            if(advertised) glPatchParameteri_ = (PFNGLPATCHPARAMETERIPROC)getprocaddress("glPatchParameteri");
            if(glPatchParameteri_)
            {
                glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &tesselationmaxlevel);
                glGetIntegerv(GL_MAX_PATCH_VERTICES, &tesselationmaxpatchvertices);
                tesselationsupported = tesselationmaxlevel > 0 && tesselationmaxpatchvertices >= 3;
            }

            if(!tesselationsupported)
            {
                tesselationmaxlevel = tesselationmaxpatchvertices = 0;
                conoutf(CON_INIT, "Hardware tessellation: unavailable");
                return;
            }

            tesselationfixed = clamp(tesselationfixed, 1, tesselationmaxlevel);
            conoutf(CON_INIT, "Hardware tessellation: available (max level %d, max patch vertices %d)", tesselationmaxlevel,
                    tesselationmaxpatchvertices);
        }

        void cleanup()
        {
            lastworldbase = lastworldshader = NULL;
            worldactive = wireframeactive = false;
        }

        bool enabled()
        {
            return ::tesselation && tesselationsupported && glPatchParameteri_ && !drawtex;
        }

        bool displacing()
        {
            return enabled() && tesselationdisplacement && tesselationscale > 0.0f;
        }

        static bool hasparam(const Shader &shader, const char *name)
        {
            loopv(shader.defaultparams) if(shader.defaultparams[i].name && !strcmp(shader.defaultparams[i].name, name)) return true;
            return false;
        }

        static bool hasheightmap(const Slot &slot)
        {
            if((slot.texmask&((1<<TEX_NORMAL)|(1<<TEX_DEPTH))) != ((1<<TEX_NORMAL)|(1<<TEX_DEPTH))) return false;
            loopv(slot.sts) if(slot.sts[i].type == TEX_NORMAL && slot.sts[i].t && slot.sts[i].t != notexture) return true;
            return false;
        }

        Shader *worldshader(Slot &slot)
        {
            Shader *base = slot.shader;
            if(!enabled() || !base || !hasheightmap(slot) || !hasparam(*base, "parallaxscale")) return NULL;
            if(base == lastworldbase && lastworldshader && lastworldshader->loaded()) return lastworldshader;

            defformatstring(name, "tess_%s", base->name);
            lastworldbase = base;
            Shader *shader = useshaderbyname(name);
            lastworldshader = shader && shader->loaded() ? shader : NULL;
            return lastworldshader;
        }

        void beginworld()
        {
            if(worldactive || !enabled()) return;
            glGetIntegerv(GL_PATCH_VERTICES, &previouspatchvertices);
            glPatchParameteri_(GL_PATCH_VERTICES, 3);
            worldactive = true;
        }

        void endworld()
        {
            if(!worldactive) return;
            glPatchParameteri_(GL_PATCH_VERTICES, previouspatchvertices);
            worldactive = false;
        }

        void setparams(Slot &slot, VSlot &vslot)
        {
            LOCALPARAMF(tessfactor, float(clamp(tesselationfixed, 1, tesselationmaxlevel)));

            Texture *diffuse = NULL;
            loopv(slot.sts) if(slot.sts[i].type == TEX_DIFFUSE)
            {
                diffuse = slot.sts[i].t;
                break;
            }
            float worldperuv = diffuse && diffuse != notexture ? vslot.scale*(diffuse->xs + diffuse->ys)/(2.0f*TEX_SCALE) : 0.0f;
            bool active = displacing() && worldperuv > 0.0f;
            LOCALPARAMF(tessparams, active ? worldperuv*tesselationscale : 0.0f, float(tesselationdebug), tesselationheightlod,
                        active ? 1.0f : 0.0f);
        }

        bool beginwireframe()
        {
            return false;
        }

        void endwireframe()
        {
            if(!wireframeactive) return;
            glPolygonMode(GL_FRONT, previouspolygonmode[0]);
            glPolygonMode(GL_BACK, previouspolygonmode[1]);
            wireframeactive = false;
        }
    }
}
