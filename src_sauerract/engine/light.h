struct PackNode
{
    PackNode *child1, *child2;
    ushort x, y, w, h;
    int available;

    PackNode(ushort x, ushort y, ushort w, ushort h) : child1(0), child2(0), x(x), y(y), w(w), h(h), available(min(w, h)) {}

    void discardchildren()
    {
        DELETEP(child1);
        DELETEP(child2);
    }

    void forceempty()
    {
        discardchildren();
        available = 0;
    }

    void reset()
    {
        discardchildren();
        available = min(w, h);
    }

    bool resize(int nw, int nh)
    {
        if(w == nw && h == nw) return false;
        discardchildren();
        w = nw;
        h = nh;
        available = min(w, h);
        return true;
    }

    ~PackNode()
    {
        discardchildren();
    }

    bool insert(ushort &tx, ushort &ty, ushort tw, ushort th);
    void reserve(ushort tx, ushort ty, ushort tw, ushort th);
};

extern bvec ambient, skylight, sunlight, moonlight;
extern float ambientscale, skylightscale, sunlightscale, moonlightscale;
extern float sunlightyaw, sunlightpitch, moonlightyaw, moonlightpitch;
extern vec sunlightdir, moonlightdir;
extern bool getatmospheremoon(vec &direction, float &halfangle);
extern float getsolareclipsevisibility(vec4 *disk = NULL);

// Lighting effects share one directional source; celestial positions remain independent.
inline const bvec &getdirectionallightcolor()
{
    return moonlight.iszero() ? sunlight : moonlight;
}

inline const vec &getdirectionallightdir()
{
    return moonlight.iszero() ? sunlightdir : moonlightdir;
}

inline float getdirectionallightscale()
{
    return moonlight.iszero() ? sunlightscale : moonlightscale;
}

inline float getdirectionallightyaw()
{
    return moonlight.iszero() ? sunlightyaw : moonlightyaw;
}

inline float getdirectionallightpitch()
{
    return moonlight.iszero() ? sunlightpitch : moonlightpitch;
}

extern float getdirectionallightvisibility(vec4 *disk = NULL);
extern int fullbright, fullbrightlevel;

extern void clearlights();
extern void initlights();
extern void brightencube(cube &c);
extern void setsurfaces(cube &c, const surfaceinfo *surfs, const vertinfo *verts, int numverts);
extern void setsurface(cube &c, int orient, const surfaceinfo &surf, const vertinfo *verts, int numverts);
extern void previewblends(const ivec &bo, const ivec &bs);

extern void calcnormals(bool lerptjoints = false);
extern void clearnormals();
extern void resetsmoothgroups();
extern int smoothangle(int id, int angle);
extern void findnormal(const vec &key, int smooth, const vec &surface, vec &v);

#define CHECK_CALCLIGHT_PROGRESS_LOCKED(exit, show_calclight_progress, before, after) \
    if(check_calclight_progress) \
    { \
        if(!calclight_canceled) \
        { \
            before; \
            show_calclight_progress(); \
            check_calclight_canceled(); \
            after; \
        } \
        if(calclight_canceled) { exit; } \
    }
#define CHECK_CALCLIGHT_PROGRESS(exit, show_calclight_progress) CHECK_CALCLIGHT_PROGRESS_LOCKED(exit, show_calclight_progress, , )

extern bool calclight_canceled;
extern volatile bool check_calclight_progress;

extern void check_calclight_canceled();


// A deterministic snapshot. Render passes consume the collected lightinfo made from this state.
struct animatedlightstate
{
    vec o, color, secondarycolor;
};
extern animatedlightstate evaluatelight(const extentity &e, int millis);

// Conservative source displacement for visibility culling and prebuilt shadow geometry.
inline vec lightmovementbounds(const extentity &e)
{
    const lightanimation &a = e.animation;
    vec movement(0, 0, 0);
    if(a.offsetamp > 0 && a.offsetfrequency > 0)
    {
        movement = vec(a.offsetaxes).abs().mul(a.offsetamp);
        if(a.offsetquantize > 0) movement.add(0.5f*a.offsetquantize);
        loopi(3) movement[i] += 1e-6f*max(fabsf(e.o[i]), 1.0f);
    }
    return movement;
}
