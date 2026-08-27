// ui.h - UI绘制函数
#ifndef UI_H
#define UI_H

#include "common.h"

// 字体初始化（加载BCFNT中文字体）
void UI_InitFont(void);
void UI_FiniFont(void);

// 图标加载
void UI_LoadIcons(void);
void UI_FreeIcons(void);

// 基础绘制
void UI_DrawRect(float x, float y, float w, float h, u32 color);
void UI_DrawRoundRect(float x, float y, float w, float h, u32 fillColor, u32 borderColor);
void UI_DrawRoundRectR(float x, float y, float w, float h, int r, u32 fillColor, u32 borderColor);
void UI_DrawShadow(float x, float y, float w, float h, int r);
void UI_DrawCircle(float x, float y, float r, u32 color);
void UI_DrawText(float x, float y, float scale, u32 color, const char* text);
void UI_DrawTextDepth(float x, float y, float depth, float scale, u32 color, const char* text);
void UI_DrawTextCenter(float x, float y, float scale, u32 color, const char* text);
void UI_DrawTextCenterDepth(float x, float y, float depth, float scale, u32 color, const char* text);
void UI_DrawTextTruncated(float x, float y, float depth, float scale, u32 color, const char* text, float maxWidth, bool bold);
void UI_DrawTextCenterInRect(float x, float y, float w, float h, float scale, u32 color, const char* text);
void UI_DrawTextCenterInRectDepth(float x, float y, float w, float h, float depth, float scale, u32 color, const char* text);
float UI_MeasureText(float scale, const char* text);
void UI_DrawButton(float x, float y, float w, float h, const char* text, u32 bgColor, u32 borderColor);
void UI_DrawButtonDisabled(float x, float y, float w, float h, const char* text);

// 状态栏
void UI_DrawStatusBarTop(void);
void UI_DrawStatusBarMain(void);

// 内置字体
extern const u8 font8x8[128][8];

#endif
