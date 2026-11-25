// CORTAO integration for Tesseract: Sauerbraten.
// Adapted from the CORTAO Carbon Engine Implementation v1.0.0.
// Copyright (c) 2026 Fenris Creations. Used under the MIT License.

#ifndef TESSELATE_CORTAO_H
#define TESSELATE_CORTAO_H

#include "engine.h"

namespace renderer
{
    namespace cortao
    {
        bool init(int width, int height, bool multisample);
        bool render(GLuint depthtexture, GLuint normaltexture, bool multisample, int depthformat, int width, int height,
                    const matrix4 &projection, const matrix4 &inverseprojection, const matrix4 &view, const matrix4 &inverseview,
                    const matrix4 &screen, const matrix3 &normalmatrix, float nearplane, float farplane);
        void cleanup();
        bool available();
        bool debugenabled();
        const char *debugname();
        GLuint outputtexture();
        GLuint debugtexture();
    }
}

#endif
