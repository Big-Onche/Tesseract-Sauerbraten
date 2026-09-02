#include "engine.h"

Texture *sky[6] = { 0, 0, 0, 0, 0, 0 }, *clouds[6] = { 0, 0, 0, 0, 0, 0 };
static Texture *nightsky = NULL;
extern bvec skyboxcolour;
extern int atmo;
extern float atmobright, atmohaze, atmodensity, atmoozone, atmoalpha, atmosunlightscale;
extern bvec atmosunlight;

static void cleanupAtmosphereDebugTimer();
static void cleanupAtmosphereTransmittanceLUT();

namespace skyboxtint
{
    static bool valid = false;
    static int texids[6] = { 0, 0, 0, 0, 0, 0 };
    static int texw[6] = { 0, 0, 0, 0, 0, 0 };
    static int texh[6] = { 0, 0, 0, 0, 0, 0 };
    static bvec tintcolour(0, 0, 0);
    static vec cubetint[6] =
    {
        vec(1, 1, 1), vec(1, 1, 1), vec(1, 1, 1),
        vec(1, 1, 1), vec(1, 1, 1), vec(1, 1, 1)
    };
    static vec2 cubefront(0, -1);

    static inline vec fetchrgba(const ImageData &img, int x, int y)
    {
        x = clamp(x, 0, img.w-1);
        y = clamp(y, 0, img.h-1);
        int pitch = img.pitch ? img.pitch : img.w * img.bpp;
        const uchar *p = img.data ? img.data + y * pitch + x * img.bpp : NULL;
        if(!p) return vec(1, 1, 1);
        switch(img.bpp)
        {
            case 1:
            case 2:
            {
                float g = p[0] / 255.0f;
                return vec(g, g, g);
            }
            case 3:
            default:
                return vec(p[0]/255.0f, p[1]/255.0f, p[2]/255.0f);
            case 4:
                return vec(p[0]/255.0f, p[1]/255.0f, p[2]/255.0f);
        }
    }

    static vec sampleface(const ImageData &img, float s, float t)
    {
        if(!img.data || img.w <= 0 || img.h <= 0 || img.compressed) return vec(1, 1, 1);
        s = clamp(s, 0.0f, 1.0f);
        t = clamp(t, 0.0f, 1.0f);
        float x = s * max(img.w - 1, 0), y = t * max(img.h - 1, 0);
        int x0 = int(floorf(x)), y0 = int(floorf(y));
        int x1 = min(x0 + 1, img.w - 1), y1 = min(y0 + 1, img.h - 1);
        float fx = x - x0, fy = y - y0;
        vec c00 = fetchrgba(img, x0, y0),
            c10 = fetchrgba(img, x1, y0),
            c01 = fetchrgba(img, x0, y1),
            c11 = fetchrgba(img, x1, y1);
        vec c0 = vec(c00).mul(1.0f - fx).add(vec(c10).mul(fx));
        vec c1 = vec(c01).mul(1.0f - fx).add(vec(c11).mul(fx));
        return c0.mul(1.0f - fy).add(c1.mul(fy));
    }

    static vec lightingtint(vec c)
    {
        c.x = max(c.x, 0.0f);
        c.y = max(c.y, 0.0f);
        c.z = max(c.z, 0.0f);

        float peak = max(c.x, max(c.y, c.z));
        if(peak > 0.72f)
        {
            float softpeak = 0.72f + (peak - 0.72f) * 0.30f;
            c.mul(softpeak / max(peak, 1e-4f));
        }

        float lum = c.x*0.2126f + c.y*0.7152f + c.z*0.0722f;
        const float keepsat = 0.88f;
        c.x = lum + (c.x - lum) * keepsat;
        c.y = lum + (c.y - lum) * keepsat;
        c.z = lum + (c.z - lum) * keepsat;
        return c;
    }

    static inline vec lerpvec(const vec &a, const vec &b, float t)
    {
        t = clamp(t, 0.0f, 1.0f);
        return vec(a).mul(1.0f - t).add(vec(b).mul(t));
    }

    static inline float smoothstepf(float e0, float e1, float x)
    {
        float t = e0 != e1 ? clamp((x - e0) / (e1 - e0), 0.0f, 1.0f) : (x >= e1 ? 1.0f : 0.0f);
        return t*t*(3.0f - 2.0f*t);
    }

    static void accumfaceavg(vec &sum, float &weight, const ImageData &img, float s, float t, float sw)
    {
        if(sw <= 0 || !img.data || img.compressed) return;
        sum.add(sampleface(img, s, t).mul(sw));
        weight += sw;
    }

    static vec averageface(const ImageData &img)
    {
        vec sum(0, 0, 0);
        float weight = 0.0f;
        static const struct tap { float s, t, w; } taps[] =
        {
            { 0.50f, 0.50f, 2.2f },
            { 0.22f, 0.22f, 0.9f }, { 0.50f, 0.22f, 1.0f }, { 0.78f, 0.22f, 0.9f },
            { 0.22f, 0.50f, 1.0f },                             { 0.78f, 0.50f, 1.0f },
            { 0.22f, 0.78f, 0.9f }, { 0.50f, 0.78f, 1.0f }, { 0.78f, 0.78f, 0.9f }
        };
        for(int i = 0; i < int(sizeof(taps)/sizeof(taps[0])); ++i)
            accumfaceavg(sum, weight, img, taps[i].s, taps[i].t, taps[i].w);
        return weight > 0 ? sum.mul(1.0f / weight) : vec(1, 1, 1);
    }

    static vec proceduraldircolor(const vec &dir)
    {
        vec fog = fogcolour.tocolor();
        vec suncol = !atmosunlight.iszero() ? atmosunlight.tocolor().mul(atmosunlightscale)
                                            : sunlight.tocolor().mul(sunlightscale);
        if(suncol.x + suncol.y + suncol.z <= 1e-4f) suncol = vec(1, 1, 1);

        float sunup = clamp(sunlightdir.z * 0.5f + 0.5f, 0.0f, 1.0f);
        float sunset = 1.0f - smoothstepf(0.35f, 0.85f, sunup);
        float haze = clamp(atmohaze * 0.20f, 0.0f, 1.0f);
        float density = clamp(1.0f - exp2f(-0.35f*max(atmodensity, 0.0f)), 0.0f, 1.0f);
        float ozone = clamp(atmoozone, 0.0f, 1.0f);
        float atmosmix = clamp(0.35f + 0.65f*atmoalpha, 0.0f, 1.0f);
        float brightscale = clamp(atmobright, 0.0f, 8.0f);
        float customsun = !atmosunlight.iszero() ? 1.0f : 0.0f;

        vec2 sunh(sunlightdir);
        if(sunh.squaredlen() > 1e-6f) sunh.normalize();
        else sunh = vec2(0, -1);

        float z = clamp(dir.z, -1.0f, 1.0f);
        float upk = smoothstepf(-0.05f, 0.85f, z);
        float downk = smoothstepf(-0.05f, 0.95f, -z);
        float horiz = 1.0f - fabsf(z);
        horiz = smoothstepf(0.0f, 1.0f, horiz);

        float sunalign = clamp(vec2(dir).dot(sunh), -1.0f, 1.0f);
        float sunglow = powf(max(sunalign, 0.0f), 4.0f) * (0.15f + 0.85f*horiz) * (0.15f + 0.85f*sunset);
        float backcool = powf(max(-sunalign, 0.0f), 2.0f) * horiz * (0.10f + 0.20f*haze);

        vec rayleigh(0.46f, 0.62f, 1.00f);
        vec zenbase = lerpvec(rayleigh, fog, 0.35f + 0.35f*haze);
        vec horbase = lerpvec(fog, lerpvec(rayleigh, fog, 0.65f), 0.35f);
        vec gndbase = lerpvec(lerpvec(fog, horbase, 0.35f), vec(0.18f, 0.18f, 0.20f), 0.35f);
        vec warm = lerpvec(vec(1.00f, 0.58f, 0.38f), suncol, 0.65f);
        vec cool = lerpvec(rayleigh, fog, 0.60f + 0.15f*ozone);

        vec c = lerpvec(horbase, zenbase, upk);
        c = lerpvec(c, gndbase, downk * (0.55f + 0.45f*density));
        c = lerpvec(c, warm, sunglow);
        c = lerpvec(c, cool, backcool);
        c = lerpvec(c, vec(c).mul(1.0f - 0.14f*ozone), 0.40f * upk);

        // Density should noticeably alter the final ambient tint.
        c = lerpvec(c, fog, 0.25f*density*(0.30f + 0.70f*horiz));
        c.mul(1.0f - 0.30f*density*(0.25f + 0.75f*(1.0f - upk)));

        // Keep atmospheric tint influence visible even when the sky is partially transparent.
        c = lerpvec(vec(1, 1, 1), c, atmosmix);

        // Ozone pushes toward cooler/blue skylight for ambient response.
        vec ozonepush = lerpvec(vec(1, 1, 1), vec(0.88f, 0.96f, 1.18f), 0.75f*ozone);
        c.mul(ozonepush);

        // Let custom atmosunlight tint influence all faces, not only forward-scattering zones.
        vec sunhint = lightingtint(vec(suncol).mul(0.80f).add(vec(0.20f, 0.20f, 0.20f)));
        c = lerpvec(c, vec(c).mul(sunhint), customsun ? 0.70f : (0.20f + 0.25f*sunset));

        c = lightingtint(c);
        c.mul(0.35f + 0.65f*brightscale);
        c.max(0.0f);
        return c;
    }

    static vec sampleproceduralface(const vec &normal, const vec &tangent, const vec &bitangent)
    {
        static const struct tap { float u, v, w; } taps[] =
        {
            { 0.50f, 0.50f, 2.2f },
            { 0.22f, 0.22f, 0.9f }, { 0.50f, 0.22f, 1.0f }, { 0.78f, 0.22f, 0.9f },
            { 0.22f, 0.50f, 1.0f },                             { 0.78f, 0.50f, 1.0f },
            { 0.22f, 0.78f, 0.9f }, { 0.50f, 0.78f, 1.0f }, { 0.78f, 0.78f, 0.9f }
        };
        vec sum(0, 0, 0);
        float weight = 0.0f;
        loopi(int(sizeof(taps)/sizeof(taps[0])))
        {
            float u = (taps[i].u - 0.5f)*1.35f, v = (taps[i].v - 0.5f)*1.35f;
            vec dir = vec(normal).add(vec(tangent).mul(u)).add(vec(bitangent).mul(v));
            if(dir.squaredlen() > 1e-6f) dir.normalize();
            else dir = normal;
            sum.add(proceduraldircolor(dir).mul(taps[i].w));
            weight += taps[i].w;
        }
        return weight > 0.0f ? sum.mul(1.0f/weight) : proceduraldircolor(normal);
    }

    static void proceduralcubetints(vec out[6], vec2 &front)
    {
        front = vec2(sunlightdir);
        if(front.squaredlen() > 1e-6f) front.normalize();
        else front = vec2(0, -1);
        vec2 right(-front.y, front.x);
        vec frontv(front.x, front.y, 0.0f),
            rightv(right.x, right.y, 0.0f),
            upv(0.0f, 0.0f, 1.0f);

        out[0] = sampleproceduralface(vec(-rightv.x, -rightv.y, 0.0f), frontv, upv); // lf
        out[1] = sampleproceduralface(vec( rightv.x,  rightv.y, 0.0f), vec(frontv).neg(), upv); // rt
        out[2] = sampleproceduralface(vec(-frontv.x, -frontv.y, 0.0f), vec(rightv).neg(), upv); // bk
        out[3] = sampleproceduralface(vec( frontv.x,  frontv.y, 0.0f), rightv, upv); // ft
        out[4] = sampleproceduralface(vec(0.0f, 0.0f, -1.0f), rightv, vec(frontv).neg()); // dn
        out[5] = sampleproceduralface(vec(0.0f, 0.0f,  1.0f), rightv, frontv); // up
    }

    static bool updateneeded()
    {
        if(!valid) return true;
        if(tintcolour != skyboxcolour) return true;
        bool havefaces = false;
        loopi(6) if(sky[i] && sky[i] != notexture) { havefaces = true; break; }
        if(!havefaces && atmo) return true; // atmospheric fallback can change with sun/atmo params
        loopi(6)
        {
            Texture *t = sky[i];
            int id = t && t != notexture ? t->id : 0;
            int w = t && t != notexture ? t->w : 0;
            int h = t && t != notexture ? t->h : 0;
            if(texids[i] != id || texw[i] != w || texh[i] != h) return true;
        }
        return false;
    }

    static void recompute()
    {
        valid = true;
        tintcolour = skyboxcolour;
        loopi(6)
        {
            Texture *t = sky[i];
            texids[i] = t && t != notexture ? t->id : 0;
            texw[i] = t && t != notexture ? t->w : 0;
            texh[i] = t && t != notexture ? t->h : 0;
        }

        loopi(6) cubetint[i] = vec(1, 1, 1);
        cubefront = vec2(0, -1);

        ImageData faceimg[6];
        bool faceloaded[6] = { false, false, false, false, false, false };
        bool anyloaded = false;
        loopi(6) if(sky[i] && sky[i] != notexture && sky[i]->name && sky[i]->w > 0 && sky[i]->h > 0)
        {
            // Texture names can contain inline commands (for example
            // <nocompress>). Use the texture-data path so those commands are
            // parsed instead of being mistaken for part of the file name.
            faceloaded[i] = loadtexturedata(sky[i]->name, faceimg[i], false) && faceimg[i].data && !faceimg[i].compressed;
            anyloaded = anyloaded || faceloaded[i];
        }

        vec skycol = skyboxcolour.tocolor();
        if(anyloaded)
        {
            loopi(6)
            {
                cubetint[i] = skycol;
                if(faceloaded[i]) cubetint[i] = lightingtint(averageface(faceimg[i])).mul(skycol);
            }
            cubefront = vec2(0, -1);
        }
        else if(atmo)
        {
            proceduralcubetints(cubetint, cubefront);
        }
        else
        {
            loopi(6) cubetint[i] = skycol;
            cubefront = vec2(0, -1);
        }

        loopi(6) if(faceimg[i].data) faceimg[i].cleanup();
    }
}

void loadsky(const char *basename, Texture *texs[6])
{
    const char *wildcard = strchr(basename, '*');
    loopi(6)
    {
        const char *side = cubemapsides[i].name;
        string name;
        copystring(name, makerelpath("packages/sky", basename));
        if(wildcard)
        {
            char *chop = strchr(name, '*');
            if(chop) { *chop = '\0'; concatstring(name, side); concatstring(name, wildcard+1); }
            texs[i] = textureload(name, 3, true, false);
        }
        else
        {
            defformatstring(ext, "_%s.jpg", side);
            concatstring(name, ext);
            if((texs[i] = textureload(name, 3, true, false))==notexture)
            {
                strcpy(name+strlen(name)-3, "png");
                texs[i] = textureload(name, 3, true, false);
            }
        }
        if(texs[i]==notexture) conoutf(CON_ERROR, "could not load side %s of sky texture %s", side, basename);
    }
    skyboxtint::valid = false;
}

Texture *cloudoverlay = NULL;

static bool haveskyfaces()
{
    loopi(6) if(sky[i] && sky[i] != notexture) return true;
    return false;
}

static void clearskyfaces(Texture *texs[6])
{
    loopi(6) texs[i] = NULL;
}

Texture *loadskyoverlay(const char *basename)
{
    const char *ext = strrchr(basename, '.');
    string name;
    copystring(name, makerelpath("packages/sky", basename));
    Texture *t = notexture;
    if(ext) t = textureload(name, 0, true, false);
    else
    {
        concatstring(name, ".jpg");
        if((t = textureload(name, 0, true, false)) == notexture)
        {
            strcpy(name+strlen(name)-3, "png");
            t = textureload(name, 0, true, false);
        }
    }
    if(t==notexture) conoutf(CON_ERROR, "could not load sky overlay texture %s", basename);
    return t;
}

static void updateskybox();
SVARFR(skybox, "", updateskybox());
CVARR(skyboxcolour, 0xFFFFFF);
FVARR(skyboxoverbright, 1, 2, 16);
FVARR(skyboxoverbrightmin, 0, 1, 16);
FVARR(skyboxoverbrightthreshold, 0, 0.7f, 1);
FVARR(spinsky, -720, 0, 720);
VARR(yawsky, 0, 0, 360);
SVARFR(cloudbox, "", { if(cloudbox[0]) loadsky(cloudbox, clouds); });
CVARR(cloudboxcolour, 0xFFFFFF);
FVARR(cloudboxalpha, 0, 1, 1);
FVARR(spinclouds, -720, 0, 720);
VARR(yawclouds, 0, 0, 360);
FVARR(cloudclip, 0, 0.5f, 1);
SVARFR(cloudlayer, "", { if(cloudlayer[0]) cloudoverlay = loadskyoverlay(cloudlayer); });
FVARR(cloudoffsetx, 0, 0, 1);
FVARR(cloudoffsety, 0, 0, 1);
FVARR(cloudscrollx, -16, 0, 16);
FVARR(cloudscrolly, -16, 0, 16);
FVARR(cloudscale, 0.001, 1, 64);
FVARR(spincloudlayer, -720, 0, 720);
VARR(yawcloudlayer, 0, 0, 360);
FVARR(cloudheight, -1, 0.2f, 1);
FVARR(cloudfade, 0, 0.2f, 1);
FVARR(cloudalpha, 0, 1, 1);
VARR(cloudsubdiv, 4, 16, 64);
CVARR(cloudcolour, 0xFFFFFF);

static void updateskybox()
{
    if(!skybox[0] || !strcmp(skybox, "0"))
    {
        clearskyfaces(sky);
        skyboxtint::valid = false;
        return;
    }
    loadsky(skybox, sky);
}

void drawenvboxface(float s0, float t0, int x0, int y0, int z0,
                    float s1, float t1, int x1, int y1, int z1,
                    float s2, float t2, int x2, int y2, int z2,
                    float s3, float t3, int x3, int y3, int z3,
                    Texture *tex)
{
    glBindTexture(GL_TEXTURE_2D, (tex ? tex : notexture)->id);
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(x3, y3, z3); gle::attribf(s3, t3);
    gle::attribf(x2, y2, z2); gle::attribf(s2, t2);
    gle::attribf(x0, y0, z0); gle::attribf(s0, t0);
    gle::attribf(x1, y1, z1); gle::attribf(s1, t1);
    xtraverts += gle::end();
}

void drawenvbox(Texture **sky = NULL, float z1clip = 0.0f, float z2clip = 1.0f, int faces = 0x3F)
{
    if(z1clip >= z2clip) return;

    float v1 = 1-z1clip, v2 = 1-z2clip;
    int w = farplane/2, z1 = int(ceil(2*w*(z1clip-0.5f))), z2 = int(ceil(2*w*(z2clip-0.5f)));

    gle::defvertex();
    gle::deftexcoord0();

    if(faces&0x01)
        drawenvboxface(0.0f, v2,  -w, -w, z2,
                       1.0f, v2,  -w,  w, z2,
                       1.0f, v1,  -w,  w, z1,
                       0.0f, v1,  -w, -w, z1, sky[0]);

    if(faces&0x02)
        drawenvboxface(1.0f, v1, w, -w, z1,
                       0.0f, v1, w,  w, z1,
                       0.0f, v2, w,  w, z2,
                       1.0f, v2, w, -w, z2, sky[1]);

    if(faces&0x04)
        drawenvboxface(1.0f, v1,  w,  w, z1,
                       0.0f, v1, -w,  w, z1,
                       0.0f, v2, -w,  w, z2,
                       1.0f, v2,  w,  w, z2, sky[2]);

    if(faces&0x08)
        drawenvboxface(1.0f, v1, -w, -w, z1,
                       0.0f, v1,  w, -w, z1,
                       0.0f, v2,  w, -w, z2,
                       1.0f, v2, -w, -w, z2, sky[3]);

    if(z1clip <= 0 && faces&0x10)
        drawenvboxface(0.0f, 1.0f, -w,  w,  -w,
                       0.0f, 0.0f,  w,  w,  -w,
                       1.0f, 0.0f,  w, -w,  -w,
                       1.0f, 1.0f, -w, -w,  -w, sky[4]);

    if(z2clip >= 1 && faces&0x20)
        drawenvboxface(0.0f, 1.0f,  w,  w, w,
                       0.0f, 0.0f, -w,  w, w,
                       1.0f, 0.0f, -w, -w, w,
                       1.0f, 1.0f,  w, -w, w, sky[5]);
}

void drawenvoverlay(Texture *overlay = NULL, float tx = 0, float ty = 0)
{
    int w = farplane/2;
    float z = w*cloudheight, tsz = 0.5f*(1-cloudfade)/cloudscale, psz = w*(1-cloudfade);
    glBindTexture(GL_TEXTURE_2D, (overlay ? overlay : notexture)->id);
    vec color = cloudcolour.tocolor();
    gle::color(color, cloudalpha);
    gle::defvertex();
    gle::deftexcoord0();
    gle::begin(GL_TRIANGLE_FAN);
    loopi(cloudsubdiv+1)
    {
        vec p(1, 1, 0);
        p.rotate_around_z((-2.0f*M_PI*i)/cloudsubdiv);
        gle::attribf(p.x*psz, p.y*psz, z);
            gle::attribf(tx + p.x*tsz, ty + p.y*tsz);
    }
    xtraverts += gle::end();
    float tsz2 = 0.5f/cloudscale;
    gle::defvertex();
    gle::deftexcoord0();
    gle::defcolor(4);
    gle::begin(GL_TRIANGLE_STRIP);
    loopi(cloudsubdiv+1)
    {
        vec p(1, 1, 0);
        p.rotate_around_z((-2.0f*M_PI*i)/cloudsubdiv);
        gle::attribf(p.x*psz, p.y*psz, z);
            gle::attribf(tx + p.x*tsz, ty + p.y*tsz);
            gle::attrib(color, cloudalpha);
        gle::attribf(p.x*w, p.y*w, z);
            gle::attribf(tx + p.x*tsz2, ty + p.y*tsz2);
            gle::attrib(color, 0.0f);
    }
    xtraverts += gle::end();
}

FVARR(fogdomeheight, -1, -0.5f, 1);
FVARR(fogdomemin, 0, 0, 1);
FVARR(fogdomemax, 0, 0, 1);
VARR(fogdomecap, 0, 1, 1);
FVARR(fogdomeclip, 0, 1, 1);
CVARR(fogdomecolour, 0);
VARR(fogdomeclouds, 0, 1, 1);
VARR(fogdomesquare, 0, 0, 1);

namespace fogdome
{
    struct vert
    {
        vec pos;
        bvec4 color;

        vert() {}
        vert(const vec &pos, const bvec &fcolor, float alpha) : pos(pos), color(fcolor, uchar(alpha*255))
        {
        }
        vert(const vert &v0, const vert &v1) : pos(vec(v0.pos).add(v1.pos).normalize()), color(v0.color)
        {
            if(v0.pos.z != v1.pos.z) color.a += uchar((v1.color.a - v0.color.a) * (pos.z - v0.pos.z) / (v1.pos.z - v0.pos.z));
        }
    } *verts = NULL;
    GLushort *indices = NULL;
    int numverts = 0, numindices = 0, capindices = 0;
    GLuint vbuf = 0, ebuf = 0;
    bvec lastcolor(0, 0, 0);
    float lastminalpha = 0, lastmaxalpha = 0, lastcapsize = -1, lastclipz = 1;

    void subdivide(int depth, int face);

    void genface(int depth, int i1, int i2, int i3)
    {
        int face = numindices; numindices += 3;
        indices[face]   = i3;
        indices[face+1] = i2;
        indices[face+2] = i1;
        subdivide(depth, face);
    }

    void subdivide(int depth, int face)
    {
        if(depth-- <= 0) return;
        int idx[6];
        loopi(3) idx[i] = indices[face+2-i];
        loopi(3)
        {
            int curvert = numverts++;
            verts[curvert] = vert(verts[idx[i]], verts[idx[(i+1)%3]]); //push on to unit sphere
            idx[3+i] = curvert;
            indices[face+2-i] = curvert;
        }
        subdivide(depth, face);
        loopi(3) genface(depth, idx[i], idx[3+i], idx[3+(i+2)%3]);
    }

    static inline int sortcap(GLushort x, GLushort y)
    {
        const vec &xv = verts[x].pos, &yv = verts[y].pos;
        return xv.y < 0 ? yv.y >= 0 || xv.x < yv.x : yv.y >= 0 && xv.x > yv.x;
    }

    static void init(const bvec &color, float minalpha = 0.0f, float maxalpha = 1.0f, float capsize = -1, float clipz = 1, int hres = 16, int depth = 2)
    {
        const int tris = hres << (2*depth);
        numverts = numindices = capindices = 0;
        verts = new vert[tris+1 + (capsize >= 0 ? 1 : 0)];
        indices = new GLushort[(tris + (capsize >= 0 ? hres<<depth : 0))*3];
        if(clipz >= 1)
        {
            verts[numverts++] = vert(vec(0.0f, 0.0f, 1.0f), color, minalpha); //build initial 'hres' sided pyramid
            loopi(hres) verts[numverts++] = vert(vec(sincos360[(360*i)/hres], 0.0f), color, maxalpha);
            loopi(hres) genface(depth, 0, i+1, 1+(i+1)%hres);
        }
        else if(clipz <= 0)
        {
            loopi(hres<<depth) verts[numverts++] = vert(vec(sincos360[(360*i)/(hres<<depth)], 0.0f), color, maxalpha);
        }
        else
        {
            float clipxy = sqrtf(1 - clipz*clipz);
            const vec2 &scm = sincos360[180/hres];
            loopi(hres)
            {
                const vec2 &sc = sincos360[(360*i)/hres];
                verts[numverts++] = vert(vec(sc.x*clipxy, sc.y*clipxy, clipz), color, minalpha);
                verts[numverts++] = vert(vec(sc.x, sc.y, 0.0f), color, maxalpha);
                verts[numverts++] = vert(vec(sc.x*scm.x - sc.y*scm.y, sc.y*scm.x + sc.x*scm.y, 0.0f), color, maxalpha);
            }
            loopi(hres)
            {
                genface(depth-1, 3*i, 3*i+1, 3*i+2);
                genface(depth-1, 3*i, 3*i+2, 3*((i+1)%hres));
                genface(depth-1, 3*i+2, 3*((i+1)%hres)+1, 3*((i+1)%hres));
            }
        }

        if(capsize >= 0)
        {
            GLushort *cap = &indices[numindices];
            int capverts = 0;
            loopi(numverts) if(!verts[i].pos.z) cap[capverts++] = i;
            verts[numverts++] = vert(vec(0.0f, 0.0f, -capsize), color, maxalpha);
            quicksort(cap, capverts, sortcap);
            loopi(capverts)
            {
                int n = capverts-1-i;
                cap[n*3] = cap[n];
                cap[n*3+1] = cap[(n+1)%capverts];
                cap[n*3+2] = numverts-1;
                capindices += 3;
            }
        }

        if(!vbuf) glGenBuffers_(1, &vbuf);
        gle::bindvbo(vbuf);
        glBufferData_(GL_ARRAY_BUFFER, numverts*sizeof(vert), verts, GL_STATIC_DRAW);
        DELETEA(verts);

        if(!ebuf) glGenBuffers_(1, &ebuf);
        gle::bindebo(ebuf);
        glBufferData_(GL_ELEMENT_ARRAY_BUFFER, (numindices + capindices)*sizeof(GLushort), indices, GL_STATIC_DRAW);
        DELETEA(indices);
    }

    void cleanup()
    {
        numverts = numindices = 0;
        if(vbuf) { glDeleteBuffers_(1, &vbuf); vbuf = 0; }
        if(ebuf) { glDeleteBuffers_(1, &ebuf); ebuf = 0; }
    }

    void draw()
    {
        float capsize = fogdomecap && fogdomeheight < 1 ? (1 + fogdomeheight) / (1 - fogdomeheight) : -1;
        bvec color = !fogdomecolour.iszero() ? fogdomecolour : fogcolour;
        if(!numverts || lastcolor != color || lastminalpha != fogdomemin || lastmaxalpha != fogdomemax || lastcapsize != capsize || lastclipz != fogdomeclip)
        {
            init(color, min(fogdomemin, fogdomemax), fogdomemax, capsize, fogdomeclip);
            lastcolor = color;
            lastminalpha = fogdomemin;
            lastmaxalpha = fogdomemax;
            lastcapsize = capsize;
            lastclipz = fogdomeclip;
        }

        gle::bindvbo(vbuf);
        gle::bindebo(ebuf);

        gle::vertexpointer(sizeof(vert), &verts->pos);
        gle::colorpointer(sizeof(vert), &verts->color);
        gle::enablevertex();
        gle::enablecolor();

        glDrawRangeElements_(GL_TRIANGLES, 0, numverts-1, numindices + fogdomecap*capindices, GL_UNSIGNED_SHORT, indices);
        xtraverts += numverts;
        glde++;

        gle::disablevertex();
        gle::disablecolor();

        gle::clearvbo();
        gle::clearebo();
    }
}

static void drawfogdome()
{
    SETVARIANT(skyfog, fogdomesquare ? 0 : -1, 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    matrix4 skymatrix = cammatrix, skyprojmatrix;
    skymatrix.settranslation(vec(cammatrix.c).mul(farplane*fogdomeheight*0.5f));
    skymatrix.scale(farplane/2, farplane/2, farplane*(0.5f - fogdomeheight*0.5f));
    skyprojmatrix.mul(projmatrix, skymatrix);
    LOCALPARAM(skymatrix, skyprojmatrix);

    fogdome::draw();

    glDisable(GL_BLEND);
}

void cleanupsky()
{
    skyboxtint::valid = false;
    fogdome::cleanup();
    cleanupAtmosphereTransmittanceLUT();
    cleanupAtmosphereDebugTimer();
}

void getskycubetints(vec colors[6], vec2 &front)
{
    if(skyboxtint::updateneeded()) skyboxtint::recompute();
    loopi(6) colors[i] = skyboxtint::cubetint[i];
    front = skyboxtint::cubefront;
    if(haveskyfaces() || atmo)
    {
        float a = (spinsky*lastmillis/1000.0f + yawsky) * -RAD;
        float ca = cosf(a), sa = sinf(a);
        front = vec2(front.x*ca - front.y*sa, front.x*sa + front.y*ca);
    }
}

static void reloadatmosphereshader()
{
    Shader *atmosphereshader = lookupshaderbyname("atmosphere"),
           *transmittanceshader = lookupshaderbyname("atmospheretransmittance");
    if(!atmosphereshader && !transmittanceshader) return;

    if(atmosphereshader) atmosphereshader->cleanup(true);
    if(transmittanceshader) transmittanceshader->cleanup(true);
    execfile("config/glsl/sky.cfg", false);
}

VARR(atmo, 0, 0, 1);
VARP(debugatmo, 0, 0, 1);
VARFP(atmoviewsteps, 1, 24, 64, reloadatmosphereshader());
VARFP(atmosunsteps, 1, 8, 32, reloadatmosphereshader());
VARFP(atmosunlut, 0, 2, 2, cleanupAtmosphereTransmittanceLUT()); // 0 = 64x64, 1 = 128x32, 2 = 128x64
FVARR(atmoplanetsize, 1e-3f, 1, 1e3f);
FVARR(atmoheight, 1e-3f, 1, 1e3f);
FVARR(atmobright, 0, 1, 16);
CVAR1R(atmosunlight, 0);
FVARR(atmosunlightscale, 0, 1, 16);
CVAR1R(atmosundisk, 0);
FVARR(atmosundisksize, 0, 12, 90);
FVARR(atmosundiskcorona, 0, 0.4f, 1);
FVARR(atmosundiskbright, 0, 1, 16);
FVARR(atmohaze, 0, 0.1f, 16);
FVARR(atmodensity, 0, 1, 16);
FVARR(atmoozone, 0, 1, 16);
FVARR(atmomultiscatter, 0, 1, 2);
FVARR(atmomieanisotropy, -0.99f, 0.8f, 0.99f);
FVARR(atmoalpha, 0, 1, 1);

static const int ATMOSPHERE_DEBUG_QUERY_COUNT = 3;
static GLuint atmosphereDebugQueries[ATMOSPHERE_DEBUG_QUERY_COUNT][2] = { { 0 } };
static int atmosphereDebugQueryCycle = 0, atmosphereDebugQueryWaiting = 0, atmosphereDebugQueryActive = -1;
static Uint64 atmosphereDebugCPUStart = 0;
static float atmosphereDebugGPUMillis = -1.0f, atmosphereDebugCPUMillis = 0.0f, atmosphereLUTRebuildMillis = 0.0f;

static GLuint atmosphereTransmittanceTex = 0, atmosphereTransmittanceFBO = 0;
static int atmosphereTransmittanceWidth = 0, atmosphereTransmittanceHeight = 0;

struct AtmosphereTransmittanceLUTCache
{
    bool valid;
    int sunsteps;
    float planetradius, atmosphereradius, inverseRayleighScaleHeight, inverseMieScaleHeight, ozonecenter, inverseOzoneHalfWidth;
    vec betarayleigh, mieextinction, betaozone;

    AtmosphereTransmittanceLUTCache() : valid(false), sunsteps(0), planetradius(0), atmosphereradius(0), inverseRayleighScaleHeight(0),
                                       inverseMieScaleHeight(0), ozonecenter(0), inverseOzoneHalfWidth(0), betarayleigh(0, 0, 0),
                                       mieextinction(0, 0, 0), betaozone(0, 0, 0)
    {
    }

    bool matches(float newplanetradius, float newatmosphereradius, float newinverseRayleighScaleHeight, float newinverseMieScaleHeight,
                 float newozonecenter, float newinverseOzoneHalfWidth, const vec &newbetarayleigh, const vec &newmieextinction,
                 const vec &newbetaozone) const
    {
        return valid && sunsteps == atmosunsteps && planetradius == newplanetradius && atmosphereradius == newatmosphereradius &&
               inverseRayleighScaleHeight == newinverseRayleighScaleHeight && inverseMieScaleHeight == newinverseMieScaleHeight &&
               ozonecenter == newozonecenter && inverseOzoneHalfWidth == newinverseOzoneHalfWidth && betarayleigh == newbetarayleigh &&
               mieextinction == newmieextinction && betaozone == newbetaozone;
    }

    void update(float newplanetradius, float newatmosphereradius, float newinverseRayleighScaleHeight, float newinverseMieScaleHeight,
                float newozonecenter, float newinverseOzoneHalfWidth, const vec &newbetarayleigh, const vec &newmieextinction,
                const vec &newbetaozone)
    {
        valid = true;
        sunsteps = atmosunsteps;
        planetradius = newplanetradius;
        atmosphereradius = newatmosphereradius;
        inverseRayleighScaleHeight = newinverseRayleighScaleHeight;
        inverseMieScaleHeight = newinverseMieScaleHeight;
        ozonecenter = newozonecenter;
        inverseOzoneHalfWidth = newinverseOzoneHalfWidth;
        betarayleigh = newbetarayleigh;
        mieextinction = newmieextinction;
        betaozone = newbetaozone;
    }
};

static AtmosphereTransmittanceLUTCache atmosphereTransmittanceCache;

static void getAtmosphereTransmittanceLUTSize(int &width, int &height)
{
    switch(atmosunlut)
    {
        case 0: width = 64; height = 64; break;
        case 1: width = 128; height = 32; break;
        default: width = 128; height = 64; break;
    }
}

static void cleanupAtmosphereTransmittanceLUT()
{
    if(atmosphereTransmittanceFBO) { glDeleteFramebuffers_(1, &atmosphereTransmittanceFBO); atmosphereTransmittanceFBO = 0; }
    if(atmosphereTransmittanceTex) { glDeleteTextures(1, &atmosphereTransmittanceTex); atmosphereTransmittanceTex = 0; }
    atmosphereTransmittanceWidth = atmosphereTransmittanceHeight = 0;
    atmosphereTransmittanceCache.valid = false;
    atmosphereLUTRebuildMillis = 0.0f;
}

static void updateAtmosphereTransmittanceLUT(float planetradius, float atmosphereradius, float inverseRayleighScaleHeight,
                                            float inverseMieScaleHeight, float ozonecenter, float inverseOzoneHalfWidth,
                                            const vec &betarayleigh, const vec &mieextinction, const vec &betaozone)
{
    int width, height;
    getAtmosphereTransmittanceLUTSize(width, height);
    bool resize = width != atmosphereTransmittanceWidth || height != atmosphereTransmittanceHeight;
    if(!resize && atmosphereTransmittanceCache.matches(planetradius, atmosphereradius, inverseRayleighScaleHeight, inverseMieScaleHeight,
                                                       ozonecenter, inverseOzoneHalfWidth, betarayleigh, mieextinction, betaozone)) return;

    Uint64 start = SDL_GetPerformanceCounter();
    if(resize)
    {
        if(atmosphereTransmittanceTex) glDeleteTextures(1, &atmosphereTransmittanceTex);
        glGenTextures(1, &atmosphereTransmittanceTex);
        createtexture(atmosphereTransmittanceTex, width, height, NULL, 3, 1, GL_RGB16F, GL_TEXTURE_2D);
        atmosphereTransmittanceWidth = width;
        atmosphereTransmittanceHeight = height;
    }
    if(!atmosphereTransmittanceFBO) glGenFramebuffers_(1, &atmosphereTransmittanceFBO);

    GLint previousFBO = 0, previousViewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    GLboolean blend = glIsEnabled(GL_BLEND), depthtest = glIsEnabled(GL_DEPTH_TEST), scissortest = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer_(GL_FRAMEBUFFER, atmosphereTransmittanceFBO);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, atmosphereTransmittanceTex, 0);
    if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating atmosphere transmittance LUT!");
    glViewport(0, 0, width, height);
    if(blend) glDisable(GL_BLEND);
    if(depthtest) glDisable(GL_DEPTH_TEST);
    if(scissortest) glDisable(GL_SCISSOR_TEST);

    SETSHADER(atmospheretransmittance);
    LOCALPARAMF(atmosphereparams, planetradius, atmosphereradius, inverseRayleighScaleHeight, inverseMieScaleHeight);
    LOCALPARAMF(atmosphereparams2, ozonecenter, inverseOzoneHalfWidth, 0.0f, 0.0f);
    LOCALPARAM(betarayleigh, betarayleigh);
    LOCALPARAM(mieextinction, mieextinction);
    LOCALPARAM(betaozone, betaozone);
    LOCALPARAMF(atmospherelutsize, float(width), float(height));
    screenquad();

    glBindFramebuffer_(GL_FRAMEBUFFER, previousFBO);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    if(blend) glEnable(GL_BLEND);
    if(depthtest) glEnable(GL_DEPTH_TEST);
    if(scissortest) glEnable(GL_SCISSOR_TEST);

    atmosphereTransmittanceCache.update(planetradius, atmosphereradius, inverseRayleighScaleHeight, inverseMieScaleHeight, ozonecenter,
                                        inverseOzoneHalfWidth, betarayleigh, mieextinction, betaozone);
    Uint64 frequency = SDL_GetPerformanceFrequency();
    atmosphereLUTRebuildMillis = frequency ? float((SDL_GetPerformanceCounter() - start) * 1000.0 / frequency) : 0.0f;
}

static void pollAtmosphereDebugTimer()
{
    if(!debugatmo || !atmosphereDebugQueries[0][0]) return;

    loopi(ATMOSPHERE_DEBUG_QUERY_COUNT) if(atmosphereDebugQueryWaiting & (1 << i))
    {
        GLint available = 0;
        glGetQueryObjectiv_(atmosphereDebugQueries[i][1], GL_QUERY_RESULT_AVAILABLE, &available);
        if(!available) continue;

        GLuint64EXT start = 0, end = 0;
        glGetQueryObjectui64v_(atmosphereDebugQueries[i][0], GL_QUERY_RESULT, &start);
        glGetQueryObjectui64v_(atmosphereDebugQueries[i][1], GL_QUERY_RESULT, &end);
        atmosphereDebugGPUMillis = end >= start ? float(end - start) * 1.0e-6f : 0.0f;
        atmosphereDebugQueryWaiting &= ~(1 << i);
    }
}

static void beginAtmosphereDebugTimer()
{
    atmosphereDebugQueryActive = -1;
    atmosphereDebugCPUStart = 0;
    if(!debugatmo) return;

    pollAtmosphereDebugTimer();
    atmosphereDebugCPUStart = SDL_GetPerformanceCounter();
    if(hasTQ && glQueryCounter_)
    {
        if(!atmosphereDebugQueries[0][0])
            glGenQueries_(ATMOSPHERE_DEBUG_QUERY_COUNT * 2, &atmosphereDebugQueries[0][0]);
        if(!(atmosphereDebugQueryWaiting & (1 << atmosphereDebugQueryCycle)))
        {
            atmosphereDebugQueryActive = atmosphereDebugQueryCycle;
            glQueryCounter_(atmosphereDebugQueries[atmosphereDebugQueryActive][0], GL_TIMESTAMP);
        }
    }
}

static void endAtmosphereDebugTimer()
{
    if(atmosphereDebugQueryActive >= 0)
    {
        glQueryCounter_(atmosphereDebugQueries[atmosphereDebugQueryActive][1], GL_TIMESTAMP);
        atmosphereDebugQueryWaiting |= 1 << atmosphereDebugQueryActive;
        atmosphereDebugQueryCycle = (atmosphereDebugQueryActive + 1) % ATMOSPHERE_DEBUG_QUERY_COUNT;
        atmosphereDebugQueryActive = -1;
    }
    if(atmosphereDebugCPUStart)
    {
        Uint64 frequency = SDL_GetPerformanceFrequency();
        atmosphereDebugCPUMillis = frequency ? float((SDL_GetPerformanceCounter() - atmosphereDebugCPUStart) * 1000.0 / frequency) : 0.0f;
        atmosphereDebugCPUStart = 0;
    }
}

static void cleanupAtmosphereDebugTimer()
{
    if(atmosphereDebugQueries[0][0]) glDeleteQueries_(ATMOSPHERE_DEBUG_QUERY_COUNT * 2, &atmosphereDebugQueries[0][0]);
    memset(atmosphereDebugQueries, 0, sizeof(atmosphereDebugQueries));
    atmosphereDebugQueryCycle = 0;
    atmosphereDebugQueryWaiting = 0;
    atmosphereDebugQueryActive = -1;
    atmosphereDebugCPUStart = 0;
    atmosphereDebugGPUMillis = -1.0f;
    atmosphereDebugCPUMillis = 0.0f;
}

void atmosphereDebugView()
{
    if(!debugatmo) return;

    pollAtmosphereDebugTimer();
    int y = 0;
    draw_text("Atmosphere", 0, y);
    y += FONTH;
    if(atmosphereDebugGPUMillis >= 0.0f) draw_textf("GPU total: %.2f ms", 0, y, atmosphereDebugGPUMillis);
    else draw_text("GPU total: n/a", 0, y);
    y += FONTH;
    draw_textf("CPU submit: %.2f ms", 0, y, atmosphereDebugCPUMillis);
    y += FONTH;
    draw_textf("View steps: %d", 0, y, atmoviewsteps);
    y += FONTH;
    draw_textf("LUT sun steps: %d", 0, y, atmosunsteps);
    y += FONTH;
    int lutwidth, lutheight;
    getAtmosphereTransmittanceLUTSize(lutwidth, lutheight);
    draw_textf("Solar LUT: %d x %d RGB16F", 0, y, lutwidth, lutheight);
    y += FONTH;
    draw_textf("Render resolution: %d x %d", 0, y, vieww, viewh);
    y += FONTH;
    draw_text("Scale: 1.00", 0, y);
    y += FONTH;
    draw_textf("LUT rebuild: %.2f ms", 0, y, atmosphereLUTRebuildMillis);
}

static bool loadnightsky()
{
    // Repeat around the horizon, but do not wrap the north/south poles together.
    if(!nightsky) nightsky = textureload("packages/sky/night.jpg", 2, true, false);
    return nightsky != notexture;
}

static void drawnightsky(float alpha)
{
    if(!loadnightsky()) return;

    SETSHADER(nightsky);

    matrix4 nightskymatrix = invcammatrix;
    nightskymatrix.settranslation(0, 0, 0);
    nightskymatrix.mul(invprojmatrix);
    LOCALPARAM(nightskymatrix, nightskymatrix);
    LOCALPARAMF(nightskyalpha, alpha);

    glBindTexture(GL_TEXTURE_2D, nightsky->id);

    if(alpha < 1)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    gle::defvertex();
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(-1, 1, 1);
    gle::attribf(1, 1, 1);
    gle::attribf(-1, -1, 1);
    gle::attribf(1, -1, 1);
    xtraverts += gle::end();

    if(alpha < 1) glDisable(GL_BLEND);
}

static void drawatmosphere(float alpha = atmoalpha)
{
    // Hillaire's Earth atmosphere model, expressed in kilometres to retain
    // enough precision on older GLSL implementations. atmoheight scales the
    // whole vertical density profile while atmoplanetsize only changes the
    // ground radius.
    const float earthradius = 6360.0f, earthatmoheight = 100.0f;
    float planetradius = earthradius*atmoplanetsize,
          atmosphereheight = earthatmoheight*atmoheight,
          inverseRayleighScaleHeight = 1.0f/max(8.0f*atmoheight, 1.0e-3f),
          inverseMieScaleHeight = 1.0f/max(1.2f*atmoheight, 1.0e-3f),
          inverseOzoneHalfWidth = 1.0f/max(15.0f*atmoheight, 1.0e-3f);

    // Scattering/absorption coefficients from Table 1 of Hillaire 2020.
    // Coefficients are km^-1. Preserve the historic atmohaze default by
    // mapping 0.1 to one Earth atmosphere worth of aerosols.
    float mieamount = max(atmohaze, 0.0f)*10.0f;
    vec betar = vec(0.005802f, 0.013558f, 0.033100f).mul(max(atmodensity, 0.0f)),
        betam = vec(0.003996f, 0.003996f, 0.003996f).mul(mieamount),
        betamextinction = vec(0.008396f, 0.008396f, 0.008396f).mul(mieamount),
        betao = vec(0.000650f, 0.001881f, 0.000085f).mul(max(atmoozone, 0.0f));
    updateAtmosphereTransmittanceLUT(planetradius, planetradius + atmosphereheight, inverseRayleighScaleHeight, inverseMieScaleHeight,
                                    25.0f*atmoheight, inverseOzoneHalfWidth, betar, betamextinction, betao);

    SETSHADER(atmosphere);

    matrix4 sunmatrix = invcammatrix;
    sunmatrix.settranslation(0, 0, 0);
    sunmatrix.mul(invprojmatrix);
    LOCALPARAM(sunmatrix, sunmatrix);
    LOCALPARAMF(atmosphereparams, planetradius, planetradius + atmosphereheight, inverseRayleighScaleHeight, inverseMieScaleHeight);
    LOCALPARAMF(atmosphereparams2, 25.0f*atmoheight, inverseOzoneHalfWidth, clamp(atmomultiscatter, 0.0f, 2.0f),
                clamp(atmomieanisotropy, -0.99f, 0.99f));
    LOCALPARAM(betarayleigh, betar);
    LOCALPARAM(betamie, betam);
    LOCALPARAM(mieextinction, betamextinction);
    LOCALPARAM(betaozone, betao);
    LOCALPARAMF(atmospherelutparams, float(atmosphereTransmittanceWidth - 1)/atmosphereTransmittanceWidth,
                float(atmosphereTransmittanceHeight - 1)/atmosphereTransmittanceHeight, 0.5f/atmosphereTransmittanceWidth,
                0.5f/atmosphereTransmittanceHeight);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atmosphereTransmittanceTex);

    vec suncolor = !atmosunlight.iszero() ? atmosunlight.tocolor().mul(atmosunlightscale) : sunlight.tocolor().mul(sunlightscale);
    extern float hdrgamma;
    vec sunscale = vec(suncolor).mul(ldrscale).pow(hdrgamma).mul(atmobright * 16);
    LOCALPARAM(sunlight, vec4(sunscale, alpha));
    vec normalizedsundir = sunlightdir;
    if(normalizedsundir.squaredlen() > 1e-8f) normalizedsundir.normalize();
    else normalizedsundir = vec(0, 0, 1);
    LOCALPARAM(sundir, normalizedsundir);

    // The shader applies camera-to-space transmittance and planet visibility.
    vec diskcolor = (!atmosundisk.iszero() ? atmosundisk.tocolor() : suncolor).mul(ldrscale).pow(hdrgamma).mul(atmosundiskbright * 4);
    LOCALPARAM(sundiskcolor, diskcolor);

    // convert from view cosine into mu^2 for limb darkening, where mu = sqrt(1 - sin^2) and sin^2 = 1 - cos^2, thus mu^2 = 1 - (1 - cos^2*scale)
    // convert corona offset into scale for mu^2, where sin = (1-corona) and thus mu^2 = 1 - (1-corona^2)
    float sundiskscale = sinf(0.5f*atmosundisksize*RAD);
    float coronamu = 1 - (1-atmosundiskcorona)*(1-atmosundiskcorona);
    if(sundiskscale > 0) LOCALPARAMF(sundiskparams, 1.0f/(sundiskscale*sundiskscale), 1.0f/max(coronamu, 1e-3f));
    else LOCALPARAMF(sundiskparams, 0, 0);

    gle::defvertex();
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(-1, 1, 1);
    gle::attribf(1, 1, 1);
    gle::attribf(-1, -1, 1);
    gle::attribf(1, -1, 1);
    xtraverts += gle::end();
}

VAR(showsky, 0, 1, 1);
VAR(clampsky, 0, 1, 1);
VARNR(skytexture, useskytexture, 0, 0, 1);
VARFR(skyshadow, 0, 0, 1, clearshadowcache());

int explicitsky = 0;

bool limitsky()
{
    return explicitsky && (useskytexture || (editmode && showsky));
}

void drawskybox(bool clear)
{
    bool havefaces = haveskyfaces();
    bool havenightsky = atmo && !havefaces && sunlightdir.z < 0 && loadnightsky();
    float nightfade = havenightsky ? clamp((-sunlightdir.z - 0.03f) / 0.17f, 0.0f, 1.0f) : 0.0f;
    nightfade *= nightfade*(3.0f - 2.0f*nightfade);
    bool limited = false;
    if(limitsky()) for(vtxarray *va = visibleva; va; va = va->next)
    {
        if(va->sky && va->occluded < OCCLUDE_BB &&
           ((va->skymax.x >= 0 && isvisiblebb(va->skymin, ivec(va->skymax).sub(va->skymin)) != VFC_NOT_VISIBLE) ||
            !insideworld(camera1->o)))
        {
            limited = true;
            break;
        }
    }
    if(limited)
    {
        glDisable(GL_DEPTH_TEST);
    }
    else
    {
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
    }

    if(clampsky) glDepthRange(1, 1);

    // A partially faded night texture needs a deterministic background before
    // the atmosphere transmits it. The ordinary opaque daytime atmosphere does
    // not: it deliberately overwrites stale HDR sky pixels below.
    if(clear || (!havefaces && (!atmo || atmoalpha < 1 || (havenightsky && nightfade < 1))))
    {
        vec skyboxcolor = skyboxcolour.tocolor().mul(ldrscale);
        glClearColor(skyboxcolor.x, skyboxcolor.y, skyboxcolor.z, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if(havefaces)
    {
        if(ldrscale < 1 && (skyboxoverbrightmin != 1 || (skyboxoverbright > 1 && skyboxoverbrightthreshold < 1)))
        {
            SETSHADER(skyboxoverbright);
            LOCALPARAMF(overbrightparams, skyboxoverbrightmin, max(skyboxoverbright, skyboxoverbrightmin), skyboxoverbrightthreshold);
        }
        else SETSHADER(skybox);

        gle::color(skyboxcolour);

        matrix4 skymatrix = cammatrix, skyprojmatrix;
        skymatrix.settranslation(0, 0, 0);
        skymatrix.rotate_around_z((spinsky*lastmillis/1000.0f+yawsky)*-RAD);
        skyprojmatrix.mul(projmatrix, skymatrix);
        LOCALPARAM(skymatrix, skyprojmatrix);

        drawenvbox(sky);
    }

    if(havenightsky) drawnightsky(nightfade);

    if(atmo && (!havefaces || atmoalpha < 1))
    {
        // The atmosphere shader outputs premultiplied in-scattering in RGB and
        // scalar view transmittance in A. This preserves the night sky behind
        // the atmosphere and keeps twilight alive below the geometric horizon.
        // Only use destination-transmittance blending when a valid background
        // was drawn this frame. With no skybox/night backdrop the HDR target can
        // contain last frame's clouds, so blending would create temporal trails.
        bool blendatmosphere = havefaces || havenightsky || atmoalpha < 1;
        if(blendatmosphere)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_SRC_ALPHA);
        }
        beginAtmosphereDebugTimer();
        drawatmosphere(atmoalpha);
        endAtmosphereDebugTimer();
        if(blendatmosphere) glDisable(GL_BLEND);
    }

    if(fogdomemax && !fogdomeclouds)
    {
        drawfogdome();
    }

    if(cloudbox[0])
    {
        SETSHADER(skybox);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        gle::color(cloudboxcolour.tocolor(), cloudboxalpha);

        matrix4 skymatrix = cammatrix, skyprojmatrix;
        skymatrix.settranslation(0, 0, 0);
        skymatrix.rotate_around_z((spinclouds*lastmillis/1000.0f+yawclouds)*-RAD);
        skyprojmatrix.mul(projmatrix, skymatrix);
        LOCALPARAM(skymatrix, skyprojmatrix);

        drawenvbox(clouds, cloudclip);

        glDisable(GL_BLEND);
    }

    if(cloudlayer[0] && cloudheight && (!volumetricClouds::volumetricclouds || !volumetricClouds::vcconfigured))
    {
        SETSHADER(skybox);

        glDisable(GL_CULL_FACE);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        matrix4 skymatrix = cammatrix, skyprojmatrix;
        skymatrix.settranslation(0, 0, 0);
        skymatrix.rotate_around_z((spincloudlayer*lastmillis/1000.0f+yawcloudlayer)*-RAD);
        skyprojmatrix.mul(projmatrix, skymatrix);
        LOCALPARAM(skymatrix, skyprojmatrix);

        drawenvoverlay(cloudoverlay, cloudoffsetx + cloudscrollx * lastmillis/1000.0f, cloudoffsety + cloudscrolly * lastmillis/1000.0f);

        glDisable(GL_BLEND);

        glEnable(GL_CULL_FACE);
    }

    if(fogdomemax && fogdomeclouds)
    {
        drawfogdome();
    }

    if(clampsky) glDepthRange(0, 1);

    if(limited)
    {
        glEnable(GL_DEPTH_TEST);
    }
    else
    {
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
    }
}

bool hasskybox()
{
    return haveskyfaces() || atmo || fogdomemax || cloudbox[0] || cloudlayer[0];
}
