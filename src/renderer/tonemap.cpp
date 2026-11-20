////////////////////////////////////////////////
// Candela tonemapping adapted for Tesseract
// By swr06
// https://github.com/swr06/Candela
// GPL-3.0 license
////////////////////////////////////////////////

#include "hdr.h"

namespace renderer
{
    bool preparebloom(int width, int height);

    namespace
    {
        GLuint tonefbo = 0, compositetex = 0, tonylut = 0;
        int tonew = -1, toneh = -1;
        bool lutattempted = false;

        void bindrect(GLenum unit, GLuint tex)
        {
            glActiveTexture_(unit);
            glBindTexture(GL_TEXTURE_RECTANGLE, tex);
        }

        bool attach(GLuint tex, const char *label)
        {
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, tex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) return true;
            conoutf(CON_ERROR, "postfx: incomplete %s framebuffer", label);
            return false;
        }

        bool loadtonylut()
        {
            if(lutattempted) return tonylut != 0;
            lutattempted = true;
            stream *file = openfile("packages/sauerract/tonymmf.dds", "rb");
            if(!file)
            {
                conoutf(CON_WARN, "postfx: Tony McMapface LUT is missing; using ACES Fitted");
                return false;
            }

            const uint magic = file->getlil<uint>();
            const uint headersize = file->getlil<uint>();
            file->seek(4, SEEK_CUR);
            const uint height = file->getlil<uint>(), width = file->getlil<uint>();
            file->seek(4, SEEK_CUR);
            const uint depth = file->getlil<uint>();
            file->seek(48, SEEK_CUR);
            const uint pixelformatsize = file->getlil<uint>();
            file->seek(4, SEEK_CUR);
            const uint fourcc = file->getlil<uint>();
            file->seek(40, SEEK_CUR);
            const uint dxgiformat = file->getlil<uint>(), dimension = file->getlil<uint>();
            file->seek(4, SEEK_CUR);
            const uint arraysize = file->getlil<uint>();
            file->seek(4, SEEK_CUR);

            const uint dds = 0x20534444U, dx10 = 0x30315844U;
            const size_t datasize = size_t(width)*height*depth*4*sizeof(float);
            if(magic != dds || headersize != 124 || pixelformatsize != 32 || fourcc != dx10 || dxgiformat != 2 || dimension != 4 || arraysize != 1 ||
               width != 48 || height != 48 || depth != 48 || file->size() - file->tell() != stream::offset(datasize))
            {
                conoutf(CON_WARN, "postfx: Tony McMapface DDS layout is invalid; using ACES Fitted");
                delete file;
                return false;
            }

            uchar *data = new uchar[datasize];
            const bool readok = file->read(data, datasize) == datasize;
            delete file;
            if(!readok)
            {
                delete[] data;
                conoutf(CON_WARN, "postfx: Tony McMapface LUT is truncated; using ACES Fitted");
                return false;
            }

            glGenTextures(1, &tonylut);
            glBindTexture(GL_TEXTURE_3D, tonylut);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glTexImage3D_(GL_TEXTURE_3D, 0, GL_RGBA32F, width, height, depth, 0, GL_RGBA, GL_FLOAT, data);
            delete[] data;
            glBindTexture(GL_TEXTURE_3D, 0);
            return true;
        }
    }

    bool preparehdr(int width, int height, bool needbloom)
    {
        if(tonew != width || toneh != height || !compositetex)
        {
            if(compositetex) { glDeleteTextures(1, &compositetex); compositetex = 0; }
            if(!tonefbo) glGenFramebuffers_(1, &tonefbo);
            glGenTextures(1, &compositetex);
            glBindTexture(GL_TEXTURE_RECTANGLE, compositetex);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, NULL);
            glBindFramebuffer_(GL_FRAMEBUFFER, tonefbo);
            if(!attach(compositetex, "HDR composite")) { cleanuptonemap(); return false; }
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);
            tonew = width;
            toneh = height;
        }
        if(needbloom)
        {
            if(!preparebloom(width, height)) return false;
        }
        else cleanupbloom();
        if(tonemap == 1) loadtonylut();
        return true;
    }

    bool combinehdr(GLuint hdrtex, int width, int height)
    {
        timer *timer = begintimer("bloom composition");
        glBindFramebuffer_(GL_FRAMEBUFFER, tonefbo);
        if(!attach(compositetex, "HDR composite"))
        {
            endtimer(timer);
            return false;
        }
        glViewport(0, 0, width, height);
        Shader *shader = useshaderbyname("combine");
        if(!shader)
        {
            endtimer(timer);
            return false;
        }
        shader->set();
        LOCALPARAMF(combineParams, bloom ? bloomstrength : 0.0f, float(debug), float(width), float(height));
        LOCALPARAMF(bloomSize0, float(brightwidth()), float(brightheight()), float(bloomwidth(0)), float(bloomheight(0)));
        LOCALPARAMF(bloomSize1, float(bloomwidth(1)), float(bloomheight(1)), float(bloomwidth(2)), float(bloomheight(2)));
        LOCALPARAMF(bloomSize2, float(bloomwidth(3)), float(bloomheight(3)), float(bloomwidth(4)), float(bloomheight(4)));
        bindrect(GL_TEXTURE0, hdrtex);
        bindrect(GL_TEXTURE1, bloombright());
        loopi(5) bindrect(GL_TEXTURE2 + i, bloommip(i));
        glActiveTexture_(GL_TEXTURE0);
        screenquad(width, height);
        endtimer(timer);
        return true;
    }

    bool tonemaphdr(GLuint outfbo, int width, int height, int aatype)
    {
        timer *timer = begintimer("tonemapping");
        const char *shadername = "tonemap";
        switch(aatype)
        {
            case AA_LUMA: shadername = "tonemapluma"; break;
            case AA_MASKED: shadername = "tonemapmasked"; break;
            case AA_SPLIT: shadername = "tonemapsplit"; break;
            case AA_SPLIT_LUMA: shadername = "tonemapsplitluma"; break;
            case AA_SPLIT_MASKED: shadername = "tonemapsplitmasked"; break;
        }
        Shader *shader = useshaderbyname(shadername);
        if(!shader)
        {
            endtimer(timer);
            return false;
        }
        shader->set();
        int selected = debug ? 7 : tonemap;
        if(selected == 1 && !loadtonylut()) selected = 0;
        LOCALPARAMF(toneParams, exposure, float(selected), tonylut ? 1.0f : 0.0f, 0.0f);
        glBindFramebuffer_(GL_FRAMEBUFFER, outfbo);
        glViewport(0, 0, width, height);
        bindrect(GL_TEXTURE0, compositetex);
        glActiveTexture_(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, tonylut);
        glActiveTexture_(GL_TEXTURE0);
        if(aatype == AA_MASKED || aatype == AA_SPLIT_MASKED) setaavelocityparams(GL_TEXTURE3);
        screenquad(width, height);
        loopi(7)
        {
            glActiveTexture_(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_RECTANGLE, 0);
        }
        glActiveTexture_(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, 0);
        glActiveTexture_(GL_TEXTURE0);
        endtimer(timer);
        return true;
    }

    void cleanuptonemap()
    {
        if(compositetex) { glDeleteTextures(1, &compositetex); compositetex = 0; }
        if(tonylut) { glDeleteTextures(1, &tonylut); tonylut = 0; }
        if(tonefbo) { glDeleteFramebuffers_(1, &tonefbo); tonefbo = 0; }
        tonew = toneh = -1;
        lutattempted = false;
    }
}
