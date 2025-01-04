// worldio.cpp: loading & saving of maps and savegames

#include "engine.h"

void validmapname(char *dst, const char *src, const char *prefix = NULL, const char *alt = "untitled", size_t maxlen = 100)
{
    if(prefix) while(*prefix) *dst++ = *prefix++;
    const char *start = dst;
    if(src) loopi(maxlen)
    {
        char c = *src++;
        if(iscubealnum(c) || c == '_' || c == '-' || c == '/' || c == '\\') *dst++ = c;
        else break;
    }
    if(dst > start) *dst = '\0';
    else if(dst != alt) copystring(dst, alt, maxlen);
}

void fixmapname(char *name)
{
    validmapname(name, name, NULL, "");
}

void getmapfilenames(const char *fname, const char *cname, char *pakname, char *mapname, char *cfgname)
{
    if(!cname) cname = fname;
    string name;
    validmapname(name, cname);
    char *slash = strpbrk(name, "/\\");
    if(slash)
    {
        copystring(pakname, name, slash-name+1);
        copystring(cfgname, slash+1, MAXSTRLEN);
    }
    else
    {
        copystring(pakname, "base", MAXSTRLEN);
        copystring(cfgname, name, MAXSTRLEN);
    }
    validmapname(mapname, fname, strpbrk(fname, "/\\") ? NULL : "");
}

static bool loadmapheader(stream *f, const char *ogzname, mapheader &hdr, octaheader &ohdr)
{
    if(f->read(&hdr, 3*sizeof(int)) != 3*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
    lilswap(&hdr.version, 2);

    if(!memcmp(hdr.magic, "TMAP", 4))
    {
        if(hdr.version>MAPVERSION) { conoutf(CON_ERROR, "map %s requires a newer version of Tesseract", ogzname); return false; }
        if(f->read(&hdr.worldsize, 6*sizeof(int)) != 6*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
        lilswap(&hdr.worldsize, 6);
        if(hdr.worldsize <= 0|| hdr.numents < 0) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
    }
    else if(!memcmp(hdr.magic, "OCTA", 4))
    {
        if(hdr.version!=OCTAVERSION) { conoutf(CON_ERROR, "map %s uses an unsupported map format version", ogzname); return false; }
        if(f->read(&ohdr.worldsize, 7*sizeof(int)) != 7*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
        lilswap(&ohdr.worldsize, 7);
        if(ohdr.worldsize <= 0|| ohdr.numents < 0) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
        memcpy(hdr.magic, "TMAP", 4);
        hdr.version = 0;
        hdr.headersize = sizeof(hdr);
        hdr.worldsize = ohdr.worldsize;
        hdr.numents = ohdr.numents;
        hdr.numpvs = ohdr.numpvs;
        hdr.blendmap = ohdr.blendmap;
        hdr.numvars = ohdr.numvars;
        hdr.numvslots = ohdr.numvslots;
    }
    else { conoutf(CON_ERROR, "map %s uses an unsupported map type", ogzname); return false; }

    return true;
}

static void fixent(entity &e, int version)
{
    if(version != 0) // sauerract | version '0' are newer maps
    {
        if(version <= 10 && e.type >= 7) e.type++;
        if(version <= 12 && e.type >= 8) e.type++;
        if(version <= 14 && e.type >= ET_MAPMODEL && e.type <= 16)
        {
            if(e.type == 16) e.type = ET_MAPMODEL;
            else e.type++;
        }
        if(version <= 20 && e.type >= ET_ENVMAP) e.type++;
        if(version <= 21 && e.type >= ET_PARTICLES) e.type++;
        if(version <= 22 && e.type >= ET_SOUND) e.type++;
        if(version <= 23 && e.type >= ET_SPOTLIGHT) e.type++;
        if(version <= 30 && (e.type == ET_MAPMODEL || e.type == ET_PLAYERSTART)) e.attr1 = (int(e.attr1)+180)%360;
        if(version <= 31 && e.type == ET_MAPMODEL) { int yaw = (int(e.attr1)%360 + 360)%360 + 7; e.attr1 = yaw - yaw%15; }
    }

    if(e.type == ET_MAPMODEL && version < 33) // sauerract | invert mdl id and pitch for sauerbraten port
    {
        int attr1 = e.attr1, attr2 = e.attr2;
        e.attr1 = attr2, e.attr2 = attr1;
    }
}

bool loadents(const char *fname, vector<entity> &ents, uint *crc)
{
    string name;
    validmapname(name, fname);
    defformatstring(ogzname, "packages/map/%s.ogz", name);
    path(ogzname);
    stream *f = opengzfile(ogzname, "rb");
    if(!f) return false;
    octaheader hdr;

    if(f->read(&hdr, 7*sizeof(int)) != 7*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); delete f; return false; }
    lilswap(&hdr.version, 6);
    if(memcmp(hdr.magic, "OCTA", 4) || hdr.worldsize <= 0|| hdr.numents < 0) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); delete f; return false; }
    if(hdr.version>MAPVERSION) { conoutf(CON_ERROR, "map %s requires a newer version of Tesseract: Sauerbraten", ogzname); delete f; return false; }
    compatheader chdr;
    if(hdr.version <= 28)
    {
        if(f->read(&chdr.lightprecision, sizeof(chdr) - 7*sizeof(int)) != sizeof(chdr) - 7*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); delete f; return false; }
    }
    else
    {
        int extra = 0;
        if(hdr.version <= 29) extra++;
        if(f->read(&hdr.blendmap, sizeof(hdr) - (7+extra)*sizeof(int)) != sizeof(hdr) - (7+extra)*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); delete f; return false; }
    }

    if(hdr.version <= 28)
    {
        lilswap(&chdr.lightprecision, 3);
        hdr.blendmap = chdr.blendmap;
        hdr.numvars = 0;
        hdr.numvslots = 0;
    }
    else
    {
        lilswap(&hdr.blendmap, 2);
        if(hdr.version <= 29) hdr.numvslots = 0;
        else lilswap(&hdr.numvslots, 1);
    }

    loopi(hdr.numvars)
    {
        int type = f->getchar(), ilen = f->getlil<ushort>();
        f->seek(ilen, SEEK_CUR);
        switch(type)
        {
            case ID_VAR: f->getlil<int>(); break;
            case ID_FVAR: f->getlil<float>(); break;
            case ID_SVAR: { int slen = f->getlil<ushort>(); f->seek(slen, SEEK_CUR); break; }
        }
    }

    string gametype;
    copystring(gametype, "fps");
    bool samegame = true;
    int eif = 0;
    if(hdr.version>=16)
    {
        int len = f->getchar();
        f->read(gametype, len+1);
    }
    if(strcmp(gametype, game::gameident()))
    {
        samegame = false;
        conoutf(CON_WARN, "WARNING: loading map from %s game, ignoring entities except for lights/mapmodels", gametype);
    }
    if(hdr.version>=16)
    {
        eif = f->getlil<ushort>();
        int extrasize = f->getlil<ushort>();
        f->seek(extrasize, SEEK_CUR);
    }

    if(hdr.version<14)
    {
        f->seek(256, SEEK_CUR);
    }
    else
    {
        ushort nummru = f->getlil<ushort>();
        f->seek(nummru*sizeof(ushort), SEEK_CUR);
    }

    loopi(min(hdr.numents, MAXENTS))
    {
        entity &e = ents.add();
        f->read(&e, sizeof(entity));
        lilswap(&e.o.x, 3);
        lilswap(&e.attr1, 5);
        fixent(e, hdr.version);
        if(eif > 0) f->seek(eif, SEEK_CUR);
        if(samegame)
        {
            entities::readent(e, NULL, hdr.version);
        }
        else if(e.type>=ET_GAMESPECIFIC || hdr.version<=14)
        {
            ents.pop();
            continue;
        }
    }

    if(crc)
    {
        f->seek(0, SEEK_END);
        *crc = f->getcrc();
    }

    delete f;

    return true;
}

#ifndef STANDALONE
string ogzname, bakname, cfgname, picname;

VARP(savebak, 0, 2, 2);
VARP(useremasteredmaps, 0, 0, 1);
VAR(loadremastered, 0, 0, 1);

void setmapfilenames(const char *fname, const char *cname = NULL)
{
    string name;
    validmapname(name, fname);
    formatstring(ogzname, "packages/map/%s.ogz", name);
    formatstring(picname, "packages/map/%s.jpg", name);
    if(savebak==1) formatstring(bakname, "packages/map/%s.BAK", name);
    else
    {
        string baktime;
        time_t t = time(NULL);
        size_t len = strftime(baktime, sizeof(baktime), "%Y-%m-%d_%H.%M.%S", localtime(&t));
        baktime[min(len, sizeof(baktime)-1)] = '\0';
        formatstring(bakname, "packages/map/%s_%s.BAK", name, baktime);
    }

    validmapname(name, cname ? cname : fname);
    formatstring(cfgname, "packages/map/%s%s.cfg", name, loadremastered && useremasteredmaps ? "_r" : "");

    path(ogzname);
    path(bakname);
    path(cfgname);
    path(picname);
}

void mapcfgname()
{
    const char *mname = game::getclientmap();
    string name;
    validmapname(name, mname);
    defformatstring(cfgname, "packages/map/%s.cfg", name);
    path(cfgname);
    result(cfgname);
}

COMMAND(mapcfgname, "");

void backup(const char *name, const char *backupname)
{
    string backupfile;
    copystring(backupfile, findfile(backupname, "wb"));
    remove(backupfile);
    rename(findfile(name, "wb"), backupfile);
}

enum { OCTSAV_CHILDREN = 0, OCTSAV_EMPTY, OCTSAV_SOLID, OCTSAV_NORMAL, OCTSAV_LODCUBE };

#define LM_PACKW 512
#define LM_PACKH 512
#define LAYER_DUP (1<<7)

struct polysurfacecompat
{
    uchar lmid[2];
    uchar verts, numverts;
};

static int savemapprogress = 0;

struct surfacecompat
{
    uchar texcoords[8];
    uchar w, h;
    ushort x, y;
    uchar lmid, layer;
};

struct normalscompat
{
    bvec normals[4];
};

struct mergecompat
{
    ushort u1, u2, v1, v2;
};

void savec(cube *c, const ivec &o, int size, stream *f, bool nolms)
{
    if((savemapprogress++&0xFFF)==0) renderprogress(float(savemapprogress)/allocnodes, "saving octree...");

    loopi(8)
    {
        ivec co(i, o, size);
        if(c[i].children)
        {
            f->putchar(OCTSAV_CHILDREN);
            savec(c[i].children, co, size>>1, f, nolms);
        }
        else
        {
            int oflags = 0, surfmask = 0, totalverts = 0;
            if(c[i].material!=MAT_AIR) oflags |= 0x40;
            if(isempty(c[i])) f->putchar(oflags | OCTSAV_EMPTY);
            else
            {
                if(!nolms)
                {
                    if(c[i].merged) oflags |= 0x80;
                    if(c[i].ext) loopj(6)
                    {
                        const surfaceinfo &surf = c[i].ext->surfaces[j];
                        if(!surf.used()) continue;
                        oflags |= 0x20;
                        surfmask |= 1<<j;
                        totalverts += surf.totalverts();
                    }
                }

                if(isentirelysolid(c[i])) f->putchar(oflags | OCTSAV_SOLID);
                else
                {
                    f->putchar(oflags | OCTSAV_NORMAL);
                    f->write(c[i].edges, 12);
                }
            }

            loopj(6) f->putlil<ushort>(c[i].texture[j]);

            if(oflags&0x40) f->putlil<ushort>(c[i].material);
            if(oflags&0x80) f->putchar(c[i].merged);
            if(oflags&0x20)
            {
                f->putchar(surfmask);
                f->putchar(totalverts);
                loopj(6) if(surfmask&(1<<j))
                {
                    surfaceinfo surf = c[i].ext->surfaces[j];
                    vertinfo *verts = c[i].ext->verts() + surf.verts;
                    int layerverts = surf.numverts&MAXFACEVERTS, numverts = surf.totalverts(),
                        vertmask = 0, vertorder = 0,
                        dim = dimension(j), vc = C[dim], vr = R[dim];
                    if(numverts)
                    {
                        if(c[i].merged&(1<<j))
                        {
                            vertmask |= 0x04;
                            if(layerverts == 4)
                            {
                                ivec v[4] = { verts[0].getxyz(), verts[1].getxyz(), verts[2].getxyz(), verts[3].getxyz() };
                                loopk(4)
                                {
                                    const ivec &v0 = v[k], &v1 = v[(k+1)&3], &v2 = v[(k+2)&3], &v3 = v[(k+3)&3];
                                    if(v1[vc] == v0[vc] && v1[vr] == v2[vr] && v3[vc] == v2[vc] && v3[vr] == v0[vr])
                                    {
                                        vertmask |= 0x01;
                                        vertorder = k;
                                        break;
                                    }
                                }
                            }
                        }
                        else
                        {
                            int vis = visibletris(c[i], j, co, size);
                            if(vis&4 || faceconvexity(c[i], j) < 0) vertmask |= 0x01;
                            if(layerverts < 4 && vis&2) vertmask |= 0x02;
                        }
                        bool matchnorm = true;
                        loopk(numverts)
                        {
                            const vertinfo &v = verts[k];
                            if(v.norm) { vertmask |= 0x80; if(v.norm != verts[0].norm) matchnorm = false; }
                        }
                        if(matchnorm) vertmask |= 0x08;
                    }
                    surf.verts = vertmask;
                    f->write(&surf, sizeof(surf));
                    bool hasxyz = (vertmask&0x04)!=0, hasnorm = (vertmask&0x80)!=0;
                    if(layerverts == 4)
                    {
                        if(hasxyz && vertmask&0x01)
                        {
                            ivec v0 = verts[vertorder].getxyz(), v2 = verts[(vertorder+2)&3].getxyz();
                            f->putlil<ushort>(v0[vc]); f->putlil<ushort>(v0[vr]);
                            f->putlil<ushort>(v2[vc]); f->putlil<ushort>(v2[vr]);
                            hasxyz = false;
                        }
                    }
                    if(hasnorm && vertmask&0x08) { f->putlil<ushort>(verts[0].norm); hasnorm = false; }
                    if(hasxyz || hasnorm) loopk(layerverts)
                    {
                        const vertinfo &v = verts[(k+vertorder)%layerverts];
                        if(hasxyz)
                        {
                            ivec xyz = v.getxyz();
                            f->putlil<ushort>(xyz[vc]); f->putlil<ushort>(xyz[vr]);
                        }
                        if(hasnorm) f->putlil<ushort>(v.norm);
                    }
                }
            }
        }
    }
}

cube *loadchildren(stream *f, const ivec &co, int size, bool oldmap, bool &failed);

static inline int convertoldmaterial(int mat)
{
    return ((mat&7)<<MATF_VOLUME_SHIFT) | (((mat>>3)&3)<<MATF_CLIP_SHIFT) | (((mat>>5)&7)<<MATF_FLAG_SHIFT);
}

void loadc(stream *f, cube &c, const ivec &co, int size, bool &failed)
{
    int octsav = f->getchar();
    switch(octsav&0x7)
    {
        case OCTSAV_CHILDREN:
            c.children = loadchildren(f, co, size>>1, false, failed);
            return;

        case OCTSAV_EMPTY:  emptyfaces(c);        break;
        case OCTSAV_SOLID:  solidfaces(c);        break;
        case OCTSAV_NORMAL: f->read(c.edges, 12); break;
        default: failed = true; return;
    }
    loopi(6) c.texture[i] = f->getlil<ushort>();
    if(octsav&0x40) c.material = f->getlil<ushort>();
    if(octsav&0x80) c.merged = f->getchar();
    if(octsav&0x20)
    {
        int surfmask, totalverts;
        surfmask = f->getchar();
        totalverts = max(f->getchar(), 0);
        newcubeext(c, totalverts, false);
        memset(c.ext->surfaces, 0, sizeof(c.ext->surfaces));
        memset(c.ext->verts(), 0, totalverts*sizeof(vertinfo));
        int offset = 0;
        loopi(6) if(surfmask&(1<<i))
        {
            surfaceinfo &surf = c.ext->surfaces[i];
            if(mapversion <= 0)
            {
                polysurfacecompat psurf;
                f->read(&psurf, sizeof(polysurfacecompat));
                surf.verts = psurf.verts;
                surf.numverts = psurf.numverts;
            }
            else f->read(&surf, sizeof(surf));
            int vertmask = surf.verts, numverts = surf.totalverts();
            if(!numverts) { surf.verts = 0; continue; }
            surf.verts = offset;
            vertinfo *verts = c.ext->verts() + offset;
            offset += numverts;
            ivec v[4], n, vo = ivec(co).mask(0xFFF).shl(3);
            int layerverts = surf.numverts&MAXFACEVERTS, dim = dimension(i), vc = C[dim], vr = R[dim], bias = 0;
            genfaceverts(c, i, v);
            bool hasxyz = (vertmask&0x04)!=0, hasuv = mapversion <= 0 && (vertmask&0x40)!=0, hasnorm = (vertmask&0x80)!=0;
            if(hasxyz)
            {
                ivec e1, e2, e3;
                n.cross((e1 = v[1]).sub(v[0]), (e2 = v[2]).sub(v[0]));
                if(n.iszero()) n.cross(e2, (e3 = v[3]).sub(v[0]));
                bias = -n.dot(ivec(v[0]).mul(size).add(vo));
            }
            else
            {
                int vis = layerverts < 4 ? (vertmask&0x02 ? 2 : 1) : 3, order = vertmask&0x01 ? 1 : 0, k = 0;
                verts[k++].setxyz(v[order].mul(size).add(vo));
                if(vis&1) verts[k++].setxyz(v[order+1].mul(size).add(vo));
                verts[k++].setxyz(v[order+2].mul(size).add(vo));
                if(vis&2) verts[k++].setxyz(v[(order+3)&3].mul(size).add(vo));
            }
            if(layerverts == 4)
            {
                if(hasxyz && vertmask&0x01)
                {
                    ushort c1 = f->getlil<ushort>(), r1 = f->getlil<ushort>(), c2 = f->getlil<ushort>(), r2 = f->getlil<ushort>();
                    ivec xyz;
                    xyz[vc] = c1; xyz[vr] = r1; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    verts[0].setxyz(xyz);
                    xyz[vc] = c1; xyz[vr] = r2; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    verts[1].setxyz(xyz);
                    xyz[vc] = c2; xyz[vr] = r2; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    verts[2].setxyz(xyz);
                    xyz[vc] = c2; xyz[vr] = r1; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    verts[3].setxyz(xyz);
                    hasxyz = false;
                }
                if(hasuv && vertmask&0x02)
                {
                    loopk(4) f->getlil<ushort>();
                    if(surf.numverts&LAYER_DUP) loopk(4) f->getlil<ushort>();
                    hasuv = false;
                }
            }
            if(hasnorm && vertmask&0x08)
            {
                ushort norm = f->getlil<ushort>();
                loopk(layerverts) verts[k].norm = norm;
                hasnorm = false;
            }
            if(hasxyz || hasuv || hasnorm) loopk(layerverts)
            {
                vertinfo &v = verts[k];
                if(hasxyz)
                {
                    ivec xyz;
                    xyz[vc] = f->getlil<ushort>(); xyz[vr] = f->getlil<ushort>();
                    xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    v.setxyz(xyz);
                }
                if(hasuv) { f->getlil<ushort>(); f->getlil<ushort>(); }
                if(hasnorm) v.norm = f->getlil<ushort>();
            }
            if(hasuv && surf.numverts&LAYER_DUP) loopk(layerverts) { f->getlil<ushort>(); f->getlil<ushort>(); }
        }
    }
}

void convertoldsurfaces(cube &c, const ivec &co, int size, surfacecompat *srcsurfs, int hassurfs, normalscompat *normals, int hasnorms, mergecompat *merges, int hasmerges)
{
    surfaceinfo dstsurfs[6];
    vertinfo verts[6*2*MAXFACEVERTS];
    int totalverts = 0, numsurfs = 6;
    memset(dstsurfs, 0, sizeof(dstsurfs));
    loopi(6) if((hassurfs|hasnorms|hasmerges)&(1<<i))
    {
        surfaceinfo &dst = dstsurfs[i];
        vertinfo *curverts = NULL;
        int numverts = 0;
        surfacecompat *src = NULL, *blend = NULL;
        if(hassurfs&(1<<i))
        {
            src = &srcsurfs[i];
            if(src->layer&2)
            {
                blend = &srcsurfs[numsurfs++];
                dst.lmid[0] = src->lmid;
                dst.lmid[1] = blend->lmid;
                dst.numverts |= LAYER_BLEND;
                if(blend->lmid >= LMID_RESERVED && (src->x != blend->x || src->y != blend->y || src->w != blend->w || src->h != blend->h || memcmp(src->texcoords, blend->texcoords, sizeof(src->texcoords))))
                    dst.numverts |= LAYER_DUP;
            }
            else if(src->layer == 1) { dst.lmid[1] = src->lmid; dst.numverts |= LAYER_BOTTOM; }
            else { dst.lmid[0] = src->lmid; dst.numverts |= LAYER_TOP; }
        }
        else dst.numverts |= LAYER_TOP;
        bool uselms = hassurfs&(1<<i) && (dst.lmid[0] >= LMID_RESERVED || dst.lmid[1] >= LMID_RESERVED || dst.numverts&~LAYER_TOP),
             usemerges = hasmerges&(1<<i) && merges[i].u1 < merges[i].u2 && merges[i].v1 < merges[i].v2,
             usenorms = hasnorms&(1<<i) && normals[i].normals[0] != bvec(128, 128, 128);
        if(uselms || usemerges || usenorms)
        {
            ivec v[4], pos[4], e1, e2, e3, n, vo = ivec(co).mask(0xFFF).shl(3);
            genfaceverts(c, i, v);
            n.cross((e1 = v[1]).sub(v[0]), (e2 = v[2]).sub(v[0]));
            if(usemerges)
            {
                const mergecompat &m = merges[i];
                int offset = -n.dot(v[0].mul(size).add(vo)),
                    dim = dimension(i), vc = C[dim], vr = R[dim];
                loopk(4)
                {
                    const ivec &coords = facecoords[i][k];
                    int cc = coords[vc] ? m.u2 : m.u1,
                        rc = coords[vr] ? m.v2 : m.v1,
                        dc = n[dim] ? -(offset + n[vc]*cc + n[vr]*rc)/n[dim] : vo[dim];
                    ivec &mv = pos[k];
                    mv[vc] = cc;
                    mv[vr] = rc;
                    mv[dim] = dc;
                }
            }
            else
            {
                int convex = (e3 = v[0]).sub(v[3]).dot(n), vis = 3;
                if(!convex)
                {
                    if(ivec().cross(e3, e2).iszero()) { if(!n.iszero()) vis = 1; }
                    else if(n.iszero()) vis = 2;
                }
                int order = convex < 0 ? 1 : 0;
                pos[0] = v[order].mul(size).add(vo);
                pos[1] = vis&1 ? v[order+1].mul(size).add(vo) : pos[0];
                pos[2] = v[order+2].mul(size).add(vo);
                pos[3] = vis&2 ? v[(order+3)&3].mul(size).add(vo) : pos[0];
            }
            curverts = verts + totalverts;
            loopk(4)
            {
                if(k > 0 && (pos[k] == pos[0] || pos[k] == pos[k-1])) continue;
                vertinfo &dv = curverts[numverts++];
                dv.setxyz(pos[k]);
                if(uselms)
                {
                    float u = src->x + (src->texcoords[k*2] / 255.0f) * (src->w - 1),
                          v = src->y + (src->texcoords[k*2+1] / 255.0f) * (src->h - 1);
                    dv.u = ushort(floor(clamp((u) * float(USHRT_MAX+1)/LM_PACKW + 0.5f, 0.0f, float(USHRT_MAX))));
                    dv.v = ushort(floor(clamp((v) * float(USHRT_MAX+1)/LM_PACKH + 0.5f, 0.0f, float(USHRT_MAX))));
                }
                else dv.u = dv.v = 0;
                dv.norm = usenorms && normals[i].normals[k] != bvec(128, 128, 128) ? encodenormal(normals[i].normals[k].tonormal().normalize()) : 0;
            }
            dst.verts = totalverts;
            dst.numverts |= numverts;
            totalverts += numverts;
            if(dst.numverts&LAYER_DUP) loopk(4)
            {
                if(k > 0 && (pos[k] == pos[0] || pos[k] == pos[k-1])) continue;
                vertinfo &bv = verts[totalverts++];
                bv.setxyz(pos[k]);
                bv.u = ushort(floor(clamp((blend->x + (blend->texcoords[k*2] / 255.0f) * (blend->w - 1)) * float(USHRT_MAX+1)/LM_PACKW, 0.0f, float(USHRT_MAX))));
                bv.v = ushort(floor(clamp((blend->y + (blend->texcoords[k*2+1] / 255.0f) * (blend->h - 1)) * float(USHRT_MAX+1)/LM_PACKH, 0.0f, float(USHRT_MAX))));
                bv.norm = usenorms && normals[i].normals[k] != bvec(128, 128, 128) ? encodenormal(normals[i].normals[k].tonormal().normalize()) : 0;
            }
        }
    }
    setsurfaces(c, dstsurfs, verts, totalverts);
}

void loadc_old(stream *f, cube &c, const ivec &co, int size, bool &failed)
{
    bool haschildren = false;
    int octsav = f->getchar();
    switch(octsav&0x7)
    {
        case OCTSAV_CHILDREN:
            c.children = loadchildren(f, co, size>>1, true, failed);
            return;

        case OCTSAV_LODCUBE: haschildren = true;    break;
        case OCTSAV_EMPTY:  emptyfaces(c);          break;
        case OCTSAV_SOLID:  solidfaces(c);          break;
        case OCTSAV_NORMAL: f->read(c.edges, 12); break;
        default: failed = true; return;
    }
    loopi(6) c.texture[i] = mapversion<14 ? f->getchar() : f->getlil<ushort>();
    if(mapversion < 7) f->seek(3, SEEK_CUR);
    else if(mapversion <= 31)
    {
        uchar mask = f->getchar();
        if(mask & 0x80)
        {
            int mat = f->getchar();
            if(mapversion < 27)
            {
                static const ushort matconv[] = { MAT_AIR, MAT_WATER, MAT_CLIP, MAT_GLASS|MAT_CLIP, MAT_NOCLIP, MAT_LAVA|MAT_DEATH, MAT_GAMECLIP, MAT_DEATH };
                c.material = size_t(mat) < sizeof(matconv)/sizeof(matconv[0]) ? matconv[mat] : MAT_AIR;
            }
            else c.material = convertoldmaterial(mat);
        }
        surfacecompat surfaces[12];
        normalscompat normals[6];
        mergecompat merges[6];
        int hassurfs = 0, hasnorms = 0, hasmerges = 0;
        if(mask & 0x3F)
        {
            int numsurfs = 6;
            loopi(numsurfs)
            {
                if(i >= 6 || mask & (1 << i))
                {
                    f->read(&surfaces[i], sizeof(surfacecompat));
                    lilswap(&surfaces[i].x, 2);
                    if(mapversion < 10) ++surfaces[i].lmid;
                    if(mapversion < 18)
                    {
                        if(surfaces[i].lmid >= LMID_AMBIENT1) ++surfaces[i].lmid;
                        if(surfaces[i].lmid >= LMID_BRIGHT1) ++surfaces[i].lmid;
                    }
                    if(mapversion < 19)
                    {
                        if(surfaces[i].lmid >= LMID_DARK) surfaces[i].lmid += 2;
                    }
                    if(i < 6)
                    {
                        if(mask & 0x40) { hasnorms |= 1<<i; f->read(&normals[i], sizeof(normalscompat)); }
                        if(surfaces[i].layer != 0 || surfaces[i].lmid != LMID_AMBIENT)
                            hassurfs |= 1<<i;
                        if(surfaces[i].layer&2) numsurfs++;
                    }
                }
            }
        }
        if(mapversion <= 8) edgespan2vectorcube(c);
        if(mapversion <= 11)
        {
            swap(c.faces[0], c.faces[2]);
            swap(c.texture[0], c.texture[4]);
            swap(c.texture[1], c.texture[5]);
            if(hassurfs&0x33)
            {
                swap(surfaces[0], surfaces[4]);
                swap(surfaces[1], surfaces[5]);
                hassurfs = (hassurfs&~0x33) | ((hassurfs&0x30)>>4) | ((hassurfs&0x03)<<4);
            }
        }
        if(mapversion >= 20)
        {
            if(octsav&0x80)
            {
                int merged = f->getchar();
                c.merged = merged&0x3F;
                if(merged&0x80)
                {
                    int mask = f->getchar();
                    if(mask)
                    {
                        hasmerges = mask&0x3F;
                        loopi(6) if(mask&(1<<i))
                        {
                            mergecompat *m = &merges[i];
                            f->read(m, sizeof(mergecompat));
                            lilswap(&m->u1, 4);
                            if(mapversion <= 25)
                            {
                                int uorigin = m->u1 & 0xE000, vorigin = m->v1 & 0xE000;
                                m->u1 = (m->u1 - uorigin) << 2;
                                m->u2 = (m->u2 - uorigin) << 2;
                                m->v1 = (m->v1 - vorigin) << 2;
                                m->v2 = (m->v2 - vorigin) << 2;
                            }
                        }
                    }
                }
            }
        }
        if(hassurfs || hasnorms || hasmerges)
            convertoldsurfaces(c, co, size, surfaces, hassurfs, normals, hasnorms, merges, hasmerges);
    }
    else
    {
        if(octsav&0x40)
        {
            if(mapversion <= 32)
            {
                int mat = f->getchar();
                c.material = convertoldmaterial(mat);
            }
            else c.material = f->getlil<ushort>();
        }
        if(octsav&0x80) c.merged = f->getchar();
        if(octsav&0x20)
        {
            int surfmask, totalverts;
            surfmask = f->getchar();
            totalverts = max(f->getchar(), 0);
            newcubeext(c, totalverts, false);
            memset(c.ext->surfaces, 0, sizeof(c.ext->surfaces));
            memset(c.ext->verts(), 0, totalverts*sizeof(vertinfo));
            int offset = 0;
            loopi(6) if(surfmask&(1<<i))
            {
                surfaceinfo &surf = c.ext->surfaces[i];
                f->read(&surf, sizeof(surfaceinfo));
                int vertmask = surf.verts, numverts = surf.totalverts();
                if(!numverts) { surf.verts = 0; continue; }
                surf.verts = offset;
                vertinfo *verts = c.ext->verts() + offset;
                offset += numverts;
                ivec v[4], n, vo = ivec(co).mask(0xFFF).shl(3);
                int layerverts = surf.numverts&MAXFACEVERTS, dim = dimension(i), vc = C[dim], vr = R[dim], bias = 0;
                genfaceverts(c, i, v);
                bool hasxyz = (vertmask&0x04)!=0, hasuv = (vertmask&0x40)!=0, hasnorm = (vertmask&0x80)!=0;
                if(hasxyz)
                {
                    ivec e1, e2, e3;
                    n.cross((e1 = v[1]).sub(v[0]), (e2 = v[2]).sub(v[0]));
                    if(n.iszero()) n.cross(e2, (e3 = v[3]).sub(v[0]));
                    bias = -n.dot(ivec(v[0]).mul(size).add(vo));
                }
                else
                {
                    int vis = layerverts < 4 ? (vertmask&0x02 ? 2 : 1) : 3, order = vertmask&0x01 ? 1 : 0, k = 0;
                    verts[k++].setxyz(v[order].mul(size).add(vo));
                    if(vis&1) verts[k++].setxyz(v[order+1].mul(size).add(vo));
                    verts[k++].setxyz(v[order+2].mul(size).add(vo));
                    if(vis&2) verts[k++].setxyz(v[(order+3)&3].mul(size).add(vo));
                }
                if(layerverts == 4)
                {
                    if(hasxyz && vertmask&0x01)
                    {
                        ushort c1 = f->getlil<ushort>(), r1 = f->getlil<ushort>(), c2 = f->getlil<ushort>(), r2 = f->getlil<ushort>();
                        ivec xyz;
                        xyz[vc] = c1; xyz[vr] = r1; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                        verts[0].setxyz(xyz);
                        xyz[vc] = c1; xyz[vr] = r2; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                        verts[1].setxyz(xyz);
                        xyz[vc] = c2; xyz[vr] = r2; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                        verts[2].setxyz(xyz);
                        xyz[vc] = c2; xyz[vr] = r1; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                        verts[3].setxyz(xyz);
                        hasxyz = false;
                    }
                    if(hasuv && vertmask&0x02)
                    {
                        int uvorder = (vertmask&0x30)>>4;
                        vertinfo &v0 = verts[uvorder], &v1 = verts[(uvorder+1)&3], &v2 = verts[(uvorder+2)&3], &v3 = verts[(uvorder+3)&3];
                        v0.u = f->getlil<ushort>(); v0.v = f->getlil<ushort>();
                        v2.u = f->getlil<ushort>(); v2.v = f->getlil<ushort>();
                        v1.u = v0.u; v1.v = v2.v;
                        v3.u = v2.u; v3.v = v0.v;
                        if(surf.numverts&LAYER_DUP)
                        {
                            vertinfo &b0 = verts[4+uvorder], &b1 = verts[4+((uvorder+1)&3)], &b2 = verts[4+((uvorder+2)&3)], &b3 = verts[4+((uvorder+3)&3)];
                            b0.u = f->getlil<ushort>(); b0.v = f->getlil<ushort>();
                            b2.u = f->getlil<ushort>(); b2.v = f->getlil<ushort>();
                            b1.u = b0.u; b1.v = b2.v;
                            b3.u = b2.u; b3.v = b0.v;
                        }
                        hasuv = false;
                    }
                }
                if(hasnorm && vertmask&0x08)
                {
                    ushort norm = f->getlil<ushort>();
                    loopk(layerverts) verts[k].norm = norm;
                    hasnorm = false;
                }
                if(hasxyz || hasuv || hasnorm) loopk(layerverts)
                {
                    vertinfo &v = verts[k];
                    if(hasxyz)
                    {
                        ivec xyz;
                        xyz[vc] = f->getlil<ushort>(); xyz[vr] = f->getlil<ushort>();
                        xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                        v.setxyz(xyz);
                    }
                    if(hasuv) { v.u = f->getlil<ushort>(); v.v = f->getlil<ushort>(); }
                    if(hasnorm) v.norm = f->getlil<ushort>();
                }
                if(surf.numverts&LAYER_DUP) loopk(layerverts)
                {
                    vertinfo &v = verts[k+layerverts], &t = verts[k];
                    v.setxyz(t.x, t.y, t.z);
                    if(hasuv) { v.u = f->getlil<ushort>(); v.v = f->getlil<ushort>(); }
                    v.norm = t.norm;
                }
            }
        }
    }

    c.children = (haschildren ? loadchildren(f, co, size>>1, true, failed) : NULL);
}

cube *loadchildren(stream *f, const ivec &co, int size, bool oldmap, bool &failed)
{
    cube *c = newcubes();
    loopi(8)
    {
        if(oldmap) loadc_old(f, c[i], ivec(i, co, size), size, failed);
        else loadc(f, c[i], ivec(i, co, size), size, failed);
        if(failed) break;
    }
    return c;
}

VAR(dbgvars, 0, 0, 1);

void savevslot(stream *f, VSlot &vs, int prev)
{
    f->putlil<int>(vs.changed);
    f->putlil<int>(prev);
    if(vs.changed & (1<<VSLOT_SHPARAM))
    {
        f->putlil<ushort>(vs.params.length());
        loopv(vs.params)
        {
            SlotShaderParam &p = vs.params[i];
            f->putlil<ushort>(strlen(p.name));
            f->write(p.name, strlen(p.name));
            loopk(4) f->putlil<float>(p.val[k]);
        }
    }
    if(vs.changed & (1<<VSLOT_SCALE)) f->putlil<float>(vs.scale);
    if(vs.changed & (1<<VSLOT_ROTATION)) f->putlil<int>(vs.rotation);
    if(vs.changed & (1<<VSLOT_OFFSET))
    {
        loopk(2) f->putlil<int>(vs.offset[k]);
    }
    if(vs.changed & (1<<VSLOT_SCROLL))
    {
        loopk(2) f->putlil<float>(vs.scroll[k]);
    }
    if(vs.changed & (1<<VSLOT_LAYER)) f->putlil<int>(vs.layer);
    if(vs.changed & (1<<VSLOT_ALPHA))
    {
        f->putlil<float>(vs.alphafront);
        f->putlil<float>(vs.alphaback);
    }
    if(vs.changed & (1<<VSLOT_COLOR))
    {
        loopk(3) f->putlil<float>(vs.colorscale[k]);
    }
    if(vs.changed & (1<<VSLOT_REFRACT))
    {
        f->putlil<float>(vs.refractscale);
        loopk(3) f->putlil<float>(vs.refractcolor[k]);
    }
    if(vs.changed & (1<<VSLOT_DETAIL)) f->putlil<int>(vs.detail);
}

void savevslots(stream *f, int numvslots)
{
    if(vslots.empty()) return;
    int *prev = new int[numvslots];
    memset(prev, -1, numvslots*sizeof(int));
    loopi(numvslots)
    {
        VSlot *vs = vslots[i];
        if(vs->changed) continue;
        for(;;)
        {
            VSlot *cur = vs;
            do vs = vs->next; while(vs && vs->index >= numvslots);
            if(!vs) break;
            prev[vs->index] = cur->index;
        }
    }
    int lastroot = 0;
    loopi(numvslots)
    {
        VSlot &vs = *vslots[i];
        if(!vs.changed) continue;
        if(lastroot < i) f->putlil<int>(-(i - lastroot));
        savevslot(f, vs, prev[i]);
        lastroot = i+1;
    }
    if(lastroot < numvslots) f->putlil<int>(-(numvslots - lastroot));
    delete[] prev;
}

void loadvslot(stream *f, VSlot &vs, int changed)
{
    vs.changed = changed;
    if(vs.changed & (1<<VSLOT_SHPARAM))
    {
        int numparams = f->getlil<ushort>();
        string name;
        loopi(numparams)
        {
            SlotShaderParam &p = vs.params.add();
            int nlen = f->getlil<ushort>();
            f->read(name, min(nlen, MAXSTRLEN-1));
            name[min(nlen, MAXSTRLEN-1)] = '\0';
            if(nlen >= MAXSTRLEN) f->seek(nlen - (MAXSTRLEN-1), SEEK_CUR);
            p.name = getshaderparamname(name);
            p.loc = -1;
            loopk(4) p.val[k] = f->getlil<float>();
        }
    }
    if(vs.changed & (1<<VSLOT_SCALE)) vs.scale = f->getlil<float>();
    if(vs.changed & (1<<VSLOT_ROTATION)) vs.rotation = clamp(f->getlil<int>(), 0, 7);
    if(vs.changed & (1<<VSLOT_OFFSET))
    {
        loopk(2) vs.offset[k] = f->getlil<int>();
    }
    if(vs.changed & (1<<VSLOT_SCROLL))
    {
        loopk(2) vs.scroll[k] = f->getlil<float>();
    }
    if(vs.changed & (1<<VSLOT_LAYER)) vs.layer = f->getlil<int>();
    if(vs.changed & (1<<VSLOT_ALPHA))
    {
        vs.alphafront = f->getlil<float>();
        vs.alphaback = f->getlil<float>();
    }
    if(vs.changed & (1<<VSLOT_COLOR))
    {
        loopk(3) vs.colorscale[k] = f->getlil<float>();
    }
    if(vs.changed & (1<<VSLOT_REFRACT))
    {
        vs.refractscale = f->getlil<float>();
        loopk(3) vs.refractcolor[k] = f->getlil<float>();
    }
    if(vs.changed & (1<<VSLOT_DETAIL)) vs.detail = f->getlil<int>();
}

void loadvslots(stream *f, int numvslots)
{
    int *prev = new (false) int[numvslots];
    if(!prev) return;
    memset(prev, -1, numvslots*sizeof(int));
    while(numvslots > 0)
    {
        int changed = f->getlil<int>();
        if(changed < 0)
        {
            loopi(-changed) vslots.add(new VSlot(NULL, vslots.length()));
            numvslots += changed;
        }
        else
        {
            prev[vslots.length()] = f->getlil<int>();
            loadvslot(f, *vslots.add(new VSlot(NULL, vslots.length())), changed);
            numvslots--;
        }
    }
    loopv(vslots) if(vslots.inrange(prev[i])) vslots[prev[i]]->next = vslots[i];
    delete[] prev;
}

bool save_world(const char *mname, bool nolms)
{
    if(!*mname) mname = game::getclientmap();
    setmapfilenames(*mname ? mname : "untitled");
    if(savebak) backup(ogzname, bakname);
    stream *f = opengzfile(ogzname, "wb");
    if(!f) { conoutf(CON_WARN, "could not write map to %s", ogzname); return false; }

    int numvslots = vslots.length();
    if(!nolms && !multiplayer(false))
    {
        numvslots = compactvslots();
        allchanged();
    }

    savemapprogress = 0;
    renderprogress(0, "saving map...");

    mapheader hdr;
    memcpy(hdr.magic, "TMAP", 4);
    hdr.version = MAPVERSION;
    hdr.headersize = sizeof(hdr);
    hdr.worldsize = worldsize;
    hdr.numents = 0;
    const vector<extentity *> &ents = entities::getents();
    loopv(ents) if(ents[i]->type!=ET_EMPTY || nolms) hdr.numents++;
    hdr.numpvs = nolms ? 0 : getnumviewcells();
    hdr.blendmap = shouldsaveblendmap();
    hdr.numvars = 0;
    hdr.numvslots = numvslots;
    enumerate(idents, ident, id,
    {
        if((id.type == ID_VAR || id.type == ID_FVAR || id.type == ID_SVAR) && id.flags&IDF_OVERRIDE && !(id.flags&IDF_READONLY) && id.flags&IDF_OVERRIDDEN) hdr.numvars++;
    });
    lilswap(&hdr.version, 8);
    f->write(&hdr, sizeof(hdr));

    enumerate(idents, ident, id,
    {
        if((id.type!=ID_VAR && id.type!=ID_FVAR && id.type!=ID_SVAR) || !(id.flags&IDF_OVERRIDE) || id.flags&IDF_READONLY || !(id.flags&IDF_OVERRIDDEN)) continue;
        f->putchar(id.type);
        f->putlil<ushort>(strlen(id.name));
        f->write(id.name, strlen(id.name));
        switch(id.type)
        {
            case ID_VAR:
                if(dbgvars) conoutf(CON_DEBUG, "wrote var %s: %d", id.name, *id.storage.i);
                f->putlil<int>(*id.storage.i);
                break;

            case ID_FVAR:
                if(dbgvars) conoutf(CON_DEBUG, "wrote fvar %s: %f", id.name, *id.storage.f);
                f->putlil<float>(*id.storage.f);
                break;

            case ID_SVAR:
                if(dbgvars) conoutf(CON_DEBUG, "wrote svar %s: %s", id.name, *id.storage.s);
                f->putlil<ushort>(strlen(*id.storage.s));
                f->write(*id.storage.s, strlen(*id.storage.s));
                break;
        }
    });

    if(dbgvars) conoutf(CON_DEBUG, "wrote %d vars", hdr.numvars);

    f->putchar((int)strlen(game::gameident()));
    f->write(game::gameident(), (int)strlen(game::gameident())+1);
    f->putlil<ushort>(entities::extraentinfosize());
    vector<char> extras;
    game::writegamedata(extras);
    f->putlil<ushort>(extras.length());
    f->write(extras.getbuf(), extras.length());

    f->putlil<ushort>(texmru.length());
    loopv(texmru) f->putlil<ushort>(texmru[i]);
    char *ebuf = new char[entities::extraentinfosize()];
    loopv(ents)
    {
        if(ents[i]->type!=ET_EMPTY || nolms)
        {
            entity tmp = *ents[i];
            lilswap(&tmp.o.x, 3);
            lilswap(&tmp.attr1, 5);
            f->write(&tmp, sizeof(entity));
            entities::writeent(*ents[i], ebuf);
            if(entities::extraentinfosize()) f->write(ebuf, entities::extraentinfosize());
        }
    }
    delete[] ebuf;

    savevslots(f, numvslots);

    renderprogress(0, "saving octree...");
    savec(worldroot, ivec(0, 0, 0), worldsize>>1, f, nolms);

    if(!nolms)
    {
        if(getnumviewcells()>0) { renderprogress(0, "saving pvs..."); savepvs(f); }
    }
    if(shouldsaveblendmap()) { renderprogress(0, "saving blendmap..."); saveblendmap(f); }

    delete f;
    conoutf("wrote map file %s", ogzname);
    return true;
}

static uint mapcrc = 0;

uint getmapcrc() { return mapcrc; }
void clearmapcrc() { mapcrc = 0; }
extern void loadLightEntities(bool msg, const char *mapName);
extern void replaceskycubes();

VARR(fixskycubes, 0, 0, 1);

void finishload(const char *mname, const char *cname, Texture *mapshot)
{
    preloadusedmapmodels(true);

    game::preload();
    flushpreloadedmodels();

    preloadmapsounds();

    entitiesinoctanodes();

    loadLightEntities(false, mname); // sauerract | load light files for maps with broken lighting
    attachentities();

    if(fixskycubes) replaceskycubes();
    mpcalclight(true, false); // sauerract | some maps needs a remip to have good looking lights

    allchanged(true);

    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    if(maptitle[0] && strcmp(maptitle, "Untitled Map by Unknown")) conoutf(CON_ECHO, "%s", maptitle);

    startmap(cname ? cname : mname);
}

bool trynewmap(const char *mname, const char *cname)
{
    int loadingstart = SDL_GetTicks();
    setmapfilenames(mname, cname);
    stream *f = opengzfile(ogzname, "rb");
    if(!f) { conoutf(CON_ERROR, "could not read map %s", ogzname); return false; }

    mapheader hdr;
    octaheader ohdr;
    memset(&ohdr, 0, sizeof(ohdr));
    if(!loadmapheader(f, ogzname, hdr, ohdr)) { delete f; return false; }

    resetmap();

    Texture *mapshot = textureload(picname, 3, true, false);
    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    setvar("mapversion", hdr.version, true, false);

    renderprogress(0, "clearing world...");

    freeocta(worldroot);
    worldroot = NULL;

    int worldscale = 0;
    while(1<<worldscale < hdr.worldsize) worldscale++;
    setvar("mapsize", 1<<worldscale, true, false);
    setvar("mapscale", worldscale, true, false);

    renderprogress(0, "loading vars...");

    loopi(hdr.numvars)
    {
        int type = f->getchar(), ilen = f->getlil<ushort>();
        string name;
        f->read(name, min(ilen, MAXSTRLEN-1));
        name[min(ilen, MAXSTRLEN-1)] = '\0';
        if(ilen >= MAXSTRLEN) f->seek(ilen - (MAXSTRLEN-1), SEEK_CUR);
        ident *id = getident(name);
        tagval val;
        string str;
        switch(type)
        {
            case ID_VAR: val.setint(f->getlil<int>()); break;
            case ID_FVAR: val.setfloat(f->getlil<float>()); break;
            case ID_SVAR:
            {
                int slen = f->getlil<ushort>();
                f->read(str, min(slen, MAXSTRLEN-1));
                str[min(slen, MAXSTRLEN-1)] = '\0';
                if(slen >= MAXSTRLEN) f->seek(slen - (MAXSTRLEN-1), SEEK_CUR);
                val.setstr(str);
                break;
            }
            default: continue;
        }
        if(id && id->flags&IDF_OVERRIDE) switch(id->type)
        {
            case ID_VAR:
            {
                int i = val.getint();
                if(id->minval <= id->maxval && i >= id->minval && i <= id->maxval)
                {
                    setvar(name, i);
                    if(dbgvars) conoutf(CON_DEBUG, "read var %s: %d", name, i);
                }
                break;
            }
            case ID_FVAR:
            {
                float f = val.getfloat();
                if(id->minvalf <= id->maxvalf && f >= id->minvalf && f <= id->maxvalf)
                {
                    setfvar(name, f);
                    if(dbgvars) conoutf(CON_DEBUG, "read fvar %s: %f", name, f);
                }
                break;
            }
            case ID_SVAR:
                setsvar(name, val.getstr());
                if(dbgvars) conoutf(CON_DEBUG, "read svar %s: %s", name, val.getstr());
                break;
        }
    }
    if(dbgvars) conoutf(CON_DEBUG, "read %d vars", hdr.numvars);

    string gametype;
    bool samegame = true;
    int len = f->getchar();
    if(len >= 0) f->read(gametype, len+1);
    gametype[max(len, 0)] = '\0';
    if(strcmp(gametype, game::gameident())!=0)
    {
        samegame = false;
        conoutf(CON_WARN, "WARNING: loading map from %s game, ignoring entities except for lights/mapmodels", gametype);
    }
    int eif = f->getlil<ushort>();
    int extrasize = f->getlil<ushort>();
    vector<char> extras;
    f->read(extras.pad(extrasize), extrasize);
    if(samegame) game::readgamedata(extras);

    texmru.shrink(0);
    ushort nummru = f->getlil<ushort>();
    loopi(nummru) texmru.add(f->getlil<ushort>());

    renderprogress(0, "loading entities...");

    vector<extentity *> &ents = entities::getents();
    int einfosize = entities::extraentinfosize();
    char *ebuf = einfosize > 0 ? new char[einfosize] : NULL;
    loopi(min(hdr.numents, MAXENTS))
    {
        extentity &e = *entities::newentity();
        ents.add(&e);
        f->read(&e, sizeof(entity));
        lilswap(&e.o.x, 3);
        lilswap(&e.attr1, 5);
        fixent(e, hdr.version);
        if(samegame)
        {
            if(einfosize > 0) f->read(ebuf, einfosize);
            entities::readent(e, ebuf, mapversion);
        }
        else
        {
            if(eif > 0) f->seek(eif, SEEK_CUR);
            if(e.type>=ET_GAMESPECIFIC)
            {
                entities::deleteentity(ents.pop());
                continue;
            }
        }
        if(!insideworld(e.o))
        {
            if(e.type != ET_LIGHT && e.type != ET_SPOTLIGHT)
            {
                conoutf(CON_WARN, "warning: ent outside of world: enttype[%s] index %d (%f, %f, %f)", entities::entname(e.type), i, e.o.x, e.o.y, e.o.z);
            }
        }
    }
    if(ebuf) delete[] ebuf;

    if(hdr.numents > MAXENTS)
    {
        conoutf(CON_WARN, "warning: map has %d entities", hdr.numents);
        f->seek((hdr.numents-MAXENTS)*(samegame ? sizeof(entity) + einfosize : eif), SEEK_CUR);
    }

    renderprogress(0, "loading slots...");
    loadvslots(f, hdr.numvslots);

    renderprogress(0, "loading octree...");
    bool failed = false;
    worldroot = loadchildren(f, ivec(0, 0, 0), hdr.worldsize>>1, false, failed);
    if(failed) conoutf(CON_ERROR, "garbage in map");

    renderprogress(0, "validating...");
    validatec(worldroot, hdr.worldsize>>1);

    if(!failed)
    {
        if(mapversion <= 0) loopi(ohdr.lightmaps)
        {
            int type = f->getchar();
            if(type&0x80)
            {
                f->getlil<ushort>();
                f->getlil<ushort>();
            }
            int bpp = 3;
            if(type&(1<<4) && (type&0x0F)!=2) bpp = 4;
            f->seek(bpp*LM_PACKW*LM_PACKH, SEEK_CUR);
        }

        if(hdr.numpvs > 0) loadpvs(f, hdr.numpvs);
        if(hdr.blendmap) loadblendmap(f, hdr.blendmap);
    }

    mapcrc = f->getcrc();
    delete f;

    conoutf("read map %s (%.1f seconds)", ogzname, (SDL_GetTicks()-loadingstart)/1000.0f);

    clearmainmenu();

    identflags |= IDF_OVERRIDDEN;
    execfile("config/default_map_settings.cfg", false);
    execfile(cfgname, false);
    identflags &= ~IDF_OVERRIDDEN;

    finishload(mname, cname, mapshot);

    return true;
}

bool tryoldmap(const char *mname, const char *cname)        // still supports all map formats that have existed since the earliest cube betas!
{
    int loadingstart = SDL_GetTicks();
    setmapfilenames(mname, cname);
    stream *f = opengzfile(ogzname, "rb");
    if(!f) { conoutf(CON_ERROR, "could not read map %s", ogzname); return false; }
    octaheader hdr;
    if(f->read(&hdr, 7*sizeof(int)) != 7*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); delete f; return false; }
    lilswap(&hdr.version, 6);
    if(memcmp(hdr.magic, "OCTA", 4) || hdr.worldsize <= 0|| hdr.numents < 0) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); delete f; return false; }
    if(hdr.version>MAPVERSION) { conoutf(CON_ERROR, "map %s requires a newer version of Cube 2: Sauerbraten", ogzname); delete f; return false; }
    compatheader chdr;
    if(hdr.version <= 28)
    {
        if(f->read(&chdr.lightprecision, sizeof(chdr) - 7*sizeof(int)) != sizeof(chdr) - 7*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); delete f; return false; }
    }
    else
    {
        int extra = 0;
        if(hdr.version <= 29) extra++;
        if(f->read(&hdr.blendmap, sizeof(hdr) - (7+extra)*sizeof(int)) != sizeof(hdr) - (7+extra)*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); delete f; return false; }
    }

    resetmap();

    Texture *mapshot = textureload(picname, 3, true, false);
    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    setvar("mapversion", hdr.version, true, false);

    if(hdr.version <= 28)
    {
        lilswap(&chdr.lightprecision, 3);
        if(chdr.lightprecision) setvar("lightprecision", chdr.lightprecision);
        if(chdr.lighterror) setvar("lighterror", chdr.lighterror);
        if(chdr.bumperror) setvar("bumperror", chdr.bumperror);
        setvar("lightlod", chdr.lightlod);
        if(chdr.ambient) setvar("ambient", chdr.ambient);
        setvar("skylight", (int(chdr.skylight[0])<<16) | (int(chdr.skylight[1])<<8) | int(chdr.skylight[2]));
        setvar("watercolour", (int(chdr.watercolour[0])<<16) | (int(chdr.watercolour[1])<<8) | int(chdr.watercolour[2]), true);
        setvar("waterfallcolour", (int(chdr.waterfallcolour[0])<<16) | (int(chdr.waterfallcolour[1])<<8) | int(chdr.waterfallcolour[2]));
        setvar("lavacolour", (int(chdr.lavacolour[0])<<16) | (int(chdr.lavacolour[1])<<8) | int(chdr.lavacolour[2]));
        setvar("fullbright", 0, true);
        if(chdr.lerpsubdivsize || chdr.lerpangle) setvar("lerpangle", chdr.lerpangle);
        if(chdr.lerpsubdivsize)
        {
            setvar("lerpsubdiv", chdr.lerpsubdiv);
            setvar("lerpsubdivsize", chdr.lerpsubdivsize);
        }
        setsvar("maptitle", chdr.maptitle);
        hdr.blendmap = chdr.blendmap;
        hdr.numvars = 0;
        hdr.numvslots = 0;
    }
    else
    {
        lilswap(&hdr.blendmap, 2);
        if(hdr.version <= 29) hdr.numvslots = 0;
        else lilswap(&hdr.numvslots, 1);
    }

    renderprogress(0, "clearing world...");

    freeocta(worldroot);
    worldroot = NULL;

    int worldscale = 0;
    while(1<<worldscale < hdr.worldsize) worldscale++;
    setvar("mapsize", 1<<worldscale, true, false);
    setvar("mapscale", worldscale, true, false);

    renderprogress(0, "loading vars...");

    loopi(hdr.numvars)
    {
        int type = f->getchar(), ilen = f->getlil<ushort>();
        string name;
        f->read(name, min(ilen, MAXSTRLEN-1));
        name[min(ilen, MAXSTRLEN-1)] = '\0';
        if(ilen >= MAXSTRLEN) f->seek(ilen - (MAXSTRLEN-1), SEEK_CUR);
        ident *id = getident(name);
        bool exists = id && id->type == type && id->flags&IDF_OVERRIDE;
        switch(type)
        {
            case ID_VAR:
            {
                int val = f->getlil<int>();
                if(exists && id->minval <= id->maxval) setvar(name, val);
                if(dbgvars) conoutf(CON_DEBUG, "read var %s: %d", name, val);
                break;
            }

            case ID_FVAR:
            {
                float val = f->getlil<float>();
                if(exists && id->minvalf <= id->maxvalf) setfvar(name, val);
                if(dbgvars) conoutf(CON_DEBUG, "read fvar %s: %f", name, val);
                break;
            }

            case ID_SVAR:
            {
                int slen = f->getlil<ushort>();
                string val;
                f->read(val, min(slen, MAXSTRLEN-1));
                val[min(slen, MAXSTRLEN-1)] = '\0';
                if(slen >= MAXSTRLEN) f->seek(slen - (MAXSTRLEN-1), SEEK_CUR);
                if(exists) setsvar(name, val);
                if(dbgvars) conoutf(CON_DEBUG, "read svar %s: %s", name, val);
                break;
            }
        }
    }
    if(dbgvars) conoutf(CON_DEBUG, "read %d vars", hdr.numvars);

    string gametype;
    copystring(gametype, "fps");
    bool samegame = true;
    int eif = 0;
    if(hdr.version>=16)
    {
        int len = f->getchar();
        f->read(gametype, len+1);
    }
    if(strcmp(gametype, game::gameident())!=0)
    {
        samegame = false;
        conoutf(CON_WARN, "WARNING: loading map from %s game, ignoring entities except for lights/mapmodels", gametype);
    }
    if(hdr.version>=16)
    {
        eif = f->getlil<ushort>();
        int extrasize = f->getlil<ushort>();
        vector<char> extras;
        f->read(extras.pad(extrasize), extrasize);
        if(samegame) game::readgamedata(extras);
    }

    texmru.shrink(0);
    if(hdr.version<14)
    {
        uchar oldtl[256];
        f->read(oldtl, sizeof(oldtl));
        loopi(256) texmru.add(oldtl[i]);
    }
    else
    {
        ushort nummru = f->getlil<ushort>();
        loopi(nummru) texmru.add(f->getlil<ushort>());
    }

    renderprogress(0, "loading entities...");

    vector<extentity *> &ents = entities::getents();
    int einfosize = entities::extraentinfosize();
    char *ebuf = einfosize > 0 ? new char[einfosize] : NULL;
    loopi(min(hdr.numents, MAXENTS))
    {
        extentity &e = *entities::newentity();
        ents.add(&e);
        f->read(&e, sizeof(entity));
        lilswap(&e.o.x, 3);
        lilswap(&e.attr1, 5);
        fixent(e, hdr.version);
        if(samegame)
        {
            if(einfosize > 0) f->read(ebuf, einfosize);
            entities::readent(e, ebuf, mapversion);
        }
        else
        {
            if(eif > 0) f->seek(eif, SEEK_CUR);
            if(e.type>=ET_GAMESPECIFIC || hdr.version<=14)
            {
                entities::deleteentity(ents.pop());
                continue;
            }
        }
        if(!insideworld(e.o))
        {
            if(e.type != ET_LIGHT && e.type != ET_SPOTLIGHT)
            {
                conoutf(CON_WARN, "warning: ent outside of world: enttype[%s] index %d (%f, %f, %f)", entities::entname(e.type), i, e.o.x, e.o.y, e.o.z);
            }
        }
        if(hdr.version <= 14 && e.type == ET_MAPMODEL)
        {
            e.o.z += e.attr3;
            if(e.attr4) conoutf(CON_WARN, "warning: mapmodel ent (index %d) uses texture slot %d", i, e.attr4);
            e.attr3 = e.attr4 = 0;
        }
    }
    if(ebuf) delete[] ebuf;

    if(hdr.numents > MAXENTS)
    {
        conoutf(CON_WARN, "warning: map has %d entities", hdr.numents);
        f->seek((hdr.numents-MAXENTS)*(samegame ? sizeof(entity) + einfosize : eif), SEEK_CUR);
    }

    renderprogress(0, "loading slots...");
    loadvslots(f, hdr.numvslots);

    renderprogress(0, "loading octree...");
    bool failed = false;
    worldroot = loadchildren(f, ivec(0, 0, 0), hdr.worldsize>>1, true, failed);
    if(failed) conoutf(CON_ERROR, "garbage in map");

    renderprogress(0, "validating...");
    validatec(worldroot, hdr.worldsize>>1);

    if(!failed)
    {
        if(hdr.version >= 7) loopi(hdr.lightmaps)
        {
            int type = f->getchar();
            if(type&0x80)
            {
                f->getlil<ushort>();
                f->getlil<ushort>();
            }
            int bpp = 3;
            if(type&(1<<4) && (type&0x0F)!=2) bpp = 4;
            f->seek(bpp*LM_PACKW*LM_PACKH, SEEK_CUR);
        }

        if(hdr.version >= 25 && hdr.numpvs > 0) loadpvs(f, hdr.numpvs);
        if(hdr.version >= 28 && hdr.blendmap) loadblendmap(f, hdr.blendmap);
    }

    mapcrc = f->getcrc();
    delete f;

    conoutf("read map %s (%.1f seconds)", ogzname, (SDL_GetTicks()-loadingstart)/1000.0f);

    clearmainmenu();

    identflags |= IDF_OVERRIDDEN;
    execfile("config/default_map_settings.cfg", false);
    execfile(cfgname, false);
    identflags &= ~IDF_OVERRIDDEN;

    finishload(mname, cname, mapshot);

    return true;
}

bool load_world(const char *mname, const char *cname)
{
    if(trynewmap(mname, cname)) return true;
    else if(tryoldmap(mname, cname)) return true;
    return false;
}

void savecurrentmap() { save_world(game::getclientmap()); }
void savemap(char *mname) { save_world(mname); }

COMMAND(savemap, "s");
COMMAND(savecurrentmap, "");

void writeobj(char *name)
{
    defformatstring(fname, "%s.obj", name);
    stream *f = openfile(path(fname), "w");
    if(!f) return;
    f->printf("# obj file of Cube 2 level\n\n");
    defformatstring(mtlname, "%s.mtl", name);
    path(mtlname);
    f->printf("mtllib %s\n\n", mtlname);
    vector<vec> verts, texcoords;
    hashtable<vec, int> shareverts(1<<16), sharetc(1<<16);
    hashtable<int, vector<ivec2> > mtls(1<<8);
    vector<int> usedmtl;
    vec bbmin(1e16f, 1e16f, 1e16f), bbmax(-1e16f, -1e16f, -1e16f);
    loopv(valist)
    {
        vtxarray &va = *valist[i];
        if(!va.edata || !va.vdata) continue;
        ushort *edata = va.edata + va.eoffset;
        vertex *vdata = va.vdata;
        ushort *idx = edata;
        loopj(va.texs)
        {
            elementset &es = va.texelems[j];
            if(usedmtl.find(es.texture) < 0) usedmtl.add(es.texture);
            vector<ivec2> &keys = mtls[es.texture];
            loopk(es.length)
            {
                const vertex &v = vdata[idx[k]];
                const vec &pos = v.pos;
                const vec &tc = v.tc;
                ivec2 &key = keys.add();
                key.x = shareverts.access(pos, verts.length());
                if(key.x == verts.length())
                {
                    verts.add(pos);
                    bbmin.min(pos);
                    bbmax.max(pos);
                }
                key.y = sharetc.access(tc, texcoords.length());
                if(key.y == texcoords.length()) texcoords.add(tc);
            }
            idx += es.length;
        }
    }

    vec center(-(bbmax.x + bbmin.x)/2, -(bbmax.y + bbmin.y)/2, -bbmin.z);
    loopv(verts)
    {
        vec v = verts[i];
        v.add(center);
        if(v.y != floor(v.y)) f->printf("v %.3f ", -v.y); else f->printf("v %d ", int(-v.y));
        if(v.z != floor(v.z)) f->printf("%.3f ", v.z); else f->printf("%d ", int(v.z));
        if(v.x != floor(v.x)) f->printf("%.3f\n", v.x); else f->printf("%d\n", int(v.x));
    }
    f->printf("\n");
    loopv(texcoords)
    {
        const vec &tc = texcoords[i];
        f->printf("vt %.6f %.6f\n", tc.x, 1-tc.y);
    }
    f->printf("\n");

    usedmtl.sort();
    loopv(usedmtl)
    {
        vector<ivec2> &keys = mtls[usedmtl[i]];
        f->printf("g slot%d\n", usedmtl[i]);
        f->printf("usemtl slot%d\n\n", usedmtl[i]);
        for(int i = 0; i < keys.length(); i += 3)
        {
            f->printf("f");
            loopk(3) f->printf(" %d/%d", keys[i+2-k].x+1, keys[i+2-k].y+1);
            f->printf("\n");
        }
        f->printf("\n");
    }
    delete f;

    f = openfile(mtlname, "w");
    if(!f) return;
    f->printf("# mtl file of Cube 2 level\n\n");
    loopv(usedmtl)
    {
        VSlot &vslot = lookupvslot(usedmtl[i], false);
        f->printf("newmtl slot%d\n", usedmtl[i]);
        f->printf("map_Kd %s\n", vslot.slot->sts.empty() ? notexture->name : path(makerelpath("media", vslot.slot->sts[0].name)));
        f->printf("\n");
    }
    delete f;

    conoutf("generated model %s", fname);
}

COMMAND(writeobj, "s");

void writecollideobj(char *name)
{
    extern bool havesel;
    extern selinfo sel;
    if(!havesel)
    {
        conoutf(CON_ERROR, "geometry for collide model not selected");
        return;
    }
    vector<extentity *> &ents = entities::getents();
    extentity *mm = NULL;
    loopv(entgroup)
    {
        extentity &e = *ents[entgroup[i]];
        if(e.type != ET_MAPMODEL || !pointinsel(sel, e.o)) continue;
        mm = &e;
        break;
    }
    if(!mm) loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL || !pointinsel(sel, e.o)) continue;
        mm = &e;
        break;
    }
    if(!mm)
    {
        conoutf(CON_ERROR, "could not find map model in selection");
        return;
    }
    model *m = loadmapmodel(mm->attr1);
    if(!m)
    {
        mapmodelinfo *mmi = getmminfo(mm->attr1);
        if(mmi) conoutf(CON_ERROR, "could not load map model: %s", mmi->name);
        else conoutf(CON_ERROR, "could not find map model: %d", mm->attr1);
        return;
    }

    matrix4x3 xform;
    m->calctransform(xform);
    float scale = mm->attr5 > 0 ? mm->attr5/100.0f : 1;
    int yaw = mm->attr2, pitch = mm->attr3, roll = mm->attr4;
    matrix3 orient;
    orient.identity();
    if(scale != 1) orient.scale(scale);
    if(yaw) orient.rotate_around_z(sincosmod360(yaw));
    if(pitch) orient.rotate_around_x(sincosmod360(pitch));
    if(roll) orient.rotate_around_y(sincosmod360(-roll));
    xform.mul(orient, mm->o, matrix4x3(xform));
    xform.invert();

    ivec selmin = sel.o, selmax = ivec(sel.s).mul(sel.grid).add(sel.o);
    vector<vec> verts;
    hashtable<vec, int> shareverts;
    vector<int> tris;
    loopv(valist)
    {
        vtxarray &va = *valist[i];
        if(va.geommin.x > selmax.x || va.geommin.y > selmax.y || va.geommin.z > selmax.z ||
           va.geommax.x < selmin.x || va.geommax.y < selmin.y || va.geommax.z < selmin.z)
            continue;
        if(!va.edata || !va.vdata) continue;
        ushort *edata = va.edata + va.eoffset;
        vertex *vdata = va.vdata;
        ushort *idx = edata;
        loopj(va.texs)
        {
            elementset &es = va.texelems[j];
            for(int k = 0; k < es.length; k += 3)
            {
                const vec &v0 = vdata[idx[k]].pos, &v1 = vdata[idx[k+1]].pos, &v2 = vdata[idx[k+2]].pos;
                if(!v0.insidebb(selmin, selmax) || !v1.insidebb(selmin, selmax) || !v2.insidebb(selmin, selmax))
                    continue;
                int i0 = shareverts.access(v0, verts.length());
                if(i0 == verts.length()) verts.add(v0);
                tris.add(i0);
                int i1 = shareverts.access(v1, verts.length());
                if(i1 == verts.length()) verts.add(v1);
                tris.add(i1);
                int i2 = shareverts.access(v2, verts.length());
                if(i2 == verts.length()) verts.add(v2);
                tris.add(i2);
            }
            idx += es.length;
        }
    }

    defformatstring(fname, "%s.obj", name);
    stream *f = openfile(path(fname), "w");
    if(!f) return;
    f->printf("# obj file of Cube 2 collide model\n\n");
    loopv(verts)
    {
        vec v = xform.transform(verts[i]);
        if(v.y != floor(v.y)) f->printf("v %.3f ", -v.y); else f->printf("v %d ", int(-v.y));
        if(v.z != floor(v.z)) f->printf("%.3f ", v.z); else f->printf("%d ", int(v.z));
        if(v.x != floor(v.x)) f->printf("%.3f\n", v.x); else f->printf("%d\n", int(v.x));
    }
    f->printf("\n");
    for(int i = 0; i < tris.length(); i += 3)
       f->printf("f %d %d %d\n", tris[i+2]+1, tris[i+1]+1, tris[i]+1);
    f->printf("\n");

    delete f;

    conoutf("generated collide model %s", fname);
}

COMMAND(writecollideobj, "s");

#endif

