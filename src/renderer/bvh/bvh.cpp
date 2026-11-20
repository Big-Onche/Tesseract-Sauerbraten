#include "engine.h"
#include "bvh.h"

#include <stddef.h>

extern GLuint hdrfbo;

namespace renderer
{
    namespace worldbvh
    {
        static const uint LEAF_BIT = 0x80000000u;
        static const int MAX_BUILD_DEPTH = 64;

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

        // These layouts are mirrored exactly by config/glsl/bvh.glsl (std430).
        struct GPUNode
        {
            float bmin[3];
            uint data0;
            float bmax[3];
            uint data1;
        };

        struct GPUVertex
        {
            float position[4];
            float texcoord[4];
        };

        struct GPUTriangle
        {
            uint vertex[3], surface;
            uint triangle, flags, blas, reserved;
        };

        struct GPUBlas
        {
            float bmin[3];
            uint root;
            float bmax[3];
            uint triangleOffset;
            uint triangleCount, vertexOffset, vertexCount, reserved;
        };

        static_assert(sizeof(GPUNode) == 32, "GPUNode must match the std430 BVHNode layout");
        static_assert(sizeof(GPUVertex) == 32, "GPUVertex must match the std430 BVHVertex layout");
        static_assert(sizeof(GPUTriangle) == 32, "GPUTriangle must match the std430 BVHTriangle layout");
        static_assert(sizeof(GPUBlas) == 48, "GPUBlas must match the std430 BVHBlas layout");
        static_assert(offsetof(GPUNode, data0) == 12 && offsetof(GPUNode, bmax) == 16 && offsetof(GPUNode, data1) == 28,
                      "GPUNode std430 offsets changed");
        static_assert(offsetof(GPUTriangle, surface) == 12 && offsetof(GPUTriangle, triangle) == 16,
                      "GPUTriangle std430 offsets changed");
        static_assert(offsetof(GPUBlas, root) == 12 && offsetof(GPUBlas, bmax) == 16 && offsetof(GPUBlas, triangleOffset) == 28,
                      "GPUBlas std430 offsets changed");

        struct BuildPrimitive
        {
            Bounds bounds;
            vec centroid;
            uint reference;
        };

        struct SourceTriangle
        {
            uint vertex[3], surface, flags;
            Bounds bounds;
        };

        struct BLASRecord
        {
            ivec origin;
            int size;
            vtxarray *source;
            bool dirty;
            Bounds bounds;
            vector<GPUNode> nodes;
            vector<GPUVertex> vertices;
            vector<GPUTriangle> triangles;

            BLASRecord() : origin(0, 0, 0), size(0), source(NULL), dirty(true) {}
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
        static vector<GPUVertex> gpuVertices;
        static vector<GPUTriangle> gpuTriangles;
        static vector<GPUBlas> gpuBlases;
        static vector<uint> gpuInstances;
        static GLuint buffers[5] = { 0, 0, 0, 0, 0 };
        static Statistics stats;
        static bool sceneReady = false, forceRebuild = true;
        static uint tlasRoot = 0;

        static void settingchanged();
        VARFP(bvh, 0, 1, 1, settingchanged());
        VARP(bvhdebug, 0, 0, 7);
        VARFP(bvhbins, 4, 16, 64, settingchanged());
        VARFP(bvhleafsize, 1, 4, 16, settingchanged());
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

        static uint buildnode(vector<BuildPrimitive> &primitives, uint start, uint count, int leafSize, vector<GPUNode> &nodes,
                              vector<uint> &ordered, uint depth, uint &maximumDepth)
        {
            maximumDepth = max(maximumDepth, depth);
            Bounds bounds, centroidBounds;
            for(uint i = start; i < start + count; ++i)
            {
                bounds.include(primitives[i].bounds);
                centroidBounds.include(primitives[i].centroid);
            }

            uint nodeIndex = uint(nodes.length());
            nodes.add();
            setnodebounds(nodes[nodeIndex], bounds);

            if(count <= uint(leafSize) || depth >= MAX_BUILD_DEPTH)
            {
                uint first = uint(ordered.length());
                for(uint i = start; i < start + count; ++i) ordered.add(primitives[i].reference);
                nodes[nodeIndex].data0 = LEAF_BIT | first;
                nodes[nodeIndex].data1 = count;
                return nodeIndex;
            }

            struct Bin
            {
                Bounds bounds;
                uint count;
                Bin() : count(0) {}
            };

            float bestCost = FLT_MAX;
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
                    float cost = leftBounds[i].area()*leftCounts[i] + rightBounds[i].area()*rightCounts[i];
                    if(cost < bestCost)
                    {
                        bestCost = cost;
                        bestAxis = k;
                        bestSplit = i;
                    }
                }
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

            uint left = buildnode(primitives, start, middle - start, leafSize, nodes, ordered, depth + 1, maximumDepth);
            uint right = buildnode(primitives, middle, start + count - middle, leafSize, nodes, ordered, depth + 1, maximumDepth);
            nodes[nodeIndex].data0 = left;
            nodes[nodeIndex].data1 = right;
            return nodeIndex;
        }

        static uint surfaceflags(const vtxarray &va, int element)
        {
            int blendStart = va.texs, alphaBackStart = blendStart + va.blends, alphaFrontStart = alphaBackStart + va.alphaback,
                refractStart = alphaFrontStart + va.alphafront;
            if(element >= refractStart) return MATERIAL_REFRACT | MATERIAL_DOUBLE_SIDED;
            if(element >= alphaFrontStart) return MATERIAL_ALPHA_FRONT | MATERIAL_DOUBLE_SIDED;
            if(element >= alphaBackStart) return MATERIAL_ALPHA_BACK | MATERIAL_DOUBLE_SIDED;
            if(element >= blendStart) return MATERIAL_BLEND;
            return MATERIAL_OPAQUE;
        }

        static bool buildblas(BLASRecord &record, vtxarray &va)
        {
            record.nodes.shrink(0);
            record.vertices.shrink(0);
            record.triangles.shrink(0);
            record.bounds = Bounds();
            if(!va.vdata || !va.edata || !va.verts || !va.texelems) return false;

            resizevector(record.vertices, va.verts);
            loopi(va.verts)
            {
                const vertex &source = va.vdata[va.voffset + i];
                GPUVertex &destination = record.vertices[i];
                destination.position[0] = source.pos.x;
                destination.position[1] = source.pos.y;
                destination.position[2] = source.pos.z;
                destination.position[3] = 1.0f;
                destination.texcoord[0] = source.tc.x;
                destination.texcoord[1] = source.tc.y;
                destination.texcoord[2] = source.tc.z;
                destination.texcoord[3] = 0.0f;
            }

            vector<SourceTriangle> sourceTriangles;
            uint indexOffset = 0;
            int elementCount = va.texs + va.blends + va.alphaback + va.alphafront + va.refract;
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
                    triangle.flags = surfaceflags(va, i);
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
            buildnode(primitives, 0, uint(primitives.length()), bvhleafsize, record.nodes, ordered, 1, maximumDepth);
            stats.maximumDepth = max(stats.maximumDepth, maximumDepth);
            resizevector(record.triangles, ordered.length());
            loopv(ordered)
            {
                const SourceTriangle &source = sourceTriangles[ordered[i]];
                GPUTriangle &destination = record.triangles[i];
                loopk(3) destination.vertex[k] = source.vertex[k];
                destination.surface = source.surface;
                destination.triangle = ordered[i];
                destination.flags = source.flags;
                destination.blas = destination.reserved = 0;
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
            if(!buffers[0]) glGenBuffers_(5, buffers);
            int start = getclockmillis();
            uploadbuffer(0, gpuNodes.empty() ? NULL : gpuNodes.getbuf(), gpuNodes.length()*sizeof(GPUNode));
            uploadbuffer(1, gpuTriangles.empty() ? NULL : gpuTriangles.getbuf(), gpuTriangles.length()*sizeof(GPUTriangle));
            uploadbuffer(2, gpuVertices.empty() ? NULL : gpuVertices.getbuf(), gpuVertices.length()*sizeof(GPUVertex));
            uploadbuffer(3, gpuBlases.empty() ? NULL : gpuBlases.getbuf(), gpuBlases.length()*sizeof(GPUBlas));
            uploadbuffer(4, gpuInstances.empty() ? NULL : gpuInstances.getbuf(), gpuInstances.length()*sizeof(uint));
            glBindBuffer_(GL_SHADER_STORAGE_BUFFER, 0);
            stats.uploadMillis = getclockmillis() - start;
            stats.gpuBytes = gpuNodes.length()*sizeof(GPUNode) + gpuTriangles.length()*sizeof(GPUTriangle) +
                             gpuVertices.length()*sizeof(GPUVertex) + gpuBlases.length()*sizeof(GPUBlas) +
                             gpuInstances.length()*sizeof(uint);
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
            vector<uint> orderedInstances;
            uint tlasDepth = 0;
            if(!tlasPrimitives.empty())
                tlasRoot = buildnode(tlasPrimitives, 0, uint(tlasPrimitives.length()), 1, tlasNodes, orderedInstances, 1, tlasDepth);
            stats.maximumDepth = max(stats.maximumDepth, tlasDepth);

            gpuNodes = tlasNodes;
            gpuVertices.shrink(0);
            gpuTriangles.shrink(0);
            gpuBlases.shrink(0);
            gpuInstances = orderedInstances;
            loopv(activeRecords)
            {
                uint blasIndex = i;
                BLASRecord &record = records[activeRecords[i]];
                GPUBlas descriptor;
                loopk(3)
                {
                    descriptor.bmin[k] = record.bounds.minimum[k];
                    descriptor.bmax[k] = record.bounds.maximum[k];
                }
                descriptor.root = uint(gpuNodes.length());
                descriptor.triangleOffset = uint(gpuTriangles.length());
                descriptor.triangleCount = uint(record.triangles.length());
                descriptor.vertexOffset = uint(gpuVertices.length());
                descriptor.vertexCount = uint(record.vertices.length());
                descriptor.reserved = 0;
                uint nodeOffset = uint(gpuNodes.length());
                loopvj(record.nodes)
                {
                    GPUNode node = record.nodes[j];
                    if(!(node.data0&LEAF_BIT))
                    {
                        node.data0 += nodeOffset;
                        node.data1 += nodeOffset;
                    }
                    else node.data0 = LEAF_BIT | ((node.data0&~LEAF_BIT) + descriptor.triangleOffset);
                    gpuNodes.add(node);
                }
                loopvj(record.triangles)
                {
                    GPUTriangle triangle = record.triangles[j];
                    loopk(3) triangle.vertex[k] += descriptor.vertexOffset;
                    triangle.blas = blasIndex;
                    triangle.triangle = uint(gpuTriangles.length());
                    gpuTriangles.add(triangle);
                }
                gpuVertices.put(record.vertices.getbuf(), record.vertices.length());
                gpuBlases.add(descriptor);
            }

            stats.triangles = uint(gpuTriangles.length());
            stats.blasCount = uint(gpuBlases.length());
            stats.tlasNodes = uint(tlasNodes.length());
            stats.totalNodes = uint(gpuNodes.length());
            stats.buildMillis = getclockmillis() - start;
            stats.generation++;
            stats.generation = max(stats.generation, 1u);
            upload();
            forceRebuild = false;
            sceneReady = !gpuBlases.empty();
        }

        void clear()
        {
            records.shrink(0);
            gpuNodes.shrink(0);
            gpuVertices.shrink(0);
            gpuTriangles.shrink(0);
            gpuBlases.shrink(0);
            gpuInstances.shrink(0);
            sceneReady = false;
            forceRebuild = true;
        }

        void cleanup()
        {
            clear();
            if(buffers[0])
            {
                glDeleteBuffers_(5, buffers);
                memset(buffers, 0, sizeof(buffers));
            }
        }

        void bind()
        {
            if(!sceneReady) return;
            loopi(5) glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, NODE_BINDING + i, buffers[i]);
        }

        bool ready() { return bvh && sceneReady; }
        unsigned int generation() { return stats.generation; }

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
            conoutf("BVH generation %u: %u triangles, %u BLAS, %u TLAS nodes, %u total nodes, maximum depth %u", stats.generation,
                    stats.triangles, stats.blasCount, stats.tlasNodes, stats.totalNodes, stats.maximumDepth);
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
