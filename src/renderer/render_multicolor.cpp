// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

#include "render_multicolor.h"
#include "render_common.h"

void RenderMulticolor(unsigned long nextClk) {
    int ci;
    int cp;
    int bt;

    #include "render_common_a.h"

    zxScreen = ((dev_mman.port7FFD & 8) ^ screensHack) ? RAM_BANK7 : RAM_BANK5;

    if (!attributesHack) {
        bt = dev_mman.ram[ zxScreen + ((zxLine & 0xC0) << 5) + ((zxLine & 7) << 8) + ((zxLine & 0x38) << 2) + pos ];
        cl = dev_mman.ram[ zxScreen + 0x2000 + ((zxLine & 0xC0) << 5) + ((zxLine & 7) << 8) + ((zxLine & 0x38) << 2) + pos ];

        if (flashColor) {
            ci = colors[((cl & 64) >> 3) | (cl & 7)];
            cp = (cl >> 3) & 7;

            if (cp) {
                cp = colors[cp];

                int r = ((unsigned int)STAGE_GETR(ci) + (unsigned int)STAGE_GETR(cp)) >> 1;
                int g = ((unsigned int)STAGE_GETG(ci) + (unsigned int)STAGE_GETG(cp)) >> 1;
                int b = ((unsigned int)STAGE_GETB(ci) + (unsigned int)STAGE_GETB(cp)) >> 1;

                ci = STAGE_MAKERGB(r, g, b);
            }

            cp = STAGE_MAKERGB(0, 0, 0);
        } else {
            if ((frames & 32) && (cl & 128)) {
                cp = colors[((cl & 64) >> 3) | (cl & 7)];
                ci = colors[((cl & 64) >> 3) | ((cl >> 3) & 7)];
            } else {
                ci = colors[((cl & 64) >> 3) | (cl & 7)];
                cp = colors[((cl & 64) >> 3) | ((cl >> 3) & 7)];
            }
        }
    } else if (attributesHack == 1) {
        bt = dev_mman.ram[ zxScreen + ((zxLine & 0xC0) << 5) + ((zxLine & 7) << 8) + ((zxLine & 0x38) << 2) + pos ];

        if (((zxLine >> 3) & 1) ^ (pos & 1)) {
            ci = STAGE_MAKERGB(0, 0, 0);
            cp = STAGE_MAKERGB(192, 192, 192);
        } else {
            ci = STAGE_MAKERGB(64, 64, 64);
            cp = STAGE_MAKERGB(255, 255, 255);
        }
    } else { // attributesHack == 2
        bt = 0x3C;
        cl = dev_mman.ram[ zxScreen + 0x2000 + ((zxLine & 0xC0) << 5) + ((zxLine & 7) << 8) + ((zxLine & 0x38) << 2) + pos ];

        if ((frames & 32) && (cl & 128)) {
            cp = colors[((cl & 64) >> 3) | (cl & 7)];
            ci = colors[((cl & 64) >> 3) | ((cl >> 3) & 7)];
        } else {
            ci = colors[((cl & 64) >> 3) | (cl & 7)];
            cp = colors[((cl & 64) >> 3) | ((cl >> 3) & 7)];
        }
    }

    *(scr++) = (bt & 128 ? ci : cp);
    *(scr++) = (bt &  64 ? ci : cp);
    *(scr++) = (bt &  32 ? ci : cp);
    *(scr++) = (bt &  16 ? ci : cp);
    *(scr++) = (bt &   8 ? ci : cp);
    *(scr++) = (bt &   4 ? ci : cp);
    *(scr++) = (bt &   2 ? ci : cp);
    *(scr)   = (bt &   1 ? ci : cp);

    #include "render_common_b.h"
}
