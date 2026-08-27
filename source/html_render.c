// html_render.c - 轻量级HTML富文本解析与渲染
// 状态机+样式栈，流式排版，适配3DS上屏400px宽度
#include "html_render.h"
#include "ui.h"
#include <citro2d.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

// ========== 配置 ==========
#define MAX_LINES 512
#define MAX_SPANS 10
#define MAX_TAG_DEPTH 20
#define SPAN_TEXT_LEN 128
#define MAX_LIST_DEPTH 8

// ========== 数据结构 ==========

// 文本片段（一行内的一段带样式文字）
typedef struct {
    char text[SPAN_TEXT_LEN];
    u32 color;
    float scale;
    u8 underline;
    u8 bold;
} Span;

// 图片类型（统一用文本占位，不下载图片）
enum {
    IMG_PLACEHOLDER = 0,  // [图片]（CID内嵌图片）
    IMG_EXTERNAL          // [外部网络图片]
};

// 渲染行
typedef struct {
    Span spans[MAX_SPANS];
    int spanCount;
    u8 align;       // HtmlAlign
    int indent;     // 左边距像素
    u8 isHR;        // 水平分割线
    u8 isImage;     // 图片占位
    u8 imgType;     // 图片类型
    char imgCid[128]; // CID标识符
    int imgWidth;   // 图片宽度
    int imgHeight;  // 图片高度
    float spacing;  // 段后额外间距
} RenderLine;

// 样式栈帧
typedef struct {
    u32 color;
    float scale;
    u8 underline;
    u8 bold;
    u8 align;
    int indent;
} StyleFrame;

// ========== 全局状态 ==========
static RenderLine g_lines[MAX_LINES];
static int g_lineCount = 0;
static int g_curLine = 0;       // 当前正在构建的行
static StyleFrame g_styleStack[MAX_TAG_DEPTH];
static int g_styleDepth = 0;
static int g_listDepth = 0;
static int g_listCounters[MAX_LIST_DEPTH]; // ol序号
static u8 g_listOrdered[MAX_LIST_DEPTH];   // 1=ol, 0=ul
static float g_baseScale = 0.45f;
static int g_maxWidth = 368;
static bool g_skipContent = false;  // 跳过script/style内容

static int g_skipDepth = 0;

// ========== 辅助函数 ==========

// 当前样式
static StyleFrame* cur_style(void) {
    if (g_styleDepth > 0) return &g_styleStack[g_styleDepth - 1];
    static StyleFrame def = { HTML_COLOR_BLACK, 0.45f, 0, 0, HTML_ALIGN_LEFT, 0 };
    return &def;
}

static void push_style(void) {
    if (g_styleDepth >= MAX_TAG_DEPTH) return;
    if (g_styleDepth > 0)
        g_styleStack[g_styleDepth] = g_styleStack[g_styleDepth - 1];
    else {
        g_styleStack[0].color = HTML_COLOR_BLACK;
        g_styleStack[0].scale = g_baseScale;
        g_styleStack[0].underline = 0;
        g_styleStack[0].bold = 0;
        g_styleStack[0].align = HTML_ALIGN_LEFT;
        g_styleStack[0].indent = 0;
    }
    g_styleDepth++;
}

static void pop_style(void) {
    if (g_styleDepth > 0) g_styleDepth--;
}

// 颜色解析：支持 #RGB #RRGGBB rgb(r,g,b) 和命名颜色
static u32 parse_color(const char* str) {
    if (!str || !*str) return HTML_COLOR_BLACK;
    while (*str == ' ') str++;

    if (*str == '#') {
        str++;
        int len = strlen(str);
        u32 r=0, g=0, b=0;
        if (len >= 6) {
            sscanf(str, "%2x%2x%2x", &r, &g, &b);
        } else if (len >= 3) {
            r = (str[0] >= 'a' ? str[0]-'a'+10 : str[0]-'0') * 17;
            g = (str[1] >= 'a' ? str[1]-'a'+10 : str[1]-'0') * 17;
            b = (str[2] >= 'a' ? str[2]-'a'+10 : str[2]-'0') * 17;
        }
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    // rgb(r,g,b)
    if (strncasecmp(str, "rgb", 3) == 0) {
        const char* p = strchr(str, '(');
        if (p) {
            int r=0, g=0, b=0;
            sscanf(p+1, "%d,%d,%d", &r, &g, &b);
            return 0xFF000000 | ((r&0xFF)<<16) | ((g&0xFF)<<8) | (b&0xFF);
        }
    }

    // 命名颜色
    if (strncasecmp(str, "red", 3) == 0) return 0xFFFF0000;
    if (strncasecmp(str, "blue", 4) == 0) return 0xFF0000FF;
    if (strncasecmp(str, "green", 5) == 0) return 0xFF008000;
    if (strncasecmp(str, "gray", 4) == 0 || strncasecmp(str, "grey", 4) == 0) return 0xFF808080;
    if (strncasecmp(str, "black", 5) == 0) return 0xFF000000;
    if (strncasecmp(str, "white", 5) == 0) return 0xFFFFFFFF;
    if (strncasecmp(str, "orange", 6) == 0) return 0xFFFFA500;
    if (strncasecmp(str, "yellow", 6) == 0) return 0xFFFFFF00;

    return HTML_COLOR_BLACK;
}

// 从style属性中提取color值
static const char* extract_style_color(const char* style) {
    const char* p = strstr(style, "color:");
    if (!p) p = strstr(style, "color :");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            return p;
        }
    }
    return NULL;
}

// 从style属性中提取text-align值
static int extract_style_align(const char* style) {
    const char* p = strstr(style, "text-align");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (strncasecmp(p, "center", 6) == 0) return HTML_ALIGN_CENTER;
    if (strncasecmp(p, "right", 5) == 0) return HTML_ALIGN_RIGHT;
    if (strncasecmp(p, "left", 4) == 0) return HTML_ALIGN_LEFT;
    return -1;
}

// HTML实体解码，返回解码后字符，p前进到实体末尾
static u32 decode_entity(const char** p) {
    const char* s = *p;
    if (*s != '&') return 0;
    s++;

    // &#数字;
    if (*s == '#') {
        s++;
        int num = 0;
        if (*s == 'x' || *s == 'X') {
            s++;
            while (isxdigit((unsigned char)*s)) {
                num = num * 16 + (isdigit((unsigned char)*s) ? *s-'0' :
                      (tolower((unsigned char)*s)-'a'+10));
                s++;
            }
        } else {
            while (isdigit((unsigned char)*s)) {
                num = num * 10 + (*s - '0');
                s++;
            }
        }
        if (*s == ';') s++;
        *p = s;
        return num;
    }

    // 命名实体
    struct { const char* name; u32 ch; } entities[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'},
        {"nbsp;", ' '}, {"apos;", '\''}, {"copy;", 0xA9}, {"reg;", 0xAE},
        {"mdash;", 0x2014}, {"ndash;", 0x2013}, {"hellip;", 0x2026},
        {NULL, 0}
    };
    for (int i = 0; entities[i].name; i++) {
        int len = strlen(entities[i].name);
        if (strncasecmp(s, entities[i].name, len) == 0) {
            *p = s + len;
            return entities[i].ch;
        }
    }

    // 未知实体，原样返回
    *p = s;
    return '&';
}

// ========== 行管理 ==========

static void new_line(void) {
    if (g_curLine >= MAX_LINES - 1) return;
    // 如果当前行有内容，提交
    if (g_lines[g_curLine].spanCount > 0 || g_lines[g_curLine].isHR ||
        g_lines[g_curLine].isImage) {
        g_curLine++;
    }
    g_lineCount = g_curLine + 1;
    // 初始化新行
    memset(&g_lines[g_curLine], 0, sizeof(RenderLine));
    StyleFrame* st = cur_style();
    g_lines[g_curLine].align = st->align;
    g_lines[g_curLine].indent = st->indent;
}

static void ensure_line(void) {
    if (g_lineCount == 0) {
        memset(&g_lines[0], 0, sizeof(RenderLine));
        g_lineCount = 1;
        g_curLine = 0;
        StyleFrame* st = cur_style();
        g_lines[0].align = st->align;
        g_lines[0].indent = st->indent;
    }
}

// 添加span到当前行，处理自动换行
static void add_span(const char* text, const char* textEnd, u32 color,
                     float scale, u8 underline, u8 bold)
{
    if (!text || text >= textEnd) return;

    // 浅色文字（白色等）在米黄背景上看不清，替换为深灰色 #555555
    {
        u8 cr = color & 0xFF;
        u8 cg = (color >> 8) & 0xFF;
        u8 cb = (color >> 16) & 0xFF;
        // 相对亮度（0-255），超过170视为太浅
        float lum = 0.299f * cr + 0.587f * cg + 0.114f * cb;
        if (lum > 170.0f)
            color = C2D_Color32(0x55, 0x55, 0x55, 0xFF);
    }

    ensure_line();

    const char* p = text;
    // 一个文本节点可能远大于 127 字节（如连续中文长段落）。
    // 必须按 ≤SPAN_TEXT_LEN-1 字节、且在完整 UTF-8 字符边界处分块循环追加，
    // 绝不能在 127 字节处停下后丢弃剩余文字（旧版本长段落会被截断）。
    while (p < textEnd)
    {
        ensure_line();
        RenderLine* line = &g_lines[g_curLine];
<<<<<<< HEAD

        // 当前行为空时，跳过纯空白块（块级标签间的换行/缩进），避免多余空行
        if (line->spanCount == 0 && !line->isHR && !line->isImage)
        {
            bool allSpace = true;
            for (const char* q = p; q < textEnd; q++)
            {
                if (*q != ' ' && *q != '\t' && *q != '\n' && *q != '\r')
                { allSpace = false; break; }
            }
            if (allSpace) return;
        }

=======
<<<<<<< HEAD

        // 当前行为空时，跳过纯空白块（块级标签间的换行/缩进），避免多余空行
        if (line->spanCount == 0 && !line->isHR && !line->isImage)
        {
            bool allSpace = true;
            for (const char* q = p; q < textEnd; q++)
            {
                if (*q != ' ' && *q != '\t' && *q != '\n' && *q != '\r')
                { allSpace = false; break; }
            }
            if (allSpace) return;
        }

=======

        // 当前行为空时，跳过纯空白块（块级标签间的换行/缩进），避免多余空行
        if (line->spanCount == 0 && !line->isHR && !line->isImage)
        {
            bool allSpace = true;
            for (const char* q = p; q < textEnd; q++)
            {
                if (*q != ' ' && *q != '\t' && *q != '\n' && *q != '\r')
                { allSpace = false; break; }
            }
            if (allSpace) return;
        }

<<<<<<< HEAD
>>>>>>> 83d91e5ea2997eb8a09b02ef775c4d1aa70edec9
>>>>>>> 02f2e74d037b2a03dd99babd9ea59e05a5bbc2f2
        // 构建一个分块 spanBuf（UTF-8 边界安全，实体整体解码）
        char spanBuf[SPAN_TEXT_LEN];
        int len = 0;
        while (p < textEnd && len < SPAN_TEXT_LEN - 1) {
            if (*p == '&') {
                const char* before = p;
                u32 ch = decode_entity(&p);
                if (ch == 0 || ch > 0x10FFFF) ch = 0xFFFD;
                int enc = (ch < 0x80) ? 1 : (ch < 0x800) ? 2 :
                          (ch < 0x10000) ? 3 : 4;
                if (len + enc > SPAN_TEXT_LEN - 1) { p = before; break; }
                if (ch < 0x80) {
                    spanBuf[len++] = (char)ch;
                } else if (ch < 0x800) {
                    spanBuf[len++] = (char)(0xC0 | (ch >> 6));
                    spanBuf[len++] = (char)(0x80 | (ch & 0x3F));
                } else if (ch < 0x10000) {
                    spanBuf[len++] = (char)(0xE0 | (ch >> 12));
                    spanBuf[len++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    spanBuf[len++] = (char)(0x80 | (ch & 0x3F));
<<<<<<< HEAD
                } else {
                    spanBuf[len++] = (char)(0xF0 | (ch >> 18));
                    spanBuf[len++] = (char)(0x80 | ((ch >> 12) & 0x3F));
                    spanBuf[len++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    spanBuf[len++] = (char)(0x80 | (ch & 0x3F));
=======
<<<<<<< HEAD
                } else {
                    spanBuf[len++] = (char)(0xF0 | (ch >> 18));
                    spanBuf[len++] = (char)(0x80 | ((ch >> 12) & 0x3F));
                    spanBuf[len++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    spanBuf[len++] = (char)(0x80 | (ch & 0x3F));
=======
                } else {
                    spanBuf[len++] = (char)(0xF0 | (ch >> 18));
                    spanBuf[len++] = (char)(0x80 | ((ch >> 12) & 0x3F));
                    spanBuf[len++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    spanBuf[len++] = (char)(0x80 | (ch & 0x3F));
=======
    // 检查是否需要换行：测量当前行已有宽度+新span宽度
    float existingW = 0;
    for (int i = 0; i < line->spanCount; i++) {
        existingW += UI_MeasureText(line->spans[i].scale, line->spans[i].text);
    }
    float spanW = UI_MeasureText(scale, spanBuf);
    int availW = g_maxWidth - line->indent;

    if (existingW + spanW > availW && existingW > 0) {
        // 需要换行
        new_line();
        line = &g_lines[g_curLine];
    }

    // 如果单个span还是超宽，在span内部断行
    spanW = UI_MeasureText(scale, spanBuf);
    if (spanW > availW) {
        // 逐字符断行（按完整UTF-8字符推进，绝不拆分多字节汉字）
        char chunk[SPAN_TEXT_LEN];
        int ci = 0;
        int lastSpace = -1;
        for (int i = 0; i < len; ) {
            // 计算当前UTF-8字符占用的字节数
            int cb = 1;
            unsigned char c0 = (unsigned char)spanBuf[i];
            if      ((c0 & 0xE0) == 0xC0) cb = 2;
            else if ((c0 & 0xF0) == 0xE0) cb = 3;
            else if ((c0 & 0xF8) == 0xF0) cb = 4;
            if (i + cb > len) cb = len - i;

            for (int b = 0; b < cb; b++) chunk[ci++] = spanBuf[i + b];
            chunk[ci] = 0;
            if (cb == 1 && spanBuf[i] == ' ') lastSpace = ci;
            float cw = UI_MeasureText(scale, chunk);
            if (cw > availW && ci > cb) {
                if (lastSpace > 0) {
                    // 按空格断
                    chunk[lastSpace - 1] = 0;
                    if (line->spanCount < MAX_SPANS) {
                        Span* sp = &line->spans[line->spanCount++];
                        strncpy(sp->text, chunk, SPAN_TEXT_LEN - 1);
                        sp->color = color; sp->scale = scale;
                        sp->underline = underline; sp->bold = bold;
                    }
                    // 剩余部分（含当前字符）移到下一行
                    int rem = ci - lastSpace;
                    memmove(chunk, chunk + lastSpace, rem);
                    ci = rem;
                    chunk[ci] = 0;
                    lastSpace = -1;
                } else {
                    // 把整个当前字符移到下一行行首，避免把多字节汉字拆坏
                    ci -= cb;
                    chunk[ci] = 0;
                    if (line->spanCount < MAX_SPANS) {
                        Span* sp = &line->spans[line->spanCount++];
                        strncpy(sp->text, chunk, SPAN_TEXT_LEN - 1);
                        sp->color = color; sp->scale = scale;
                        sp->underline = underline; sp->bold = bold;
                    }
                    memcpy(chunk, spanBuf + i, cb);
                    ci = cb;
                    chunk[ci] = 0;
>>>>>>> cb02601b9040b431421dc9202cb8064d869f1903
>>>>>>> 83d91e5ea2997eb8a09b02ef775c4d1aa70edec9
>>>>>>> 02f2e74d037b2a03dd99babd9ea59e05a5bbc2f2
                }
            } else if (*p == '\r') {
                p++;
            } else if (*p == '\n' || *p == '\t') {
                if (len + 1 > SPAN_TEXT_LEN - 1) break;
                spanBuf[len++] = ' ';
                p++;
            } else {
                // 完整 UTF-8 字符：放不下整块就留给下一分块，绝不拆开汉字
                int cb = 1;
                unsigned char c0 = (unsigned char)*p;
                if      ((c0 & 0xE0) == 0xC0) cb = 2;
                else if ((c0 & 0xF0) == 0xE0) cb = 3;
                else if ((c0 & 0xF8) == 0xF0) cb = 4;
                if (p + cb > textEnd) cb = (int)(textEnd - p);
                if (len + cb > SPAN_TEXT_LEN - 1) break;
                for (int b = 0; b < cb; b++) spanBuf[len++] = p[b];
                p += cb;
<<<<<<< HEAD
            }
        }
        spanBuf[len] = 0;
        if (len == 0) {
            // 理论上不会到达；强制推进防止死循环
            if (p < textEnd) p++; else break;
            continue;
        }

        // 逐字符把本分块填入当前行。当前行可能已留有上一分块的内容，
        // 必须先把当前行填满再换行，否则会出现"整行/半行"交替的稀疏排版。
        float existW = 0;
        for (int si = 0; si < line->spanCount; si++)
            existW += UI_MeasureText(line->spans[si].scale, line->spans[si].text);
        int availW = g_maxWidth - line->indent;

        char run[SPAN_TEXT_LEN];
        int ri = 0;
        int lastSpace = -1;
        int ii = 0;

        // 把 run[0..ri) 作为一个 span 追加到当前行，并累计宽度
        #define EMIT_RUN() do { \
            if (ri > 0 && line->spanCount < MAX_SPANS) { \
                run[ri] = 0; \
                Span* _sp = &line->spans[line->spanCount++]; \
                strncpy(_sp->text, run, SPAN_TEXT_LEN - 1); \
                _sp->text[SPAN_TEXT_LEN-1] = 0; \
                _sp->color = color; _sp->scale = scale; \
                _sp->underline = underline; _sp->bold = bold; \
                existW += UI_MeasureText(scale, run); \
            } \
        } while(0)

        while (ii < len)
        {
            int cb = 1;
            unsigned char c0 = (unsigned char)spanBuf[ii];
            if      ((c0 & 0xE0) == 0xC0) cb = 2;
            else if ((c0 & 0xF0) == 0xE0) cb = 3;
            else if ((c0 & 0xF8) == 0xF0) cb = 4;
            if (ii + cb > len) cb = len - ii;

            // 试把当前字符加入 run
            for (int b = 0; b < cb; b++) run[ri + b] = spanBuf[ii + b];
            int newRi = ri + cb;
            run[newRi] = 0;
            float runW = UI_MeasureText(scale, run);

            if (existW + runW > availW && (ri > 0 || existW > 0))
            {
                // 当前行放不下：先输出能放下的部分
                int cut = ri;
                int carryStart = ri;
                if (lastSpace > 0) { cut = lastSpace; carryStart = lastSpace; }

                // 关键：先把放不下的尾部（含当前字符）拷出，
                // 不能先 run[cut]=0，否则会覆盖尾部首字节，使带到新行的内容变空串丢字。
                char tail[SPAN_TEXT_LEN];
                int carryLen = newRi - carryStart;
                memcpy(tail, run + carryStart, carryLen);

                run[cut] = 0;
                ri = cut;
                EMIT_RUN();

                new_line();
                line = &g_lines[g_curLine];
                existW = 0;

                // 尾部整体带到新行行首，并去掉行首空格
                int sk = 0;
                while (sk < carryLen && tail[sk] == ' ') sk++;
                carryLen -= sk;
                memcpy(run, tail + sk, carryLen);
                run[carryLen] = 0;
                ri = carryLen;
                lastSpace = -1;
                for (int k = 0; k + 1 < ri; ) {
                    int cl = 1;
                    unsigned char rc = (unsigned char)run[k];
                    if      ((rc & 0xE0) == 0xC0) cl = 2;
                    else if ((rc & 0xF0) == 0xE0) cl = 3;
                    else if ((rc & 0xF8) == 0xF0) cl = 4;
                    if (cl == 1 && run[k] == ' ') lastSpace = k + 1;
                    k += cl;
                }
=======
<<<<<<< HEAD
>>>>>>> 02f2e74d037b2a03dd99babd9ea59e05a5bbc2f2
            }
            else
            {
                ri = newRi;
                if (cb == 1 && spanBuf[ii] == ' ') lastSpace = ri;
            }
            ii += cb;
        }
<<<<<<< HEAD
        EMIT_RUN();
        #undef EMIT_RUN
=======
        spanBuf[len] = 0;
        if (len == 0) {
            // 理论上不会到达；强制推进防止死循环
            if (p < textEnd) p++; else break;
            continue;
        }

        // 逐字符把本分块填入当前行。当前行可能已留有上一分块的内容，
        // 必须先把当前行填满再换行，否则会出现"整行/半行"交替的稀疏排版。
        float existW = 0;
        for (int si = 0; si < line->spanCount; si++)
            existW += UI_MeasureText(line->spans[si].scale, line->spans[si].text);
        int availW = g_maxWidth - line->indent;

        char run[SPAN_TEXT_LEN];
        int ri = 0;
        int lastSpace = -1;
        int ii = 0;

        // 把 run[0..ri) 作为一个 span 追加到当前行，并累计宽度
        #define EMIT_RUN() do { \
            if (ri > 0 && line->spanCount < MAX_SPANS) { \
                run[ri] = 0; \
                Span* _sp = &line->spans[line->spanCount++]; \
                strncpy(_sp->text, run, SPAN_TEXT_LEN - 1); \
                _sp->text[SPAN_TEXT_LEN-1] = 0; \
                _sp->color = color; _sp->scale = scale; \
                _sp->underline = underline; _sp->bold = bold; \
                existW += UI_MeasureText(scale, run); \
            } \
        } while(0)

        while (ii < len)
        {
            int cb = 1;
            unsigned char c0 = (unsigned char)spanBuf[ii];
            if      ((c0 & 0xE0) == 0xC0) cb = 2;
            else if ((c0 & 0xF0) == 0xE0) cb = 3;
            else if ((c0 & 0xF8) == 0xF0) cb = 4;
            if (ii + cb > len) cb = len - ii;

            // 试把当前字符加入 run
            for (int b = 0; b < cb; b++) run[ri + b] = spanBuf[ii + b];
            int newRi = ri + cb;
            run[newRi] = 0;
            float runW = UI_MeasureText(scale, run);

            if (existW + runW > availW && (ri > 0 || existW > 0))
            {
                // 当前行放不下：先输出能放下的部分
                int cut = ri;
                int carryStart = ri;
                if (lastSpace > 0) { cut = lastSpace; carryStart = lastSpace; }

                // 关键：先把放不下的尾部（含当前字符）拷出，
                // 不能先 run[cut]=0，否则会覆盖尾部首字节，使带到新行的内容变空串丢字。
                char tail[SPAN_TEXT_LEN];
                int carryLen = newRi - carryStart;
                memcpy(tail, run + carryStart, carryLen);

                run[cut] = 0;
                ri = cut;
                EMIT_RUN();

                new_line();
                line = &g_lines[g_curLine];
                existW = 0;

                // 尾部整体带到新行行首，并去掉行首空格
                int sk = 0;
                while (sk < carryLen && tail[sk] == ' ') sk++;
                carryLen -= sk;
                memcpy(run, tail + sk, carryLen);
                run[carryLen] = 0;
                ri = carryLen;
                lastSpace = -1;
                for (int k = 0; k + 1 < ri; ) {
                    int cl = 1;
                    unsigned char rc = (unsigned char)run[k];
                    if      ((rc & 0xE0) == 0xC0) cl = 2;
                    else if ((rc & 0xF0) == 0xE0) cl = 3;
                    else if ((rc & 0xF8) == 0xF0) cl = 4;
                    if (cl == 1 && run[k] == ' ') lastSpace = k + 1;
                    k += cl;
                }
            }
            else
            {
                ri = newRi;
                if (cb == 1 && spanBuf[ii] == ' ') lastSpace = ri;
            }
            ii += cb;
        }
        EMIT_RUN();
        #undef EMIT_RUN
=======
            }
            i += cb;
        }
        spanBuf[len] = 0;
        if (len == 0) {
            // 理论上不会到达；强制推进防止死循环
            if (p < textEnd) p++; else break;
            continue;
        }

        // 检查是否需要换行：测量当前行已有宽度+本分块宽度
        float existingW = 0;
        for (int i = 0; i < line->spanCount; i++) {
            existingW += UI_MeasureText(line->spans[i].scale, line->spans[i].text);
        }
        float spanW = UI_MeasureText(scale, spanBuf);
        int availW = g_maxWidth - line->indent;

        if (existingW + spanW > availW && existingW > 0) {
            // 需要换行
            new_line();
            line = &g_lines[g_curLine];
        }

        // 如果单个分块还是超宽，在分块内部断行
        spanW = UI_MeasureText(scale, spanBuf);
        if (spanW > availW) {
            // 逐字符断行（按完整UTF-8字符推进，绝不拆分多字节汉字）
            char chunk[SPAN_TEXT_LEN];
            int ci = 0;
            int lastSpace = -1;
            for (int i = 0; i < len; ) {
                // 计算当前UTF-8字符占用的字节数
                int cb = 1;
                unsigned char c0 = (unsigned char)spanBuf[i];
                if      ((c0 & 0xE0) == 0xC0) cb = 2;
                else if ((c0 & 0xF0) == 0xE0) cb = 3;
                else if ((c0 & 0xF8) == 0xF0) cb = 4;
                if (i + cb > len) cb = len - i;

                for (int b = 0; b < cb; b++) chunk[ci++] = spanBuf[i + b];
                chunk[ci] = 0;
                if (cb == 1 && spanBuf[i] == ' ') lastSpace = ci;
                float cw = UI_MeasureText(scale, chunk);
                if (cw > availW && ci > cb) {
                    if (lastSpace > 0) {
                        // 按空格断
                        chunk[lastSpace - 1] = 0;
                        if (line->spanCount < MAX_SPANS) {
                            Span* sp = &line->spans[line->spanCount++];
                            strncpy(sp->text, chunk, SPAN_TEXT_LEN - 1);
                            sp->text[SPAN_TEXT_LEN-1] = 0;
                            sp->color = color; sp->scale = scale;
                            sp->underline = underline; sp->bold = bold;
                        }
                        // 剩余部分（含当前字符）移到下一行
                        int rem = ci - lastSpace;
                        memmove(chunk, chunk + lastSpace, rem);
                        ci = rem;
                        chunk[ci] = 0;
                        lastSpace = -1;
                    } else {
                        // 把整个当前字符移到下一行行首，避免把多字节汉字拆坏
                        ci -= cb;
                        chunk[ci] = 0;
                        if (line->spanCount < MAX_SPANS) {
                            Span* sp = &line->spans[line->spanCount++];
                            strncpy(sp->text, chunk, SPAN_TEXT_LEN - 1);
                            sp->text[SPAN_TEXT_LEN-1] = 0;
                            sp->color = color; sp->scale = scale;
                            sp->underline = underline; sp->bold = bold;
                        }
                        memcpy(chunk, spanBuf + i, cb);
                        ci = cb;
                        chunk[ci] = 0;
                    }
                    new_line();
                    line = &g_lines[g_curLine];
                }
                i += cb;
            }
            if (ci > 0 && line->spanCount < MAX_SPANS) {
                chunk[ci] = 0;
                Span* sp = &line->spans[line->spanCount++];
                strncpy(sp->text, chunk, SPAN_TEXT_LEN - 1);
                sp->text[SPAN_TEXT_LEN-1] = 0;
                sp->color = color; sp->scale = scale;
                sp->underline = underline; sp->bold = bold;
            }
        } else {
            // 直接添加
            if (line->spanCount < MAX_SPANS) {
                Span* sp = &line->spans[line->spanCount++];
                strncpy(sp->text, spanBuf, SPAN_TEXT_LEN - 1);
                sp->text[SPAN_TEXT_LEN-1] = 0;
                sp->color = color; sp->scale = scale;
                sp->underline = underline; sp->bold = bold;
            }
        }
>>>>>>> 83d91e5ea2997eb8a09b02ef775c4d1aa70edec9
>>>>>>> 02f2e74d037b2a03dd99babd9ea59e05a5bbc2f2
    }
}

// 添加分割线行
static void add_hr_line(void) {
    new_line();
    ensure_line();
    g_lines[g_curLine].isHR = 1;
    new_line();
}

// 添加图片行（统一用文本占位，不下载图片）
static void add_image_line(const char* src) {
    new_line();
    ensure_line();
    RenderLine* line = &g_lines[g_curLine];
    line->isImage = 1;
    line->imgType = IMG_PLACEHOLDER;
    line->imgWidth = 0;
    line->imgHeight = 0;
    line->imgCid[0] = 0;

    if (src && src[0]) {
        // 外部网络图片（http/https开头）显示不同占位文本
        if (strncasecmp(src, "http://", 7) == 0 ||
            strncasecmp(src, "https://", 8) == 0) {
            line->imgType = IMG_EXTERNAL;
        }
        // CID内嵌图片和其他情况统一显示[图片]
    }
    new_line();
}

// ========== 标签解析 ==========

// 从标签中提取属性值（如 href="..."），返回值存入buf
static void get_attr(const char* tag, const char* attrName, char* buf, int bufSize) {
    const char* p = strstr(tag, attrName);
    if (!p) { buf[0] = 0; return; }
    p += strlen(attrName);
    while (*p == ' ' || *p == '=') p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') { quote = *p; p++; }
    int i = 0;
    while (*p && i < bufSize - 1) {
        if (quote) {
            if (*p == quote) break;
        } else {
            if (*p == ' ' || *p == '>' || *p == '/') break;
        }
        buf[i++] = *p++;
    }
    buf[i] = 0;
}

// 解析开始标签
static void handle_open_tag(const char* tag, int tagLen) {
    char tagName[32];
    int i = 0;
    // 跳过标签名前的空白
    const char* p = tag;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && *p != '/' && *p != '>' && i < 31) {
        tagName[i++] = tolower((unsigned char)*p++);
    }
    tagName[i] = 0;

    // 跳过script/style/head内容
    if (strcmp(tagName, "script") == 0 || strcmp(tagName, "style") == 0 ||
        strcmp(tagName, "head") == 0) {
        g_skipContent = true;
        g_skipDepth++;
        return;
    }

    // 自闭合标签
    bool selfClose = (tag[tagLen - 1] == '/');

    if (strcmp(tagName, "br") == 0) {
        new_line();
        return;
    }
    if (strcmp(tagName, "hr") == 0) {
        add_hr_line();
        return;
    }
    if (strcmp(tagName, "img") == 0) {
        char srcVal[256];
        get_attr(tag, "src", srcVal, sizeof(srcVal));
        add_image_line(srcVal);
        return;
    }

    // 段落类标签
    if (strcmp(tagName, "p") == 0 || strcmp(tagName, "div") == 0 ||
        strcmp(tagName, "blockquote") == 0 || strcmp(tagName, "pre") == 0 ||
        strcmp(tagName, "table") == 0 || strcmp(tagName, "tr") == 0) {
        new_line();
        push_style();
        if (strcmp(tagName, "blockquote") == 0) {
            cur_style()->indent += 20;
            cur_style()->color = HTML_COLOR_GRAY;
        }
        // 解析align属性
        char alignVal[32];
        get_attr(tag, "align", alignVal, sizeof(alignVal));
        if (strcasecmp(alignVal, "center") == 0) cur_style()->align = HTML_ALIGN_CENTER;
        else if (strcasecmp(alignVal, "right") == 0) cur_style()->align = HTML_ALIGN_RIGHT;
        // 解析style中的text-align
        char styleVal[256];
        get_attr(tag, "style", styleVal, sizeof(styleVal));
        if (styleVal[0]) {
            int a = extract_style_align(styleVal);
            if (a >= 0) cur_style()->align = a;
            const char* c = extract_style_color(styleVal);
            if (c) cur_style()->color = parse_color(c);
        }
        return;
    }

    // 标题
    if (strlen(tagName) == 2 && tagName[0] == 'h' && tagName[1] >= '1' && tagName[1] <= '3') {
        new_line();
        push_style();
        int level = tagName[1] - '0';
        cur_style()->scale = g_baseScale + (3 - level) * 0.12f; // h1=0.69, h2=0.57, h3=0.45+0.12
        cur_style()->bold = 1;
        g_lines[g_curLine].spacing = 6.0f;
        return;
    }
    if (strlen(tagName) == 2 && tagName[0] == 'h' && tagName[1] >= '4' && tagName[1] <= '6') {
        new_line();
        push_style();
        cur_style()->bold = 1;
        return;
    }

    // 加粗
    if (strcmp(tagName, "b") == 0 || strcmp(tagName, "strong") == 0) {
        push_style();
        cur_style()->bold = 1;
        return;
    }

    // 斜体（3DS不支持，仅入栈保持嵌套平衡）
    if (strcmp(tagName, "i") == 0 || strcmp(tagName, "em") == 0) {
        push_style();
        return;
    }

    // 下划线
    if (strcmp(tagName, "u") == 0 || strcmp(tagName, "ins") == 0) {
        push_style();
        cur_style()->underline = 1;
        return;
    }

    // 删除线（不支持，入栈）
    if (strcmp(tagName, "s") == 0 || strcmp(tagName, "strike") == 0 ||
        strcmp(tagName, "del") == 0) {
        push_style();
        return;
    }

    // 链接
    if (strcmp(tagName, "a") == 0) {
        push_style();
        cur_style()->color = HTML_COLOR_LINK;
        cur_style()->underline = 1;
        return;
    }

    // font标签
    if (strcmp(tagName, "font") == 0) {
        push_style();
        char colorVal[32];
        get_attr(tag, "color", colorVal, sizeof(colorVal));
        if (colorVal[0]) cur_style()->color = parse_color(colorVal);
        return;
    }

    // span标签
    if (strcmp(tagName, "span") == 0) {
        push_style();
        char styleVal[256];
        get_attr(tag, "style", styleVal, sizeof(styleVal));
        if (styleVal[0]) {
            const char* c = extract_style_color(styleVal);
            if (c) cur_style()->color = parse_color(c);
            int a = extract_style_align(styleVal);
            if (a >= 0) cur_style()->align = a;
        }
        char colorVal[32];
        get_attr(tag, "color", colorVal, sizeof(colorVal));
        if (colorVal[0]) cur_style()->color = parse_color(colorVal);
        return;
    }

    // 列表
    if (strcmp(tagName, "ul") == 0 || strcmp(tagName, "ol") == 0) {
        new_line();
        if (g_listDepth < MAX_LIST_DEPTH) {
            g_listOrdered[g_listDepth] = (strcmp(tagName, "ol") == 0) ? 1 : 0;
            g_listCounters[g_listDepth] = 0;
            g_listDepth++;
        }
        push_style();
        cur_style()->indent += 16;
        return;
    }

    if (strcmp(tagName, "li") == 0) {
        new_line();
        // 添加项目符号
        char bullet[16];
        if (g_listDepth > 0 && g_listOrdered[g_listDepth - 1]) {
            g_listCounters[g_listDepth - 1]++;
            snprintf(bullet, sizeof(bullet), "%d. ", g_listCounters[g_listDepth - 1]);
        } else {
            strcpy(bullet, "\xe2\x80\xa2 "); // •
        }
        StyleFrame* st = cur_style();
        add_span(bullet, bullet + strlen(bullet), st->color, st->scale, 0, 0);
        return;
    }

    // 其他标签：body, html, td, th, dd, dt, section, article, header, footer, nav等
    // 入栈以保持嵌套平衡
    push_style();
}

// 解析结束标签
static void handle_close_tag(const char* tag) {
    char tagName[32];
    int i = 0;
    const char* p = tag;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && *p != '>' && i < 31) {
        tagName[i++] = tolower((unsigned char)*p++);
    }
    tagName[i] = 0;

    // script/style/head结束
    if (strcmp(tagName, "script") == 0 || strcmp(tagName, "style") == 0 ||
        strcmp(tagName, "head") == 0) {
        if (g_skipDepth > 0) g_skipDepth--;
        if (g_skipDepth == 0) g_skipContent = false;
        return;
    }

    if (strcmp(tagName, "p") == 0 || strcmp(tagName, "div") == 0 ||
        strcmp(tagName, "blockquote") == 0 || strcmp(tagName, "pre") == 0 ||
        strcmp(tagName, "table") == 0 || strcmp(tagName, "tr") == 0 ||
        strcmp(tagName, "b") == 0 || strcmp(tagName, "strong") == 0 ||
        strcmp(tagName, "i") == 0 || strcmp(tagName, "em") == 0 ||
        strcmp(tagName, "u") == 0 || strcmp(tagName, "ins") == 0 ||
        strcmp(tagName, "s") == 0 || strcmp(tagName, "strike") == 0 ||
        strcmp(tagName, "del") == 0 || strcmp(tagName, "a") == 0 ||
        strcmp(tagName, "font") == 0 || strcmp(tagName, "span") == 0 ||
        strcmp(tagName, "ul") == 0 || strcmp(tagName, "ol") == 0 ||
        strcmp(tagName, "li") == 0 || strcmp(tagName, "td") == 0 ||
        strcmp(tagName, "th") == 0 || strcmp(tagName, "dd") == 0 ||
        strcmp(tagName, "dt") == 0 || strcmp(tagName, "section") == 0 ||
        strcmp(tagName, "article") == 0 || strcmp(tagName, "header") == 0 ||
        strcmp(tagName, "footer") == 0 || strcmp(tagName, "nav") == 0 ||
        strcmp(tagName, "main") == 0 || strcmp(tagName, "aside") == 0 ||
        strcmp(tagName, "address") == 0 || strcmp(tagName, "figure") == 0 ||
        strcmp(tagName, "figcaption") == 0 || strcmp(tagName, "details") == 0 ||
        strcmp(tagName, "summary") == 0) {
        pop_style();
        if (strcmp(tagName, "p") == 0 || strcmp(tagName, "div") == 0 ||
            strcmp(tagName, "blockquote") == 0 || strcmp(tagName, "li") == 0 ||
            strcmp(tagName, "tr") == 0) {
            new_line();
        }
        if (strcmp(tagName, "ul") == 0 || strcmp(tagName, "ol") == 0) {
            if (g_listDepth > 0) g_listDepth--;
            new_line();
        }
        return;
    }

    // h1-h6
    if (strlen(tagName) == 2 && tagName[0] == 'h' && tagName[1] >= '1' && tagName[1] <= '6') {
        pop_style();
        new_line();
        return;
    }

    // br/hr/img是自闭合或无结束标签，忽略
}

// ========== 公开接口 ==========

void HtmlRender_SetEmailContext(const char* email, unsigned int uid) {
    (void)email;
    (void)uid;
    // 图片不再下载/缓存，无需加载映射表
}

void HtmlRender_FreeTextures(void) {
    // 无纹理缓存需要释放
}

void HtmlRender_Clear(void) {
    memset(g_lines, 0, sizeof(g_lines));
    g_lineCount = 0;
    g_curLine = 0;
    g_styleDepth = 0;
    g_listDepth = 0;
    g_skipContent = false;
    g_skipDepth = 0;
}

int HtmlRender_Parse(const char* html, int htmlLen, int maxWidth, float baseScale) {
    HtmlRender_Clear();
    if (!html || htmlLen <= 0) return 0;

    g_maxWidth = maxWidth;
    g_baseScale = baseScale;

    // 初始化默认样式
    g_styleStack[0].color = HTML_COLOR_BLACK;
    g_styleStack[0].scale = baseScale;
    g_styleStack[0].underline = 0;
    g_styleStack[0].bold = 0;
    g_styleStack[0].align = HTML_ALIGN_LEFT;
    g_styleStack[0].indent = 0;
    g_styleDepth = 1;

    const char* p = html;
    const char* end = html + htmlLen;

    while (p < end) {
        if (g_skipContent) {
            // 寻找结束标签
            if (*p == '<') {
                const char* close = p + 1;
                if (*close == '/') {
                    // 检查是否是script/style/head结束
                    char tagBuf[32];
                    int ti = 0;
                    const char* tp = close + 1;
                    while (*tp == ' ') tp++;
                    while (*tp && *tp != ' ' && *tp != '>' && ti < 31)
                        tagBuf[ti++] = tolower((unsigned char)*tp++);
                    tagBuf[ti] = 0;
                    if (strcmp(tagBuf, "script") == 0 || strcmp(tagBuf, "style") == 0 ||
                        strcmp(tagBuf, "head") == 0) {
                        if (g_skipDepth > 0) g_skipDepth--;
                        if (g_skipDepth == 0) g_skipContent = false;
                        p = strchr(p, '>');
                        if (p) p++; else break;
                        continue;
                    }
                }
            }
            p++;
            continue;
        }

        if (*p == '<') {
            // 解析标签
            const char* tagStart = p + 1;
            const char* tagEnd = strchr(p, '>');
            if (!tagEnd) break;
            int tagLen = (int)(tagEnd - tagStart);

            if (tagLen > 0) {
                if (*tagStart == '/') {
                    // 结束标签
                    handle_close_tag(tagStart + 1);
                } else if (*tagStart == '!') {
                    // 注释或DOCTYPE，跳过
                } else {
                    // 开始标签
                    // 处理自闭合
                    bool selfClose = (tagLen > 0 && tagStart[tagLen - 1] == '/');
                    handle_open_tag(tagStart, tagLen);
                    // 自闭合标签需要弹出样式（如<br/>已在内部处理）
                    // 对于其他自闭合标签（如<input/>），弹栈
                    if (selfClose && g_styleDepth > 1) {
                        // 检查是否是我们已处理的自闭合标签
                        // br/hr/img已在handle_open_tag中处理，不会push_style
                        // 其他自闭合标签pop一次
                        // 简单处理：不pop，因为大多数自闭合标签我们没push
                    }
                }
            }
            p = tagEnd + 1;
        } else {
            // 文本内容：收集到下一个'<'
            const char* textStart = p;
            while (p < end && *p != '<') p++;
            if (p > textStart) {
                StyleFrame* st = cur_style();
                add_span(textStart, p, st->color, st->scale, st->underline, st->bold);
            }
        }
    }

    // 确保最后一行被计数
    if (g_lineCount == 0) g_lineCount = 1;
    return g_lineCount;
}

int HtmlRender_GetTotalLines(void) {
    return g_lineCount;
}

void HtmlRender_Render(int startX, int startY, int maxWidth, int maxHeight,
                       int scrollLine, float lineHeight, int* outTotalLines) {
    if (outTotalLines) *outTotalLines = g_lineCount;

    int visibleLines = (int)(maxHeight / lineHeight);
    int y = startY;

    for (int i = scrollLine; i < g_lineCount && y < startY + maxHeight; i++) {
        RenderLine* line = &g_lines[i];
        int x = startX + line->indent;

        if (line->isHR) {
            // 水平分割线
            int hrW = maxWidth - line->indent - 20;
            C2D_DrawRectSolid(x + 10, y + lineHeight / 2, 0.5f, hrW, 1,
                              C2D_Color32(0xBD, 0xBD, 0xBD, 0xFF));
            y += (int)lineHeight;
            continue;
        }

        if (line->isImage) {
            // 图片统一用文本占位，不下载不渲染
            const char* txt = (line->imgType == IMG_EXTERNAL) ? "[外部网络图片]" : "[图片]";
            float pw = UI_MeasureText(0.45f, txt);
            int px = x + (maxWidth - line->indent - (int)pw) / 2;
            UI_DrawText(px, y + 2, 0.45f, HTML_COLOR_GRAY, txt);
            y += (int)lineHeight;
            continue;
        }

        if (line->spanCount == 0) {
            // 空行
            y += (int)lineHeight;
            continue;
        }

        // 计算整行总宽度（用于对齐）
        float totalW = 0;
        for (int s = 0; s < line->spanCount; s++) {
            totalW += UI_MeasureText(line->spans[s].scale, line->spans[s].text);
        }

        int drawX = x;
        if (line->align == HTML_ALIGN_CENTER) {
            drawX = x + (int)((maxWidth - line->indent - totalW) / 2);
        } else if (line->align == HTML_ALIGN_RIGHT) {
            drawX = x + (int)(maxWidth - line->indent - totalW);
        }

        // 绘制每个span
        for (int s = 0; s < line->spanCount; s++) {
            Span* sp = &line->spans[s];
            float sx = (float)drawX;
            float sy = (float)y;

            // 加粗模拟：多次偏移绘制
            if (sp->bold) {
                UI_DrawTextDepth(sx, sy, 0.5f, sp->scale, sp->color, sp->text);
                UI_DrawTextDepth(sx + 0.5f, sy, 0.5f, sp->scale, sp->color, sp->text);
            } else {
                UI_DrawTextDepth(sx, sy, 0.5f, sp->scale, sp->color, sp->text);
            }

            // 下划线
            if (sp->underline) {
                float w = UI_MeasureText(sp->scale, sp->text);
                C2D_DrawRectSolid(sx, sy + sp->scale * 24.0f - 2, 0.5f,
                                  w, 1, sp->color);
            }

            drawX += (int)UI_MeasureText(sp->scale, sp->text);
        }

        y += (int)(lineHeight + line->spacing);
    }
}
