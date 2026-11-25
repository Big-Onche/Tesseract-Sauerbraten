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
        const int MAX_EYE_MIPS = 16;
        GLuint tonefbo = 0, compositetex = 0, tonylut = 0;
        GLuint eyefbo = 0, eyelumtex[MAX_EYE_MIPS] = { 0 }, eyeadapttex[2] = { 0 };
        int tonew = -1, toneh = -1;
        int eyew = -1, eyeh = -1, eyelevels = 0, eyelumw[MAX_EYE_MIPS] = { 0 }, eyelumh[MAX_EYE_MIPS] = { 0 }, eyeadaptindex = 0;
        int lastadaptmillis = 0, lastadaptdebug = 0;
        bool eyeadaptvalid = false;
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

        void allocrect(GLuint &tex, int width, int height)
        {
            if(!tex) glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_RECTANGLE, tex);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, NULL);
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

    void reseteyeadaptation()
    {
        eyeadaptvalid = false;
        lastadaptmillis = 0;
    }

    void cleanupeyeadaptation()
    {
        loopi(MAX_EYE_MIPS) if(eyelumtex[i]) { glDeleteTextures(1, &eyelumtex[i]); eyelumtex[i] = 0; }
        loopi(2) if(eyeadapttex[i]) { glDeleteTextures(1, &eyeadapttex[i]); eyeadapttex[i] = 0; }
        if(eyefbo) { glDeleteFramebuffers_(1, &eyefbo); eyefbo = 0; }
        eyew = eyeh = -1;
        eyelevels = eyeadaptindex = 0;
        lastadaptdebug = 0;
        reseteyeadaptation();
    }

    bool prepareeyeadaptation(int width, int height)
    {
        if(eyew == width && eyeh == height && eyelevels && eyeadapttex[0] && eyeadapttex[1]) return true;
        cleanupeyeadaptation();

        eyew = width;
        eyeh = height;
        int levelw = max((width + 1)/2, 1), levelh = max((height + 1)/2, 1);
        while(eyelevels < MAX_EYE_MIPS)
        {
            eyelumw[eyelevels] = levelw;
            eyelumh[eyelevels] = levelh;
            ++eyelevels;
            if(levelw == 1 && levelh == 1) break;
            levelw = max((levelw + 1)/2, 1);
            levelh = max((levelh + 1)/2, 1);
        }
        if(eyelumw[eyelevels - 1] != 1 || eyelumh[eyelevels - 1] != 1)
        {
            conoutf(CON_ERROR, "postfx: eye-adaptation reduction exceeded %d levels", MAX_EYE_MIPS);
            cleanupeyeadaptation();
            return false;
        }

        glGenFramebuffers_(1, &eyefbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, eyefbo);
        loopi(eyelevels)
        {
            allocrect(eyelumtex[i], eyelumw[i], eyelumh[i]);
            if(!attach(eyelumtex[i], "eye-adaptation luminance")) { cleanupeyeadaptation(); return false; }
        }
        loopi(2)
        {
            allocrect(eyeadapttex[i], 1, 1);
            if(!attach(eyeadapttex[i], "eye-adaptation history")) { cleanupeyeadaptation(); return false; }
        }
        glBindTexture(GL_TEXTURE_RECTANGLE, 0);
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        return true;
    }

    bool updateeyeadaptation(GLuint hdrtex, int width, int height)
    {
        if(drawtex || (eyeadaptvalid && lastadaptmillis == lastmillis)) return true;
        if(!prepareeyeadaptation(width, height)) return false;

        timer *timer = begintimer("eye adaptation");
        Shader *luminanceshader = useshaderbyname("eyeluminance"), *reduceshader = useshaderbyname("eyereduce"),
               *adaptshader = useshaderbyname("eyeadapt");
        if(!luminanceshader || !reduceshader || !adaptshader)
        {
            endtimer(timer);
            return false;
        }

        glBindFramebuffer_(GL_FRAMEBUFFER, eyefbo);
        if(!attach(eyelumtex[0], "eye-adaptation luminance")) { endtimer(timer); return false; }
        glViewport(0, 0, eyelumw[0], eyelumh[0]);
        luminanceshader->set();
        bindrect(GL_TEXTURE0, hdrtex);
        glActiveTexture_(GL_TEXTURE0);
        screenquad(width, height);

        reduceshader->set();
        loopi(eyelevels - 1)
        {
            const int level = i + 1;
            if(!attach(eyelumtex[level], "eye-adaptation reduction")) { endtimer(timer); return false; }
            glViewport(0, 0, eyelumw[level], eyelumh[level]);
            bindrect(GL_TEXTURE0, eyelumtex[i]);
            glActiveTexture_(GL_TEXTURE0);
            screenquad(eyelumw[i], eyelumh[i]);
        }

        const int nextadapt = 1 - eyeadaptindex;
        if(!attach(eyeadapttex[nextadapt], "eye-adaptation history")) { endtimer(timer); return false; }
        glViewport(0, 0, 1, 1);
        adaptshader->set();
        const float elapsed = eyeadaptvalid ? max(lastmillis - lastadaptmillis, 0)/1000.0f : 0.0f;
        const float brightenretention = expf(-eyeadaptbrightenspeed*elapsed), darkenretention = expf(-eyeadaptdarkenspeed*elapsed);
        LOCALPARAMF(adaptParams, brightenretention, darkenretention, eyeadaptvalid ? 1.0f : 0.0f, 0.0f);
        bindrect(GL_TEXTURE0, eyelumtex[eyelevels - 1]);
        bindrect(GL_TEXTURE1, eyeadapttex[eyeadaptindex]);
        glActiveTexture_(GL_TEXTURE0);
        screenquad(1, 1);
        eyeadaptindex = nextadapt;
        eyeadaptvalid = true;
        lastadaptmillis = lastmillis;

        if(eyeadaptationdebug && lastmillis - lastadaptdebug >= 500)
        {
            float pixel[3] = { 0, 0, 0 };
            glReadPixels(0, 0, 1, 1, GL_RGB, GL_FLOAT, pixel);
            const float minimum = min(eyeadaptminexposure, eyeadaptmaxexposure), maximum = max(eyeadaptminexposure, eyeadaptmaxexposure);
            const float automatic = clamp(eyeadaptsensitivity/max(pixel[0], 1e-4f), minimum, maximum);
            conoutf(CON_INFO, "eye adaptation: luminance %.4f, automatic %.3fx, final %.3fx", pixel[0], automatic, automatic*exposure);
            lastadaptdebug = lastmillis;
        }

        glActiveTexture_(GL_TEXTURE0);
        endtimer(timer);
        return true;
    }

    GLuint eyeadaptationtexture() { return eyeadapttex[eyeadaptindex]; }
    bool eyeadaptationready() { return eyeadaptvalid; }

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
        const float minimum = min(eyeadaptminexposure, eyeadaptmaxexposure), maximum = max(eyeadaptminexposure, eyeadaptmaxexposure);
        const bool adapt = eyeadaptation && eyeadaptationready();
        LOCALPARAMF(eyeParams, eyeadaptsensitivity, minimum, maximum, adapt ? 1.0f : 0.0f);
        glBindFramebuffer_(GL_FRAMEBUFFER, outfbo);
        glViewport(0, 0, width, height);
        bindrect(GL_TEXTURE0, compositetex);
        bindrect(GL_TEXTURE1, adapt ? eyeadaptationtexture() : 0);
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
