// Included inside renderer::tesselation, after the material resolver.
// GPU layout: two texels per welded vertex, linked edge/corner lists, three texels per edge,
// four per incident surface and five per material. All indices are exact float integers.
static vector<vec4> seamdata, seamheights;
static hashtable<int, int> seammaterials;
static hashtable<GLuint, int> seamheightmaps;

struct seamblendcell
{
    int address, x, y;
    seamblendcell(int address, int x, int y) : address(address), x(x), y(y) {}
};
static vector<seamblendcell> seamblendcells;
static bool seamblenddirty = false;
static float seamblendscale = 0.0f;

void invalidateblendseams()
{
    if(seamblendscale != blendmaptexelscale()) seamsdirty = true;
    seamblenddirty = true;
    clearshadowcache();
}

static vec4 seamblendvalues(BlendMapCache *cache, int x, int y)
{
    return vec4(lookupblendtexel(cache, x, y)/255.0f, lookupblendtexel(cache, x+1, y)/255.0f,
                lookupblendtexel(cache, x, y+1)/255.0f, lookupblendtexel(cache, x+1, y+1)/255.0f);
}

static int addseamblend(const geometricedge &edge, BlendMapCache *cache)
{
    vector<float> cuts;
    cuts.add(0.0f);
    cuts.add(1.0f);
    loopi(2)
    {
        float delta = edge.b[i]-edge.a[i];
        if(fabsf(delta) < 1e-8f) continue;
        int first = int(floorf(min(edge.a[i], edge.b[i])*seamblendscale - 0.5f))+1;
        int last = int(floorf(max(edge.a[i], edge.b[i])*seamblendscale - 0.5f));
        for(int cell = first; cell <= last; ++cell)
        {
            float t = ((cell+0.5f)/seamblendscale-edge.a[i])/delta;
            if(t > 0.0f && t < 1.0f) cuts.add(t);
        }
    }
    cuts.sort();
    int header = seamdata.length(), count = 0;
    seamdata.add(vec4(0, 0, 0, 0));
    for(int i = 0; i+1 < cuts.length(); ++i)
    {
        if(cuts[i+1] <= cuts[i]) continue;
        vec middle(edge.a);
        middle.lerp(edge.b, (cuts[i]+cuts[i+1])*0.5f);
        int x = int(floorf(middle.x*seamblendscale - 0.5f)), y = int(floorf(middle.y*seamblendscale - 0.5f));
        seamdata.add(vec4(cuts[i], cuts[i+1], float(x), float(y)));
        seamblendcells.add(seamblendcell(seamdata.length(), x, y));
        seamdata.add(seamblendvalues(cache, x, y));
        ++count;
    }
    seamdata[header] = vec4(float(header+1), float(count), seamblendscale, 0);
    return header;
}

static bool sameseamsurface(const seamedge &a, const seamedge &b)
{
    return a.base == b.base && a.layer == b.layer && a.na == b.na && a.nb == b.nb && a.mapping == b.mapping;
}

static int addseamheight(Texture *texture)
{
    if(!texture || texture == notexture) return -1;
    int *existing = seamheightmaps.access(texture->id);
    if(existing) return *existing;
    int header = seamdata.length();
    seamdata.add(vec4(0, 0, 0, 0));
    int levels = 0;
    for(int w = texture->w, h = texture->h; w > 0 && h > 0; w = max(w/2, 1), h = max(h/2, 1))
    {
        ++levels;
        if(!texture->mipmap || (w == 1 && h == 1)) break;
    }
    loopi(levels) seamdata.add(vec4(0, 0, 0, 0));
    glBindTexture(GL_TEXTURE_2D, texture->id);
    loopi(levels)
    {
        GLint w = 0, h = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, i, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, i, GL_TEXTURE_HEIGHT, &h);
        if(w <= 0 || h <= 0) { levels = i; break; }
        vector<float> pixels;
        pixels.pad(w*h*4);
        glGetTexImage(GL_TEXTURE_2D, i, GL_RGBA, GL_FLOAT, pixels.getbuf());
        int offset = seamheights.length();
        for(int j = 0; j < w*h; j += 4)
        {
            vec4 heights(0, 0, 0, 0);
            loopk(4) if(j+k < w*h) heights[k] = pixels[4*(j+k)+3];
            seamheights.add(heights);
        }
        seamdata[header+1+i] = vec4(float(offset), float(w), float(h), float((texture->clamp&3) | (texture->type&Texture::MIRROR ? 4 : 0)));
    }
    seamdata[header] = vec4(float(header+1), float(levels), 0, 0);
    seamheightmaps[texture->id] = levels ? header : -1;
    return levels ? header : -1;
}

static int addseammaterial(const VSlot *source)
{
    if(!source) return -1;
    int *existing = seammaterials.access(source->index);
    if(existing) return *existing;
    VSlot &vslot = lookupvslot(source->index);
    tessmaterial material = gettessmaterial(vslot);
    if(!material.displaced || !material.parallax)
    {
        seammaterials[vslot.index] = -1;
        return -1;
    }
    seamheightbound = max(seamheightbound, max(fabsf(material.parallax[1]), fabsf(material.parallax[0] + material.parallax[1]))*
                                          material.worldperuv);
    int normal = addseamheight(material.normal), detail = addseamheight(material.detailnormal ? material.detailnormal : material.normal);
    if(normal < 0) return -1;
    if(detail < 0) detail = normal;
    const texrotation &rotation = texrotations[vslot.rotation];
    float xs = float(material.diffuse->xs), ys = float(material.diffuse->ys), scale = TEX_SCALE/vslot.scale;
    vec2 scroll(vslot.scroll);
    if(rotation.swapxy) swap(scroll.x, scroll.y);
    scroll.x *= rotation.flipx ? -1.0f : 1.0f;
    scroll.y *= rotation.flipy ? -1.0f : 1.0f;
    float detailscale = vslot.detail ? TEX_SCALE/lookupvslot(vslot.detail).scale : scale;
    int address = seamdata.length();
    seamdata.add(vec4(material.parallax[0]*material.worldperuv, material.parallax[1]*material.worldperuv, float(normal), float(detail)));
    seamdata.add(vec4(scale/(rotation.flipx ? -xs : xs), scale/(rotation.flipy ? -ys : ys),
                     -(rotation.swapxy ? vslot.offset.y : vslot.offset.x)/(rotation.flipx ? -xs : xs),
                     -(rotation.swapxy ? vslot.offset.x : vslot.offset.y)/(rotation.flipy ? -ys : ys)));
    bool hasdetail = material.detailnormal && material.detailnormal != notexture;
    seamdata.add(vec4(scale/xs, scale/ys, hasdetail ? detailscale/material.detailnormal->xs : scale/xs,
                     hasdetail ? detailscale/material.detailnormal->ys : scale/ys));
    seamdata.add(vec4(material.tripbias ? material.tripbias[0] : 0.3f, material.tripbias ? material.tripbias[1] : 0.3f,
                     material.tripbias ? material.tripbias[2] : 0.3f, rotation.swapxy ? 1.0f : 0.0f));
    seamdata.add(vec4(scroll.x, scroll.y, material.triplanar ? 1.0f : 0.0f, 0));
    seammaterials[vslot.index] = address;
    return address;
}

static float seamedgecoverage(int material, const vec &delta, const vec &mapping)
{
    if(material < 0) return 0.0f;
    const vec4 &displace = seamdata[material], &uv = seamdata[material+1], &scale = seamdata[material+2],
               &bias = seamdata[material+3], &options = seamdata[material+4];
    const vec4 &normal = seamdata[int(seamdata[int(displace.z)].x)], &detail = seamdata[int(seamdata[int(displace.w)].x)];
    if(options.z > 0.5f)
        return max(vec2(delta.y*scale.x*normal.y, delta.z*scale.y*normal.z).magnitude(),
                   max(vec2(delta.x*scale.x*normal.y, delta.z*scale.y*normal.z).magnitude(),
                       vec2(delta.x*scale.z*detail.y, delta.y*scale.w*detail.z).magnitude()));
    int dim = mapping.x > 0.5f ? 0 : (mapping.y > 0.5f ? 1 : 2);
    float s = dim == 0 ? delta.y : delta.x, t = dim == 2 ? delta.y : -delta.z;
    if(bias.w > 0.5f) swap(s, t);
    return vec2(s*uv.x*normal.y, t*uv.y*normal.z).magnitude();
}

static bool sameseammaterial(int a, int b)
{
    if(a == b) return true;
    if(a < 0 || b < 0) return false;
    loopi(5) if(seamdata[a+i] != seamdata[b+i]) return false;
    return true;
}

static bool sameseamnormal(const vec4 &a, const vec4 &b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static void addseamcorner(int vertex, int surface, int endpoint)
{
    // Deduplicate the two edges of a face and the duplicate texlayer draw. Valence must not bias the corner solution.
    vec4 normal = seamdata[surface+endpoint], plane = seamdata[surface+2];
    for(int node = int(seamdata[2*vertex+1].x); node >= 0; node = int(seamdata[node].z))
    {
        int other = int(seamdata[node].x), end = int(seamdata[node].y);
        if(sameseamnormal(seamdata[other+end], normal) && seamdata[other+2] == plane &&
           sameseammaterial(int(seamdata[other].w), int(seamdata[surface].w)) &&
           sameseammaterial(int(seamdata[other+1].w), int(seamdata[surface+1].w))) return;
    }
    int node = seamdata.length();
    seamdata.add(vec4(float(surface), float(endpoint), seamdata[2*vertex+1].x, 0));
    seamdata[2*vertex+1].x = float(node);
}

static bool uploadseamtexture(GLuint &texture, vector<vec4> &data, int width, int limit)
{
    int height = max((data.length()+width-1)/width, 1);
    if(height > limit || data.length() >= (1<<24)) return false;
    while(data.length() < width*height) data.add(vec4(0, 0, 0, 0));
    if(!texture) glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, data.getbuf());
    return true;
}

static void buildseamgroup(const geometricedge &key, vector<const seamedge *> &entries, BlendMapCache *blendcache)
{
    vector<const seamedge *> sides;
    loopv(entries)
    {
        bool duplicate = false;
        loopvj(sides) if(entries[i]->side == sides[j]->side && sameseamsurface(*entries[i], *sides[j]) && entries[i]->plane == sides[j]->plane)
        {
            duplicate = true;
            break;
        }
        if(!duplicate) sides.add(entries[i]);
    }
    const seamedge &first = *sides[0];
    int a = first.va, b = first.vb;
    bool compatible = sides.length() == 2 && sides[0]->side != sides[1]->side &&
                      sides[0]->na == sides[1]->na && sides[0]->nb == sides[1]->nb && sides[0]->mapping == sides[1]->mapping;
    bool pinned = sides.length() != 2;
    vec delta = vec(key.b).sub(key.a);
    float length = delta.magnitude(), budget = length*0.2f;
    loopv(sides)
    {
        vec cross;
        cross.cross(vec(sides[i]->opposite).sub(key.a), delta);
        budget = min(budget, 0.2f*cross.magnitude()/max(length, 1e-6f));
    }
    int blendprofile = -1;
    loopv(sides) if(sides[i]->layer) { blendprofile = addseamblend(key, blendcache); break; }
    vector<int> surfaces;
    float coverage = 0.0f;
    loopv(sides)
    {
        const seamedge &side = *sides[i];
        int base = addseammaterial(side.base), layer = addseammaterial(side.layer);
        if(base < 0 && layer < 0) pinned = true;
        coverage = max(coverage, max(seamedgecoverage(base, delta, side.mapping), seamedgecoverage(layer, delta, side.mapping)));
        int surface = seamdata.length();
        surfaces.add(surface);
        seamdata.add(vec4(side.na, float(base)));
        seamdata.add(vec4(side.nb, float(layer)));
        seamdata.add(vec4(side.mapping, side.layer ? 1.0f : 0.0f));
        seamdata.add(vec4(side.opposite, float(blendprofile)));
        addseamcorner(a, surface, 0);
        addseamcorner(b, surface, 1);
    }
    if(compatible)
    {
        int firstsurface = surfaces[0], second = surfaces[1];
        compatible = sameseammaterial(int(seamdata[firstsurface].w), int(seamdata[second].w)) &&
                     sameseammaterial(int(seamdata[firstsurface+1].w), int(seamdata[second+1].w)) &&
                     seamdata[firstsurface+2].w == seamdata[second+2].w;
    }
    int edge = seamdata.length();
    seamdata.add(vec4(float(a), float(b), float(edge+3), float(surfaces.length())));
    int flags = pinned ? 1 : (compatible ? 0 : 2);
    seamdata.add(vec4(float(flags), budget, seamtjoints.access(key.a) ? 1.0f : 0.0f,
                     seamtjoints.access(key.b) ? 1.0f : 0.0f));
    seamdata.add(vec4(coverage, 0, 0, 0));
    loopv(surfaces) seamdata.add(vec4(float(surfaces[i]), 0, 0, 0));
    loopi(2)
    {
        int vertex = i ? b : a, other = i ? a : b;
        int node = seamdata.length();
        seamdata.add(vec4(float(other), float(edge), seamdata[2*vertex].w, 0));
        seamdata[2*vertex].w = float(node);
        seamdata[2*vertex+1].y += 1;
        seamdata[2*vertex+1].z = min(seamdata[2*vertex+1].z, budget);
        if(flags) seamdata[2*vertex+1].w = max(seamdata[2*vertex+1].w, pinned ? 2.0f : 1.0f);
    }
    if(pinned) ++remeshinfo.seamopen;
    else if(compatible) ++remeshinfo.seamcompatible;
    else ++remeshinfo.seamincompatible;
}

static void syncseams()
{
    if(!seamsdirty)
    {
        if(!seamblenddirty || !seamtexture || seamtexturesize <= 0) return;
        seamblenddirty = false;
        if(seamblendcells.empty()) return;
        BlendMapCache *cache = newblendmapcache();
        setblendmaporigin(cache, ivec(0, 0, 0), worldsize);
        loopv(seamblendcells)
        {
            const seamblendcell &cell = seamblendcells[i];
            seamdata[cell.address] = seamblendvalues(cache, cell.x, cell.y);
        }
        freeblendmapcache(cache);
        GLint binding = 0;
        glActiveTexture_(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glBindTexture(GL_TEXTURE_2D, seamtexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, seamtexturesize, seamdata.length()/seamtexturesize,
                        GL_RGBA, GL_FLOAT, seamdata.getbuf());
        glBindTexture(GL_TEXTURE_2D, binding);
        return;
    }
    seamsdirty = seamblenddirty = false;
    seamblendcells.setsize(0);
    seamblendscale = blendmaptexelscale();
    seamdata.setsize(0);
    seamheights.setsize(0);
    seamheightbound = 0.0f;
    seammaterials.clear();
    seamheightmaps.clear();
    GLint binding = 0, packalignment = 4, unpackalignment = 4, limit = 0;
    glActiveTexture_(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packalignment);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackalignment);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    seamtexturesize = min(limit, 2048);
    loopv(seampositions)
    {
        seamdata.add(vec4(seampositions[i], -1));
        seamdata.add(vec4(-1, 0, 1e16f, 0));
    }
    hashtable<geometricedge, vector<const seamedge *> > groups;
    loopv(seamgeometrylist) loopvj(seamgeometrylist[i]->edges)
    {
        const seamedge &edge = seamgeometrylist[i]->edges[j];
        groups[edge.edge].add(&edge);
    }
    remeshinfo.seamcompatible = remeshinfo.seamincompatible = remeshinfo.seamopen = 0;
    BlendMapCache *blendcache = newblendmapcache();
    setblendmaporigin(blendcache, ivec(0, 0, 0), worldsize);
    enumeratekt(groups, geometricedge, key, vector<const seamedge *>, entries, buildseamgroup(key, entries, blendcache));
    freeblendmapcache(blendcache);
    if(!uploadseamtexture(seamtexture, seamdata, seamtexturesize, limit) ||
       !uploadseamtexture(seamheighttexture, seamheights, seamtexturesize, limit))
    {
        conoutf(CON_ERROR, "tessellation constraints exceed GPU table capacity; tessellation disabled");
        ::tesselation = 0;
        seamtexturesize = 0;
    }
    glBindTexture(GL_TEXTURE_2D, binding);
    glPixelStorei(GL_PACK_ALIGNMENT, packalignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackalignment);
    seamheights.setsize(0);
}

static void cleanupseams()
{
    if(seamtexture) glDeleteTextures(1, &seamtexture);
    if(seamheighttexture) glDeleteTextures(1, &seamheighttexture);
    seamtexture = seamheighttexture = 0;
    seamsdirty = true;
}
