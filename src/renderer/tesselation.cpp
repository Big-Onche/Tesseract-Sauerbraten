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
VARP(tesselationadaptive, 0, 1, 1);
VARP(tesselationmin, 1, 1, 64);
VARFP(tesselationmax, 1, 64, 64,
{
    if(tesselationmaxlevel > 0) tesselationmax = min(tesselationmax, tesselationmaxlevel);
});
FVARP(tesselationpixels, 1.0f, 8.0f, 256.0f);
FVARP(tesselationtexels, 1.0f, 8.0f, 256.0f);
FVARP(tesselationworld, 1.0f, 32.0f, 4096.0f);
FVARP(tesselationscale, 0.0f, 1.0f, 16.0f);
FVARP(tesselationheightlod, 0.0f, 0.0f, 12.0f);
VARP(tesselationautolod, 0, 1, 1);
FVARP(tesselationlodbias, -4.0f, -0.5f, 4.0f);
VARP(tesselationdisplacement, 0, 1, 1);
FVARFP(tesselationdistance, 0.0f, 256.0f, 1048576.0f, clearshadowcache());
FVARFP(tesselationfade, 0.0f, 128.0f, 1048576.0f, clearshadowcache());
VARFP(tesselationseams, 0, 1, 1,
{
    allchanged();
    clearshadowcache();
});
FVARFP(tesselationseamwidth, 0.0f, 4.0f, 1024.0f, clearshadowcache());
FVARFP(tesselationseamangle, 0.0f, 30.0f, 180.0f,
{
    allchanged();
    clearshadowcache();
});
VARFP(tesselationremesh, 0, 0, 1, allchanged());
FVARFP(tesselationpatchsize, 4.0f, 32.0f, 512.0f, allchanged());
VARFP(tesselationremeshquality, 0, 1, 2, allchanged());
VARFP(tesselationshadows, 0, 1, 1,
{
    clearshadowcache();
});
// 0 off, 1 wireframe shadow casters
VARP(tesselationshadowdebug, 0, 0, 1);
// 0 off, 1 generated grid, 2 height, 3 displacement, 4 base edges, 5 factors, 6 hardware limit, 7 density, 8 mip,
// 9 original normal, 10 rasterized geometric normal, 11 reconstructed normal, 12 material normal, 13 depth, 14 tangent frame,
// 15 seam distance, 16 final seam attenuation, 17 base displacement, 18 secondary displacement, 19 blend weight,
// 20 final blended displacement
VARP(tesselationdebug, 0, 0, 20);

extern void *getprocaddress(const char *name);
extern bool hasext(const char *ext);

namespace renderer
{
    namespace tesselation
    {
        struct remeshedge
        {
            ushort a, b;

            remeshedge() {}
            remeshedge(ushort x, ushort y) : a(min(x, y)), b(max(x, y)) {}

            bool operator==(const remeshedge &o) const { return a == o.a && b == o.b; }
        };

        static inline uint hthash(const remeshedge &e) { return uint(e.a) | (uint(e.b)<<16); }
        static inline bool htcmp(const remeshedge &a, const remeshedge &b) { return a == b; }

        struct geometricedge
        {
            vec a, b;

            geometricedge() {}
            geometricedge(const vec &x, const vec &y)
            {
                bool ordered = x.x < y.x || (x.x == y.x && (x.y < y.y || (x.y == y.y && x.z <= y.z)));
                a = ordered ? x : y;
                b = ordered ? y : x;
            }

            bool operator==(const geometricedge &o) const { return a == o.a && b == o.b; }
        };

        static inline uint hthash(const geometricedge &e) { return hthash(e.a) ^ (hthash(e.b)*0x9E3779B9U); }
        static inline bool htcmp(const geometricedge &a, const geometricedge &b) { return a == b; }

        struct seamedge
        {
            geometricedge edge;
            ushort a, b;
            const VSlot *base, *layer;
            bool displaced;
            int next;

            seamedge() : a(0), b(0), base(NULL), layer(NULL), displaced(false), next(-1) {}
        };

        static vector<seamedge> seamedges;
        static hashtable<geometricedge, int> seamheads;
        static bool remeshsession = false;

        struct remeshstats
        {
            int originaltris, patches, vertices, boundaryedges, degenerates, elongated, millis, seamcompatible, seamincompatible,
                seamopen, qualitybins[5], sizebins[3];
            double edgesum, qualitysum;
            float maxedge, minquality;

            void reset()
            {
                originaltris = patches = vertices = boundaryedges = degenerates = elongated = millis = seamcompatible = seamincompatible =
                    seamopen = 0;
                loopi(5) qualitybins[i] = 0;
                loopi(3) sizebins[i] = 0;
                edgesum = qualitysum = 0.0;
                maxedge = 0.0f;
                minquality = 1.0f;
            }
        };

        static remeshstats remeshinfo;
        static Shader *lastworldbase = NULL, *lastworldshader = NULL;
        static GLint previouspatchvertices = 3, previouspolygonmode[2] = { GL_FILL, GL_FILL };
        static bool worldactive = false, shadowactive = false, wireframeactive = false;

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
            tesselationmax = clamp(tesselationmax, 1, tesselationmaxlevel);
            conoutf(CON_INIT, "Hardware tessellation: available (max level %d, max patch vertices %d)", tesselationmaxlevel,
                    tesselationmaxpatchvertices);
        }

        void cleanup()
        {
            lastworldbase = lastworldshader = NULL;
            worldactive = shadowactive = wireframeactive = false;
        }

        bool enabled()
        {
            return ::tesselation && tesselationsupported && glPatchParameteri_ && !drawtex;
        }

        bool displacing()
        {
            return enabled() && tesselationdisplacement && tesselationscale > 0.0f && tesselationdistance > 0.0f;
        }

        bool shadowsenabled()
        {
            return displacing() && tesselationshadows;
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

        static const float *findparam(const Slot &slot, const VSlot &vslot, const char *name)
        {
            loopv(vslot.params) if(vslot.params[i].name && !strcmp(vslot.params[i].name, name)) return vslot.params[i].val;
            loopv(slot.params) if(slot.params[i].name && !strcmp(slot.params[i].name, name)) return slot.params[i].val;
            loopv(slot.shader->defaultparams)
                if(slot.shader->defaultparams[i].name && !strcmp(slot.shader->defaultparams[i].name, name)) return slot.shader->defaultparams[i].val;
            return NULL;
        }

        static bool candisplace(const Slot &slot)
        {
            return slot.shader && hasheightmap(slot) && hasparam(*slot.shader, "parallaxscale");
        }

        static void resolvelayers(const VSlot &vslot, const VSlot *other, int layermask, const VSlot *&base, const VSlot *&layer)
        {
            if(!other)
            {
                base = &vslot;
                layer = NULL;
            }
            else if(layermask&LAYER_TOP)
            {
                base = &vslot;
                layer = other;
            }
            else
            {
                base = other;
                layer = &vslot;
            }
        }

        bool eligible(const VSlot &vslot, const VSlot *other, int layermask, const ivec &bbmin, const ivec &bbmax)
        {
            const VSlot *base, *layer;
            resolvelayers(vslot, other, layermask, base, layer);
            bool displaced = (base->slot && candisplace(*base->slot)) || (layer && layer->slot && candisplace(*layer->slot));
            return enabled() && tesselationdistance > 0.0f && camera1 && displaced &&
                   camera1->o.dist_to_bb(bbmin, bbmax) < tesselationdistance;
        }

        static float trianglequality(const vec &a, const vec &b, const vec &c)
        {
            vec ab = vec(b).sub(a), ac = vec(c).sub(a), cross;
            cross.cross(ab, ac);
            float edgesq = ab.squaredlen() + vec(c).sub(b).squaredlen() + ac.squaredlen();
            return edgesq > 1e-12f ? 2.0f*sqrtf(3.0f)*cross.magnitude()/edgesq : 0.0f;
        }

        static vertex midpoint(const vertex &a, const vertex &b)
        {
            vertex v;
            v.pos = vec(a.pos).add(b.pos).mul(0.5f);
            v.tc = vec(a.tc).add(b.tc).mul(0.5f);
            v.norm.lerp(a.norm, b.norm, 0.5f);
            v.tangent.lerp(a.tangent, b.tangent, 0.5f);
            v.tangent.w = a.tangent.w == b.tangent.w ? a.tangent.w : (a.tangent.w >= 128 ? 255 : 0);
            return v;
        }

        static void addtriangle(vector<ushort> &out, ushort a, ushort b, ushort c)
        {
            if(a == b || b == c || c == a) return;
            out.add(a); out.add(b); out.add(c);
        }

        static ushort splitedge(vector<vertex> &verts, hashtable<remeshedge, ushort> &midpoints, ushort a, ushort b)
        {
            remeshedge edge(a, b);
            ushort *found = midpoints.access(edge);
            if(found) return *found;
            if(verts.length() >= USHRT_MAX) return USHRT_MAX;
            ushort index = ushort(verts.length());
            verts.add(midpoint(verts[a], verts[b]));
            midpoints[edge] = index;
            return index;
        }

        static bool refine(vector<vertex> &verts, vector<ushort> &indices, float target)
        {
            // Split a geometric edge once for every incident triangle. Repeating this pass produces a conforming dyadic mesh and
            // makes boundary subdivision deterministic across material and vertex-array boundaries without moving the source surface.
            const float targetsq = target*target;
            vector<ushort> output;
            hashtable<remeshedge, ushort> midpoints;
            bool changed = false;
            output.reserve(indices.length()*2);
            for(int i = 0; i+2 < indices.length(); i += 3)
            {
                ushort a = indices[i], b = indices[i+1], c = indices[i+2], ab = USHRT_MAX, bc = USHRT_MAX, ca = USHRT_MAX;
                bool sab = vec(verts[b].pos).sub(verts[a].pos).squaredlen() > targetsq,
                     sbc = vec(verts[c].pos).sub(verts[b].pos).squaredlen() > targetsq,
                     sca = vec(verts[a].pos).sub(verts[c].pos).squaredlen() > targetsq;
                int mask = (sab ? 1 : 0) | (sbc ? 2 : 0) | (sca ? 4 : 0);
                if(!mask) { addtriangle(output, a, b, c); continue; }
                changed = true;
                if(sab) ab = splitedge(verts, midpoints, a, b);
                if(sbc) bc = splitedge(verts, midpoints, b, c);
                if(sca) ca = splitedge(verts, midpoints, c, a);
                if((sab && ab == USHRT_MAX) || (sbc && bc == USHRT_MAX) || (sca && ca == USHRT_MAX)) return false;
                switch(mask)
                {
                    case 1: addtriangle(output, a, ab, c); addtriangle(output, ab, b, c); break;
                    case 2: addtriangle(output, b, bc, a); addtriangle(output, bc, c, a); break;
                    case 4: addtriangle(output, c, ca, b); addtriangle(output, ca, a, b); break;
                    case 3: addtriangle(output, b, bc, ab); addtriangle(output, a, ab, c); addtriangle(output, ab, bc, c); break;
                    case 5: addtriangle(output, a, ab, ca); addtriangle(output, ab, b, c); addtriangle(output, ab, c, ca); break;
                    case 6: addtriangle(output, c, ca, bc); addtriangle(output, a, b, ca); addtriangle(output, b, bc, ca); break;
                    case 7:
                        addtriangle(output, a, ab, ca); addtriangle(output, ab, b, bc);
                        addtriangle(output, ca, bc, c); addtriangle(output, ab, bc, ca);
                        break;
                }
            }
            if(changed)
            {
                indices.setsize(0);
                indices.move(output);
            }
            return true;
        }

        struct edgeowner
        {
            int triangle;
            ushort third;
            bool forward;

            edgeowner() : triangle(-1), third(0), forward(false) {}
        };

        static bool flipqualityedges(vector<vertex> &verts, vector<ushort> &indices, float target)
        {
            // A flip preserves the source surface only for a strictly convex, coplanar, attribute-compatible quad. Concave pairs are
            // common around octree boundaries; flipping those moves both triangles outside their original union and exposes a hole.
            hashtable<remeshedge, edgeowner> owners;
            vector<uchar> used;
            used.growbuf(indices.length()/3);
            loopi(indices.length()/3) used.add(0);
            bool changed = false;
            for(int i = 0; i+2 < indices.length(); i += 3)
            {
                ushort tri[3] = { indices[i], indices[i+1], indices[i+2] };
                loopj(3)
                {
                    ushort x = tri[j], y = tri[(j+1)%3], opposite = tri[(j+2)%3];
                    remeshedge edge(x, y);
                    edgeowner *owner = owners.access(edge);
                    if(!owner)
                    {
                        edgeowner &added = owners[edge];
                        added.triangle = i/3;
                        added.third = opposite;
                        added.forward = x == edge.a;
                        continue;
                    }
                    int first = owner->triangle, second = i/3;
                    if(used[first] || used[second] || owner->forward == (x == edge.a)) continue;
                    ushort c = owner->third, d = opposite;
                    if(c == d) continue;
                    const vertex &va = verts[edge.a], &vb = verts[edge.b], &vc = verts[c], &vd = verts[d];
                    if(va.norm != vb.norm || va.norm != vc.norm || va.norm != vd.norm ||
                       va.tangent != vb.tangent || va.tangent != vc.tangent || va.tangent != vd.tangent)
                        continue;
                    const vec &pa = verts[edge.a].pos, &pb = verts[edge.b].pos, &pc = verts[c].pos, &pd = verts[d].pos;
                    if(pc.squaredist(pd) > target*target*1.0001f) continue;
                    vec n1, n2;
                    n1.cross(vec(pb).sub(pa), vec(pc).sub(pa));
                    n2.cross(vec(pa).sub(pb), vec(pd).sub(pb));
                    float n1len = n1.magnitude(), n2len = n2.magnitude();
                    if(n1len <= 1e-6f || n2len <= 1e-6f || n1.dot(n2) < n1len*n2len*0.9995f) continue;
                    vec facenormal = n1;
                    if(!owner->forward) facenormal.neg();
                    vec newdiagonal = vec(pd).sub(pc), toca = vec(pa).sub(pc), tocb = vec(pb).sub(pc), aside, bside;
                    aside.cross(newdiagonal, toca);
                    bside.cross(newdiagonal, tocb);
                    float asideplane = aside.dot(facenormal), bsideplane = bside.dot(facenormal);
                    float sidetolerance = facenormal.magnitude()*newdiagonal.magnitude()*max(toca.magnitude(), tocb.magnitude())*1e-5f;
                    if(fabsf(asideplane) <= sidetolerance || fabsf(bsideplane) <= sidetolerance ||
                       (asideplane < 0.0f) == (bsideplane < 0.0f))
                        continue;
                    float oldquality = min(trianglequality(pa, pb, pc), trianglequality(pb, pa, pd));
                    float newquality = min(trianglequality(pc, pa, pd), trianglequality(pc, pd, pb));
                    if(newquality <= oldquality + 0.01f) continue;
                    ushort firsttri[3] = { c, edge.a, d }, secondtri[3] = { c, d, edge.b };
                    vec firstnormal, secondnormal;
                    firstnormal.cross(vec(verts[firsttri[1]].pos).sub(verts[firsttri[0]].pos),
                                      vec(verts[firsttri[2]].pos).sub(verts[firsttri[0]].pos));
                    secondnormal.cross(vec(verts[secondtri[1]].pos).sub(verts[secondtri[0]].pos),
                                       vec(verts[secondtri[2]].pos).sub(verts[secondtri[0]].pos));
                    if(firstnormal.dot(facenormal) < 0.0f) swap(firsttri[1], firsttri[2]);
                    if(secondnormal.dot(facenormal) < 0.0f) swap(secondtri[1], secondtri[2]);
                    loopk(3) { indices[3*first+k] = firsttri[k]; indices[3*second+k] = secondtri[k]; }
                    used[first] = used[second] = 1;
                    changed = true;
                    break;
                }
            }
            return changed;
        }

        static void recordseamedges(VSlot &vslot, VSlot *other, int layermask, vector<vertex> &verts, vector<ushort> &indices, bool displaced)
        {
            if(!remeshsession) return;
            const VSlot *base, *layer;
            resolvelayers(vslot, other, layermask, base, layer);
            for(int i = 0; i+2 < indices.length(); i += 3) loopj(3)
            {
                ushort a = indices[i+j], b = indices[i+(j+1)%3];
                if(a == b) continue;
                seamedge &entry = seamedges.add();
                entry.edge = geometricedge(verts[a].pos, verts[b].pos);
                entry.a = a;
                entry.b = b;
                entry.base = base;
                entry.layer = layer;
                entry.displaced = displaced;
                int *head = seamheads.access(entry.edge);
                entry.next = head ? *head : -1;
                seamheads[entry.edge] = seamedges.length()-1;
            }
        }

        static bool seamendpointcompatible(const vertex &a, const vertex &b, float normalthreshold)
        {
            vec an = bvec(a.norm).tonormal(), bn = bvec(b.norm).tonormal();
            float lengths = an.magnitude()*bn.magnitude();
            return lengths > 1e-6f && an.dot(bn) >= normalthreshold*lengths;
        }

        static bool seamedgegeometrycompatible(const seamedge &a, const seamedge &b, vector<vertex> &verts, float normalthreshold)
        {
            bool sameorder = verts[a.a].pos == verts[b.a].pos;
            const vertex &aa = verts[a.a], &ab = verts[a.b], &ba = verts[sameorder ? b.a : b.b], &bb = verts[sameorder ? b.b : b.a];
            return seamendpointcompatible(aa, ba, normalthreshold) && seamendpointcompatible(ab, bb, normalthreshold);
        }

        static bool sameblendpair(const seamedge &a, const seamedge &b)
        {
            return a.base == b.base && a.layer == b.layer;
        }

        static bool blendselectedalongedge(const seamedge &blended, const seamedge &plain, vector<vertex> &verts, BlendMapCache *cache)
        {
            int selected = plain.base == blended.base ? 0xFF : (plain.base == blended.layer ? 0 : -1);
            if(selected < 0 || plain.layer) return false;
            const vec &start = verts[blended.a].pos, &end = verts[blended.b].pos;
            int samples = max(int(ceilf(start.dist(end))), 1);
            loopi(samples + 1)
            {
                vec position(start);
                position.lerp(end, float(i)/samples);
                if(lookupblendmap(cache, position) != selected) return false;
            }
            return true;
        }

        void beginremesh(vector<vertex> &verts)
        {
            remeshsession = tesselationseams != 0;
            seamedges.setsize(0);
            seamheads.clear();
            loopv(verts) verts[i].tc.z = -1.0f;
        }

        void endremesh(vector<vertex> &verts)
        {
            if(!remeshsession) return;
            const float infinity = 1e16f;
            vector<float> distances;
            vector<uchar> relevant, queued;
            vector<vector<ushort> > adjacency;
            distances.growbuf(verts.length());
            relevant.growbuf(verts.length());
            queued.growbuf(verts.length());
            adjacency.growbuf(verts.length());
            loopv(verts)
            {
                distances.add(infinity);
                relevant.add(0);
                queued.add(0);
                adjacency.add();
            }
            loopv(seamedges) if(seamedges[i].displaced)
            {
                const seamedge &edge = seamedges[i];
                relevant[edge.a] = relevant[edge.b] = 1;
                adjacency[edge.a].add(edge.b);
                adjacency[edge.b].add(edge.a);
            }

            float normalthreshold = cosf(tesselationseamangle*RAD);
            BlendMapCache *blendcache = newblendmapcache();
            setblendmaporigin(blendcache, ivec(0, 0, 0), worldsize);
            enumeratekt(seamheads, geometricedge, edge, int, head,
            {
                (void)edge;
                vector<int> group;
                for(int current = head; current >= 0; current = seamedges[current].next) group.add(current);
                int count = group.length();
                int first = count ? group[0] : -1;
                bool compatible = false;
                if(first >= 0)
                {
                    const seamedge &reference = seamedges[first];
                    bool samesignature = reference.displaced && count == (reference.layer ? 4 : 2);
                    loopv(group) if(group[i] != first &&
                                    (!sameblendpair(reference, seamedges[group[i]]) ||
                                     !seamedgegeometrycompatible(reference, seamedges[group[i]], verts, normalthreshold)))
                        samesignature = false;
                    compatible = samesignature;
                }
                if(!compatible && count == 3)
                {
                    int blended = -1;
                    int otherblended = -1;
                    int plain = -1;
                    int blendcount = 0;
                    int plaincount = 0;
                    loopv(group)
                    {
                        const seamedge &candidate = seamedges[group[i]];
                        if(candidate.layer)
                        {
                            if(blended < 0) blended = group[i];
                            else otherblended = group[i];
                            ++blendcount;
                        }
                        else { plain = group[i]; ++plaincount; }
                    }
                    compatible = blendcount == 2 && plaincount == 1 && seamedges[blended].displaced &&
                                 sameblendpair(seamedges[blended], seamedges[otherblended]) &&
                                 seamedgegeometrycompatible(seamedges[blended], seamedges[otherblended], verts, normalthreshold) &&
                                 seamedgegeometrycompatible(seamedges[blended], seamedges[plain], verts, normalthreshold) &&
                                 blendselectedalongedge(seamedges[blended], seamedges[plain], verts, blendcache);
                }
                if(compatible) ++remeshinfo.seamcompatible;
                else
                {
                    if(count == 1) ++remeshinfo.seamopen;
                    else ++remeshinfo.seamincompatible;
                    for(int current = head; current >= 0; current = seamedges[current].next) if(seamedges[current].displaced)
                    {
                        const seamedge &boundary = seamedges[current];
                        distances[boundary.a] = distances[boundary.b] = 0.0f;
                    }
                }
            });
            freeblendmapcache(blendcache);

            vector<ushort> queue;
            loopv(verts) if(relevant[i] && distances[i] == 0.0f)
            {
                queue.add(ushort(i));
                queued[i] = 1;
            }
            for(int offset = 0; offset < queue.length(); ++offset)
            {
                ushort from = queue[offset];
                queued[from] = 0;
                loopv(adjacency[from])
                {
                    ushort to = adjacency[from][i];
                    float candidate = distances[from] + verts[from].pos.dist(verts[to].pos);
                    if(candidate + 1e-5f >= distances[to]) continue;
                    distances[to] = candidate;
                    if(!queued[to])
                    {
                        queue.add(to);
                        queued[to] = 1;
                    }
                }
            }
            loopv(verts) if(relevant[i]) verts[i].tc.z = distances[i];
            remeshsession = false;
            seamedges.setsize(0);
            seamheads.clear();
        }

        bool remesh(VSlot &vslot, VSlot *other, int layermask, vector<vertex> &verts, vector<ushort> &indices, int maxindices)
        {
            const VSlot *base, *layer;
            resolvelayers(vslot, other, layermask, base, layer);
            bool displaced = (base->slot && candisplace(*base->slot)) || (layer && layer->slot && candisplace(*layer->slot));
            if(!tesselationremesh || indices.empty() || !displaced)
            {
                if(!indices.empty()) recordseamedges(vslot, other, layermask, verts, indices, displaced);
                return false;
            }
            int start = getclockmillis(), originalverts = verts.length(), originaltris = indices.length()/3;
            vector<ushort> original;
            original.put(indices.getbuf(), indices.length());

            float target = tesselationpatchsize;
            bool changed = false;
            loopi(16)
            {
                int oldtris = indices.length();
                if(!refine(verts, indices, target))
                {
                    verts.setsize(originalverts);
                    indices.setsize(0);
                    indices.move(original);
                    recordseamedges(vslot, other, layermask, verts, indices, displaced);
                    return false;
                }
                if(indices.length() == oldtris) break;
                changed = true;
                if(indices.length() > maxindices)
                {
                    verts.setsize(originalverts);
                    indices.setsize(0);
                    indices.move(original);
                    recordseamedges(vslot, other, layermask, verts, indices, displaced);
                    return false;
                }
            }
            int flippasses = tesselationremeshquality == 2 ? 8 : (tesselationremeshquality == 1 ? 2 : 0);
            loopi(flippasses)
            {
                if(!flipqualityedges(verts, indices, target)) break;
                changed = true;
            }

            remeshinfo.originaltris += originaltris;
            remeshinfo.patches += indices.length()/3;
            remeshinfo.vertices += verts.length() - originalverts;
            remeshinfo.millis += max(getclockmillis() - start, 0);
            hashtable<remeshedge, int> edgeuses;
            for(int i = 0; i+2 < indices.length(); i += 3)
            {
                const vec &a = verts[indices[i]].pos, &b = verts[indices[i+1]].pos, &c = verts[indices[i+2]].pos;
                float e0 = a.dist(b), e1 = b.dist(c), e2 = c.dist(a), longest = max(e0, max(e1, e2)), quality = trianglequality(a, b, c);
                loopj(3)
                {
                    remeshedge edge(indices[i+j], indices[i+(j+1)%3]);
                    int *uses = edgeuses.access(edge);
                    if(uses) ++*uses;
                    else edgeuses[edge] = 1;
                }
                remeshinfo.edgesum += e0 + e1 + e2;
                remeshinfo.qualitysum += quality;
                remeshinfo.maxedge = max(remeshinfo.maxedge, longest);
                remeshinfo.minquality = min(remeshinfo.minquality, quality);
                remeshinfo.qualitybins[clamp(int(quality*5.0f), 0, 4)]++;
                remeshinfo.sizebins[longest <= target*0.5f ? 0 : (longest <= target*1.0001f ? 1 : 2)]++;
                if(quality <= 1e-5f) remeshinfo.degenerates++;
                else if(quality < 0.25f) remeshinfo.elongated++;
            }
            enumeratekt(edgeuses, remeshedge, edge, int, uses,
            {
                (void)edge;
                if(uses == 1) remeshinfo.boundaryedges++;
            });
            recordseamedges(vslot, other, layermask, verts, indices, displaced);
            return changed;
        }

        void resetremeshstats() { remeshinfo.reset(); }

        void printremeshstats()
        {
            int edges = remeshinfo.patches*3;
            conoutf(CON_INFO, "tessellation remesh: %d original triangles, %d patches, %d new vertices, %d ms",
                    remeshinfo.originaltris, remeshinfo.patches, remeshinfo.vertices, remeshinfo.millis);
            conoutf(CON_INFO, "edges: %.2f average, %.2f maximum; quality: %.3f average, %.3f minimum; %d elongated, %d degenerate",
                    edges ? float(remeshinfo.edgesum/edges) : 0.0f, remeshinfo.maxedge,
                    remeshinfo.patches ? float(remeshinfo.qualitysum/remeshinfo.patches) : 0.0f,
                    remeshinfo.patches ? remeshinfo.minquality : 0.0f, remeshinfo.elongated, remeshinfo.degenerates);
            conoutf(CON_INFO, "quality bins [0-.2/.2-.4/.4-.6/.6-.8/.8-1]: %d/%d/%d/%d/%d; size [<=.5x/<=1x/>1x]: %d/%d/%d; boundaries: %d",
                    remeshinfo.qualitybins[0], remeshinfo.qualitybins[1], remeshinfo.qualitybins[2], remeshinfo.qualitybins[3],
                    remeshinfo.qualitybins[4], remeshinfo.sizebins[0], remeshinfo.sizebins[1], remeshinfo.sizebins[2],
                    remeshinfo.boundaryedges);
            conoutf(CON_INFO, "seams: %d compatible, %d incompatible, %d open/unmatched", remeshinfo.seamcompatible,
                    remeshinfo.seamincompatible, remeshinfo.seamopen);
            conoutf(CON_INFO, "additional geometry memory: %d bytes", remeshinfo.vertices*int(sizeof(vertex)) +
                    max(remeshinfo.patches - remeshinfo.originaltris, 0)*3*int(sizeof(ushort)));
        }

        Shader *worldshader(Slot &slot)
        {
            Shader *base = slot.shader;
            if(!enabled() || !base) return NULL;
            if(base == lastworldbase && lastworldshader && lastworldshader->loaded()) return lastworldshader;

            defformatstring(name, "tess_%s", base->name);
            lastworldbase = base;
            Shader *shader = useshaderbyname(name);
            lastworldshader = shader && shader->loaded() ? shader : NULL;
            return lastworldshader;
        }

        Shader *shadowshader(VSlot &vslot, VSlot *other, int layermask, bool transparent)
        {
            const VSlot *base, *layer;
            resolvelayers(vslot, other, layermask, base, layer);
            const Slot *displaced = base->slot && candisplace(*base->slot) ? base->slot :
                                    (layer && layer->slot && candisplace(*layer->slot) ? layer->slot : NULL);
            if(!shadowsenabled() || !displaced || !displaced->shader) return NULL;
            defformatstring(name, "tess_shadow%s_%s", transparent ? "alpha" : "", displaced->shader->name);
            Shader *shader = useshaderbyname(name);
            return shader && shader->loaded() ? shader : NULL;
        }

        bool hasshadowcasters()
        {
            if(!shadowsenabled()) return false;
            for(vtxarray *va = shadowva; va; va = va->rnext)
            {
                int numelems = va->texs + va->blends + va->alphaback + va->alphafront + va->refract;
                loopi(numelems)
                {
                    VSlot &vslot = lookupvslot(va->texelems[i].texture);
                    elementset &es = va->texelems[i];
                    if(eligible(vslot, es.layertexture != USHRT_MAX ? &lookupvslot(es.layertexture) : NULL, es.layer,
                                va->geommin, va->geommax)) return true;
                }
            }
            return false;
        }

        float shadowmargin()
        {
            if(!shadowsenabled()) return 0.0f;
            static int lastupdate = -1;
            static float margin = 0.0f;
            if(lastupdate == lastmillis) return margin;
            lastupdate = lastmillis;
            margin = 0.0f;
            loopv(vslots)
            {
                if(!vslots[i] || !vslots[i]->slot) continue;
                VSlot &vslot = *vslots[i];
                Slot &slot = *vslot.slot;
                if(!candisplace(slot)) continue;
                Texture *diffuse = NULL;
                loopvj(slot.sts) if(slot.sts[j].type == TEX_DIFFUSE) { diffuse = slot.sts[j].t; break; }
                const float *parallax = findparam(slot, vslot, "parallaxscale");
                if(!diffuse || diffuse == notexture || !parallax) continue;
                float worldperuv = vslot.scale*(diffuse->xs + diffuse->ys)/(2.0f*TEX_SCALE);
                margin = max(margin, max(fabsf(parallax[1]), fabsf(parallax[0] + parallax[1]))*worldperuv*tesselationscale);
            }
            return margin;
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

        void beginshadow()
        {
            if(shadowactive || !shadowsenabled()) return;
            glGetIntegerv(GL_PATCH_VERTICES, &previouspatchvertices);
            glPatchParameteri_(GL_PATCH_VERTICES, 3);
            if(tesselationshadowdebug)
            {
                glGetIntegerv(GL_POLYGON_MODE, previouspolygonmode);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                wireframeactive = true;
            }
            shadowactive = true;
        }

        void endshadow()
        {
            if(!shadowactive) return;
            if(wireframeactive) endwireframe();
            glPatchParameteri_(GL_PATCH_VERTICES, previouspatchvertices);
            shadowactive = false;
        }

        struct tessmaterial
        {
            Texture *diffuse, *normal, *detailnormal;
            const float *parallax, *tripbias;
            float worldperuv;
            bool displaced, triplanar;

            tessmaterial() : diffuse(NULL), normal(NULL), detailnormal(NULL), parallax(NULL), tripbias(NULL), worldperuv(0.0f),
                displaced(false), triplanar(false) {}
        };

        static tessmaterial gettessmaterial(const VSlot &vslot)
        {
            tessmaterial material;
            if(!vslot.slot) return material;
            Slot &slot = *vslot.slot;
            loopv(slot.sts)
            {
                if(slot.sts[i].type == TEX_DIFFUSE) material.diffuse = slot.sts[i].t;
                else if(slot.sts[i].type == TEX_NORMAL) material.normal = slot.sts[i].t;
            }
            if(vslot.detail)
            {
                VSlot &detail = lookupvslot(vslot.detail);
                loopv(detail.slot->sts) if(detail.slot->sts[i].type == TEX_NORMAL) material.detailnormal = detail.slot->sts[i].t;
            }
            material.parallax = slot.shader ? findparam(slot, vslot, "parallaxscale") : NULL;
            material.tripbias = slot.shader ? findparam(slot, vslot, "triplanarbias") : NULL;
            material.worldperuv = material.diffuse && material.diffuse != notexture ?
                                  vslot.scale*(material.diffuse->xs + material.diffuse->ys)/(2.0f*TEX_SCALE) : 0.0f;
            material.displaced = candisplace(slot) && material.worldperuv > 0.0f;
            material.triplanar = slot.shader && slot.shader->type&SHADER_TRIPLANAR;
            return material;
        }

        static void bindtesstexture(int unit, Texture *texture)
        {
            glActiveTexture_(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, texture && texture != notexture ? texture->id : notexture->id);
        }

        static void settessmaterialparams(bool base, const VSlot &vslot, const tessmaterial &material)
        {
            const texrotation &rotation = texrotations[vslot.rotation];
            float xs = material.diffuse && material.diffuse != notexture ? float(material.diffuse->xs) : 1.0f,
                  ys = material.diffuse && material.diffuse != notexture ? float(material.diffuse->ys) : 1.0f,
                  scale = TEX_SCALE/vslot.scale,
                  sk = scale/(rotation.flipx ? -xs : xs), tk = scale/(rotation.flipy ? -ys : ys),
                  soff = -(rotation.swapxy ? vslot.offset.y : vslot.offset.x)/xs,
                  toff = -(rotation.swapxy ? vslot.offset.x : vslot.offset.y)/ys;
            vec2 scroll(vslot.scroll);
            if(rotation.swapxy) swap(scroll.x, scroll.y);
            scroll.x *= lastmillis*(rotation.flipx ? -1.0f : 1.0f);
            scroll.y *= lastmillis*(rotation.flipy ? -1.0f : 1.0f);

            float displacementscale = material.displaced && displacing() ? material.parallax[0]*material.worldperuv*tesselationscale : 0.0f,
                  displacementbias = material.displaced && displacing() ? material.parallax[1]*material.worldperuv*tesselationscale : 0.0f,
                  normalwidth = material.normal && material.normal != notexture ? float(material.normal->xs) : 1.0f,
                  normalheight = material.normal && material.normal != notexture ? float(material.normal->ys) : 1.0f,
                  detailwidth = material.detailnormal && material.detailnormal != notexture ? float(material.detailnormal->xs) : normalwidth,
                  detailheight = material.detailnormal && material.detailnormal != notexture ? float(material.detailnormal->ys) : normalheight;
            float detailscale = scale;
            if(vslot.detail)
            {
                VSlot &detail = lookupvslot(vslot.detail);
                detailscale = TEX_SCALE/detail.scale;
            }
            float triscalez = material.detailnormal && material.detailnormal != notexture ? detailscale/material.detailnormal->xs : scale/xs,
                  triscalew = material.detailnormal && material.detailnormal != notexture ? detailscale/material.detailnormal->ys : scale/ys,
                  biasx = material.tripbias ? material.tripbias[0] : 0.3f,
                  biasy = material.tripbias ? material.tripbias[1] : 0.3f,
                  biasz = material.tripbias ? material.tripbias[2] : 0.3f;
            if(base)
            {
                LOCALPARAMF(tessbasedisplace, displacementscale, displacementbias, material.displaced && displacing() ? 1.0f : 0.0f,
                            material.triplanar ? 1.0f : 0.0f);
                LOCALPARAMF(tessbaseuvparams, sk, tk, soff + scroll.x, toff + scroll.y);
                LOCALPARAMF(tessbaseuvrotation, rotation.swapxy ? 1.0f : 0.0f);
                LOCALPARAMF(tessbasemapsize, normalwidth, normalheight, detailwidth, detailheight);
                LOCALPARAMF(tessbasetriscale, scale/xs, scale/ys, triscalez, triscalew);
                LOCALPARAMF(tessbasetripbias, biasx, biasy, biasz, material.detailnormal && material.detailnormal != notexture ? 1.0f : 0.0f);
            }
            else
            {
                LOCALPARAMF(tesslayerdisplace, displacementscale, displacementbias, material.displaced && displacing() ? 1.0f : 0.0f,
                            material.triplanar ? 1.0f : 0.0f);
                LOCALPARAMF(tesslayeruvparams, sk, tk, soff + scroll.x, toff + scroll.y);
                LOCALPARAMF(tesslayeruvrotation, rotation.swapxy ? 1.0f : 0.0f);
                LOCALPARAMF(tesslayermapsize, normalwidth, normalheight, detailwidth, detailheight);
                LOCALPARAMF(tesslayertriscale, scale/xs, scale/ys, triscalez, triscalew);
                LOCALPARAMF(tesslayertripbias, biasx, biasy, biasz, material.detailnormal && material.detailnormal != notexture ? 1.0f : 0.0f);
            }
        }

        void setparams(Slot &slot, VSlot &vslot, VSlot *other, int layermask)
        {
            LOCALPARAM(camera, camera1->o);
            LOCALPARAMF(tessfactor, float(clamp(tesselationfixed, 1, tesselationmaxlevel)));
            LOCALPARAMF(tessshadowalpha, slot.texmask&(1<<TEX_ALPHA) ? 1.0f : 0.0f);

            const VSlot *basevslot, *layervslot;
            resolvelayers(vslot, other, layermask, basevslot, layervslot);
            tessmaterial current = gettessmaterial(vslot), base = gettessmaterial(*basevslot), layer;
            if(layervslot) layer = gettessmaterial(*layervslot);
            bool active = displacing() && (base.displaced || (layervslot && layer.displaced));
            bool mixedplain = layervslot && (!base.normal || base.normal == notexture || !layer.normal || layer.normal == notexture);
            const float *parallax = current.parallax, *tripbias = current.tripbias;
            LOCALPARAMF(parallaxscale, parallax ? parallax[0] : 0.0f, parallax ? parallax[1] : 0.0f,
                        parallax ? parallax[2] : 0.0f, parallax ? parallax[3] : 0.0f);
            LOCALPARAMF(triplanarbias, tripbias ? tripbias[0] : 0.3f, tripbias ? tripbias[1] : 0.3f,
                        tripbias ? tripbias[2] : 0.3f, tripbias ? tripbias[3] : 0.0f);
            float debugworldperuv = max(base.worldperuv, layervslot ? layer.worldperuv : 0.0f);
            LOCALPARAMF(tessparams, active ? debugworldperuv*tesselationscale : 0.0f, float(tesselationdebug), tesselationheightlod,
                        active ? 1.0f : 0.0f);
            LOCALPARAMF(tessblendparams, layervslot ? 1.0f : 0.0f, layervslot == &vslot ? 1.0f : 0.0f,
                        base.displaced ? 1.0f : 0.0f, layervslot && layer.displaced ? 1.0f : 0.0f);
            settessmaterialparams(true, *basevslot, base);
            tessmaterial emptylayer;
            settessmaterialparams(false, layervslot ? *layervslot : *basevslot, layervslot ? layer : emptylayer);
            bindtesstexture(8, base.normal);
            bindtesstexture(9, base.detailnormal ? base.detailnormal : base.normal);
            if(layervslot)
            {
                bindtesstexture(10, layer.normal);
                bindtesstexture(11, layer.detailnormal ? layer.detailnormal : layer.normal);
            }
            glActiveTexture_(GL_TEXTURE0);
            LOCALPARAMF(tessadaptiveparams, float(clamp(tesselationmin, 1, tesselationmaxlevel)),
                        float(clamp(tesselationmax, 1, tesselationmaxlevel)), tesselationpixels, tesselationtexels);
            LOCALPARAMF(tessadaptiveoptions, tesselationworld, tesselationadaptive && !mixedplain ? 1.0f : 0.0f,
                        tesselationautolod && !mixedplain ? 1.0f : 0.0f, tesselationlodbias);
            LOCALPARAMF(tessviewparams, 0.5f*vieww, 0.5f*viewh, float(tesselationmaxlevel), 0.0f);
            float fadedistance = clamp(tesselationfade, 0.0f, tesselationdistance);
            LOCALPARAMF(tessdistanceparams, tesselationdistance, tesselationdistance - fadedistance, fadedistance, 0.0f);
            float basemagnitude = active && base.parallax ? max(fabsf(base.parallax[1]), fabsf(base.parallax[0] + base.parallax[1]))*
                                  base.worldperuv*tesselationscale : 0.0f;
            float layermagnitude = active && layervslot && layer.parallax ?
                                   max(fabsf(layer.parallax[1]), fabsf(layer.parallax[0] + layer.parallax[1]))*
                                   layer.worldperuv*tesselationscale : 0.0f;
            float displacementmagnitude = max(basemagnitude, layermagnitude);
            LOCALPARAMF(tessblenddebugscale, basemagnitude, layermagnitude, displacementmagnitude, 0.0f);
            float seamwidth = max(tesselationseamwidth, 2.0f*displacementmagnitude);
            LOCALPARAMF(tessseamparams, tesselationseams ? 1.0f : 0.0f, seamwidth, tesselationseamangle, 0.0f);
            LOCALPARAMF(tessmapsize, current.normal && current.normal != notexture ? float(current.normal->xs) : 1.0f,
                        current.normal && current.normal != notexture ? float(current.normal->ys) : 1.0f);
            LOCALPARAMF(tessdetailmapsize, current.detailnormal && current.detailnormal != notexture ? float(current.detailnormal->xs) : 1.0f,
                        current.detailnormal && current.detailnormal != notexture ? float(current.detailnormal->ys) : 1.0f);
        }

        bool beginwireframe()
        {
            if(wireframeactive || tesselationdebug != 4) return false;
            glGetIntegerv(GL_POLYGON_MODE, previouspolygonmode);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            wireframeactive = true;
            return true;
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

ICOMMAND(tesselationremeshstats, "", (), renderer::tesselation::printremeshstats());
