// ui.c - UI绘制函数（支持中文BCFNT字体）
#include "ui.h"
#include "keyboard.h"
#include "network.h"
#include <citro2d.h>
#include <time.h>
#include <math.h>
#include <3ds/services/mcuhwc.h>

// ========== BCFNT中文字体 ==========
static C2D_Font s_font = NULL;
static C2D_TextBuf s_textBuf = NULL;
#define UI_TEXTBUF_SIZE 8192

// ========== 补充符号字体 ==========
static C2D_Font s_symbolFont = NULL;
static C2D_TextBuf s_symbolTextBuf = NULL;
static int s_missingGlyphMain = -1;
static int s_missingGlyphSym = -1;

// ========== 图标纹理 ==========
static C2D_SpriteSheet s_boltSheet = NULL;
static C2D_Image s_boltIcon;
static C2D_SpriteSheet s_wifiOnSheet = NULL;
static C2D_Image s_wifiOnIcon;
static C2D_SpriteSheet s_wifiOffSheet = NULL;
static C2D_Image s_wifiOffIcon;

void UI_LoadIcons(void)
{
    s_boltSheet = C2D_SpriteSheetLoad("romfs:/bolt.t3x");
    if (s_boltSheet)
    {
        s_boltIcon = C2D_SpriteSheetGetImage(s_boltSheet, 0);
        if (!s_boltIcon.subtex) { C2D_SpriteSheetFree(s_boltSheet); s_boltSheet = NULL; }
    }
    s_wifiOnSheet = C2D_SpriteSheetLoad("romfs:/wifi_on.t3x");
    if (s_wifiOnSheet)
    {
        s_wifiOnIcon = C2D_SpriteSheetGetImage(s_wifiOnSheet, 0);
        if (!s_wifiOnIcon.subtex) { C2D_SpriteSheetFree(s_wifiOnSheet); s_wifiOnSheet = NULL; }
    }
    s_wifiOffSheet = C2D_SpriteSheetLoad("romfs:/wifi_off.t3x");
    if (s_wifiOffSheet)
    {
        s_wifiOffIcon = C2D_SpriteSheetGetImage(s_wifiOffSheet, 0);
        if (!s_wifiOffIcon.subtex) { C2D_SpriteSheetFree(s_wifiOffSheet); s_wifiOffSheet = NULL; }
    }
}

void UI_FreeIcons(void)
{
    if (s_boltSheet)
    {
        C2D_SpriteSheetFree(s_boltSheet);
        s_boltSheet = NULL;
    }
    if (s_wifiOnSheet)
    {
        C2D_SpriteSheetFree(s_wifiOnSheet);
        s_wifiOnSheet = NULL;
    }
    if (s_wifiOffSheet)
    {
        C2D_SpriteSheetFree(s_wifiOffSheet);
        s_wifiOffSheet = NULL;
    }
}

void UI_InitFont(void)
{
    // 尝试加载romfs中的中文字体
    s_font = C2D_FontLoad("romfs:/ui-menu-font.bcfnt");
    if (s_font)
    {
        s_textBuf = C2D_TextBufNew(UI_TEXTBUF_SIZE);
        if (s_textBuf)
        {
            C2D_FontSetFilter(s_font, GPU_LINEAR, GPU_LINEAR);
            // 获取主字体的缺失字形索引（用一个肯定不存在的码点查询）
            s_missingGlyphMain = C2D_FontGlyphIndexFromCodePoint(s_font, 0xFFFE);
        }
        else
        {
            C2D_FontFree(s_font);
            s_font = NULL;
        }
    }

    // 加载补充符号字体
    s_symbolFont = C2D_FontLoad("romfs:/symbol-font.bcfnt");
    if (s_symbolFont)
    {
        s_symbolTextBuf = C2D_TextBufNew(UI_TEXTBUF_SIZE);
        if (s_symbolTextBuf)
        {
            C2D_FontSetFilter(s_symbolFont, GPU_LINEAR, GPU_LINEAR);
            s_missingGlyphSym = C2D_FontGlyphIndexFromCodePoint(s_symbolFont, 0xFFFE);
        }
        else
        {
            C2D_FontFree(s_symbolFont);
            s_symbolFont = NULL;
        }
    }
}

void UI_FiniFont(void)
{
    if (s_symbolTextBuf)
    {
        C2D_TextBufDelete(s_symbolTextBuf);
        s_symbolTextBuf = NULL;
    }
    if (s_symbolFont)
    {
        C2D_FontFree(s_symbolFont);
        s_symbolFont = NULL;
    }
    if (s_textBuf)
    {
        C2D_TextBufDelete(s_textBuf);
        s_textBuf = NULL;
    }
    if (s_font)
    {
        C2D_FontFree(s_font);
        s_font = NULL;
    }
}

// 检查字体是否可用
static bool FontReady(void)
{
    return s_font != NULL && s_textBuf != NULL;
}

// ========== 多字体回退渲染 ==========

// 解码一个UTF-8字符，返回码点，*len写入字节长度
static u32 utf8_decode(const char* s, int* len)
{
    const unsigned char* p = (const unsigned char*)s;
    if (*p < 0x80) { *len = 1; return *p; }
    if ((*p & 0xE0) == 0xC0) { *len = 2; return ((u32)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((*p & 0xF0) == 0xE0) { *len = 3; return ((u32)(p[0] & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    if ((*p & 0xF8) == 0xF0) { *len = 4; return ((u32)(p[0] & 0x07) << 18) | ((u32)(p[1] & 0x3F) << 12) | ((u32)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); }
    *len = 1;
    return *p;
}

// 检查主字体是否包含某码点
static bool main_font_has(u32 cp)
{
    if (!s_font) return false;
    int gi = C2D_FontGlyphIndexFromCodePoint(s_font, cp);
    return gi != s_missingGlyphMain;
}

// 检查符号字体是否包含某码点
static bool symbol_font_has(u32 cp)
{
    if (!s_symbolFont) return false;
    int gi = C2D_FontGlyphIndexFromCodePoint(s_symbolFont, cp);
    return gi != s_missingGlyphSym;
}

// 选择字体：0=主字体, 1=符号字体, -1=都没有
static int select_font(u32 cp)
{
    if (main_font_has(cp)) return 0;
    if (symbol_font_has(cp)) return 1;
    return -1;
}

#define MAX_SEG_TEXT 512

// 多字体绘制文本（核心函数）
// drawX/drawY: 绘制起点; depth: 深度; scale: 缩放; color: 颜色
// text: UTF-8文本; outWidth: 输出总宽度（可为NULL）
static void draw_text_fallback(float drawX, float drawY, float depth, float scale, u32 color, const char* text, float* outWidth)
{
    if (!text || !*text) { if (outWidth) *outWidth = 0; return; }

    float curX = drawX;
    char segBuf[MAX_SEG_TEXT];
    int segLen = 0;
    int curFont = -1; // 当前段使用的字体

    // 刷新当前段
    #define FLUSH_SEGMENT() do { \
        if (segLen > 0) { \
            segBuf[segLen] = 0; \
            C2D_Text c2dText; \
            if (curFont == 0) { \
                C2D_TextFontParse(&c2dText, s_font, s_textBuf, segBuf); \
                C2D_TextOptimize(&c2dText); \
                float w, h; \
                C2D_TextGetDimensions(&c2dText, scale, scale, &w, &h); \
                C2D_DrawText(&c2dText, C2D_WithColor, curX, drawY, depth, scale, scale, color); \
                curX += w; \
                C2D_TextBufClear(s_textBuf); \
            } else if (curFont == 1) { \
                C2D_TextFontParse(&c2dText, s_symbolFont, s_symbolTextBuf, segBuf); \
                C2D_TextOptimize(&c2dText); \
                float w, h; \
                C2D_TextGetDimensions(&c2dText, scale, scale, &w, &h); \
                C2D_DrawText(&c2dText, C2D_WithColor, curX, drawY, depth, scale, scale, color); \
                curX += w; \
                C2D_TextBufClear(s_symbolTextBuf); \
            } \
            segLen = 0; \
        } \
    } while(0)

    const char* p = text;
    while (*p)
    {
        int charLen;
        u32 cp = utf8_decode(p, &charLen);

        int f = select_font(cp);

        // 如果字体切换了，刷新当前段
        if (f != curFont && segLen > 0)
        {
            FLUSH_SEGMENT();
        }

        if (f < 0)
        {
            // 两个字体都没有该字符，用主字体渲染（会显示缺失字形）
            f = 0;
            if (f != curFont && segLen > 0) FLUSH_SEGMENT();
        }

        curFont = f;

        // 添加字符到当前段
        if (segLen + charLen < MAX_SEG_TEXT - 1)
        {
            memcpy(segBuf + segLen, p, charLen);
            segLen += charLen;
        }
        else
        {
            // 段缓冲区满，先刷新
            FLUSH_SEGMENT();
            curFont = f;
            memcpy(segBuf, p, charLen);
            segLen = charLen;
        }

        p += charLen;
    }

    FLUSH_SEGMENT();

    #undef FLUSH_SEGMENT

    if (outWidth) *outWidth = curX - drawX;
}

// 多字体测量文本宽度
static float measure_text_fallback(float scale, const char* text)
{
    if (!text || !*text) return 0;

    float totalW = 0;
    char segBuf[MAX_SEG_TEXT];
    int segLen = 0;
    int curFont = -1;

    #define MEASURE_FLUSH() do { \
        if (segLen > 0) { \
            segBuf[segLen] = 0; \
            C2D_Text c2dText; \
            if (curFont == 0) { \
                C2D_TextFontParse(&c2dText, s_font, s_textBuf, segBuf); \
                C2D_TextOptimize(&c2dText); \
                float w, h; \
                C2D_TextGetDimensions(&c2dText, scale, scale, &w, &h); \
                totalW += w; \
                C2D_TextBufClear(s_textBuf); \
            } else if (curFont == 1) { \
                C2D_TextFontParse(&c2dText, s_symbolFont, s_symbolTextBuf, segBuf); \
                C2D_TextOptimize(&c2dText); \
                float w, h; \
                C2D_TextGetDimensions(&c2dText, scale, scale, &w, &h); \
                totalW += w; \
                C2D_TextBufClear(s_symbolTextBuf); \
            } \
            segLen = 0; \
        } \
    } while(0)

    const char* p = text;
    while (*p)
    {
        int charLen;
        u32 cp = utf8_decode(p, &charLen);
        int f = select_font(cp);
        if (f < 0) f = 0;

        if (f != curFont && segLen > 0) MEASURE_FLUSH();
        curFont = f;

        if (segLen + charLen < MAX_SEG_TEXT - 1)
        {
            memcpy(segBuf + segLen, p, charLen);
            segLen += charLen;
        }
        else
        {
            MEASURE_FLUSH();
            curFont = f;
            memcpy(segBuf, p, charLen);
            segLen = charLen;
        }
        p += charLen;
    }
    MEASURE_FLUSH();
    #undef MEASURE_FLUSH

    return totalW;
}

// 内置8x8 ASCII像素字体（标准font8x8，低位在左）
const u8 font8x8[128][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x00
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x10
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 空格 0x20
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // !
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // $
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // &
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // (
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // /
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 9
    {0x00,0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x06}, // ;
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // <
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // M
    {0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x00}, // N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // _
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // a
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // b
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // c
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, // d
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, // e
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, // f
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // g
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // h
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // i
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // j
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // k
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // l
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // m
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // n
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // o
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // p
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // q
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // r
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // s
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // t
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // u
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // v
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // w
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // x
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // y
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // z
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // }
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

// ========== 8x8点阵字体回退绘制 ==========
static void DrawTextBitmap(float x, float y, float scale, u32 color, const char* text)
{
    float px = x;
    float py = y;
    int c;
    while ((c = (unsigned char)*text++) != 0)
    {
        if (c == '\n')
        {
            px = x;
            py += 8 * scale + 2;
            continue;
        }
        if (c < 32 || c > 126) c = '?';

        const u8* glyph = font8x8[c];
        for (int row = 0; row < 8; row++)
        {
            u8 bits = glyph[row];
            for (int col = 0; col < 8; col++)
            {
                if (bits & (1 << col))
                {
                    C2D_DrawRectSolid(px + col * scale, py + row * scale, 0.0f, scale, scale, color);
                }
            }
        }
        px += 8 * scale + 1;
    }
}

// ========== 基础图形绘制 ==========
void UI_DrawRect(float x, float y, float w, float h, u32 color)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

// 圆角矩形（半径4px）
void UI_DrawRoundRectR(float x, float y, float w, float h, int r, u32 fillColor, u32 borderColor)
{
    if (r < 1) r = 1;
    if (r * 2 > (int)w) r = (int)w / 2;
    if (r * 2 > (int)h) r = (int)h / 2;

    // 填充主体
    C2D_DrawRectSolid(x + r, y, 0, w - 2*r, h, fillColor);
    C2D_DrawRectSolid(x, y + r, 0, w, h - 2*r, fillColor);

    // 四个角填充（逐行用圆弧公式计算宽度）
    for (int dy = 0; dy < r; dy++)
    {
        int dx = (int)sqrtf((float)(r*r - (r-1-dy)*(r-1-dy)));
        // 左上角
        C2D_DrawRectSolid(x + r - dx, y + dy, 0, dx, 1, fillColor);
        // 右上角
        C2D_DrawRectSolid(x + w - r, y + dy, 0, dx, 1, fillColor);
        // 左下角
        C2D_DrawRectSolid(x + r - dx, y + h - 1 - dy, 0, dx, 1, fillColor);
        // 右下角
        C2D_DrawRectSolid(x + w - r, y + h - 1 - dy, 0, dx, 1, fillColor);
    }

    // 边框
    if (borderColor != fillColor)
    {
        // 上下边
        C2D_DrawRectSolid(x + r, y, 0, w - 2*r, 1, borderColor);
        C2D_DrawRectSolid(x + r, y + h - 1, 0, w - 2*r, 1, borderColor);
        // 左右边
        C2D_DrawRectSolid(x, y + r, 0, 1, h - 2*r, borderColor);
        C2D_DrawRectSolid(x + w - 1, y + r, 0, 1, h - 2*r, borderColor);
        // 四个角边框
        for (int dy = 0; dy < r; dy++)
        {
            int dx = (int)sqrtf((float)(r*r - (r-1-dy)*(r-1-dy)));
            // 左上角外沿
            C2D_DrawRectSolid(x + r - dx, y + dy, 0, 1, 1, borderColor);
            // 右上角外沿
            C2D_DrawRectSolid(x + w - r + dx - 1, y + dy, 0, 1, 1, borderColor);
            // 左下角外沿
            C2D_DrawRectSolid(x + r - dx, y + h - 1 - dy, 0, 1, 1, borderColor);
            // 右下角外沿
            C2D_DrawRectSolid(x + w - r + dx - 1, y + h - 1 - dy, 0, 1, 1, borderColor);
        }
    }
}

void UI_DrawRoundRect(float x, float y, float w, float h, u32 fillColor, u32 borderColor)
{
    UI_DrawRoundRectR(x, y, w, h, 6, fillColor, borderColor);
}

// 按钮阴影（下方半透明黑色圆角）
void UI_DrawShadow(float x, float y, float w, float h, int r)
{
    UI_DrawRoundRectR(x, y + 2, w, h, r, C2D_Color32(0, 0, 0, 35), C2D_Color32(0, 0, 0, 35));
}

void UI_DrawCircle(float x, float y, float r, u32 color)
{
    for (float dy = -r; dy <= r; dy += 1.0f)
    {
        for (float dx = -r; dx <= r; dx += 1.0f)
        {
            if (dx*dx + dy*dy <= r*r)
            {
                C2D_DrawRectSolid(x + dx, y + dy, 0.0f, 1, 1, color);
            }
        }
    }
}

// ========== 文字绘制（支持中文） ==========
// 注意：scale参数对于BCFNT字体是缩放因子，BCFNT自然高度约30px
// scale=0.5 约15px高，scale=0.6 约18px，scale=0.7 约21px
void UI_DrawText(float x, float y, float scale, u32 color, const char* text)
{
    if (!text) return;

    if (FontReady())
    {
        draw_text_fallback(x, y, 0.5f, scale, color, text, NULL);
    }
    else
    {
        DrawTextBitmap(x, y, scale, color, text);
    }
}

void UI_DrawTextDepth(float x, float y, float depth, float scale, u32 color, const char* text)
{
    if (!text) return;
    if (FontReady())
    {
        draw_text_fallback(x, y, depth, scale, color, text, NULL);
    }
    else
    {
        DrawTextBitmap(x, y, scale, color, text);
    }
}

void UI_DrawTextCenter(float x, float y, float scale, u32 color, const char* text)
{
    if (!text) return;

    if (FontReady())
    {
        float width = measure_text_fallback(scale, text);
        draw_text_fallback(x - width / 2, y, 0.5f, scale, color, text, NULL);
    }
    else
    {
        int len = strlen(text);
        float w = len * 8 * scale + (len - 1);
        DrawTextBitmap(x - w / 2, y, scale, color, text);
    }
}

void UI_DrawTextCenterDepth(float x, float y, float depth, float scale, u32 color, const char* text)
{
    if (!text) return;
    if (FontReady())
    {
        float width = measure_text_fallback(scale, text);
        draw_text_fallback(x - width / 2, y, depth, scale, color, text, NULL);
    }
    else
    {
        int len = strlen(text);
        float w = len * 8 * scale + (len - 1);
        DrawTextBitmap(x - w / 2, y, scale, color, text);
    }
}

// 在指定矩形内水平垂直居中绘制文字（y为矩形顶部，h为矩形高度）
void UI_DrawTextCenterInRect(float x, float y, float w, float h, float scale, u32 color, const char* text)
{
    if (!text) return;

    if (FontReady())
    {
        float tw = measure_text_fallback(scale, text);
        float th = 33.0f * scale;
        float drawX = x + (w - tw) / 2;
        float drawY = y + h / 2.0f - th * 0.35f;
        draw_text_fallback(drawX, drawY, 0.5f, scale, color, text, NULL);
    }
    else
    {
        int len = strlen(text);
        float tw = len * 8 * scale + (len - 1);
        float th = 8 * scale;
        DrawTextBitmap(x + (w - tw)/2, y + (h - th)/2, scale, color, text);
    }
}

// 在指定矩形内水平垂直居中绘制文字（可指定depth）
void UI_DrawTextCenterInRectDepth(float x, float y, float w, float h, float depth, float scale, u32 color, const char* text)
{
    if (!text) return;

    if (FontReady())
    {
        float tw = measure_text_fallback(scale, text);
        float th = 33.0f * scale;
        float drawX = x + (w - tw) / 2;
        float drawY = y + h / 2.0f - th * 0.35f;
        draw_text_fallback(drawX, drawY, depth, scale, color, text, NULL);
    }
    else
    {
        int len = strlen(text);
        float tw = len * 8 * scale + (len - 1);
        float th = 8 * scale;
        DrawTextBitmap(x + (w - tw)/2, y + (h - th)/2, scale, color, text);
    }
}

// 测量文字宽度
float UI_MeasureText(float scale, const char* text)
{
    if (!text) return 0;
    if (FontReady())
    {
        return measure_text_fallback(scale, text);
    }
    return (float)strlen(text) * 8 * scale;
}

// 截断文字到指定宽度，超出加省略号；bold通过偏移1px重绘模拟加粗
void UI_DrawTextTruncated(float x, float y, float depth, float scale, u32 color, const char* text, float maxWidth, bool bold)
{
    if (!text || !*text) return;
    if (FontReady())
    {
        float tw = measure_text_fallback(scale, text);

        if (tw <= maxWidth)
        {
            if (bold)
                draw_text_fallback(x + 1, y, depth, scale, color, text, NULL);
            draw_text_fallback(x, y, depth, scale, color, text, NULL);
        }
        else
        {
            // 逐字符缩短直到能放下 "文字…"
            char buf[256];
            int len = (int)strlen(text);
            if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
            memcpy(buf, text, len);
            buf[len] = 0;

            while (len > 0)
            {
                buf[--len] = 0;
                char withEll[260];
                snprintf(withEll, sizeof(withEll), "%s…", buf);
                tw = measure_text_fallback(scale, withEll);
                if (tw <= maxWidth)
                {
                    if (bold)
                        draw_text_fallback(x + 1, y, depth, scale, color, withEll, NULL);
                    draw_text_fallback(x, y, depth, scale, color, withEll, NULL);
                    break;
                }
            }
        }
    }
    else
    {
        DrawTextBitmap(x, y, scale, color, text);
    }
}

// 粗体居中文字（通过多次偏移绘制模拟粗体）
// ========== 按钮 ==========
void UI_DrawButton(float x, float y, float w, float h, const char* text, u32 bgColor, u32 borderColor)
{
    UI_DrawShadow(x, y, w, h, 6);
    UI_DrawRoundRect(x, y, w, h, bgColor, borderColor);
    UI_DrawTextCenter(x + w / 2, y + (h - 15) / 2 + 3, 0.5f, COLOR_WHITE, text);
}

void UI_DrawButtonDisabled(float x, float y, float w, float h, const char* text)
{
    UI_DrawRoundRect(x, y, w, h, C2D_Color32(0xBD, 0xBD, 0xBD, 0xFF), COLOR_BORDER_GRAY);
    UI_DrawTextCenter(x + w / 2, y + (h - 15) / 2 + 3, 0.5f, C2D_Color32(0x75, 0x75, 0x75, 0xFF), text);
}

// ========== 系统信息获取 ==========
// 获取系统时间字符串 "M/D(周) HH:MM"
static void GetTimeString(char* buf, size_t bufSize)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    snprintf(buf, bufSize, "%d/%d(%s) %02d:%02d",
        tm.tm_mon + 1, tm.tm_mday, weekdays[tm.tm_wday],
        tm.tm_hour, tm.tm_min);
}

// 获取电量百分比
static int GetBatteryPercent(void)
{
    u8 level = 100;
    Result rc = MCUHWC_GetBatteryLevel(&level);
    if (R_SUCCEEDED(rc))
    {
        // MCUHWC_GetBatteryLevel 直接返回 0-100 的百分比
        if (level > 100) level = 100;
        return (int)level;
    }
    return 100;
}


// ========== 状态栏 ==========
void UI_DrawStatusBarTop(void)
{
    // 统一状态栏 34px高 背景#ede6b0（比页面背景稍深）
    u32 barBg = C2D_Color32(0xED, 0xE6, 0xB0, 0xFF);
    u32 barText = C2D_Color32(0x44, 0x44, 0x44, 0xFF);
    C2D_DrawRectSolid(0, 0, 0, SCREEN_TOP_W, 34, barBg);

    // 底部投影（悬浮效果）
    C2D_DrawRectSolid(0, 34, 0, SCREEN_TOP_W, 1, C2D_Color32(0x00, 0x00, 0x00, 0x30));
    C2D_DrawRectSolid(0, 35, 0, SCREEN_TOP_W, 1, C2D_Color32(0x00, 0x00, 0x00, 0x18));
    C2D_DrawRectSolid(0, 36, 0, SCREEN_TOP_W, 1, C2D_Color32(0x00, 0x00, 0x00, 0x0A));

    // 左侧：网络状态图标 + 简洁文字
    bool wifiConnected = Network_CheckConnection();
    const char* wifiText = wifiConnected ? "在线" : "离线";
    float wifiIconSize = 20.0f;
    float wifiIconScale = wifiIconSize / 32.0f;
    float wifiIconY = 5.0f;
    if (wifiConnected && s_wifiOnSheet && s_wifiOnIcon.subtex)
        C2D_DrawImageAt(s_wifiOnIcon, 4, wifiIconY, 0.5f, NULL, wifiIconScale, wifiIconScale);
    else if (!wifiConnected && s_wifiOffSheet && s_wifiOffIcon.subtex)
        C2D_DrawImageAt(s_wifiOffIcon, 4, wifiIconY, 0.5f, NULL, wifiIconScale, wifiIconScale);
    UI_DrawText(4 + wifiIconSize + 4, 8, 0.5f, barText, wifiText);

    // 右侧：闪电图标 + 电量百分比
    int bat = GetBatteryPercent();
    char batStr[8];
    snprintf(batStr, sizeof(batStr), "%d%%", bat);
    float batW = UI_MeasureText(0.6f, batStr);
    // 闪电图标（用户提供的图片，14px高）
    float boltScale = 14.0f / 32.0f;
    float boltSize = 14.0f;
    float boltX = SCREEN_TOP_W - batW - 8 - 4 - boltSize;
    float boltY = (34 - boltSize) / 2.0f;
    if (s_boltSheet && s_boltIcon.subtex)
        C2D_DrawImageAt(s_boltIcon, boltX, boltY, 0.5f, NULL, boltScale, boltScale);
    UI_DrawText(SCREEN_TOP_W - batW - 8, 8, 0.6f, barText, batStr);

    // 时间（在闪电左侧）
    char timeStr[32];
    GetTimeString(timeStr, sizeof(timeStr));
    float tw = UI_MeasureText(0.6f, timeStr);
    UI_DrawText(boltX - tw - 8, 8, 0.6f, barText, timeStr);
}

void UI_DrawStatusBarMain(void)
{
    // 统一使用同一个状态栏
    UI_DrawStatusBarTop();
}
