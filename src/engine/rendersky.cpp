#include "engine.h"

Texture *sky[6] = { 0, 0, 0, 0, 0, 0 }, *clouds[6] = { 0, 0, 0, 0, 0, 0 };
static Texture *deepStarsTexture = NULL, *milkyWayTexture = NULL, *milkyWayNoiseTexture = NULL, *atmosphereMoonTexture = NULL;
extern bvec skyboxcolour;
extern int atmo;
extern float atmobright, atmohaze, atmodensity, atmoozone, atmoalpha, atmosunlightscale;
extern bvec atmosunlight;

static void cleanupAtmosphereDebugTimer();
static void cleanupDeepStarsDebugTimer();
static void cleanupMilkyWayDebugTimer();
static void cleanupRealStars();
static void cleanupAtmosphereRenderTarget();
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
                                            : getdirectionallightcolor().tocolor().mul(getdirectionallightscale());
        if(suncol.x + suncol.y + suncol.z <= 1e-4f) suncol = vec(1, 1, 1);

        float sunup = clamp(getdirectionallightdir().z * 0.5f + 0.5f, 0.0f, 1.0f);
        float sunset = 1.0f - smoothstepf(0.35f, 0.85f, sunup);
        float haze = clamp(atmohaze * 0.20f, 0.0f, 1.0f);
        float density = clamp(1.0f - exp2f(-0.35f*max(atmodensity, 0.0f)), 0.0f, 1.0f);
        float ozone = clamp(atmoozone, 0.0f, 1.0f);
        float atmosmix = clamp(0.35f + 0.65f*atmoalpha, 0.0f, 1.0f);
        float brightscale = clamp(atmobright, 0.0f, 8.0f);
        float customsun = !atmosunlight.iszero() ? 1.0f : 0.0f;

        vec2 sunh(getdirectionallightdir());
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
        front = vec2(getdirectionallightdir());
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
    cleanupAtmosphereRenderTarget();
    cleanupAtmosphereTransmittanceLUT();
    cleanupAtmosphereDebugTimer();
    cleanupDeepStarsDebugTimer();
    cleanupMilkyWayDebugTimer();
    cleanupRealStars();
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
VARFP(atmoviewsteps, 1, 24, 64, reloadatmosphereshader());
VARFP(atmosunsteps, 1, 8, 32, reloadatmosphereshader());
VARFP(atmosunlut, 0, 2, 2, cleanupAtmosphereTransmittanceLUT()); // 0 = 64x64, 1 = 128x32, 2 = 128x64
FVARP(atmoscale, 0.125f, 0.25f, 1.0f);
FVARR(atmoplanetsize, 1e-3f, 1, 1e3f);
FVARR(atmoheight, 1e-3f, 1, 1e3f);
FVARR(atmobright, 0, 1, 16);
CVAR1R(atmosunlight, 0);
FVARR(atmosunlightscale, 0, 1, 16);
CVAR1R(atmosundisk, 0);
FVARR(atmosundisksize, 0, 6, 90);
FVARR(atmosundiskcorona, 0, 0.6f, 1);
FVARR(atmosundiskbright, 0, 4, 16);
FVARR(atmomoon, 0, 0, 1);
FVARR(atmomoonsize, 0, 5.75, 90);
FVARR(atmohaze, 0, 0.1f, 16);
FVARR(atmodensity, 0, 1, 16);
FVARR(atmoozone, 0, 1, 16);
FVARR(atmomultiscatter, 0, 1, 2);
FVARR(atmomieanisotropy, -0.99f, 0.8f, 0.99f);
FVARR(atmotwilightmie, 0, 1.75f, 4);
FVARR(atmotwilightrayleigh, 0, 1, 4);
FVARR(atmotwilightantisolar, 0, 0.18f, 1);
FVARR(atmotwilightgrazing, 0.1f, 0.5f, 4);
FVARR(atmotwilightgreen, 0, 0.75f, 4);
FVARR(atmotwilightambient, 0, 1.5f, 4);
FVARR(atmocelestialcontrast, 0, 512, 1024);
FVARR(atmocelestialminvisibility, 0, 0.15f, 1);
FVARR(atmoalpha, 0, 1, 1);

// global sky values
FVAR(skyoffset, -360, 0, 360);
FVARR(skylatitude, -90, 45, 90);
FVARR(skydate, -365250, 0, 365250); // whole local-solar days since 2000-01-01
FVARR(skytime, 0, 0, 24); // local solar hours; the fixed reference longitude is zero

// real stars set in sky.cfg
VARR(realstars, 0, 1, 1);
FVAR(realstarsbright, 0, 12, 16);
FVAR(realstarssize, 0.5f, 3.0f, 8);
FVAR(realstarstwinkle, 0, 4, 8);
FVAR(realstarstwinklespeed, 0, 16, 32);
FVAR(realstarsmaglimit, -2, 5, 8);

// milky way rendering
VARR(milkyway, 0, 1, 1);
FVARR(milkywaybright, 0, 0.3f, 16);
FVARR(milkywaysaturation, 0, 4, 8);
FVARR(milkywaywidth, 5, 90, 180);
FVARR(milkywaydetail, 0, 2, 4);
FVARR(milkywaydust, 0, 0.6f, 1);
FVARR(milkywaycore, 0, 0.4f, 2);
FVARR(milkywaycorewarmth, 0, 0.5f, 2);
VARR(milkywaystars, 0, 1, 1);
FVARR(milkywaystarsbright, 0, 1, 16);
FVARR(milkywaystarsdensity, 0, 6, 16);
FVARR(milkywaystarssize, 0.25f, 0.75f, 6);
FVARR(milkywaystarsmaskpower, 0.25f, 1.5f, 4);
VARR(milkywaystarsseed, 0, 420, 0xFFFFFF);

// fake stars filling the sky
FVARR(deepstarssize, 2, 24, 512);
VARR(deepstarsseed, 0, 1337, 0xFFFFFF);
FVARR(deepstarsbright, 0, 0.75, 16);
VARR(deepstarsrotate, 0, 1, 1);
VARR(deepstarsflip, 0, 1, 1);

// debug
VAR(debugatmo, 0, 0, 1);
VAR(debugsky, 0, 0, 1);
VAR(showconstellations, 0, 0, 1);

static bool isSkyCalendarLeapYear(int year)
{
    return year%4 == 0 && (year%100 != 0 || year%400 == 0);
}

static int getSkyCalendarMonthDays(int month, int year)
{
    static const int monthdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if(month < 1 || month > 12) return 0;
    return month == 2 && isSkyCalendarLeapYear(year) ? 29 : monthdays[month - 1];
}

static bool isValidSkyCalendarDate(int day, int month, int year)
{
    return year >= 1000 && year <= 2999 && day >= 1 && day <= getSkyCalendarMonthDays(month, year);
}

static int skyCalendarDateToOffset(int day, int month, int year)
{
    // Fliegel-Van Flandern Gregorian calendar-to-Julian-day conversion.
    int a = (14 - month)/12, y = year + 4800 - a, m = month + 12*a - 3;
    int julianday = day + (153*m + 2)/5 + 365*y + y/4 - y/100 + y/400 - 32045;
    return julianday - 2451545;
}

static void skyCalendarOffsetToDate(int offset, int &day, int &month, int &year)
{
    int l = clamp(offset, -365250, 365250) + 2451545 + 68569;
    int n = 4*l/146097;
    l -= (146097*n + 3)/4;
    int i = 4000*(l + 1)/1461001;
    l = l - 1461*i/4 + 31;
    int j = 80*l/2447;
    day = l - 2447*j/80;
    l = j/11;
    month = j + 2 - 12*l;
    year = 100*(n - 49) + i + l;
}

ICOMMAND(skydatevalid, "iii", (int *day, int *month, int *year), intret(isValidSkyCalendarDate(*day, *month, *year) ? 1 : 0));
ICOMMAND(skymonthdays, "ii", (int *month, int *year), intret(getSkyCalendarMonthDays(*month, *year)));
ICOMMAND(skydatefromcalendar, "iii", (int *day, int *month, int *year),
{
    intret(isValidSkyCalendarDate(*day, *month, *year) ? skyCalendarDateToOffset(*day, *month, *year) : 0);
});
ICOMMAND(skydatecalendar, "i", (int *offset),
{
    int day;
    int month;
    int year;
    skyCalendarOffsetToDate(*offset, day, month, year);
    result(tempformatstring("%d %d %d", day, month, year));
});

struct RealStarVertex
{
    vec direction;
    bvec4 color;
    vec2 params;
    int id;

    RealStarVertex(const vec &direction, const bvec4 &color, float magnitude, float seed, int id)
        : direction(direction), color(color), params(magnitude, seed), id(id)
    {
    }
};

struct ConstellationSegment
{
    int first, second;

    ConstellationSegment(int first, int second) : first(first), second(second) {}
};

static vector<RealStarVertex> realStarCatalog;
static vector<ConstellationSegment> constellationCatalog;
static GLuint realStarsVBO = 0, constellationLinesVBO = 0;
static int constellationLineVertices = 0;
static bool realStarsCatalogLoaded = false, realStarsCatalogLoading = false;

static void addRealStar(int id, const char *constellation, float rightascension, float declination, float magnitude, float red, float green, float blue, int seed)
{
    if(!realStarsCatalogLoading || !constellation || !constellation[0]) return;
    float ra = rightascension*15.0f*RAD, dec = declination*RAD, cosdec = cosf(dec);
    vec direction(cosdec*cosf(ra), cosdec*sinf(ra), sinf(dec));
    bvec4 color(uchar(clamp(int(red*255.0f + 0.5f), 0, 255)), uchar(clamp(int(green*255.0f + 0.5f), 0, 255)),
                uchar(clamp(int(blue*255.0f + 0.5f), 0, 255)), 255);
    float phase = float(seed ? seed : id & 0xFFFF)/65535.0f;
    realStarCatalog.add(RealStarVertex(direction, color, magnitude, phase, id));
}

ICOMMAND(realstar, "isffffffi", (int *id, char *constellation, float *rightascension, float *declination, float *magnitude, float *red, float *green, float *blue, int *seed),
{
    addRealStar(*id, constellation, *rightascension, *declination, *magnitude, *red, *green, *blue, *seed);
});

ICOMMAND(constellationline, "ii", (int *first, int *second),
{
    if(realStarsCatalogLoading && *first != *second) constellationCatalog.add(ConstellationSegment(*first, *second));
});

static float getLocalSiderealDegrees()
{
    // IAU-compatible Greenwich mean sidereal angle at J2000. skydate is an
    // integer civil-date offset, while skytime and spinsky provide the daily
    // local-solar/game-time rotation without any fragment-shader trigonometry.
    float angle = 280.46061837f + 0.98564736629f*skydate + 15.04106864f*(skytime - 12.0f) + spinsky*lastmillis/1000.0f + skyoffset;
    angle = fmodf(angle, 360.0f);
    return angle < 0.0f ? angle + 360.0f : angle;
}

static void getDeepStarsTransforms(matrix3 &worldFromEquatorial, matrix3 *equatorialFromWorld = NULL)
{
    float latitude = skylatitude*RAD;
    float localSiderealAngle = (180.0f + skyoffset + spinsky*lastmillis/1000.0f)*RAD;
    float sinLatitude = sinf(latitude), cosLatitude = cosf(latitude);
    float sinSidereal = sinf(localSiderealAngle), cosSidereal = cosf(localSiderealAngle);
    worldFromEquatorial = matrix3(vec(-sinSidereal, -sinLatitude*cosSidereal, cosLatitude*cosSidereal), vec(cosSidereal, -sinLatitude*sinSidereal, cosLatitude*sinSidereal), vec(0, cosLatitude, sinLatitude));
    if(equatorialFromWorld) equatorialFromWorld->transpose(worldFromEquatorial);
}

static void getCelestialTransforms(matrix3 &worldFromEquatorial, matrix3 *equatorialFromWorld = NULL)
{
    float latitude = skylatitude*RAD;
    float localSiderealAngle = getLocalSiderealDegrees()*RAD;
    float sinLatitude = sinf(latitude), cosLatitude = cosf(latitude);
    float sinSidereal = sinf(localSiderealAngle), cosSidereal = cosf(localSiderealAngle);
    worldFromEquatorial = matrix3(vec(-sinSidereal, -sinLatitude*cosSidereal, cosLatitude*cosSidereal),
                                  vec(cosSidereal, -sinLatitude*sinSidereal, cosLatitude*sinSidereal),
                                  vec(0, cosLatitude, sinLatitude));
    if(equatorialFromWorld) equatorialFromWorld->transpose(worldFromEquatorial);
}

static void getGalacticFromWorld(matrix3 &galacticFromWorld)
{
    // Fixed IAU J2000 equatorial-to-galactic frame. +X is the Galactic Center,
    // +Z is the North Galactic Pole (RA 192.8595, Dec +27.1283 degrees).
    static const matrix3 galacticFromEquatorial(vec(-0.0548755604f, 0.4941094279f, -0.8676661490f),
                                                 vec(-0.8734370902f, -0.4448296300f, -0.1980763734f),
                                                 vec(-0.4838350155f, 0.7469822445f, 0.4559837762f));
    matrix3 worldFromEquatorial, equatorialFromWorld;
    getCelestialTransforms(worldFromEquatorial, &equatorialFromWorld);
    galacticFromWorld.mul(galacticFromEquatorial, equatorialFromWorld);
}

static vec getAtmosphereMoonDirection()
{
    return vec(moonlightyaw*RAD, moonlightpitch*RAD).normalize();
}

bool getatmospheremoon(vec &direction, float &halfangle)
{
    direction = getAtmosphereMoonDirection();
    halfangle = 0.5f*atmomoonsize*RAD;
    return atmo && atmomoon && atmoalpha > 0.0f && halfangle > 0.0f;
}

static float circleOverlapArea(float radius1, float radius2, float separation)
{
    if(radius1 <= 0.0f || radius2 <= 0.0f || separation >= radius1 + radius2) return 0.0f;
    if(separation <= fabsf(radius1 - radius2))
    {
        float radius = min(radius1, radius2);
        return M_PI*radius*radius;
    }

    float separationSquared = separation*separation, radius1Squared = radius1*radius1, radius2Squared = radius2*radius2;
    float angle1 = acosf(clamp((separationSquared + radius1Squared - radius2Squared)/(2.0f*separation*radius1), -1.0f, 1.0f));
    float angle2 = acosf(clamp((separationSquared + radius2Squared - radius1Squared)/(2.0f*separation*radius2), -1.0f, 1.0f));
    float lens = sqrtf(max((-separation + radius1 + radius2)*(separation + radius1 - radius2)*
                           (separation - radius1 + radius2)*(separation + radius1 + radius2), 0.0f));
    return radius1Squared*angle1 + radius2Squared*angle2 - 0.5f*lens;
}

float getsolareclipsevisibility(vec4 *disk)
{
    if(disk) *disk = vec4(0, 0, 0, 0);
    vec moondirection;
    float moonradius;
    if(!getatmospheremoon(moondirection, moonradius)) return 1.0f;

    float sunradius = 0.5f*atmosundisksize*RAD;
    if(sunradius <= 0.0f) return 1.0f;

    vec sundirection = sunlightdir;
    if(sundirection.squaredlen() <= 1.0e-8f) sundirection = vec(0, 0, 1);
    else sundirection.normalize();
    float separation = acosf(clamp(sundirection.dot(moondirection), -1.0f, 1.0f));
    if(separation >= sunradius + moonradius) return 1.0f;
    if(disk)
    {
        // Angular tangent-plane coordinates in solar-radius units; rotate into CSM XY at the caller.
        vec tangent = vec(moondirection).sub(vec(sundirection).mul(sundirection.dot(moondirection)));
        float length = tangent.magnitude();
        if(length > 1e-6f) tangent.mul(separation/(sunradius*length));
        else tangent = vec(0, 0, 0);
        *disk = vec4(tangent, moonradius/sunradius);
    }
    float covered = circleOverlapArea(sunradius, moonradius, separation)/(M_PI*sunradius*sunradius);
    return 1.0f - clamp(covered*atmoalpha, 0.0f, 1.0f);
}

static const int ATMOSPHERE_DEBUG_QUERY_COUNT = 3, ATMOSPHERE_DEBUG_TIMESTAMP_COUNT = 4;
static GLuint atmosphereDebugQueries[ATMOSPHERE_DEBUG_QUERY_COUNT][ATMOSPHERE_DEBUG_TIMESTAMP_COUNT] = { { 0 } };
static int atmosphereDebugQueryCycle = 0, atmosphereDebugQueryWaiting = 0, atmosphereDebugQueryActive = -1;
static Uint64 atmosphereDebugCPUStart = 0;
static float atmosphereDebugGPUMillis = -1.0f, atmosphereDebugRaymarchGPUMillis = -1.0f, atmosphereDebugUpscaleGPUMillis = -1.0f,
             atmosphereDebugSunGPUMillis = -1.0f, atmosphereDebugCPUMillis = 0.0f, atmosphereLUTRebuildMillis = 0.0f;

static GLuint atmosphereTransmittanceTex = 0, atmosphereTransmittanceFBO = 0;
static int atmosphereTransmittanceWidth = 0, atmosphereTransmittanceHeight = 0;
static GLuint atmosphereRenderTex = 0, atmosphereRenderFBO = 0;
static int atmosphereRenderWidth = 0, atmosphereRenderHeight = 0;

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

    bool matches(float newplanetradius, float newatmosphereradius, float newinverseRayleighScaleHeight, float newinverseMieScaleHeight, float newozonecenter, float newinverseOzoneHalfWidth, const vec &newbetarayleigh, const vec &newmieextinction, const vec &newbetaozone) const
    {
        return valid && sunsteps == atmosunsteps && planetradius == newplanetradius && atmosphereradius == newatmosphereradius &&
               inverseRayleighScaleHeight == newinverseRayleighScaleHeight && inverseMieScaleHeight == newinverseMieScaleHeight &&
               ozonecenter == newozonecenter && inverseOzoneHalfWidth == newinverseOzoneHalfWidth && betarayleigh == newbetarayleigh &&
               mieextinction == newmieextinction && betaozone == newbetaozone;
    }

    void update(float newplanetradius, float newatmosphereradius, float newinverseRayleighScaleHeight, float newinverseMieScaleHeight, float newozonecenter, float newinverseOzoneHalfWidth, const vec &newbetarayleigh, const vec &newmieextinction, const vec &newbetaozone)
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

static void updateAtmosphereTransmittanceLUT(float planetradius, float atmosphereradius, float inverseRayleighScaleHeight, float inverseMieScaleHeight, float ozonecenter, float inverseOzoneHalfWidth, const vec &betarayleigh, const vec &mieextinction, const vec &betaozone)
{
    int width, height;
    getAtmosphereTransmittanceLUTSize(width, height);
    bool resize = width != atmosphereTransmittanceWidth || height != atmosphereTransmittanceHeight;
    if(!resize && atmosphereTransmittanceCache.matches(planetradius, atmosphereradius, inverseRayleighScaleHeight, inverseMieScaleHeight, ozonecenter, inverseOzoneHalfWidth, betarayleigh, mieextinction, betaozone)) return;

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

    atmosphereTransmittanceCache.update(planetradius, atmosphereradius, inverseRayleighScaleHeight, inverseMieScaleHeight, ozonecenter, inverseOzoneHalfWidth, betarayleigh, mieextinction, betaozone);
    Uint64 frequency = SDL_GetPerformanceFrequency();
    atmosphereLUTRebuildMillis = frequency ? float((SDL_GetPerformanceCounter() - start) * 1000.0 / frequency) : 0.0f;
}

static void cleanupAtmosphereRenderTarget()
{
    if(atmosphereRenderFBO) { glDeleteFramebuffers_(1, &atmosphereRenderFBO); atmosphereRenderFBO = 0; }
    if(atmosphereRenderTex) { glDeleteTextures(1, &atmosphereRenderTex); atmosphereRenderTex = 0; }
    atmosphereRenderWidth = atmosphereRenderHeight = 0;
}

static void ensureAtmosphereRenderTarget()
{
    int width = max(int(ceilf(vieww*atmoscale)), 1), height = max(int(ceilf(viewh*atmoscale)), 1);
    if(atmosphereRenderTex && atmosphereRenderFBO && width == atmosphereRenderWidth && height == atmosphereRenderHeight) return;

    cleanupAtmosphereRenderTarget();
    atmosphereRenderWidth = width;
    atmosphereRenderHeight = height;

    GLint previousFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO);
    glActiveTexture_(GL_TEXTURE0);
    glGenTextures(1, &atmosphereRenderTex);
    createtexture(atmosphereRenderTex, width, height, NULL, 3, 1, hasAFBO && hasTF ? GL_RGBA16F : GL_RGBA8, GL_TEXTURE_RECTANGLE);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers_(1, &atmosphereRenderFBO);
    glBindFramebuffer_(GL_FRAMEBUFFER, atmosphereRenderFBO);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, atmosphereRenderTex, 0);
    if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("Failed allocating atmosphere render target!");
    glBindFramebuffer_(GL_FRAMEBUFFER, previousFBO);
}

static void pollAtmosphereDebugTimer()
{
    if(!debugatmo || !atmosphereDebugQueries[0][0]) return;

    loopi(ATMOSPHERE_DEBUG_QUERY_COUNT) if(atmosphereDebugQueryWaiting & (1 << i))
    {
        GLint available = 0;
        glGetQueryObjectiv_(atmosphereDebugQueries[i][3], GL_QUERY_RESULT_AVAILABLE, &available);
        if(!available) continue;

        GLuint64EXT start = 0, rayend = 0, upscaleend = 0, end = 0;
        glGetQueryObjectui64v_(atmosphereDebugQueries[i][0], GL_QUERY_RESULT, &start);
        glGetQueryObjectui64v_(atmosphereDebugQueries[i][1], GL_QUERY_RESULT, &rayend);
        glGetQueryObjectui64v_(atmosphereDebugQueries[i][2], GL_QUERY_RESULT, &upscaleend);
        glGetQueryObjectui64v_(atmosphereDebugQueries[i][3], GL_QUERY_RESULT, &end);
        atmosphereDebugGPUMillis = end >= start ? float(end - start) * 1.0e-6f : 0.0f;
        atmosphereDebugRaymarchGPUMillis = rayend >= start ? float(rayend - start) * 1.0e-6f : 0.0f;
        atmosphereDebugUpscaleGPUMillis = upscaleend >= rayend ? float(upscaleend - rayend) * 1.0e-6f : 0.0f;
        atmosphereDebugSunGPUMillis = end >= upscaleend ? float(end - upscaleend) * 1.0e-6f : 0.0f;
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
            glGenQueries_(ATMOSPHERE_DEBUG_QUERY_COUNT * ATMOSPHERE_DEBUG_TIMESTAMP_COUNT, &atmosphereDebugQueries[0][0]);
        if(!(atmosphereDebugQueryWaiting & (1 << atmosphereDebugQueryCycle)))
            atmosphereDebugQueryActive = atmosphereDebugQueryCycle;
    }
}

static void beginAtmosphereRaymarchDebugTimer()
{
    if(atmosphereDebugQueryActive >= 0)
        glQueryCounter_(atmosphereDebugQueries[atmosphereDebugQueryActive][0], GL_TIMESTAMP);
}

static void endAtmosphereRaymarchDebugTimer()
{
    if(atmosphereDebugQueryActive >= 0)
        glQueryCounter_(atmosphereDebugQueries[atmosphereDebugQueryActive][1], GL_TIMESTAMP);
}

static void endAtmosphereUpscaleDebugTimer()
{
    if(atmosphereDebugQueryActive >= 0)
        glQueryCounter_(atmosphereDebugQueries[atmosphereDebugQueryActive][2], GL_TIMESTAMP);
}

static void endAtmosphereSunDebugTimer()
{
    if(atmosphereDebugQueryActive >= 0)
        glQueryCounter_(atmosphereDebugQueries[atmosphereDebugQueryActive][3], GL_TIMESTAMP);
}

static void endAtmosphereDebugTimer()
{
    if(atmosphereDebugQueryActive >= 0)
    {
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
    if(atmosphereDebugQueries[0][0])
        glDeleteQueries_(ATMOSPHERE_DEBUG_QUERY_COUNT * ATMOSPHERE_DEBUG_TIMESTAMP_COUNT, &atmosphereDebugQueries[0][0]);
    memset(atmosphereDebugQueries, 0, sizeof(atmosphereDebugQueries));
    atmosphereDebugQueryCycle = 0;
    atmosphereDebugQueryWaiting = 0;
    atmosphereDebugQueryActive = -1;
    atmosphereDebugCPUStart = 0;
    atmosphereDebugGPUMillis = -1.0f;
    atmosphereDebugRaymarchGPUMillis = -1.0f;
    atmosphereDebugUpscaleGPUMillis = -1.0f;
    atmosphereDebugSunGPUMillis = -1.0f;
    atmosphereDebugCPUMillis = 0.0f;
}

void atmosphereDebugView()
{
    if(!debugatmo) return;

    pollAtmosphereDebugTimer();
    int y = debugsky ? 13*FONTH : 0;
    draw_text("Atmosphere", 0, y);
    y += FONTH;
    if(atmosphereDebugGPUMillis >= 0.0f) draw_textf("GPU total: %.2f ms", 0, y, atmosphereDebugGPUMillis);
    else draw_text("GPU total: n/a", 0, y);
    y += FONTH;
    if(atmosphereDebugRaymarchGPUMillis >= 0.0f) draw_textf("GPU raymarch: %.2f ms", 0, y, atmosphereDebugRaymarchGPUMillis);
    else draw_text("GPU raymarch: n/a", 0, y);
    y += FONTH;
    if(atmosphereDebugUpscaleGPUMillis >= 0.0f) draw_textf("GPU upscale/composite: %.2f ms", 0, y, atmosphereDebugUpscaleGPUMillis);
    else draw_text("GPU upscale/composite: n/a", 0, y);
    y += FONTH;
    if(atmosphereDebugSunGPUMillis >= 0.0f) draw_textf("GPU sun disk: %.2f ms", 0, y, atmosphereDebugSunGPUMillis);
    else draw_text("GPU sun disk: n/a", 0, y);
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
    draw_textf("Raymarch resolution: %d x %d", 0, y, atmosphereRenderWidth, atmosphereRenderHeight);
    y += FONTH;
    draw_textf("Scale: %.2f", 0, y, atmoscale);
    y += FONTH;
    draw_textf("Solar visibility: %.1f%%", 0, y, 100.0f*getsolareclipsevisibility());
    y += FONTH;
    draw_textf("LUT rebuild: %.2f ms", 0, y, atmosphereLUTRebuildMillis);
}

static const int DEEP_STARS_DEBUG_QUERY_COUNT = 3, MILKY_WAY_DEBUG_QUERY_COUNT = 3, REAL_STARS_DEBUG_QUERY_COUNT = 3;
static GLuint deepStarsDebugQueries[DEEP_STARS_DEBUG_QUERY_COUNT][2] = { { 0 } };
static int deepStarsDebugQueryCycle = 0, deepStarsDebugQueryWaiting = 0, deepStarsDebugQueryActive = -1;
static float deepStarsDebugGPUMillis = -1.0f;
static int deepStarsRenderedPatches = 0, deepStarsTilesPerFace = 0;
static GLuint milkyWayDebugQueries[MILKY_WAY_DEBUG_QUERY_COUNT][2] = { { 0 } };
static int milkyWayDebugQueryCycle = 0, milkyWayDebugQueryWaiting = 0, milkyWayDebugQueryActive = -1;
static float milkyWayDebugGPUMillis = -1.0f;
static GLuint realStarsDebugQueries[REAL_STARS_DEBUG_QUERY_COUNT][2] = { { 0 } };
static int realStarsDebugQueryCycle = 0, realStarsDebugQueryWaiting = 0, realStarsDebugQueryActive = -1;
static float realStarsDebugGPUMillis = -1.0f;

static void pollDeepStarsDebugTimer()
{
    if(!debugsky || !deepStarsDebugQueries[0][0]) return;

    loopi(DEEP_STARS_DEBUG_QUERY_COUNT) if(deepStarsDebugQueryWaiting & (1 << i))
    {
        GLint available = 0;
        glGetQueryObjectiv_(deepStarsDebugQueries[i][1], GL_QUERY_RESULT_AVAILABLE, &available);
        if(!available) continue;

        GLuint64EXT start = 0, end = 0;
        glGetQueryObjectui64v_(deepStarsDebugQueries[i][0], GL_QUERY_RESULT, &start);
        glGetQueryObjectui64v_(deepStarsDebugQueries[i][1], GL_QUERY_RESULT, &end);
        deepStarsDebugGPUMillis = end >= start ? float(end - start)*1.0e-6f : 0.0f;
        deepStarsDebugQueryWaiting &= ~(1 << i);
    }
}

static void beginDeepStarsDebugTimer()
{
    deepStarsDebugQueryActive = -1;
    if(!debugsky) return;

    pollDeepStarsDebugTimer();
    if(hasTQ && glQueryCounter_)
    {
        if(!deepStarsDebugQueries[0][0]) glGenQueries_(DEEP_STARS_DEBUG_QUERY_COUNT*2, &deepStarsDebugQueries[0][0]);
        if(!(deepStarsDebugQueryWaiting & (1 << deepStarsDebugQueryCycle)))
        {
            deepStarsDebugQueryActive = deepStarsDebugQueryCycle;
            glQueryCounter_(deepStarsDebugQueries[deepStarsDebugQueryActive][0], GL_TIMESTAMP);
        }
    }
}

static void endDeepStarsDebugTimer()
{
    if(deepStarsDebugQueryActive < 0) return;
    glQueryCounter_(deepStarsDebugQueries[deepStarsDebugQueryActive][1], GL_TIMESTAMP);
    deepStarsDebugQueryWaiting |= 1 << deepStarsDebugQueryActive;
    deepStarsDebugQueryCycle = (deepStarsDebugQueryActive + 1)%DEEP_STARS_DEBUG_QUERY_COUNT;
    deepStarsDebugQueryActive = -1;
}

static void cleanupDeepStarsDebugTimer()
{
    if(deepStarsDebugQueries[0][0]) glDeleteQueries_(DEEP_STARS_DEBUG_QUERY_COUNT*2, &deepStarsDebugQueries[0][0]);
    memset(deepStarsDebugQueries, 0, sizeof(deepStarsDebugQueries));
    deepStarsDebugQueryCycle = 0;
    deepStarsDebugQueryWaiting = 0;
    deepStarsDebugQueryActive = -1;
    deepStarsDebugGPUMillis = -1.0f;
}

static void pollMilkyWayDebugTimer()
{
    if(!debugsky || !milkyWayDebugQueries[0][0]) return;

    loopi(MILKY_WAY_DEBUG_QUERY_COUNT) if(milkyWayDebugQueryWaiting & (1 << i))
    {
        GLint available = 0;
        glGetQueryObjectiv_(milkyWayDebugQueries[i][1], GL_QUERY_RESULT_AVAILABLE, &available);
        if(!available) continue;

        GLuint64EXT start = 0, end = 0;
        glGetQueryObjectui64v_(milkyWayDebugQueries[i][0], GL_QUERY_RESULT, &start);
        glGetQueryObjectui64v_(milkyWayDebugQueries[i][1], GL_QUERY_RESULT, &end);
        milkyWayDebugGPUMillis = end >= start ? float(end - start)*1.0e-6f : 0.0f;
        milkyWayDebugQueryWaiting &= ~(1 << i);
    }
}

static void beginMilkyWayDebugTimer()
{
    milkyWayDebugQueryActive = -1;
    if(!debugsky) return;

    pollMilkyWayDebugTimer();
    if(hasTQ && glQueryCounter_)
    {
        if(!milkyWayDebugQueries[0][0]) glGenQueries_(MILKY_WAY_DEBUG_QUERY_COUNT*2, &milkyWayDebugQueries[0][0]);
        if(!(milkyWayDebugQueryWaiting & (1 << milkyWayDebugQueryCycle)))
        {
            milkyWayDebugQueryActive = milkyWayDebugQueryCycle;
            glQueryCounter_(milkyWayDebugQueries[milkyWayDebugQueryActive][0], GL_TIMESTAMP);
        }
    }
}

static void endMilkyWayDebugTimer()
{
    if(milkyWayDebugQueryActive < 0) return;
    glQueryCounter_(milkyWayDebugQueries[milkyWayDebugQueryActive][1], GL_TIMESTAMP);
    milkyWayDebugQueryWaiting |= 1 << milkyWayDebugQueryActive;
    milkyWayDebugQueryCycle = (milkyWayDebugQueryActive + 1)%MILKY_WAY_DEBUG_QUERY_COUNT;
    milkyWayDebugQueryActive = -1;
}

static void cleanupMilkyWayDebugTimer()
{
    if(milkyWayDebugQueries[0][0]) glDeleteQueries_(MILKY_WAY_DEBUG_QUERY_COUNT*2, &milkyWayDebugQueries[0][0]);
    memset(milkyWayDebugQueries, 0, sizeof(milkyWayDebugQueries));
    milkyWayDebugQueryCycle = 0;
    milkyWayDebugQueryWaiting = 0;
    milkyWayDebugQueryActive = -1;
    milkyWayDebugGPUMillis = -1.0f;
}

static void pollRealStarsDebugTimer()
{
    if(!debugsky || !realStarsDebugQueries[0][0]) return;

    loopi(REAL_STARS_DEBUG_QUERY_COUNT) if(realStarsDebugQueryWaiting & (1 << i))
    {
        GLint available = 0;
        glGetQueryObjectiv_(realStarsDebugQueries[i][1], GL_QUERY_RESULT_AVAILABLE, &available);
        if(!available) continue;

        GLuint64EXT start = 0, end = 0;
        glGetQueryObjectui64v_(realStarsDebugQueries[i][0], GL_QUERY_RESULT, &start);
        glGetQueryObjectui64v_(realStarsDebugQueries[i][1], GL_QUERY_RESULT, &end);
        realStarsDebugGPUMillis = end >= start ? float(end - start)*1.0e-6f : 0.0f;
        realStarsDebugQueryWaiting &= ~(1 << i);
    }
}

static void beginRealStarsDebugTimer()
{
    realStarsDebugQueryActive = -1;
    if(!debugsky) return;

    pollRealStarsDebugTimer();
    if(hasTQ && glQueryCounter_)
    {
        if(!realStarsDebugQueries[0][0]) glGenQueries_(REAL_STARS_DEBUG_QUERY_COUNT*2, &realStarsDebugQueries[0][0]);
        if(!(realStarsDebugQueryWaiting & (1 << realStarsDebugQueryCycle)))
        {
            realStarsDebugQueryActive = realStarsDebugQueryCycle;
            glQueryCounter_(realStarsDebugQueries[realStarsDebugQueryActive][0], GL_TIMESTAMP);
        }
    }
}

static void endRealStarsDebugTimer()
{
    if(realStarsDebugQueryActive < 0) return;
    glQueryCounter_(realStarsDebugQueries[realStarsDebugQueryActive][1], GL_TIMESTAMP);
    realStarsDebugQueryWaiting |= 1 << realStarsDebugQueryActive;
    realStarsDebugQueryCycle = (realStarsDebugQueryActive + 1)%REAL_STARS_DEBUG_QUERY_COUNT;
    realStarsDebugQueryActive = -1;
}

static void cleanupRealStarsDebugTimer()
{
    if(realStarsDebugQueries[0][0]) glDeleteQueries_(REAL_STARS_DEBUG_QUERY_COUNT*2, &realStarsDebugQueries[0][0]);
    memset(realStarsDebugQueries, 0, sizeof(realStarsDebugQueries));
    realStarsDebugQueryCycle = 0;
    realStarsDebugQueryWaiting = 0;
    realStarsDebugQueryActive = -1;
    realStarsDebugGPUMillis = -1.0f;
}

static bool ensureRealStars()
{
    if(!realStarsCatalogLoaded)
    {
        realStarsCatalogLoading = true;
        realStarCatalog.setsize(0);
        constellationCatalog.setsize(0);
        execfile("config/sky.cfg", false);
        realStarsCatalogLoading = false;
        realStarsCatalogLoaded = true;
    }
    if(!realStarCatalog.length()) return false;
    if(!realStarsVBO)
    {
        glGenBuffers_(1, &realStarsVBO);
        gle::bindvbo(realStarsVBO);
        glBufferData_(GL_ARRAY_BUFFER, realStarCatalog.length()*sizeof(RealStarVertex), realStarCatalog.getbuf(), GL_STATIC_DRAW);
        gle::clearvbo();
    }
    return true;
}

static const RealStarVertex *findRealStar(int id)
{
    loopv(realStarCatalog) if(realStarCatalog[i].id == id) return &realStarCatalog[i];
    return NULL;
}

static bool ensureConstellationLines()
{
    if(!ensureRealStars() || !constellationCatalog.length()) return false;
    if(!constellationLinesVBO)
    {
        vector<vec> vertices;
        loopv(constellationCatalog)
        {
            const RealStarVertex *first = findRealStar(constellationCatalog[i].first),
                                 *second = findRealStar(constellationCatalog[i].second);
            if(!first || !second) continue;
            vertices.add(first->direction);
            vertices.add(second->direction);
        }
        constellationLineVertices = vertices.length();
        if(!constellationLineVertices) return false;
        glGenBuffers_(1, &constellationLinesVBO);
        gle::bindvbo(constellationLinesVBO);
        glBufferData_(GL_ARRAY_BUFFER, constellationLineVertices*sizeof(vec), vertices.getbuf(), GL_STATIC_DRAW);
        gle::clearvbo();
    }
    return constellationLineVertices > 0;
}

static void cleanupRealStars()
{
    if(realStarsVBO) { glDeleteBuffers_(1, &realStarsVBO); realStarsVBO = 0; }
    if(constellationLinesVBO) { glDeleteBuffers_(1, &constellationLinesVBO); constellationLinesVBO = 0; }
    constellationLineVertices = 0;
    cleanupRealStarsDebugTimer();
}

static bool loadDeepStars()
{
    if(!deepStarsTexture) deepStarsTexture = textureload("packages/sky/deep_stars.png", 3, true, false);
    return deepStarsTexture != notexture;
}

static void drawDeepStars(float alpha)
{
    if(!loadDeepStars()) return;

    SETSHADER(deepstars);

    matrix4 deepstarsmatrix = invcammatrix;
    deepstarsmatrix.settranslation(0, 0, 0);
    deepstarsmatrix.mul(invprojmatrix);
    LOCALPARAM(deepstarsmatrix, deepstarsmatrix);
    matrix3 worldFromEquatorial, equatorialFromWorld;
    getDeepStarsTransforms(worldFromEquatorial, &equatorialFromWorld);
    LOCALPARAM(deepstarsrotation, equatorialFromWorld);
    LOCALPARAMF(deepstarsparams, float(deepStarsTilesPerFace), float(deepstarsseed), deepstarsbright, alpha);
    LOCALPARAMF(deepstarsoptions, float(deepstarsrotate), float(deepstarsflip));

    glBindTexture(GL_TEXTURE_2D, deepStarsTexture->id);

    if(alpha < 1)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    beginDeepStarsDebugTimer();
    gle::defvertex();
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(-1, 1, 1);
    gle::attribf(1, 1, 1);
    gle::attribf(-1, -1, 1);
    gle::attribf(1, -1, 1);
    xtraverts += gle::end();
    endDeepStarsDebugTimer();

    if(alpha < 1) glDisable(GL_BLEND);
}

static bool loadMilkyWay()
{
    // Clamp only vertically: galactic longitude must wrap at the mask seam.
    if(!milkyWayTexture) milkyWayTexture = textureload("packages/sky/milky_way.png", 2, true, false);
    if(!milkyWayNoiseTexture) milkyWayNoiseTexture = textureload("packages/noise/grass_wind.jpg", 0, true, false);
    return milkyWayTexture != notexture && milkyWayNoiseTexture != notexture;
}

static void drawMilkyWay(float nightfade)
{
    if(!milkyway || nightfade <= 0.0f || !loadMilkyWay()) return;

    SETSHADER(milkyway);
    matrix4 milkywaymatrix = invcammatrix;
    milkywaymatrix.settranslation(0, 0, 0);
    milkywaymatrix.mul(invprojmatrix);
    LOCALPARAM(milkywaymatrix, milkywaymatrix);
    matrix3 galacticFromWorld;
    getGalacticFromWorld(galacticFromWorld);
    LOCALPARAM(milkywayrotation, galacticFromWorld);
    LOCALPARAMF(milkywayparams, milkywaybright*ldrscale, milkywaysaturation, 0.5f*milkywaywidth*RAD, milkywaydetail);
    LOCALPARAMF(milkywayparams2, milkywaydust, milkywaycore, milkywaycorewarmth, nightfade);
    LOCALPARAMF(milkywaystarsparams, milkywaystars ? milkywaystarsbright*ldrscale : 0.0f, milkywaystarsdensity, milkywaystarssize,
                milkywaystarsmaskpower);
    LOCALPARAMF(milkywaystarseedparam, float(milkywaystarsseed));

    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, milkyWayNoiseTexture->id);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, milkyWayTexture->id);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    beginMilkyWayDebugTimer();
    gle::defvertex();
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(-1, 1, 1);
    gle::attribf(1, 1, 1);
    gle::attribf(-1, -1, 1);
    gle::attribf(1, -1, 1);
    xtraverts += gle::end();
    endMilkyWayDebugTimer();
    glDisable(GL_BLEND);
}

static void drawRealStars(float nightfade)
{
    if(!realstars || nightfade <= 0.0f || !ensureRealStars()) return;

    SETSHADER(realstars);
    matrix3 worldFromEquatorial;
    getCelestialTransforms(worldFromEquatorial);
    matrix4 realstarsmatrix = cammatrix;
    realstarsmatrix.settranslation(0, 0, 0);
    realstarsmatrix.mul(worldFromEquatorial);
    matrix4 projected;
    projected.mul(projmatrix, realstarsmatrix);
    LOCALPARAM(realstarsmatrix, projected);
    LOCALPARAM(realstarsworld, worldFromEquatorial);
    const float referenceFov = 100.0f;
    float fovscale = clamp(tanf(0.5f*referenceFov*RAD)/max(tanf(0.5f*curfov*RAD), 1.0e-4f), 0.25f, 8.0f);
    LOCALPARAMF(realstarsparams, realstarsbright*ldrscale, realstarssize*fovscale, realstarsmaglimit, nightfade);
    bool havetransmittance = atmosphereTransmittanceTex && atmosphereTransmittanceWidth > 0 && atmosphereTransmittanceHeight > 0;
    LOCALPARAMF(realstarstwinkleparams, realstarstwinkle, realstarstwinklespeed, lastmillis/1000.0f, havetransmittance ? 1.0f : 0.0f);
    if(havetransmittance)
    {
        LOCALPARAMF(atmospherelutparams, float(atmosphereTransmittanceWidth - 1)/atmosphereTransmittanceWidth,
                    float(atmosphereTransmittanceHeight - 1)/atmosphereTransmittanceHeight, 0.5f/atmosphereTransmittanceWidth,
                    0.5f/atmosphereTransmittanceHeight);
        float planetradius = 6360.0f*atmoplanetsize;
        LOCALPARAMF(atmosphereradii, planetradius, planetradius + 100.0f*atmoheight);
    }
    else
    {
        LOCALPARAMF(atmospherelutparams, 1.0f, 1.0f, 0.0f, 0.0f);
        LOCALPARAMF(atmosphereradii, 1.0f, 1.0f);
    }
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, havetransmittance ? atmosphereTransmittanceTex : notexture->id);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    GLboolean programpointsize = glIsEnabled(GL_PROGRAM_POINT_SIZE);
    if(!programpointsize) glEnable(GL_PROGRAM_POINT_SIZE);

    gle::bindvbo(realStarsVBO);
    const RealStarVertex *star = 0;
    glVertexAttribPointer_(gle::ATTRIB_VERTEX, 3, GL_FLOAT, GL_FALSE, sizeof(RealStarVertex), star->direction.v);
    glVertexAttribPointer_(gle::ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RealStarVertex), star->color.v);
    glVertexAttribPointer_(gle::ATTRIB_TEXCOORD0, 2, GL_FLOAT, GL_FALSE, sizeof(RealStarVertex), star->params.v);
    glEnableVertexAttribArray_(gle::ATTRIB_VERTEX);
    glEnableVertexAttribArray_(gle::ATTRIB_COLOR);
    glEnableVertexAttribArray_(gle::ATTRIB_TEXCOORD0);
    beginRealStarsDebugTimer();
    glDrawArrays(GL_POINTS, 0, realStarCatalog.length());
    endRealStarsDebugTimer();
    xtraverts += realStarCatalog.length();
    glDisableVertexAttribArray_(gle::ATTRIB_VERTEX);
    glDisableVertexAttribArray_(gle::ATTRIB_COLOR);
    glDisableVertexAttribArray_(gle::ATTRIB_TEXCOORD0);
    gle::clearvbo();

    if(!programpointsize) glDisable(GL_PROGRAM_POINT_SIZE);
    glDisable(GL_BLEND);
}

static void drawConstellationLines(float nightfade)
{
    if(!realstars || !showconstellations || nightfade <= 0.0f || !ensureConstellationLines()) return;

    SETSHADER(constellationlines);
    matrix3 worldFromEquatorial;
    getCelestialTransforms(worldFromEquatorial);
    matrix4 realstarsmatrix = cammatrix;
    realstarsmatrix.settranslation(0, 0, 0);
    realstarsmatrix.mul(worldFromEquatorial);
    matrix4 projected;
    projected.mul(projmatrix, realstarsmatrix);
    LOCALPARAM(realstarsmatrix, projected);
    LOCALPARAM(realstarsworld, worldFromEquatorial);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    GLboolean linesmooth = glIsEnabled(GL_LINE_SMOOTH);
    if(!linesmooth) glEnable(GL_LINE_SMOOTH);
    gle::bindvbo(constellationLinesVBO);
    gle::enablevertex();
    gle::vertexpointer(sizeof(vec), (const vec *)0);

    // A broad low-alpha pass gives the links a restrained halo without a blur texture.
    LOCALPARAMF(constellationparams, ldrscale, nightfade, 0.10f);
    glLineWidth(4.25f);
    glDrawArrays(GL_LINES, 0, constellationLineVertices);

    // Keep the readable line faint and soft so it does not compete with the real stars.
    LOCALPARAMF(constellationparams, ldrscale, nightfade, 0.32f);
    glLineWidth(1.75f);
    glDrawArrays(GL_LINES, 0, constellationLineVertices);
    xtraverts += 2*constellationLineVertices;
    gle::disablevertex();
    gle::clearvbo();
    glLineWidth(1.0f);
    if(!linesmooth) glDisable(GL_LINE_SMOOTH);
    glDisable(GL_BLEND);
}

void skyDebugView()
{
    if(!debugsky) return;

    pollDeepStarsDebugTimer();
    pollMilkyWayDebugTimer();
    pollRealStarsDebugTimer();
    int y = 0;
    draw_text("Sky", 0, y);
    y += FONTH;
    if(deepStarsDebugGPUMillis >= 0.0f) draw_textf("Deep stars GPU: %.2f ms", 0, y, deepStarsDebugGPUMillis);
    else draw_text("Deep stars GPU: n/a", 0, y);
    y += FONTH;
    if(milkyWayDebugGPUMillis >= 0.0f) draw_textf("Milky Way GPU: %.2f ms", 0, y, milkyWayDebugGPUMillis);
    else draw_text("Milky Way GPU: n/a", 0, y);
    y += FONTH;
    if(realStarsDebugGPUMillis >= 0.0f) draw_textf("Real stars GPU: %.2f ms", 0, y, realStarsDebugGPUMillis);
    else draw_text("Real stars GPU: n/a", 0, y);
    y += FONTH;
    if(milkyWayTexture && milkyWayTexture != notexture)
        draw_textf("Milky Way mask: %d x %d", 0, y, milkyWayTexture->w, milkyWayTexture->h);
    else draw_text("Milky Way mask: 2048 x 512 (not loaded)", 0, y);
    y += FONTH;
    draw_textf("Milky Way width: %.2f degrees", 0, y, milkywaywidth);
    y += FONTH;
    draw_textf("Milky Way detail: %s", 0, y,
               milkywaydetail > 0.0f || milkywaydust > 0.0f || milkywaycore > 0.0f ? "2-sample medium + fine" : "off");
    y += FONTH;
    draw_textf("Sky latitude: %.2f degrees", 0, y, skylatitude);
    y += FONTH;
    draw_textf("Sidereal rotation: %.2f degrees", 0, y, getLocalSiderealDegrees());
    y += FONTH;
    draw_textf("Deep stars patches: %d (%d x %d per face)", 0, y, deepStarsRenderedPatches, deepStarsTilesPerFace, deepStarsTilesPerFace);
    y += FONTH;
    float patchsize = deepStarsTilesPerFace ? 90.0f/deepStarsTilesPerFace : deepstarssize;
    draw_textf("Deep stars patch size: %.2f degrees", 0, y, patchsize);
    y += FONTH;
    if(deepStarsTexture && deepStarsTexture != notexture)
        draw_textf("Deep stars texture: %d x %d", 0, y, deepStarsTexture->w, deepStarsTexture->h);
    else draw_text("Deep stars texture: not loaded", 0, y);
    y += FONTH;
    draw_textf("Deep stars seed: %d", 0, y, deepstarsseed);
}

static bool calcAtmosphereCelestialScissor(const vec &direction, float angularsize, int &x, int &y, int &w, int &h)
{
    float halfangle = clamp(0.5f*angularsize*RAD, 1.0e-5f, 0.49f*float(M_PI)), coshalf = cosf(halfangle), sinhalf = sinf(halfangle);
    vec tangent = fabsf(direction.z) < 0.999f ? vec(-direction.y, direction.x, 0).normalize() : vec(1, 0, 0),
        bitangent = vec().cross(direction, tangent).normalize();
    float minx = 1.0e16f, miny = 1.0e16f, maxx = -1.0e16f, maxy = -1.0e16f;
    bool projected = false, clipped = false;
    const int edgepoints = 16;
    loopi(edgepoints + 1)
    {
        vec sampledir = direction;
        if(i)
        {
            float angle = 2.0f*M_PI*float(i - 1)/edgepoints;
            sampledir.mul(coshalf).madd(tangent, sinhalf*cosf(angle)).madd(bitangent, sinhalf*sinf(angle));
        }
        vec samplepoint(camera1->o);
        samplepoint.madd(sampledir, max(nearplane*4.0f, 1.0f));
        vec4 clip;
        camprojmatrix.transform(samplepoint, clip);
        if(clip.w <= 1.0e-4f)
        {
            clipped = true;
            continue;
        }
        float ndcx = clip.x/clip.w, ndcy = clip.y/clip.w;
        minx = min(minx, ndcx);
        miny = min(miny, ndcy);
        maxx = max(maxx, ndcx);
        maxy = max(maxy, ndcy);
        projected = true;
    }
    if(!projected) return false;
    if(clipped) { minx = miny = -1.0f; maxx = maxy = 1.0f; }

    const int margin = 3;
    int left = clamp(int(floorf((minx*0.5f + 0.5f)*vieww)) - margin, 0, vieww),
        bottom = clamp(int(floorf((miny*0.5f + 0.5f)*viewh)) - margin, 0, viewh),
        right = clamp(int(ceilf((maxx*0.5f + 0.5f)*vieww)) + margin, 0, vieww),
        top = clamp(int(ceilf((maxy*0.5f + 0.5f)*viewh)) + margin, 0, viewh);
    x = left;
    y = bottom;
    w = max(right - left, 0);
    h = max(top - bottom, 0);
    return w > 0 && h > 0;
}

static bool beginAtmosphereCelestialScissor(const vec &direction, float angularsize, GLboolean &hadscissor, GLint oldscissor[4])
{
    int x, y, w, h;
    if(!calcAtmosphereCelestialScissor(direction, angularsize, x, y, w, h)) return false;
    hadscissor = glIsEnabled(GL_SCISSOR_TEST);
    if(hadscissor)
    {
        glGetIntegerv(GL_SCISSOR_BOX, oldscissor);
        int right = min(x + w, oldscissor[0] + oldscissor[2]), top = min(y + h, oldscissor[1] + oldscissor[3]);
        x = max(x, oldscissor[0]);
        y = max(y, oldscissor[1]);
        w = max(right - x, 0);
        h = max(top - y, 0);
        if(!w || !h) return false;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    return true;
}

static void endAtmosphereCelestialScissor(GLboolean hadscissor, const GLint oldscissor[4])
{
    if(hadscissor) glScissor(oldscissor[0], oldscissor[1], oldscissor[2], oldscissor[3]);
    else glDisable(GL_SCISSOR_TEST);
}

static void drawAtmosphereCelestialQuad()
{
    gle::defvertex();
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(-1, 1, 1);
    gle::attribf(1, 1, 1);
    gle::attribf(-1, -1, 1);
    gle::attribf(1, -1, 1);
    xtraverts += gle::end();
}

static void setAtmosphereCelestialParams(const matrix4 &celestialmatrix, float planetradius, float atmosphereradius, float alpha)
{
    LOCALPARAM(celestialmatrix, celestialmatrix);
    LOCALPARAMF(atmosphereparams, planetradius, atmosphereradius, 0.0f, 0.0f);
    LOCALPARAMF(atmospherelutparams, float(atmosphereTransmittanceWidth - 1)/atmosphereTransmittanceWidth,
                float(atmosphereTransmittanceHeight - 1)/atmosphereTransmittanceHeight, 0.5f/atmosphereTransmittanceWidth,
                0.5f/atmosphereTransmittanceHeight);
    LOCALPARAMF(celestialalpha, alpha);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atmosphereTransmittanceTex);
}

static void drawAtmosphereSun(const matrix4 &celestialmatrix, const vec &direction, const vec &moondirection, const vec &suncolor,
                              float planetradius, float atmosphereradius, float alpha)
{
    float sundiskscale = sinf(0.5f*atmosundisksize*RAD);
    if(!moonlight.iszero() || alpha <= 0.0f || sundiskscale <= 0.0f || atmosundiskbright <= 0.0f) return;

    extern float hdrgamma;
    vec diskcolor = vec(!atmosundisk.iszero() ? atmosundisk.tocolor() : suncolor).pow(hdrgamma).mul(ldrscale).mul(atmosundiskbright*4);
    float coshalf = sqrtf(max(1.0f - sundiskscale*sundiskscale, 0.0f)), horizontal = sqrtf(max(1.0f - direction.z*direction.z, 0.0f));
    if(diskcolor.iszero() || direction.z*coshalf + horizontal*sundiskscale <= 0.0f) return;

    GLboolean hadscissor = GL_FALSE;
    GLint oldscissor[4] = { 0, 0, vieww, viewh };
    if(!beginAtmosphereCelestialScissor(direction, atmosundisksize, hadscissor, oldscissor)) return;

    float coronamu = 1 - (1-atmosundiskcorona)*(1-atmosundiskcorona);
    GLboolean hadblend = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    SETSHADER(atmospheresundisk);
    setAtmosphereCelestialParams(celestialmatrix, planetradius, atmosphereradius, alpha);
    LOCALPARAM(sundir, direction);
    LOCALPARAM(moondir, moondirection);
    LOCALPARAM(sundiskcolor, diskcolor);
    LOCALPARAMF(sundiskparams, 1.0f/(sundiskscale*sundiskscale), 1.0f/max(coronamu, 1e-3f));
    float moonhalfangle = 0.5f*atmomoonsize*RAD;
    if(atmomoon && moonhalfangle > 0.0f)
    {
        float feather = min(max(moonhalfangle*0.015f, 0.00005f), moonhalfangle*0.5f);
        LOCALPARAMF(solareclipseparams, cosf(moonhalfangle + feather), cosf(max(moonhalfangle - feather, 0.0f)), alpha);
    }
    else LOCALPARAMF(solareclipseparams, 1.0f, 1.0f, 0.0f);
    drawAtmosphereCelestialQuad();
    if(hadblend) glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    else glDisable(GL_BLEND);
    endAtmosphereCelestialScissor(hadscissor, oldscissor);
}

static bool loadAtmosphereMoon()
{
    if(!atmosphereMoonTexture) atmosphereMoonTexture = textureload("packages/sky/moon.png", 3, true, false);
    return atmosphereMoonTexture != notexture;
}

static void drawAtmosphereMoon(const matrix4 &celestialmatrix, const vec &sundirection, const vec &suncolor, float planetradius,
                               float atmosphereradius, float alpha)
{
    float halfangle = 0.5f*atmomoonsize*RAD, sinhalf = sinf(halfangle);
    if(!atmomoon || alpha <= 0.0f || sinhalf <= 0.0f) return;

    float yaw = moonlightyaw*RAD, pitch = moonlightpitch*RAD;
    vec direction(yaw, pitch), moonright(cosf(yaw), sinf(yaw), 0),
        moonup(sinf(yaw)*sinf(pitch), -cosf(yaw)*sinf(pitch), cosf(pitch));
    float coshalf = cosf(halfangle), horizontal = sqrtf(max(1.0f - direction.z*direction.z, 0.0f));
    if(direction.z*coshalf + horizontal*sinhalf <= 0.0f || !loadAtmosphereMoon()) return;

    // Moon radiance is constant, but its apparent contrast collapses against a
    // daylight atmosphere. Keep a trace of night-side earthshine near new moon.
    float daylight = clamp((sundirection.z + 0.08f)/0.20f, 0.0f, 1.0f);
    daylight *= daylight*(3.0f - 2.0f*daylight);
    daylight *= clamp(atmobright*alpha, 0.0f, 1.0f);
    float apparentbrightness = 1.4f + (0.18f - 1.4f)*daylight, earthshine = 0.012f*(1.0f - daylight);
    vec moonlightcolor = suncolor;
    extern float hdrgamma;
    moonlightcolor.pow(hdrgamma);
    float maxsuncolor = max(max(moonlightcolor.x, moonlightcolor.y), moonlightcolor.z);
    if(maxsuncolor > 1.0e-4f) moonlightcolor.mul(1.0f/maxsuncolor);
    else moonlightcolor = vec(1);

    GLboolean hadscissor = GL_FALSE;
    GLint oldscissor[4] = { 0, 0, vieww, viewh };
    if(!beginAtmosphereCelestialScissor(direction, atmomoonsize, hadscissor, oldscissor)) return;

    GLboolean hadblend = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    SETSHADER(atmospheremoon);
    setAtmosphereCelestialParams(celestialmatrix, planetradius, atmosphereradius, alpha);
    LOCALPARAM(moondir, direction);
    LOCALPARAM(moonright, moonright);
    LOCALPARAM(moonup, moonup);
    LOCALPARAM(sundir, sundirection);
    LOCALPARAM(moonlightcolor, moonlightcolor);
    LOCALPARAMF(moonparams, 0.5f/sinhalf, coshalf, apparentbrightness, earthshine);
    float umbraangle = min(halfangle*2.65f, float(M_PI)), penumbraangle = min(halfangle*4.6f, float(M_PI));
    LOCALPARAMF(lunareclipseparams, 2.0f*sinf(0.5f*umbraangle), 2.0f*sinf(0.5f*penumbraangle));
    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, atmosphereMoonTexture->id);
    glActiveTexture_(GL_TEXTURE0);
    drawAtmosphereCelestialQuad();
    if(hadblend) glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    else glDisable(GL_BLEND);
    endAtmosphereCelestialScissor(hadscissor, oldscissor);
}

static void drawatmosphere(float alpha = atmoalpha)
{
    ensureAtmosphereRenderTarget();

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
    // Strengthen the green-band ozone extinction. Its small vertical effect is
    // amplified naturally along grazing solar paths, where the unwanted
    // yellow-green twilight cast occurs.
    betao.y *= 1.0f + max(atmotwilightgreen, 0.0f);
    updateAtmosphereTransmittanceLUT(planetradius, planetradius + atmosphereheight, inverseRayleighScaleHeight, inverseMieScaleHeight,
                                    25.0f*atmoheight, inverseOzoneHalfWidth, betar, betamextinction, betao);

    GLint previousFBO = 0, previousViewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    GLboolean blend = glIsEnabled(GL_BLEND), depthtest = glIsEnabled(GL_DEPTH_TEST), scissortest = glIsEnabled(GL_SCISSOR_TEST);
    glBindFramebuffer_(GL_FRAMEBUFFER, atmosphereRenderFBO);
    glViewport(0, 0, atmosphereRenderWidth, atmosphereRenderHeight);
    if(blend) glDisable(GL_BLEND);
    if(depthtest) glDisable(GL_DEPTH_TEST);
    if(scissortest) glDisable(GL_SCISSOR_TEST);

    SETSHADER(atmosphere);

    matrix4 sunmatrix = invcammatrix;
    sunmatrix.settranslation(0, 0, 0);
    sunmatrix.mul(invprojmatrix);
    LOCALPARAM(sunmatrix, sunmatrix);
    LOCALPARAMF(atmosphereparams, planetradius, planetradius + atmosphereheight, inverseRayleighScaleHeight, inverseMieScaleHeight);
    LOCALPARAMF(atmosphereparams2, 25.0f*atmoheight, inverseOzoneHalfWidth, clamp(atmomultiscatter, 0.0f, 2.0f),
                clamp(atmomieanisotropy, -0.99f, 0.99f));
    LOCALPARAMF(atmospherecontrastparams, atmocelestialcontrast, atmocelestialminvisibility);
    LOCALPARAMF(atmospheretwilightparams, atmotwilightmie, atmotwilightrayleigh, atmotwilightantisolar,
                atmotwilightgrazing*RAD);
    LOCALPARAMF(atmospheretwilightambient, atmotwilightambient);
    LOCALPARAM(betarayleigh, betar);
    LOCALPARAM(betamie, betam);
    LOCALPARAM(mieextinction, betamextinction);
    LOCALPARAM(betaozone, betao);
    LOCALPARAMF(atmospherelutparams, float(atmosphereTransmittanceWidth - 1)/atmosphereTransmittanceWidth,
                float(atmosphereTransmittanceHeight - 1)/atmosphereTransmittanceHeight, 0.5f/atmosphereTransmittanceWidth,
                0.5f/atmosphereTransmittanceHeight);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atmosphereTransmittanceTex);

    vec suncolor = !atmosunlight.iszero() ? atmosunlight.tocolor().mul(atmosunlightscale)
                                        : getdirectionallightcolor().tocolor().mul(getdirectionallightscale());
    extern float hdrgamma;
    vec sunscale = vec(suncolor).pow(hdrgamma).mul(ldrscale).mul(atmobright * 16);
    vec normalizedsundir = sunlightdir;
    if(normalizedsundir.squaredlen() > 1e-8f) normalizedsundir.normalize();
    else normalizedsundir = vec(0, 0, 1);
    // Scattering follows the active light; the solar direction still controls celestial geometry and lunar phase.
    LOCALPARAM(sundir, getdirectionallightdir());
    vec normalizedmoondir = getAtmosphereMoonDirection();
    float eclipsevisibility = getdirectionallightvisibility();

    // A total eclipse retains a small cool multiple-scattered component from
    // the uneclipsed horizon while direct solar energy follows disk coverage.
    float totality = 1.0f - eclipsevisibility;
    totality *= totality*(3.0f - 2.0f*totality);
    float atmosphericsun = 0.015f + 0.985f*eclipsevisibility;
    sunscale.mul(atmosphericsun).mul(vec(1.0f - 0.45f*totality, 1.0f - 0.25f*totality, 1.0f));
    LOCALPARAM(sunlight, vec4(sunscale, alpha));

    beginAtmosphereRaymarchDebugTimer();
    gle::defvertex();
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(-1, 1, 1);
    gle::attribf(1, 1, 1);
    gle::attribf(-1, -1, 1);
    gle::attribf(1, -1, 1);
    xtraverts += gle::end();
    endAtmosphereRaymarchDebugTimer();

    glBindFramebuffer_(GL_FRAMEBUFFER, previousFBO);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    if(blend) glEnable(GL_BLEND);
    if(depthtest) glEnable(GL_DEPTH_TEST);
    if(scissortest) glEnable(GL_SCISSOR_TEST);

    // The moon is an opaque celestial foreground over the night-sky texture.
    // Draw it before the atmosphere so stars are occluded first, then apply
    // atmospheric transmittance and in-scattering to the combined background.
    drawAtmosphereMoon(sunmatrix, normalizedsundir, suncolor, planetradius, planetradius + atmosphereheight, alpha);

    // The source already contains premultiplied radiance and destination
    // transmittance. One bilinear sample reconstructs it; z=1 lets the existing
    // sky depth test reject geometry without any depth-texture fetches.
    SETSHADER(atmosphereupscale);
    LOCALPARAMF(atmosphereupscalesize, float(atmosphereRenderWidth), float(atmosphereRenderHeight));
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE, atmosphereRenderTex);
    gle::defvertex();
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(-1, 1, 1);
    gle::attribf(1, 1, 1);
    gle::attribf(-1, -1, 1);
    gle::attribf(1, -1, 1);
    xtraverts += gle::end();
    endAtmosphereUpscaleDebugTimer();

    drawAtmosphereSun(sunmatrix, normalizedsundir, normalizedmoondir, suncolor, planetradius, planetradius + atmosphereheight, alpha);
    endAtmosphereSunDebugTimer();
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
    bool havenightsky = atmo && !havefaces && sunlightdir.z < 0;
    bool havedeepstars = havenightsky && loadDeepStars();
    bool havemilkyway = havenightsky && milkyway && loadMilkyWay();
    bool havecelestialbackground = havedeepstars || havemilkyway;
    deepStarsTilesPerFace = max(int(ceilf(90.0f/deepstarssize)), 1);
    deepStarsRenderedPatches = havedeepstars ? 6*deepStarsTilesPerFace*deepStarsTilesPerFace : 0;
    bool havemoon = atmo && atmomoon && atmoalpha > 0.0f && atmomoonsize > 0.0f;
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
    if(clear || havemoon || (!havefaces && (!atmo || atmoalpha < 1 || (havecelestialbackground && nightfade < 1))))
    {
        // With an opaque procedural atmosphere the physical boundary radiance is
        // black. Feeding skyboxcolour through its transmittance contaminates
        // twilight and lets the moon reveal a dark cutout in that artificial
        // backdrop when it occludes stars.
        bool opaqueatmosphere = atmo && !havefaces && atmoalpha >= 1.0f;
        vec skyboxcolor = opaqueatmosphere ? vec(0) : skyboxcolour.tocolor().mul(ldrscale);
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

    if(havenightsky)
    {
        if(havedeepstars) drawDeepStars(nightfade);
        if(havemilkyway) drawMilkyWay(nightfade);
        drawConstellationLines(nightfade);
        drawRealStars(nightfade);
    }

    if(atmo && (!havefaces || atmoalpha < 1))
    {
        // The atmosphere shader outputs premultiplied in-scattering in RGB and
        // scalar view transmittance in A. This preserves the night sky behind
        // the atmosphere and keeps twilight alive below the geometric horizon.
        // Only use destination-transmittance blending when a valid background
        // was drawn this frame. With no skybox/night backdrop the HDR target can
        // contain last frame's clouds, so blending would create temporal trails.
        bool blendatmosphere = havefaces || havecelestialbackground || havemoon || atmoalpha < 1;
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
