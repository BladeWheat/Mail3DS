#ifndef HTML_RENDER_H
#define HTML_RENDER_H

#include <3ds.h>

// ========== HTML富文本渲染器 ==========
// 轻量级HTML解析+排版+渲染，状态机+样式栈，不构建DOM树
// 支持标签：br, p, div, h1-h3, hr, b/strong, i/em, u,
//           font color, span style color/background-color,
//           text-align, a, ul/ol/li, img

// 颜色常量
#define HTML_COLOR_BLACK     0xFF212121
#define HTML_COLOR_GRAY      0xFF757575
#define HTML_COLOR_BLUE      0xFF1976D2
#define HTML_COLOR_LINK      0xFF1565C0
#define HTML_COLOR_WHITE     0xFFFFFFFF

// 对齐方式
typedef enum {
    HTML_ALIGN_LEFT = 0,
    HTML_ALIGN_CENTER,
    HTML_ALIGN_RIGHT
} HtmlAlign;

// 初始化/清空
void HtmlRender_Clear(void);

// 解析HTML并排版，返回总行数（供滚动计算）
// maxWidth: 可用宽度像素; baseScale: 基础字号
int HtmlRender_Parse(const char* html, int htmlLen, int maxWidth, float baseScale);

// 渲染可见区域
// startX/startY: 绘制起点; maxHeight: 可见区域高度
// scrollLine: 滚动偏移（行数）; lineHeight: 行高
// outTotalLines: 输出总行数
void HtmlRender_Render(int startX, int startY, int maxWidth, int maxHeight,
                       int scrollLine, float lineHeight, int* outTotalLines);

// 获取总行数
int HtmlRender_GetTotalLines(void);

// 设置当前邮件上下文（保留接口，图片已改为文本占位）
void HtmlRender_SetEmailContext(const char* email, unsigned int uid);

// 释放纹理资源（保留接口，当前为空实现）
void HtmlRender_FreeTextures(void);

#endif
