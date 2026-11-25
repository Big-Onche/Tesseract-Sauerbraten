////////////////////////////////////////////////
// Candela bloom adapted for Tesseract
// By swr06
// https://github.com/swr06/Candela
// GPL-3.0 license
////////////////////////////////////////////////

#include "hdr.h"

namespace renderer
{
    namespace
    {
        const int NUM_BLOOM_MIPS = 5;
        GLuint bloomfbo = 0, brighttex = 0, bloomtex[NUM_BLOOM_MIPS] = { 0 }, bloomtemp[NUM_BLOOM_MIPS] = { 0 };
        int resourcew = -1, resourceh = -1, brightw = 0, brighth = 0, mipw[NUM_BLOOM_MIPS] = { 0 }, miph[NUM_BLOOM_MIPS] = { 0 };

        void allocrect(GLuint &tex, int width, int height)
        {
            if(!tex) glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_RECTANGLE, tex);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, NULL);
        }

        bool attach(GLuint tex, const char *label)
        {
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, tex, 0);
            if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) return true;
            conoutf(CON_ERROR, "postfx: incomplete %s framebuffer", label);
            return false;
        }

        bool bindtarget(GLuint tex, int width, int height, const char *label)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER, bloomfbo);
            if(!attach(tex, label)) return false;
            glViewport(0, 0, width, height);
            return true;
        }

        void bindrect(GLenum unit, GLuint tex)
        {
            glActiveTexture_(unit);
            glBindTexture(GL_TEXTURE_RECTANGLE, tex);
        }
    }

    bool preparebloom(int width, int height)
    {
        if(resourcew == width && resourceh == height && brighttex) return true;
        cleanupbloom();

        resourcew = width;
        resourceh = height;
        brightw = max(width/2, 1);
        brighth = max(height/2, 1);
        const int basew = max(width/4, 1), baseh = max(height/4, 1);
        const int divisors[NUM_BLOOM_MIPS] = { 1, 2, 4, 8, 10 };
        loopi(NUM_BLOOM_MIPS)
        {
            mipw[i] = max(basew/divisors[i], 1);
            miph[i] = max(baseh/divisors[i], 1);
        }

        glGenFramebuffers_(1, &bloomfbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, bloomfbo);
        allocrect(brighttex, brightw, brighth);
        if(!attach(brighttex, "bloom mask")) { cleanupbloom(); return false; }
        loopi(NUM_BLOOM_MIPS)
        {
            allocrect(bloomtex[i], mipw[i], miph[i]);
            if(!attach(bloomtex[i], "bloom mip")) { cleanupbloom(); return false; }
            allocrect(bloomtemp[i], mipw[i], miph[i]);
            if(!attach(bloomtemp[i], "bloom scratch")) { cleanupbloom(); return false; }
        }
        glBindTexture(GL_TEXTURE_RECTANGLE, 0);
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        return true;
    }

    bool renderbloom(GLuint hdrtex, int width, int height)
    {
        if(!preparebloom(width, height)) return false;

        timer *extracttimer = begintimer("bloom extraction");
        if(!bindtarget(brighttex, brightw, brighth, "bloom mask"))
        {
            endtimer(extracttimer);
            return false;
        }
        Shader *maskshader = useshaderbyname("bloommask");
        if(!maskshader)
        {
            endtimer(extracttimer);
            return false;
        }
        maskshader->set();
        LOCALPARAMF(bloomParams, bloomthreshold, bloomsoftknee, 0.0f, 0.0f);
        bindrect(GL_TEXTURE0, hdrtex);
        glActiveTexture_(GL_TEXTURE0);
        screenquad(width, height);
        endtimer(extracttimer);

        timer *blurtimer = begintimer("bloom blur");
        Shader *blurshader = useshaderbyname("bloomblur");
        if(!blurshader)
        {
            endtimer(blurtimer);
            return false;
        }
        const bool wideflags[NUM_BLOOM_MIPS] = { true, false, true, false, true };
        GLuint source = brighttex;
        int sourcew = brightw, sourceh = brighth;
        loopi(NUM_BLOOM_MIPS)
        {
            blurshader->set();
            LOCALPARAMF(blurParams, 1.0f, wideflags[i] ? 1.0f : 0.0f, float(sourcew)/max(sourceh, 1), 0.0f);
            if(!bindtarget(bloomtemp[i], mipw[i], miph[i], "horizontal bloom blur"))
            {
                endtimer(blurtimer);
                return false;
            }
            bindrect(GL_TEXTURE0, source);
            glActiveTexture_(GL_TEXTURE0);
            screenquad(sourcew, sourceh);

            blurshader->set();
            LOCALPARAMF(blurParams, 0.0f, wideflags[i] ? 1.0f : 0.0f, float(mipw[i])/max(miph[i], 1), 0.0f);
            if(!bindtarget(bloomtex[i], mipw[i], miph[i], "vertical bloom blur"))
            {
                endtimer(blurtimer);
                return false;
            }
            bindrect(GL_TEXTURE0, bloomtemp[i]);
            glActiveTexture_(GL_TEXTURE0);
            screenquad(mipw[i], miph[i]);

            source = bloomtex[i];
            sourcew = mipw[i];
            sourceh = miph[i];
        }
        endtimer(blurtimer);
        return true;
    }

    void cleanupbloom()
    {
        if(brighttex) { glDeleteTextures(1, &brighttex); brighttex = 0; }
        loopi(NUM_BLOOM_MIPS)
        {
            if(bloomtex[i]) { glDeleteTextures(1, &bloomtex[i]); bloomtex[i] = 0; }
            if(bloomtemp[i]) { glDeleteTextures(1, &bloomtemp[i]); bloomtemp[i] = 0; }
        }
        if(bloomfbo) { glDeleteFramebuffers_(1, &bloomfbo); bloomfbo = 0; }
        resourcew = resourceh = -1;
        brightw = brighth = 0;
    }

    GLuint bloombright() { return brighttex; }
    GLuint bloommip(int level) { return level >= 0 && level < NUM_BLOOM_MIPS ? bloomtex[level] : 0; }
    int bloomwidth(int level) { return level >= 0 && level < NUM_BLOOM_MIPS ? mipw[level] : 0; }
    int bloomheight(int level) { return level >= 0 && level < NUM_BLOOM_MIPS ? miph[level] : 0; }
    int brightwidth() { return brightw; }
    int brightheight() { return brighth; }
}
