#include "engine.h"
#include "bvh.h"

#include <stddef.h>

extern GLuint hdrfbo;

namespace renderer
{
    extern float pbrroughnessscale, pbrspecularscale;

    namespace worldbvh
    {
        static const uint LEAF_BIT = 0x80000000u;
        static const int MAX_BUILD_DEPTH = 64;
        static const float BVH_TRAVERSAL_COST = 1.0f, BVH_INTERSECTION_COST = 1.0f;

        struct Bounds
        {
            vec minimum, maximum;

            Bounds() : minimum(FLT_MAX, FLT_MAX, FLT_MAX), maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX) {}

            void include(const vec &p) { minimum.min(p); maximum.max(p); }
            void include(const Bounds &b) { minimum.min(b.minimum); maximum.max(b.maximum); }
            vec center() const { return vec(minimum).add(maximum).mul(0.5f); }
            vec extent() const { return vec(maximum).sub(minimum); }
            bool valid() const { return minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z; }

            float area() const
            {
                vec e = extent();
                return max(e.x*e.y + e.y*e.z + e.z*e.x, 0.0f);
            }
        };

        // These layouts are mirrored exactly by sauerract/config/glsl/bvh.glsl (std430).
        struct GPUNode
        {
            float bmin[3];
            uint data0;
            float bmax[3];
            uint data1;
        };

        struct GPUTriangle
        {
            float a[3];
            uint surface;
            float edge1[3];
            uint reserved0;
            float edge2[3];
            uint blas;
        };

        struct GPUTriangleUV
        {
            float texcoord[3][2];
        };

        struct GPUMaterial
        {
            uint diffuseHandle[2], glowHandle[2];
            float albedo[3], roughness;
            float emission[3], specular;
        };

        static_assert(sizeof(GPUNode) == 32, "GPUNode must match the std430 BVHNode layout");
        static_assert(sizeof(GPUTriangle) == 48, "GPUTriangle must match the std430 BVHTriangle layout");
        static_assert(sizeof(GPUTriangleUV) == 24, "GPUTriangleUV must match the std430 BVHTriangleUV layout");
        static_assert(sizeof(GPUMaterial) == 48, "GPUMaterial must match the std430 BVHMaterial layout");
        static_assert(offsetof(GPUMaterial, albedo) == 16 && offsetof(GPUMaterial, emission) == 32,
                      "GPUMaterial std430 offsets changed");
        static_assert(offsetof(GPUNode, data0) == 12 && offsetof(GPUNode, bmax) == 16 && offsetof(GPUNode, data1) == 28,
                      "GPUNode std430 offsets changed");
        static_assert(offsetof(GPUTriangle, surface) == 12 && offsetof(GPUTriangle, edge1) == 16 && offsetof(GPUTriangle, reserved0) == 28 &&
                          offsetof(GPUTriangle, edge2) == 32,
                      "GPUTriangle std430 offsets changed");

        struct BuildPrimitive
        {
            Bounds bounds;
            vec centroid;
            uint reference;
        };

        struct SourceTriangle
        {
            uint vertex[3], surface;
            Bounds bounds;
        };

        struct BLASRecord
        {
            ivec origin;
            int size;
            vtxarray *source;
            bool dirty;
            uint maximumDepth;
            Bounds bounds;
            vector<GPUNode> nodes;
            vector<GPUTriangle> triangles;
            vector<GPUTriangleUV> triangleUVs;

            BLASRecord() : origin(0, 0, 0), size(0), source(NULL), dirty(true), maximumDepth(0) {}
        };

        struct Statistics
        {
            uint triangles, blasCount, tlasNodes, totalNodes, maximumDepth, generation;
            uint rebuiltBlas, preservedBlas, fullRebuilds, incrementalRebuilds;
            int buildMillis, uploadMillis;
            size_t gpuBytes;

            Statistics() : triangles(0), blasCount(0), tlasNodes(0), totalNodes(0), maximumDepth(0), generation(0), rebuiltBlas(0),
                           preservedBlas(0), fullRebuilds(0), incrementalRebuilds(0), buildMillis(0), uploadMillis(0), gpuBytes(0) {}
        };

        static vector<BLASRecord> records;
        static vector<GPUNode> gpuNodes;
        static vector<GPUTriangle> gpuTriangles;
        static vector<GPUTriangleUV> gpuTriangleUVs;
        static vector<GPUMaterial> gpuMaterials;
        static vector<GLuint64> residentHandles;
        static GLuint buffers[4] = { 0, 0, 0, 0 };
        static Statistics stats;
        static bool sceneReady = false, forceRebuild = true;
        static uint tlasRoot = 0, traversalStackSize = 8;

        static void settingchanged();
        VARFP(bvh, 0, 1, 1, settingchanged());
        VARP(bvhdebug, 0, 0, 7);
        VARFP(bvhbins, 4, 16, 64, settingchanged());
        VARFP(bvhleafsize, 2, 4, 4, settingchanged());
        FVARP(bvhepsilon, 0.00001f, 0.001f, 1.0f);

        template<class T>
        static void resizevector(vector<T> &values, int length)
        {
            while(values.length() < length) values.add();
            if(values.length() > length) values.shrink(length);
        }

        static int longestaxis(const Bounds &bounds)
        {
            vec extent = bounds.extent();
            return extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
        }

        static void setnodebounds(GPUNode &node, const Bounds &bounds)
        {
            loopk(3)
            {
                node.bmin[k] = bounds.minimum[k];
                node.bmax[k] = bounds.maximum[k];
            }
        }

        static void buildnodeat(vector<BuildPrimitive> &primitives, uint start, uint count, int leafSize, vector<GPUNode> &nodes,
                                uint nodeIndex, vector<uint> &ordered, uint depth, uint depthLimit, uint &maximumDepth, bool forceLeafSize)
        {
            maximumDepth = max(maximumDepth, depth);
            Bounds bounds, centroidBounds;
            for(uint i = start; i < start + count; ++i)
            {
                bounds.include(primitives[i].bounds);
                centroidBounds.include(primitives[i].centroid);
            }

            setnodebounds(nodes[nodeIndex], bounds);
            auto makeleaf = [&]()
            {
                uint first = uint(ordered.length());
                for(uint i = start; i < start + count; ++i) ordered.add(primitives[i].reference);
                nodes[nodeIndex].data0 = LEAF_BIT | first;
                nodes[nodeIndex].data1 = count;
            };

            if(count <= uint(leafSize) || depth >= depthLimit)
            {
                makeleaf();
                return;
            }

            struct Bin
            {
                Bounds bounds;
                uint count;
                Bin() : count(0) {}
            };

            float bestCost = FLT_MAX, inverseParentArea = 1.0f/max(bounds.area(), 1e-12f);
            int bestAxis = -1, bestSplit = -1;
            loopk(3)
            {
                float minimum = centroidBounds.minimum[k], extent = centroidBounds.maximum[k] - minimum;
                if(extent <= 1e-7f) continue;
                vector<Bin> bins;
                resizevector(bins, bvhbins);
                for(uint i = start; i < start + count; ++i)
                {
                    int bin = min(int((primitives[i].centroid[k] - minimum)*bvhbins/extent), bvhbins - 1);
                    bins[bin].count++;
                    bins[bin].bounds.include(primitives[i].bounds);
                }
                vector<Bounds> leftBounds, rightBounds;
                vector<uint> leftCounts, rightCounts;
                resizevector(leftBounds, bvhbins - 1);
                resizevector(rightBounds, bvhbins - 1);
                resizevector(leftCounts, bvhbins - 1);
                resizevector(rightCounts, bvhbins - 1);
                Bounds left, right;
                uint leftCount = 0, rightCount = 0;
                for(int i = 0; i < bvhbins - 1; ++i)
                {
                    leftCount += bins[i].count;
                    if(bins[i].count) left.include(bins[i].bounds);
                    leftBounds[i] = left;
                    leftCounts[i] = leftCount;
                    int r = bvhbins - 1 - i;
                    rightCount += bins[r].count;
                    if(bins[r].count) right.include(bins[r].bounds);
                    rightBounds[r - 1] = right;
                    rightCounts[r - 1] = rightCount;
                }
                for(int i = 0; i < bvhbins - 1; ++i)
                {
                    if(!leftCounts[i] || !rightCounts[i]) continue;
                    float cost = BVH_TRAVERSAL_COST + BVH_INTERSECTION_COST*
                                 (leftBounds[i].area()*leftCounts[i] + rightBounds[i].area()*rightCounts[i])*inverseParentArea;
                    if(cost < bestCost)
                    {
                        bestCost = cost;
                        bestAxis = k;
                        bestSplit = i;
                    }
                }
            }

            if(!forceLeafSize && (bestAxis < 0 || bestCost >= BVH_INTERSECTION_COST*count))
            {
                makeleaf();
                return;
            }

            uint middle = start + count/2;
            if(bestAxis >= 0)
            {
                float minimum = centroidBounds.minimum[bestAxis], extent = centroidBounds.maximum[bestAxis] - minimum;
                middle = start;
                for(uint i = start; i < start + count; ++i)
                {
                    int bin = min(int((primitives[i].centroid[bestAxis] - minimum)*bvhbins/extent), bvhbins - 1);
                    if(bin <= bestSplit) swap(primitives[i], primitives[middle++]);
                }
            }
            if(middle == start || middle == start + count)
            {
                middle = start + count/2;
                int axis = longestaxis(centroidBounds);
                quicksort(&primitives[start], count,
                          [axis](const BuildPrimitive &a, const BuildPrimitive &b) { return a.centroid[axis] < b.centroid[axis]; });
            }

            uint childPair = uint(nodes.length());
            ASSERT(!(childPair&1u));
            nodes.add();
            nodes.add();
            nodes[nodeIndex].data0 = childPair;
            nodes[nodeIndex].data1 = 0;
            buildnodeat(primitives, start, middle - start, leafSize, nodes, childPair, ordered, depth + 1, depthLimit, maximumDepth,
                        forceLeafSize);
            buildnodeat(primitives, middle, start + count - middle, leafSize, nodes, childPair + 1, ordered, depth + 1, depthLimit,
                        maximumDepth, forceLeafSize);
        }

        static void buildnode(vector<BuildPrimitive> &primitives, uint start, uint count, int leafSize, vector<GPUNode> &nodes,
                              vector<uint> &ordered, uint depth, uint depthLimit, uint &maximumDepth, bool forceLeafSize = false)
        {
            ASSERT(nodes.empty());
            GPUNode padding = {};
            nodes.add();
            // Keep every allocated child pair 64-byte aligned, including the root's children.
            nodes.add(padding);
            buildnodeat(primitives, start, count, leafSize, nodes, 0, ordered, depth, depthLimit, maximumDepth, forceLeafSize);
        }

        static uint flatteneddepth(const vector<GPUNode> &tlasNodes, uint nodeIndex, uint depth, const vector<uint> &orderedBlases,
                                   const vector<uint> &activeRecords)
        {
            const GPUNode &node = tlasNodes[nodeIndex];
            if(node.data0&LEAF_BIT)
            {
                uint blas = orderedBlases[node.data0&~LEAF_BIT];
                return depth + records[activeRecords[blas]].maximumDepth - 1;
            }
            return max(flatteneddepth(tlasNodes, node.data0, depth + 1, orderedBlases, activeRecords),
                       flatteneddepth(tlasNodes, node.data0 + 1, depth + 1, orderedBlases, activeRecords));
        }

        static bool buildblas(BLASRecord &record, vtxarray &va)
        {
            record.nodes.shrink(0);
            record.triangles.shrink(0);
            record.triangleUVs.shrink(0);
            record.maximumDepth = 0;
            record.bounds = Bounds();
            if(!va.vdata || !va.edata || !va.verts || !va.texelems) return false;

            vector<SourceTriangle> sourceTriangles;
            uint indexOffset = 0;
            int elementCount = va.texs;
            loopi(elementCount)
            {
                const elementset &elements = va.texelems[i];
                for(int index = 0; index + 2 < elements.length; index += 3)
                {
                    const ushort *indices = va.edata + va.eoffset + indexOffset + index;
                    if(indices[0] < va.voffset || indices[1] < va.voffset || indices[2] < va.voffset) continue;
                    uint local[3] = { uint(indices[0] - va.voffset), uint(indices[1] - va.voffset), uint(indices[2] - va.voffset) };
                    if(local[0] >= uint(va.verts) || local[1] >= uint(va.verts) || local[2] >= uint(va.verts)) continue;
                    const vec &a = va.vdata[indices[0]].pos, &b = va.vdata[indices[1]].pos, &c = va.vdata[indices[2]].pos;
                    vec edge1 = vec(b).sub(a), edge2 = vec(c).sub(a), geometricNormal;
                    if(geometricNormal.cross(edge1, edge2).squaredlen() <= 1e-12f) continue;
                    SourceTriangle triangle;
                    triangle.vertex[0] = local[0]; triangle.vertex[1] = local[1]; triangle.vertex[2] = local[2];
                    triangle.surface = elements.texture;
                    triangle.bounds.include(a); triangle.bounds.include(b); triangle.bounds.include(c);
                    sourceTriangles.add(triangle);
                    record.bounds.include(triangle.bounds);
                }
                indexOffset += elements.length;
            }
            if(sourceTriangles.empty()) return false;

            vector<BuildPrimitive> primitives;
            resizevector(primitives, sourceTriangles.length());
            loopv(sourceTriangles)
            {
                primitives[i].bounds = sourceTriangles[i].bounds;
                primitives[i].centroid = sourceTriangles[i].bounds.center();
                primitives[i].reference = i;
            }
            vector<uint> ordered;
            uint maximumDepth = 0;
            buildnode(primitives, 0, uint(primitives.length()), bvhleafsize, record.nodes, ordered, 1, MAX_BUILD_DEPTH, maximumDepth);
            record.maximumDepth = maximumDepth;
            resizevector(record.triangles, ordered.length());
            resizevector(record.triangleUVs, ordered.length());
            loopv(ordered)
            {
                const SourceTriangle &source = sourceTriangles[ordered[i]];
                const vertex &a = va.vdata[va.voffset + source.vertex[0]], &b = va.vdata[va.voffset + source.vertex[1]],
                             &c = va.vdata[va.voffset + source.vertex[2]];
                GPUTriangle &destination = record.triangles[i];
                loopk(3)
                {
                    destination.a[k] = a.pos[k];
                    destination.edge1[k] = b.pos[k] - a.pos[k];
                    destination.edge2[k] = c.pos[k] - a.pos[k];
                }
                destination.surface = source.surface;
                destination.reserved0 = 0;
                destination.blas = 0;
                GPUTriangleUV &triangleUV = record.triangleUVs[i];
                triangleUV.texcoord[0][0] = a.tc.x; triangleUV.texcoord[0][1] = a.tc.y;
                triangleUV.texcoord[1][0] = b.tc.x; triangleUV.texcoord[1][1] = b.tc.y;
                triangleUV.texcoord[2][0] = c.tc.x; triangleUV.texcoord[2][1] = c.tc.y;
            }
            return true;
        }

        static BLASRecord *findrecord(const ivec &origin, int size)
        {
            loopv(records) if(records[i].origin == origin && records[i].size == size) return &records[i];
            return NULL;
        }

        static void uploadbuffer(int index, const void *data, size_t bytes)
        {
            glBindBuffer_(GL_SHADER_STORAGE_BUFFER, buffers[index]);
            glBufferData_(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(bytes), bytes ? data : NULL, GL_DYNAMIC_DRAW);
        }

        static void upload()
        {
            if(!buffers[0]) glGenBuffers_(4, buffers);
            int start = getclockmillis();
            uploadbuffer(0, gpuNodes.empty() ? NULL : gpuNodes.getbuf(), gpuNodes.length()*sizeof(GPUNode));
            uploadbuffer(1, gpuTriangles.empty() ? NULL : gpuTriangles.getbuf(), gpuTriangles.length()*sizeof(GPUTriangle));
            uploadbuffer(2, gpuTriangleUVs.empty() ? NULL : gpuTriangleUVs.getbuf(), gpuTriangleUVs.length()*sizeof(GPUTriangleUV));
            uploadbuffer(3, gpuMaterials.empty() ? NULL : gpuMaterials.getbuf(), gpuMaterials.length()*sizeof(GPUMaterial));
            glBindBuffer_(GL_SHADER_STORAGE_BUFFER, 0);
            stats.uploadMillis = getclockmillis() - start;
            stats.gpuBytes = gpuNodes.length()*sizeof(GPUNode) + gpuTriangles.length()*sizeof(GPUTriangle) +
                             gpuTriangleUVs.length()*sizeof(GPUTriangleUV) + gpuMaterials.length()*sizeof(GPUMaterial);
        }

        static const float *findmaterialparam(const VSlot &vslot, const char *name)
        {
            loopv(vslot.params) if(vslot.params[i].name && !strcmp(vslot.params[i].name, name)) return vslot.params[i].val;
            if(vslot.slot) loopv(vslot.slot->params)
            {
                if(vslot.slot->params[i].name && !strcmp(vslot.slot->params[i].name, name)) return vslot.slot->params[i].val;
            }
            if(vslot.slot && vslot.slot->shader) loopv(vslot.slot->shader->defaultparams)
            {
                if(vslot.slot->shader->defaultparams[i].name && !strcmp(vslot.slot->shader->defaultparams[i].name, name))
                    return vslot.slot->shader->defaultparams[i].val;
            }
            return NULL;
        }

        static Texture *findmaterialtexture(Slot &slot, int type)
        {
            if(!slot.loaded) slot.load();
            loopv(slot.sts)
            {
                Slot::Tex &texture = slot.sts[i];
                if(texture.type != type) continue;
                if(texture.t) return texture.t;
                if(texture.combined >= 0 && slot.sts.inrange(texture.combined)) return slot.sts[texture.combined].t;
                return NULL;
            }
            return NULL;
        }

        static GLuint64 materialhandle(Texture *texture)
        {
            if(!hasBindlessTexture || !texture || !texture->id) return 0;
            GLuint64 handle = glGetTextureHandleARB_(texture->id);
            loopv(residentHandles) if(residentHandles[i] == handle) return handle;
            glMakeTextureHandleResidentARB_(handle);
            residentHandles.add(handle);
            return handle;
        }

        static void buildmaterials()
        {
            uint maximumSurface = 0;
            loopv(gpuTriangles) maximumSurface = max(maximumSurface, gpuTriangles[i].surface);
            gpuMaterials.shrink(0);
            if(gpuTriangles.empty()) return;
            resizevector(gpuMaterials, int(maximumSurface + 1));
            loopv(gpuMaterials)
            {
                GPUMaterial &material = gpuMaterials[i];
                memset(&material, 0, sizeof(material));
                material.specular = -1.0f;
                if(!vslots.inrange(i) || !vslots[i] || !vslots[i]->slot) continue;
                const VSlot &vslot = *vslots[i];
                Slot &slot = *vslot.slot;
                GLuint64 diffuseHandle = materialhandle(findmaterialtexture(slot, TEX_DIFFUSE));
                GLuint64 glowHandle = materialhandle(findmaterialtexture(slot, TEX_GLOW));
                material.diffuseHandle[0] = uint(diffuseHandle);
                material.diffuseHandle[1] = uint(diffuseHandle>>32);
                material.glowHandle[0] = uint(glowHandle);
                material.glowHandle[1] = uint(glowHandle>>32);
                material.albedo[0] = vslot.colorscale.x;
                material.albedo[1] = vslot.colorscale.y;
                material.albedo[2] = vslot.colorscale.z;
                const float *gloss = findmaterialparam(vslot, "gloss"), *specscale = findmaterialparam(vslot, "specscale");
                float glossBucket = clamp(gloss ? gloss[0] : 1.0f, 0.0f, 2.0f);
                float exponent = 5.0f + 17.0f*glossBucket;
                material.roughness = specscale ? clamp(sqrtf(2.0f/(exponent + 2.0f))*renderer::pbrroughnessscale, 0.045f, 1.0f) : 1.0f;
                float specular = specscale ? max(specscale[0], 0.0f) : 0.0f;
                material.specular = clamp(0.08f*specular*renderer::pbrspecularscale, 0.0f, 0.25f);
                if(vslot.slot->texmask&(1<<TEX_GLOW))
                {
                    material.emission[0] = vslot.glowcolor.x;
                    material.emission[1] = vslot.glowcolor.y;
                    material.emission[2] = vslot.glowcolor.z;
                }
            }
        }

        void invalidate(vtxarray *va)
        {
            if(!va) return;
            loopv(records) if(records[i].source == va)
            {
                records[i].source = NULL;
                records[i].dirty = true;
                sceneReady = false;
                break;
            }
        }

        void sync(bool force)
        {
            if(!bvh)
            {
                sceneReady = false;
                return;
            }
            if(glversion < 430 || !glBindBufferBase_)
            {
                sceneReady = false;
                conoutf(CON_WARN, "BVH requires OpenGL 4.3 shader-storage buffers (OpenGL 4.5 recommended)");
                return;
            }

            int start = getclockmillis();
            bool full = force || forceRebuild;
            stats.maximumDepth = stats.rebuiltBlas = stats.preservedBlas = 0;
            if(full)
            {
                records.shrink(0);
                stats.fullRebuilds++;
            }
            else stats.incrementalRebuilds++;

            loopv(valist)
            {
                vtxarray *va = valist[i];
                BLASRecord *record = findrecord(va->o, va->size);
                if(!record)
                {
                    record = &records.add();
                    record->origin = va->o;
                    record->size = va->size;
                }
                if(record->source != va) record->dirty = true;
                record->source = va;
            }
            bool haveStaleRecords = false;
            loopv(records) if(!records[i].source) { haveStaleRecords = true; break; }
            if(haveStaleRecords)
            {
                vector<BLASRecord> liveRecords;
                loopv(records) if(records[i].source) liveRecords.add(records[i]);
                records.shrink(0);
                records.move(liveRecords);
            }
            loopv(records)
            {
                BLASRecord &record = records[i];
                if(record.dirty)
                {
                    buildblas(record, *record.source);
                    record.dirty = false;
                    stats.rebuiltBlas++;
                }
                else stats.preservedBlas++;
            }
            vector<uint> activeRecords;
            loopv(records) if(!records[i].triangles.empty()) activeRecords.add(i);
            vector<BuildPrimitive> tlasPrimitives;
            resizevector(tlasPrimitives, activeRecords.length());
            loopv(activeRecords)
            {
                BLASRecord &record = records[activeRecords[i]];
                tlasPrimitives[i].bounds = record.bounds;
                tlasPrimitives[i].centroid = record.bounds.center();
                tlasPrimitives[i].reference = i;
            }
            vector<GPUNode> tlasNodes;
            vector<uint> orderedBlases;
            uint tlasDepth = 0;
            if(!tlasPrimitives.empty())
            {
                uint depthLimit = max(uint(MAX_BUILD_DEPTH), uint(activeRecords.length()));
                buildnode(tlasPrimitives, 0, uint(tlasPrimitives.length()), 1, tlasNodes, orderedBlases, 1, depthLimit, tlasDepth, true);
            }
            stats.maximumDepth = tlasNodes.empty() ? 0 : flatteneddepth(tlasNodes, 0, 1, orderedBlases, activeRecords);
            traversalStackSize = max((stats.maximumDepth + 8u)&~7u, 8u);

            vector<uint> tlasLeaves;
            resizevector(tlasLeaves, activeRecords.length());
            loopv(tlasLeaves) tlasLeaves[i] = ~0u;
            bool validTLAS = true;
            loopv(tlasNodes)
            {
                const GPUNode &node = tlasNodes[i];
                if(!(node.data0&LEAF_BIT)) continue;
                uint orderedIndex = node.data0&~LEAF_BIT;
                if(node.data1 != 1 || orderedIndex >= uint(orderedBlases.length()) ||
                   orderedBlases[orderedIndex] >= uint(tlasLeaves.length()))
                {
                    validTLAS = false;
                    break;
                }
                tlasLeaves[orderedBlases[orderedIndex]] = i;
            }
            loopv(tlasLeaves) if(tlasLeaves[i] == ~0u)
            {
                validTLAS = false;
                break;
            }
            if(!validTLAS)
            {
                sceneReady = false;
                conoutf(CON_ERROR, "failed to flatten BVH: TLAS leaves must contain exactly one BLAS");
                return;
            }

            gpuNodes = tlasNodes;
            tlasRoot = 0;
            gpuTriangles.shrink(0);
            gpuTriangleUVs.shrink(0);
            loopv(activeRecords)
            {
                BLASRecord &record = records[activeRecords[i]];
                uint triangleOffset = uint(gpuTriangles.length()), descendantOffset = uint(gpuNodes.length()) - 2;
                GPUNode root = record.nodes[0];
                if(!(root.data0&LEAF_BIT)) root.data0 += descendantOffset;
                else root.data0 = LEAF_BIT | ((root.data0&~LEAF_BIT) + triangleOffset);
                gpuNodes[tlasLeaves[i]] = root;
                for(int j = 2; j < record.nodes.length(); ++j)
                {
                    GPUNode node = record.nodes[j];
                    if(!(node.data0&LEAF_BIT)) node.data0 += descendantOffset;
                    else node.data0 = LEAF_BIT | ((node.data0&~LEAF_BIT) + triangleOffset);
                    gpuNodes.add(node);
                }
                loopvj(record.triangles)
                {
                    GPUTriangle triangle = record.triangles[j];
                    triangle.blas = i;
                    gpuTriangles.add(triangle);
                }
                gpuTriangleUVs.put(record.triangleUVs.getbuf(), record.triangleUVs.length());
            }
            loopv(gpuNodes) if(i != 1 && !(gpuNodes[i].data0&LEAF_BIT))
            {
                ASSERT(!(gpuNodes[i].data0&1u));
                ASSERT(gpuNodes[i].data0 + 1 < uint(gpuNodes.length()));
            }

            buildmaterials();

            stats.triangles = uint(gpuTriangles.length());
            stats.blasCount = uint(activeRecords.length());
            stats.tlasNodes = tlasNodes.empty() ? 0 : uint(tlasNodes.length() - 1);
            stats.totalNodes = uint(gpuNodes.length());
            stats.buildMillis = getclockmillis() - start;
            stats.generation++;
            stats.generation = max(stats.generation, 1u);
            upload();
            forceRebuild = false;
            sceneReady = !activeRecords.empty();
        }

        void clear()
        {
            if(glMakeTextureHandleNonResidentARB_) loopv(residentHandles) glMakeTextureHandleNonResidentARB_(residentHandles[i]);
            residentHandles.shrink(0);
            records.shrink(0);
            gpuNodes.shrink(0);
            gpuTriangles.shrink(0);
            gpuTriangleUVs.shrink(0);
            gpuMaterials.shrink(0);
            sceneReady = false;
            forceRebuild = true;
        }

        void cleanup()
        {
            clear();
            if(buffers[0])
            {
                glDeleteBuffers_(4, buffers);
                memset(buffers, 0, sizeof(buffers));
            }
        }

        void bind()
        {
            if(!sceneReady) return;
            loopi(4) glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, NODE_BINDING + i, buffers[i]);
        }

        bool ready() { return bvh && sceneReady; }
        unsigned int generation() { return stats.generation; }
        unsigned int root() { return tlasRoot; }
        unsigned int stacksize() { return traversalStackSize; }
        unsigned int materialcount() { return uint(gpuMaterials.length()); }

        static void settingchanged()
        {
            forceRebuild = true;
            sceneReady = false;
            if(bvh && !valist.empty()) sync(true);
        }

        static void rebuildcommand() { forceRebuild = true; sync(true); }
        COMMANDN(bvhrebuild, rebuildcommand, "");

        static void printstats()
        {
            conoutf("BVH generation %u: %u triangles, %u BLAS, %u TLAS nodes, %u total nodes, maximum depth %u, stack %u", stats.generation,
                    stats.triangles, stats.blasCount, stats.tlasNodes, stats.totalNodes, stats.maximumDepth, traversalStackSize);
            conoutf("BVH build: %d ms CPU, upload submission: %d ms CPU, GPU buffers: %.2f MiB", stats.buildMillis, stats.uploadMillis,
                    stats.gpuBytes/(1024.0f*1024.0f));
            conoutf("BVH last update: %u rebuilt BLAS, %u preserved BLAS; %u full and %u incremental rebuilds total", stats.rebuiltBlas,
                    stats.preservedBlas, stats.fullRebuilds, stats.incrementalRebuilds);
            conoutf("BVH geometry: world element sets including blend/alpha/refract; sky, decals, material surfaces, and mapmodels excluded");
        }
        COMMANDN(bvhstats, printstats, "");

        void debugdraw()
        {
            if(!bvhdebug || !ready() || drawtex || mainmenu) return;
            Shader *shader = useshaderbyname("bvhdebug");
            if(!shader || shader->isnull()) return;
            glBindFramebuffer_(GL_FRAMEBUFFER, ::hdrfbo);
            glViewport(0, 0, vieww, viewh);
            bind();
            shader->set();
            LOCALPARAM(bvhInvViewProj, invcamprojmatrix);
            LOCALPARAMF(bvhParams, float(bvhdebug), bvhepsilon, float(farplane), float(tlasRoot));
            LOCALPARAMF(bvhViewport, float(vieww), float(viewh), 1.0f/max(vieww, 1), 1.0f/max(viewh, 1));
            screenquad(vieww, viewh);
        }
    }
}
