// keyboard.h - 自定义软键盘
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "common.h"

// 初始化键盘
void Keyboard_Init(void);

// 打开键盘
// initialText: 初始文本
// maxLen: 最大长度
// isPassword: 是否密码模式
// isMultiline: 是否多行模式（正文输入用）
void Keyboard_Open(const char* initialText, int maxLen, bool isPassword, bool isMultiline);

// 关闭键盘，confirm=是否保存输入
void Keyboard_Close(bool confirm);

// 渲染键盘（下屏）
void Keyboard_Draw(void);

// 处理键盘触摸，返回true表示键盘已关闭
bool Keyboard_HandleTouch(touchPosition* touch);

// 处理物理按键输入（B/Y/X/A/方向键/START/SELECT），返回true表示键盘已关闭
bool Keyboard_HandleInput(u32 kDown, u32 kHeld);

// 当前键盘是否激活
bool Keyboard_IsActive(void);

// 获取键盘输入结果
const char* Keyboard_GetText(void);

#endif
