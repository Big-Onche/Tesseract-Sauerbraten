// CORTAO integration for Tesseract: Sauerbraten.
// Adapted from the CORTAO Carbon Engine Implementation v1.0.0.
// Copyright (c) 2026 Fenris Creations. Used under the MIT License.
// See packages/sauerract/cortao/LICENSE.md for the license.

#include "cortao.h"

namespace renderer
{
    namespace cortao
    {
        enum { SOURCE_SHARED = 0, SOURCE_PREPASS, SOURCE_MAIN, SOURCE_FILTER, SOURCE_TEMPORAL, SOURCE_DEBUG, SOURCE_COUNT };

        static char *sources[SOURCE_COUNT] = { NULL, NULL, NULL, NULL, NULL, NULL };
        static GLuint prepassprograms[2] = { 0, 0 }, mainprograms[5] = { 0, 0, 0, 0, 0 }, filterprogram = 0, temporalprogram = 0;
        static GLuint debugprogram = 0, packedtexture = 0, luttexture = 0, rawtexture = 0, filteredtexture = 0;
        static GLuint historytexture[2] = { 0, 0 }, historypacked[2] = { 0, 0 };
        static GLuint debugoutput = 0, clearfbo = 0;
        static int resourcewidth = -1, resourceheight = -1, mipcount = 0, historyindex = 0;
        static int historyquality = -1, historyfilter = -1;
        static uint temporalframe = 0;
        static bool initialized = false, supported = true, reportedfailure = false, historyvalid = false;
        static float historyradius = -1.0f, historymaxscreenratio = -1.0f, historystrength = -1.0f, historymipbias = -1.0f;
        static float historynormalbias = -1.0f, historyslopecompensation = -1.0f, historybackfacethreshold = -1.0f;
        static float historybackfaceboost = -1.0f, historyfilterdepth = -1.0f, historyfilternormal = -1.0f;
        static matrix4 previousscreenmatrix, previousviewmatrix;

        VARP(cortaoquality, 0, 1, 4);
        FVAR(cortaoradius, 0.01f, 7.0f, 256.0f);
        FVAR(cortaomaxscreenratio, 0.01f, 0.2f, 0.5f);
        FVARR(cortaostrength, 0.0f, 2.0f, 4.0f);
        FVAR(cortaomipbias, -8.0f, -4.0f, 2.0f);
        FVAR(cortaonormalbias, 0.0f, 1.0f, 4.0f);
        FVAR(cortaoslopecompensation, 1.0f, 4.0f, 16.0f);
        FVAR(cortaobackfacethreshold, -1.0f, 0.5f, 1.0f);
        FVAR(cortaobackfaceboost, 0.0f, 0.0f, 2.0f);

        VAR(cortaofilter, 0, 1, 1);
        FVAR(cortaofilterdepth, 0.0f, 512.0f, 2048.0f);
        FVAR(cortaofilternormal, 0.0f, 128.0f, 512.0f);
        VAR(cortaotemporal, 0, 1, 1);
        FVAR(cortaotemporalweight, 0.0f, 0.9f, 0.98f);
        FVAR(cortaotemporaldepth, 0.001f, 0.02f, 0.25f);
        FVAR(cortaotemporalnormal, 0.0f, 0.8f, 1.0f);

        VAR(cortaodebug, 0, 0, 10);
        VAR(cortaodebugmip, 0, 4, 7);
        FVAR(cortaodebugdepth, 1.0f, 64.0f, 65536.0f);
        FVAR(cortaodebugworldscale, 0.01f, 5.0f, 256.0f);
        FVAR(cortaodebugpixelscale, 1.0f, 128.0f, 4096.0f);
        FVAR(cortaodebugpositionscale, 1.0f, 64.0f, 65536.0f);
        VAR(cortaodebuglutslice, 0, 8, 15);

        static void deleteprogram(GLuint &program)
        {
            if(program) glDeleteProgram_(program);
            program = 0;
        }

        static void cleanupprograms()
        {
            loopi(2) deleteprogram(prepassprograms[i]);
            loopi(5) deleteprogram(mainprograms[i]);
            deleteprogram(filterprogram);
            deleteprogram(temporalprogram);
            deleteprogram(debugprogram);
        }

        static int sourceindex(const char *name)
        {
            static const char * const names[SOURCE_COUNT] = { "shared", "prepass", "main", "filter", "temporal", "debug" };
            loopi(SOURCE_COUNT) if(!strcmp(name, names[i])) return i;
            return -1;
        }

        void cortaoshader(char *name, char *source)
        {
            int index = sourceindex(name);
            if(index < 0)
            {
                conoutf(CON_WARN, "CORTAO: ignoring unknown compute source '%s'", name);
                return;
            }
            DELETEA(sources[index]);
            sources[index] = newstring(source);
            cleanupprograms();
            historyvalid = false;
            temporalframe = 0;
            supported = true;
            reportedfailure = false;
        }
        COMMAND(cortaoshader, "ss");

        static void showlog(GLuint object, bool shader, const char *name)
        {
            GLint length = 0;
            if(shader) glGetShaderiv_(object, GL_INFO_LOG_LENGTH, &length);
            else glGetProgramiv_(object, GL_INFO_LOG_LENGTH, &length);
            if(length <= 1) return;
            vector<char> log;
            log.setsize(length + 1);
            GLsizei written = 0;
            if(shader) glGetShaderInfoLog_(object, length, &written, log.getbuf());
            else glGetProgramInfoLog_(object, length, &written, log.getbuf());
            log[min(int(written), length)] = '\0';
            conoutf(CON_ERROR, "CORTAO %s %s log:\n%s", name, shader ? "compile" : "link", log.getbuf());
        }

        static GLuint compileprogram(const char *name, const char *defines, int stage)
        {
            if(!sources[SOURCE_SHARED] || !sources[stage])
            {
                conoutf(CON_ERROR, "CORTAO: compute shader source '%s' was not loaded from config/glsl/cortao.cfg", name);
                return 0;
            }
            static const char *version = "#version 430 core\n";
            const GLchar *parts[] = { version, defines, sources[SOURCE_SHARED], sources[stage] };
            GLuint shader = glCreateShader_(GL_COMPUTE_SHADER);
            glShaderSource_(shader, 4, parts, NULL);
            glCompileShader_(shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv_(shader, GL_COMPILE_STATUS, &compiled);
            showlog(shader, true, name);
            if(!compiled)
            {
                glDeleteShader_(shader);
                return 0;
            }

            GLuint program = glCreateProgram_();
            glAttachShader_(program, shader);
            glLinkProgram_(program);
            glDeleteShader_(shader);
            GLint linked = GL_FALSE;
            glGetProgramiv_(program, GL_LINK_STATUS, &linked);
            showlog(program, false, name);
            if(!linked)
            {
                glDeleteProgram_(program);
                return 0;
            }
            return program;
        }

        static GLint uniformloc(GLuint program, const char *name)
        {
            return glGetUniformLocation_(program, name);
        }

        static void uniform1i(GLuint program, const char *name, int value)
        {
            GLint location = uniformloc(program, name);
            if(location >= 0) glUniform1i_(location, value);
        }

        static void uniform1ui(GLuint program, const char *name, uint value)
        {
            GLint location = uniformloc(program, name);
            if(location >= 0) glUniform1ui_(location, value);
        }

        static void uniform1f(GLuint program, const char *name, float value)
        {
            GLint location = uniformloc(program, name);
            if(location >= 0) glUniform1f_(location, value);
        }

        static void uniform2f(GLuint program, const char *name, float x, float y)
        {
            GLint location = uniformloc(program, name);
            if(location >= 0) glUniform2f_(location, x, y);
        }

        static void uniform3f(GLuint program, const char *name, float x, float y, float z)
        {
            GLint location = uniformloc(program, name);
            if(location >= 0) glUniform3f_(location, x, y, z);
        }

        static void uniform4f(GLuint program, const char *name, float x, float y, float z, float w)
        {
            GLint location = uniformloc(program, name);
            if(location >= 0) glUniform4f_(location, x, y, z, w);
        }

        static void uniformmatrix4(GLuint program, const char *name, const matrix4 &value)
        {
            GLint location = uniformloc(program, name);
            if(location >= 0) glUniformMatrix4fv_(location, 1, GL_FALSE, value.a.v);
        }

        static void deleteresources()
        {
            GLuint textures[] =
            {
                packedtexture, luttexture, rawtexture, filteredtexture, historytexture[0], historytexture[1], historypacked[0], historypacked[1],
                debugoutput
            };
            loopi(9) if(textures[i]) glDeleteTextures(1, &textures[i]);
            packedtexture = luttexture = rawtexture = filteredtexture = historytexture[0] = historytexture[1] = 0;
            historypacked[0] = historypacked[1] = debugoutput = 0;
            if(clearfbo) glDeleteFramebuffers_(1, &clearfbo);
            clearfbo = 0;
            resourcewidth = resourceheight = -1;
            mipcount = 0;
            historyindex = 0;
            historyquality = historyfilter = -1;
            temporalframe = 0;
            historyvalid = false;
            initialized = false;
        }

        static void fail(const char *reason)
        {
            supported = false;
            initialized = false;
            if(!reportedfailure)
            {
                conoutf(CON_WARN, "CORTAO unavailable: %s; using original ambient obscurance", reason);
                reportedfailure = true;
            }
            cleanupprograms();
            deleteresources();
        }

        static const char *loadlut()
        {
            static const char *filename = "packages/sauerract/cortao/24x24x16x16_R16_UINT.bin";
            static const int elements = 24*24*16, bytes = elements*sizeof(ushort);
            static string error;
            stream *file = openfile(filename, "rb");
            if(!file)
            {
                formatstring(error, "lookup table not found at %s", filename);
                return error;
            }
            if(file->size() != bytes)
            {
                delete file;
                formatstring(error, "lookup table at %s is not exactly %d bytes", filename, bytes);
                return error;
            }
            vector<ushort> data;
            data.reserve(elements);
            data.advance(elements);
            size_t offset = 0;
            while(offset < bytes)
            {
                size_t read = file->read((uchar *)data.getbuf() + offset, bytes - offset);
                if(!read) break;
                offset += read;
            }
            delete file;
            if(offset != bytes)
            {
                formatstring(error, "could not read the complete lookup table at %s", filename);
                return error;
            }

            while(glGetError() != GL_NO_ERROR) {}
            glGenTextures(1, &luttexture);
            glBindTexture(GL_TEXTURE_3D, luttexture);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glTexImage3D_(GL_TEXTURE_3D, 0, GL_R16UI, 24, 24, 16, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, data.getbuf());
            GLenum glerror = glGetError();
            if(glerror != GL_NO_ERROR)
            {
                glDeleteTextures(1, &luttexture);
                luttexture = 0;
                formatstring(error, "OpenGL rejected the lookup-table upload (error 0x%04X)", glerror);
                return error;
            }
            return NULL;
        }

        static void allocaterectangle(GLuint &texture, GLenum format)
        {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_RECTANGLE, texture);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            GLenum external = format == GL_RGBA8 ? GL_RGBA : (format == GL_R32UI ? GL_RED_INTEGER : GL_RED);
            GLenum type = format == GL_R32UI ? GL_UNSIGNED_INT : GL_UNSIGNED_BYTE;
            glTexImage2D(GL_TEXTURE_RECTANGLE, 0, format, resourcewidth, resourceheight, 0, external, type, NULL);
        }

        static bool allocateresources(int width, int height)
        {
            deleteresources();
            glGetError();
            resourcewidth = width;
            resourceheight = height;
            mipcount = 1;
            while(mipcount < 8 && (max(width, height) >> mipcount) > 0) ++mipcount;

            glGenTextures(1, &packedtexture);
            glBindTexture(GL_TEXTURE_2D, packedtexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            if(hasTS) glTexStorage2D_(GL_TEXTURE_2D, mipcount, GL_R32UI, width, height);
            else loopi(mipcount)
                glTexImage2D(GL_TEXTURE_2D, i, GL_R32UI, max(width >> i, 1), max(height >> i, 1), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);

            allocaterectangle(rawtexture, GL_R8);
            allocaterectangle(filteredtexture, GL_R8);
            loopi(2)
            {
                allocaterectangle(historytexture[i], GL_R8);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                allocaterectangle(historypacked[i], GL_R32UI);
            }
            allocaterectangle(debugoutput, GL_RGBA8);
            glGenFramebuffers_(1, &clearfbo);
            GLint previousfbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, clearfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rawtexture, 0);
            GLenum drawbuffer = GL_COLOR_ATTACHMENT0;
            glDrawBuffers_(1, &drawbuffer);
            bool complete = glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer_(GL_FRAMEBUFFER, previousfbo);
            previousscreenmatrix.identity();
            previousviewmatrix.identity();
            return complete && glGetError() == GL_NO_ERROR;
        }

        static bool ensureprograms(bool multisample)
        {
            int ms = multisample ? 1 : 0;
            if(!prepassprograms[ms])
            {
                const char *defines = multisample ? "#define CORTAO_MSAA 1\n" : "#define CORTAO_MSAA 0\n";
                prepassprograms[ms] = compileprogram(multisample ? "prepass-msaa" : "prepass", defines, SOURCE_PREPASS);
            }
            int quality = clamp(cortaoquality, 0, 4);
            if(!mainprograms[quality])
            {
                static const int samples[] = { 4, 16, 24, 32, 64 };
                defformatstring(defines, "#define CORTAO_SAMPLE_COUNT %d\n#define CORTAO_RAY_COUNT 16\n", samples[quality]);
                mainprograms[quality] = compileprogram("main", defines, SOURCE_MAIN);
            }
            if(cortaofilter && !filterprogram) filterprogram = compileprogram("filter", "", SOURCE_FILTER);
            if(!temporalprogram) temporalprogram = compileprogram("temporal", "", SOURCE_TEMPORAL);
            if(cortaodebug && !debugprogram) debugprogram = compileprogram("debug", "", SOURCE_DEBUG);
            return prepassprograms[ms] && mainprograms[quality] && (!cortaofilter || filterprogram) && temporalprogram &&
                   (!cortaodebug || debugprogram);
        }

        bool init(int width, int height, bool multisample)
        {
            if(!hasCompute || glversion < 430)
            {
                fail("OpenGL 4.3 compute shaders and image load/store are required");
                return false;
            }
            if(!supported) return false;
            if(width <= 0 || height <= 0 || width > 65535 || height > 65535)
            {
                fail("framebuffer dimensions are outside CORTAO's 1..65535 range");
                return false;
            }
            if(resourcewidth != width || resourceheight != height)
            {
                if(!allocateresources(width, height))
                {
                    fail("failed to allocate compute textures");
                    return false;
                }
            }
            if(!luttexture)
            {
                const char *error = loadlut();
                if(error)
                {
                    fail(error);
                    return false;
                }
            }
            if(!ensureprograms(multisample))
            {
                fail("a compute shader failed to compile or link");
                return false;
            }
            initialized = true;
            return true;
        }

        static void cleartexture(GLuint texture, GLenum format)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, clearfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, texture, 0);
            GLenum drawbuffer = GL_COLOR_ATTACHMENT0;
            glDrawBuffers_(1, &drawbuffer);
            const GLfloat white[4] = { 1.0f, 1.0f, 1.0f, 1.0f }, black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            glClearBufferfv_(GL_COLOR, 0, format == GL_RGBA8 ? black : white);
        }

        static void clearpackedtexture()
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, clearfbo);
            GLenum drawbuffer = GL_COLOR_ATTACHMENT0;
            glDrawBuffers_(1, &drawbuffer);
            const GLuint clearvalue[4] = { 0, 0, 0, 0 };
            loopi(mipcount)
            {
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, packedtexture, i);
                glClearBufferuiv_(GL_COLOR, 0, clearvalue);
            }
        }

        static void bindinput(GLenum unit, GLenum target, GLuint texture)
        {
            glActiveTexture_(unit);
            glBindTexture(target, texture);
        }

        static void restorestate()
        {
            loopi(8) glBindImageTexture_(i, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
            glUseProgram_(0);
            Shader::lastshader = NULL;
            glActiveTexture_(GL_TEXTURE0);
        }

        static uint getrandomseed(uint frame)
        {
            uint value = 0x6D2B79F5u + frame*0x9E3779B9u;
            return value*(value^(value >> 15u));
        }

        static bool historyparameterschanged(int quality)
        {
            bool changed = historyquality != quality || historyfilter != cortaofilter || historyradius != cortaoradius ||
                           historymaxscreenratio != cortaomaxscreenratio || historystrength != cortaostrength ||
                           historymipbias != cortaomipbias || historynormalbias != cortaonormalbias ||
                           historyslopecompensation != cortaoslopecompensation || historybackfacethreshold != cortaobackfacethreshold ||
                           historybackfaceboost != cortaobackfaceboost || historyfilterdepth != cortaofilterdepth ||
                           historyfilternormal != cortaofilternormal;
            historyquality = quality;
            historyfilter = cortaofilter;
            historyradius = cortaoradius;
            historymaxscreenratio = cortaomaxscreenratio;
            historystrength = cortaostrength;
            historymipbias = cortaomipbias;
            historynormalbias = cortaonormalbias;
            historyslopecompensation = cortaoslopecompensation;
            historybackfacethreshold = cortaobackfacethreshold;
            historybackfaceboost = cortaobackfaceboost;
            historyfilterdepth = cortaofilterdepth;
            historyfilternormal = cortaofilternormal;
            return changed;
        }

        bool render(GLuint depthtexture, GLuint normaltexture, bool multisample, int depthformat, int width, int height,
                    const matrix4 &projection, const matrix4 &inverseprojection, const matrix4 &view, const matrix4 &inverseview,
                    const matrix4 &screen, const matrix3 &normalmatrix, float nearplanevalue, float farplanevalue)
        {
            if(!init(width, height, multisample)) return false;

            int quality = clamp(cortaoquality, 0, 4);
            if(historyparameterschanged(quality))
            {
                historyvalid = false;
                temporalframe = 0;
            }
            GLenum inputtarget = multisample ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_RECTANGLE;
            cleartexture(rawtexture, GL_R8);
            cleartexture(filteredtexture, GL_R8);
            if(cortaodebug) cleartexture(debugoutput, GL_RGBA8);
            clearpackedtexture();
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);

            bool temporalactive = cortaotemporal && !drawtex;
            uint randomseed = temporalactive ? getrandomseed(temporalframe) : 0x6D2B79F5u;

            timer *prepasstimer = begintimer("CORTAO prepass");
            GLuint program = prepassprograms[multisample ? 1 : 0];
            glUseProgram_(program);
            bindinput(GL_TEXTURE0, inputtarget, depthtexture);
            bindinput(GL_TEXTURE1, inputtarget, normaltexture);
            loopi(mipcount) glBindImageTexture_(i, packedtexture, i, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
            uniform2f(program, "uResolution", float(width), float(height));
            uniform1i(program, "uMipCount", mipcount);
            uniform1i(program, "uDepthFormat", depthformat);
            uniform1f(program, "uNearPlane", nearplanevalue);
            uniform1f(program, "uFarPlane", farplanevalue);
            uniform1f(program, "uDepthScale", (nearplanevalue - farplanevalue)/(nearplanevalue*farplanevalue));
            uniform1f(program, "uDepthOffset", 1.0f/nearplanevalue);
            uniform1ui(program, "uRandomSeed", randomseed);
            GLint normalmatrixloc = uniformloc(program, "uNormalMatrix");
            if(normalmatrixloc >= 0) glUniformMatrix3fv_(normalmatrixloc, 1, GL_FALSE, normalmatrix.a.v);
            glDispatchCompute_((width + 7)/8, (height + 7)/8, 1);
            glMemoryBarrier_(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            endtimer(prepasstimer);

            timer *maintimer = begintimer("CORTAO main pass");
            program = mainprograms[quality];
            glUseProgram_(program);
            bindinput(GL_TEXTURE1, inputtarget, normaltexture);
            if(multisample) bindinput(GL_TEXTURE5, inputtarget, normaltexture);
            bindinput(GL_TEXTURE2, GL_TEXTURE_2D, packedtexture);
            bindinput(GL_TEXTURE3, GL_TEXTURE_3D, luttexture);
            glBindImageTexture_(0, rawtexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
            uniform2f(program, "uResolution", float(width), float(height));
            uniform1i(program, "uMultisample", multisample ? 1 : 0);
            uniform1ui(program, "uRandomSeed", randomseed);
            float invx = inverseprojection.a.x, invy = inverseprojection.b.y;
            uniform1f(program, "uNormalBias", invx/float(width)*sqrtf(2.0f)*cortaonormalbias);
            float maxapparentradius = invy*cortaomaxscreenratio*2.0f;
            uniform1f(program, "uMaxRadius", cortaoradius);
            uniform1f(program, "uMaxApparentRadius", maxapparentradius/sqrtf(maxapparentradius*maxapparentradius + 1.0f));
            uniform4f(program, "uUnprojectParams", 2.0f*invx, 2.0f*invy, -invx + inverseprojection.d.x,
                      -invy + inverseprojection.d.y);
            uniform4f(program, "uProjectionParams", 0.5f*projection.a.x, 0.5f*projection.b.y, -0.5f + 0.5f*projection.c.x,
                      -0.5f + 0.5f*projection.c.y);
            uniform1f(program, "uMipBias", log2f(max(projection.b.y*float(height)*0.5f, 1.0e-6f)) + cortaomipbias);
            uniform1f(program, "uInverseMaxSlopeCompensation", 1.0f/max(cortaoslopecompensation, 1.0f));
            uniform1f(program, "uBackfaceThreshold", cortaobackfacethreshold);
            uniform1f(program, "uBackfaceBoost", cortaobackfaceboost);
            uniform1f(program, "uStrength", cortaostrength);
            normalmatrixloc = uniformloc(program, "uNormalMatrix");
            if(normalmatrixloc >= 0) glUniformMatrix3fv_(normalmatrixloc, 1, GL_FALSE, normalmatrix.a.v);
            glDispatchCompute_((width + 15)/16, (height + 15)/16, 1);
            glMemoryBarrier_(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            endtimer(maintimer);

            if(cortaofilter)
            {
                timer *filtertimer = begintimer("CORTAO filter");
                program = filterprogram;
                glUseProgram_(program);
                bindinput(GL_TEXTURE2, GL_TEXTURE_2D, packedtexture);
                bindinput(GL_TEXTURE4, GL_TEXTURE_RECTANGLE, rawtexture);
                glBindImageTexture_(0, filteredtexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
                uniform2f(program, "uResolution", float(width), float(height));
                uniform1f(program, "uDepthSensitivity", cortaofilterdepth);
                uniform1f(program, "uNormalSensitivity", cortaofilternormal);
                glDispatchCompute_((width + 15)/16, (height + 15)/16, 1);
                glMemoryBarrier_(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
                endtimer(filtertimer);
            }

            timer *temporaltimer = begintimer("CORTAO temporal resolve");
            program = temporalprogram;
            glUseProgram_(program);
            bindinput(GL_TEXTURE2, GL_TEXTURE_2D, packedtexture);
            bindinput(GL_TEXTURE4, GL_TEXTURE_RECTANGLE, cortaofilter ? filteredtexture : rawtexture);
            bool usehistory = temporalactive && historyvalid;
            int previousindex = usehistory ? historyindex : 1, writeindex = usehistory ? 1 - historyindex : 0;
            bindinput(GL_TEXTURE6, GL_TEXTURE_RECTANGLE, historytexture[previousindex]);
            bindinput(GL_TEXTURE7, GL_TEXTURE_RECTANGLE, historypacked[previousindex]);
            glBindImageTexture_(0, historytexture[writeindex], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
            glBindImageTexture_(1, historypacked[writeindex], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
            uniform2f(program, "uResolution", float(width), float(height));
            uniform1i(program, "uHistoryValid", usehistory ? 1 : 0);
            uniform1f(program, "uHistoryWeight", cortaotemporalweight);
            uniform1f(program, "uDepthThreshold", cortaotemporaldepth);
            uniform1f(program, "uNormalThreshold", cortaotemporalnormal);
            uniform4f(program, "uUnprojectParams", 2.0f*invx, 2.0f*invy, -invx + inverseprojection.d.x,
                      -invy + inverseprojection.d.y);
            matrix4 reprojectmatrix, previousviewfromcurrent;
            reprojectmatrix.muld(previousscreenmatrix, inverseview);
            previousviewfromcurrent.muld(previousviewmatrix, inverseview);
            uniformmatrix4(program, "uReprojectMatrix", reprojectmatrix);
            uniformmatrix4(program, "uPreviousViewFromCurrent", previousviewfromcurrent);
            glDispatchCompute_((width + 7)/8, (height + 7)/8, 1);
            glMemoryBarrier_(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            endtimer(temporaltimer);

            historyindex = writeindex;
            if(temporalactive)
            {
                historyvalid = true;
                ++temporalframe;
                previousscreenmatrix = screen;
                previousviewmatrix = view;
            }
            else
            {
                historyvalid = false;
                temporalframe = 0;
            }

            if(cortaodebug)
            {
                program = debugprogram;
                glUseProgram_(program);
                bindinput(GL_TEXTURE2, GL_TEXTURE_2D, packedtexture);
                bindinput(GL_TEXTURE3, GL_TEXTURE_3D, luttexture);
                bindinput(GL_TEXTURE4, GL_TEXTURE_RECTANGLE, rawtexture);
                bindinput(GL_TEXTURE6, GL_TEXTURE_RECTANGLE, outputtexture());
                glBindImageTexture_(0, debugoutput, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
                uniform2f(program, "uResolution", float(width), float(height));
                uniform1i(program, "uDebugMode", cortaodebug);
                uniform1i(program, "uDebugMip", min(cortaodebugmip, mipcount - 1));
                uniform1i(program, "uMipCount", mipcount);
                uniform1i(program, "uDebugLUTSlice", cortaodebuglutslice);
                uniform1f(program, "uMaxRadius", cortaoradius);
                uniform1f(program, "uMaxApparentRadius", maxapparentradius/sqrtf(maxapparentradius*maxapparentradius + 1.0f));
                uniform1f(program, "uProjectionY", projection.b.y);
                uniform1f(program, "uDepthDisplayRange", cortaodebugdepth);
                uniform1f(program, "uWorldRadiusDisplayRange", cortaodebugworldscale);
                uniform1f(program, "uPixelRadiusDisplayRange", cortaodebugpixelscale);
                uniform1f(program, "uPositionDisplayRange", cortaodebugpositionscale);
                uniform3f(program, "uLUTSize", 24.0f, 24.0f, 16.0f);
                uniform4f(program, "uUnprojectParams", 2.0f*invx, 2.0f*invy, -invx + inverseprojection.d.x,
                          -invy + inverseprojection.d.y);
                uniform1f(program, "uMipBias", log2f(max(projection.b.y*float(height)*0.5f, 1.0e-6f)) + cortaomipbias);
                glDispatchCompute_((width + 7)/8, (height + 7)/8, 1);
                glMemoryBarrier_(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            }

            restorestate();
            return true;
        }

        void cleanup()
        {
            cleanupprograms();
            deleteresources();
        }

        bool available() { return initialized && supported; }
        GLuint outputtexture() { return historytexture[historyindex]; }
        bool debugenabled() { return cortaodebug != 0; }

        const char *debugname()
        {
            if(cortaodebug == 2 && cortaotemporal) return "temporally resolved occlusion";
            static const char * const names[] =
            {
                "disabled", "raw occlusion", "filtered occlusion", "linear depth", "view-space normals", "packed-depth mip",
                "world-space radius", "screen-space radius", "view-space position", "mip selection", "occluder LUT"
            };
            return names[clamp(cortaodebug, 0, int(sizeof(names)/sizeof(names[0])) - 1)];
        }

        GLuint debugtexture()
        {
            return cortaodebug ? debugoutput : outputtexture();
        }
    }
}
