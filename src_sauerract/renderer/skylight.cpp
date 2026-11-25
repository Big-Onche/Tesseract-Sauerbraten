#include "engine.h"
#include "skylight.h"

namespace renderer
{
    namespace skylight
    {
        static int visibilitywidth = -1, visibilityheight = -1, visibilitydepth = -1, visibilityworldsize = -1;
        static GLuint visibilitytexture = 0;
        static vector<uchar> visibilitypixels;
        static bool visibilitydirty = false;
        static ivec visibilitydirtymin(0, 0, 0), visibilitydirtymax(0, 0, 0);

        static void reset();

        FVARR(skycubelight, 0, 1.75f, 4.0f);
        FVARR(skycubelightmin, 0, 0.05f, 1.0f);
        VARFP(skycubeocclusion, 0, 1, 1, reset());
        VARFP(skycubeocclreduce, 0, 4, 6, reset());
        FVARFP(skycubeoccloffset, 0, 2.0f, 64.0f, reset());
        FVARFP(skycubeocclmaxdist, 1, 65536.0f, 1e7f, reset());
        FVARFP(skycubeocclsoft, 0, 256.0f, 1e6f, reset());
        VARP(skycubeocclrate, 1, 1, 8);
        VARP(skycubeocclbatch, 1, 8, 1024);
        FVARP(skycubeocclblend, 0, 0.35f, 1.0f);
        ICOMMAND(rebuildskylightmap, "", (), { reset(); setup(); });

        static inline int visibilityresolution()
        {
            int mapsize = max(worldsize, 1);
            return clamp(max(mapsize >> skycubeocclreduce, 4), 4, 64);
        }

        void invalidate()
        {
            if(worldsize <= 0) return;
            visibilitydirty = true;
            visibilitydirtymin = ivec(0, 0, 0);
            visibilitydirtymax = ivec(worldsize, worldsize, worldsize);
        }

        void invalidate(const ivec &bbmin, const ivec &bbmax)
        {
            if(worldsize <= 0) return;
            ivec dirtymin = bbmin, dirtymax = bbmax;
            dirtymin.max(0);
            dirtymax.min(worldsize);
            if(dirtymin.x >= dirtymax.x || dirtymin.y >= dirtymax.y || dirtymin.z >= dirtymax.z) return;
            if(!visibilitydirty)
            {
                visibilitydirty = true;
                visibilitydirtymin = dirtymin;
                visibilitydirtymax = dirtymax;
            }
            else
            {
                visibilitydirtymin.min(dirtymin);
                visibilitydirtymax.max(dirtymax);
            }
        }

        static inline uchar computesample(int x, int y, int z, float cell, float startoffset, float maxdist, float soft, const vec &up)
        {
            vec start((x + 0.5f)*cell, (y + 0.5f)*cell, (z + 0.5f)*cell);
            if(startoffset > 0.0f)
            {
                if(raycube(start, up, startoffset, RAY_CLIPMAT|RAY_POLY|RAY_SKIPFIRST) < startoffset) return 0;
                start.madd(up, startoffset);
            }
            float updist = raycube(start, up, maxdist, RAY_CLIPMAT|RAY_POLY|RAY_SKIPFIRST);
            float visibility = soft <= 1e-3f ? (updist >= maxdist ? 1.0f : 0.0f) : clamp((updist - (maxdist - soft))/soft, 0.0f, 1.0f);
            return uchar(clamp(int(visibility*255.0f + 0.5f), 0, 255));
        }

        static void build()
        {
            int resolution = visibilityresolution();
            if(resolution == visibilitywidth && resolution == visibilityheight && resolution == visibilitydepth && visibilitytexture &&
               visibilityworldsize == worldsize) return;

            visibilitywidth = visibilityheight = visibilitydepth = resolution;
            int total = visibilitywidth*visibilityheight*visibilitydepth;
            visibilitypixels.setsize(0);
            visibilitypixels.reserve(total);
            visibilitypixels.advance(total);

            if(!skycubeocclusion) memset(visibilitypixels.getbuf(), 255, total);
            else
            {
                float maxdist = max(skycubeocclmaxdist, 1.0f), startoffset = max(skycubeoccloffset, 0.0f), soft = max(skycubeocclsoft, 0.0f);
                vec up(0, 0, 1);
                float cell = float(max(worldsize, 1))/float(resolution);
                loop(z, visibilitydepth) loop(y, visibilityheight) loop(x, visibilitywidth)
                {
                    visibilitypixels[(z*visibilityheight + y)*visibilitywidth + x] = computesample(x, y, z, cell, startoffset, maxdist, soft, up);
                }
            }

            if(!visibilitytexture) glGenTextures(1, &visibilitytexture);
            create3dtexture(visibilitytexture, visibilitywidth, visibilityheight, visibilitydepth, visibilitypixels.getbuf(), 7, 1,
                            hasTRG ? GL_R8 : GL_LUMINANCE8);
            visibilityworldsize = worldsize;
            visibilitydirty = false;
        }

        static void update()
        {
            if(!visibilitydirty) return;
            visibilitydirty = false;
            if(!visibilitytexture || visibilitywidth <= 0 || visibilityheight <= 0 || visibilitydepth <= 0 || worldsize <= 0 ||
               visibilitypixels.empty() || !skycubeocclusion) return;

            float mapsize = float(max(worldsize, 1));
            float xscale = visibilitywidth/mapsize, yscale = visibilityheight/mapsize, zscale = visibilitydepth/mapsize;
            int x1 = clamp(int(floor(visibilitydirtymin.x*xscale)) - 1, 0, visibilitywidth - 1),
                y1 = clamp(int(floor(visibilitydirtymin.y*yscale)) - 1, 0, visibilityheight - 1), z1 = 0;
            int x2 = clamp(int(ceil(visibilitydirtymax.x*xscale)) + 1, 1, visibilitywidth),
                y2 = clamp(int(ceil(visibilitydirtymax.y*yscale)) + 1, 1, visibilityheight),
                z2 = clamp(int(ceil(visibilitydirtymax.z*zscale)) + 1, 1, visibilitydepth);
            if(x1 >= x2 || y1 >= y2 || z1 >= z2) return;

            float maxdist = max(skycubeocclmaxdist, 1.0f), startoffset = max(skycubeoccloffset, 0.0f), soft = max(skycubeocclsoft, 0.0f);
            float cell = mapsize/float(visibilitywidth);
            vec up(0, 0, 1);
            int width = x2 - x1, height = y2 - y1, depth = z2 - z1, total = width*height*depth;
            vector<uchar> subpixels;
            subpixels.reserve(total);
            subpixels.advance(total);
            int index = 0;
            for(int z = z1; z < z2; z++) for(int y = y1; y < y2; y++) for(int x = x1; x < x2; x++)
            {
                uchar visibility = computesample(x, y, z, cell, startoffset, maxdist, soft, up);
                visibilitypixels[(z*visibilityheight + y)*visibilitywidth + x] = visibility;
                subpixels[index++] = visibility;
            }

            if(glTexSubImage3D_)
            {
                GLint unpack = 4;
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
                if(unpack != 1) glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glBindTexture(GL_TEXTURE_3D, visibilitytexture);
                glTexSubImage3D_(GL_TEXTURE_3D, 0, x1, y1, z1, width, height, depth, hasTRG ? GL_RED : GL_LUMINANCE, GL_UNSIGNED_BYTE,
                                 subpixels.getbuf());
                if(unpack != 1) glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
            }
            else create3dtexture(visibilitytexture, visibilitywidth, visibilityheight, visibilitydepth, visibilitypixels.getbuf(), 7, 1,
                                 hasTRG ? GL_R8 : GL_LUMINANCE8);
        }

        void setup()
        {
            int resolution = visibilityresolution();
            if(!visibilitytexture || visibilitywidth != resolution || visibilityheight != resolution || visibilitydepth != resolution ||
               visibilityworldsize != worldsize) build();
            else update();
        }

        static void reset()
        {
            if(visibilitytexture) glDeleteTextures(1, &visibilitytexture);
            visibilitytexture = 0;
            visibilitywidth = visibilityheight = visibilitydepth = visibilityworldsize = -1;
            visibilitydirty = false;
            visibilitypixels.setsize(0);
        }

        void cleanup()
        {
            reset();
        }

        void bindtexture(int unit)
        {
            glActiveTexture_(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_3D, visibilitytexture);
        }

        void bindocclusionparams()
        {
            if(visibilitywidth > 0 && visibilityheight > 0 && visibilitydepth > 0 && worldsize > 0)
            {
                float invworld = 1.0f/float(worldsize);
                GLOBALPARAMF(skyocclscale, invworld*(1.0f - 1.0f/visibilitywidth), invworld*(1.0f - 1.0f/visibilityheight),
                             invworld*(1.0f - 1.0f/visibilitydepth));
                GLOBALPARAMF(skyoccloffset, 0.5f/visibilitywidth, 0.5f/visibilityheight, 0.5f/visibilitydepth);
            }
            else
            {
                GLOBALPARAMF(skyocclscale, 0.0f, 0.0f, 0.0f);
                GLOBALPARAMF(skyoccloffset, 0.0f, 0.0f, 0.0f);
            }
        }

        void bindambientparams()
        {
            bindocclusionparams();
            vec colors[6];
            vec2 front;
            getskycubetints(colors, front);
            GLOBALPARAM(skylf, colors[0]);
            GLOBALPARAM(skyrt, colors[1]);
            GLOBALPARAM(skybk, colors[2]);
            GLOBALPARAM(skyft, colors[3]);
            GLOBALPARAM(skydn, colors[4]);
            GLOBALPARAM(skyup, colors[5]);
            GLOBALPARAMF(skyfront, front.x, front.y);
            GLOBALPARAMF(skyambientparams, (drawtex || (editmode && fullbright)) ? 0.0f : clamp(skycubelight, 0.0f, 4.0f),
                         clamp(skycubelightmin, 0.0f, 1.0f));
        }
    }
}
