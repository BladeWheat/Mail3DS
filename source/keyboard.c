// keyboard.c - 自定义软键盘（支持中文拼音输入）
#include "keyboard.h"
#include "ui.h"
#include "ime_pinyin.h"
#include <stdio.h>
#include <string.h>

// ========== 拼音输入法全局 ==========
static PinyinIme* s_pinyin = NULL;
static int s_candidateIndex = 0;  // 当前选中的候选词索引
static int s_candidatePage = 0;   // 候选词翻页
static bool s_multiline = false;  // 多行模式

// ========== 键盘布局常量 ==========
// 多行模式下的Y偏移
#define KB_MULTI_OFFSET  24
// 候选词栏
#define KB_CAND_Y       0
#define KB_CAND_H       32
// 输入显示区
#define KB_INPUT_Y      35
#define KB_INPUT_H      28
#define KB_INPUT_H_MULTI 52  // 多行模式输入区高度
// 三行字母键
#define KB_KEY_W        29
#define KB_KEY_H        30
#define KB_KEY_STEP     31
#define KB_ROW1_Y       68
#define KB_ROW2_Y       102
#define KB_ROW3_Y       136
// 删除键
#define KB_DEL_X        270
#define KB_DEL_W        46
// 底部功能键
#define KB_ACTION_Y     172
#define KB_ACTION_H     35
#define KB_BTN_LANG_X   5
#define KB_BTN_LANG_W   47
#define KB_BTN_SYM_X    56
#define KB_BTN_SYM_W    47
#define KB_BTN_SPACE_X  107
#define KB_BTN_SPACE_W  69
#define KB_BTN_CANCEL_X 180
#define KB_BTN_CANCEL_W 56
#define KB_BTN_OK_X     240
#define KB_BTN_OK_W     75

// 动态获取Y坐标（多行模式下移）
static inline int KB_Y(int baseY)
{
    return s_multiline ? baseY + KB_MULTI_OFFSET : baseY;
}
static inline int KB_INPUT_H_ACTUAL(void)
{
    return s_multiline ? KB_INPUT_H_MULTI : KB_INPUT_H;
}

// ========== 键位定义 ==========
static const char* kb_lower[] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm"
};

static const char* kb_upper[] = {
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM"
};

static const char* kb_symbol[] = {
    "1234567890",
    "-/:;()$&@\"",
    ".,?!'#+_%"
};

static const char* const* GetCurrentLayout(void)
{
    switch (g_app.kbMode)
    {
        case KB_UPPER: return kb_upper;
        case KB_SYMBOL: return kb_symbol;
        case KB_PINYIN: return kb_lower;  // 拼音模式用小写字母
        default: return kb_lower;
    }
}

// ========== 初始化 ==========
void Keyboard_Init(void)
{
    g_app.keyboardActive = false;
    g_app.kbMode = KB_LOWER;
    g_app.kbBuffer[0] = 0;
    g_app.kbCursor = 0;
    g_app.kbMaxLen = 256;
    g_app.kbIsPassword = false;

    // 加载拼音字典
    s_pinyin = ime_create("romfs:/pinyin_dict.bin");
    s_candidateIndex = 0;
    s_candidatePage = 0;
}

void Keyboard_Open(const char* initialText, int maxLen, bool isPassword, bool isMultiline)
{
    g_app.keyboardActive = true;
    g_app.kbMode = KB_LOWER;
    g_app.kbMaxLen = maxLen;
    g_app.kbIsPassword = isPassword;
    s_multiline = isMultiline;

    if (initialText)
    {
        strncpy(g_app.kbBuffer, initialText, maxLen - 1);
        g_app.kbBuffer[maxLen - 1] = 0;
    }
    else
    {
        g_app.kbBuffer[0] = 0;
    }
    g_app.kbCursor = strlen(g_app.kbBuffer);

    // 清空拼音状态
    if (s_pinyin) ime_clear(s_pinyin);
    s_candidateIndex = 0;
    s_candidatePage = 0;
}

void Keyboard_Close(bool confirm)
{
    g_app.keyboardActive = false;
    if (s_pinyin) ime_clear(s_pinyin);
}

bool Keyboard_IsActive(void)
{
    return g_app.keyboardActive;
}

const char* Keyboard_GetText(void)
{
    return g_app.kbBuffer;
}

// ========== 文本操作 ==========
static void InsertUTF8(const char* str)
{
    int len = strlen(g_app.kbBuffer);
    int slen = strlen(str);
    if (len + slen >= g_app.kbMaxLen - 1) return;

    memmove(g_app.kbBuffer + g_app.kbCursor + slen,
            g_app.kbBuffer + g_app.kbCursor,
            len - g_app.kbCursor + 1);
    memcpy(g_app.kbBuffer + g_app.kbCursor, str, slen);
    g_app.kbCursor += slen;
}

static void InsertChar(char c)
{
    char buf[2] = {c, 0};
    InsertUTF8(buf);
}

static void Backspace(void)
{
    if (g_app.kbCursor <= 0) return;
    int len = strlen(g_app.kbBuffer);
    memmove(g_app.kbBuffer + g_app.kbCursor - 1,
            g_app.kbBuffer + g_app.kbCursor,
            len - g_app.kbCursor + 1);
    g_app.kbCursor--;
}

static void ClearAll(void)
{
    g_app.kbBuffer[0] = 0;
    g_app.kbCursor = 0;
}

// ========== 按键绘制 ==========
static void DrawKey(float x, float y, float w, float h, const char* label, bool accent, bool func)
{
    u32 borderColor = accent ? COLOR_BLUE_DARK : (func ? COLOR_BTN_GRAY_DARK : COLOR_BORDER_GRAY);
    u32 fillColor = accent ? COLOR_BLUE : (func ? COLOR_BTN_GRAY : COLOR_KEY_BG);
    u32 textColor = (accent || func) ? COLOR_WHITE : COLOR_TEXT_PRIMARY;

    UI_DrawShadow(x, y, w, h, 5);
    UI_DrawRoundRectR(x, y, w, h, 5, fillColor, borderColor);

    if (label)
    {
        float scale = (strlen(label) <= 1) ? 0.55f : 0.4f;
        UI_DrawTextCenter(x + w / 2, y + h / 2 - 8, scale, textColor, label);
    }
}

// ========== 碰撞检测 ==========
static bool Hit(float x, float y, float w, float h, int px, int py)
{
    return px >= (int)x && px < (int)(x + w) && py >= (int)y && py < (int)(y + h);
}

// ========== 提交候选词 ==========
static void CommitCandidate(int index)
{
    if (!s_pinyin) return;
    const char* hanzi = ime_commit(s_pinyin, index);
    if (hanzi)
    {
        InsertUTF8(hanzi);
    }
    s_candidateIndex = 0;
    s_candidatePage = 0;
}

// ========== 物理按键处理 ==========
bool Keyboard_HandleInput(u32 kDown, u32 kHeld)
{
    if (!g_app.keyboardActive) return false;

    bool pinyinActive = (g_app.kbMode == KB_PINYIN && s_pinyin && ime_active(s_pinyin));

    // B键：拼音模式下先退格拼音，否则退格文本
    if (kHeld & KEY_B)
    {
        if (pinyinActive)
        {
            ime_backspace(s_pinyin);
            s_candidateIndex = 0;
            s_candidatePage = 0;
        }
        else
        {
            Backspace();
        }
    }

    // Y键：切换模式（小写→大写→符号→拼音→小写）
    if (kDown & KEY_Y)
    {
        if (g_app.kbMode == KB_LOWER) g_app.kbMode = KB_UPPER;
        else if (g_app.kbMode == KB_UPPER) g_app.kbMode = KB_SYMBOL;
        else if (g_app.kbMode == KB_SYMBOL) g_app.kbMode = KB_PINYIN;
        else g_app.kbMode = KB_LOWER;
        if (s_pinyin) ime_clear(s_pinyin);
        s_candidateIndex = 0;
        s_candidatePage = 0;
    }

    // X键：清空
    if (kDown & KEY_X)
    {
        if (pinyinActive)
        {
            ime_clear(s_pinyin);
            s_candidateIndex = 0;
            s_candidatePage = 0;
        }
        else
        {
            ClearAll();
        }
    }

    // 左右：拼音模式下选择候选词，否则移动光标
    if (pinyinActive)
    {
        if (kDown & KEY_LEFT)
        {
            if (s_candidateIndex > 0) s_candidateIndex--;
        }
        if (kDown & KEY_RIGHT)
        {
            int count = ime_candidate_count(s_pinyin);
            if (s_candidateIndex < count - 1) s_candidateIndex++;
        }
        // 上下翻页
        if (kDown & KEY_UP)
        {
            // 上翻页（暂不实现复杂翻页，直接跳到开头）
            s_candidateIndex = 0;
        }
        if (kDown & KEY_DOWN)
        {
            int count = ime_candidate_count(s_pinyin);
            if (count > 0) s_candidateIndex = count - 1;
        }
        // A键：提交候选词
        if (kDown & KEY_A)
        {
            CommitCandidate(s_candidateIndex);
        }
    }
    else
    {
        if (kDown & KEY_LEFT)
        {
            if (g_app.kbCursor > 0) g_app.kbCursor--;
        }
        if (kDown & KEY_RIGHT)
        {
            if (g_app.kbCursor < (int)strlen(g_app.kbBuffer)) g_app.kbCursor++;
        }
        // A键：多行模式下换行，单行模式下确定
        if (kDown & KEY_A)
        {
            if (s_multiline)
                InsertChar('\n');
            else
            {
                g_app.keyboardActive = false;
                return true;
            }
        }
    }

    // START：确认关闭
    if (kDown & KEY_START)
    {
        // 如果有未提交的拼音，先提交第一个候选词
        if (pinyinActive)
        {
            CommitCandidate(0);
        }
        g_app.keyboardActive = false;
        return true;
    }

    // SELECT：取消关闭
    if (kDown & KEY_SELECT)
    {
        g_app.keyboardActive = false;
        return true;
    }

    return false;
}

// ========== 触摸处理 ==========
bool Keyboard_HandleTouch(touchPosition* touch)
{
    if (!g_app.keyboardActive) return false;

    int tx = touch->px;
    int ty = touch->py;

    bool pinyinActive = (g_app.kbMode == KB_PINYIN && s_pinyin && ime_active(s_pinyin));

    // 候选词栏点击（拼音模式下）
    if (pinyinActive && ty >= KB_CAND_Y && ty < KB_CAND_Y + KB_CAND_H)
    {
        int count = ime_candidate_count(s_pinyin);
        // 每个候选词宽度约40px，从x=8开始
        for (int i = 0; i < count && i < 7; i++)
        {
            int cx = 8 + i * 44;
            if (tx >= cx && tx < cx + 40)
            {
                CommitCandidate(i);
                return false;
            }
        }
        return false;
    }

    const char* const* layout = GetCurrentLayout();
    bool isSymbolHit = (g_app.kbMode == KB_SYMBOL);
    float rowYs[] = {KB_Y(KB_ROW1_Y), KB_Y(KB_ROW2_Y), KB_Y(KB_ROW3_Y)};
    float rowXs[] = {4, isSymbolHit ? 4.0f : 19.0f, isSymbolHit ? 0.0f : 50.0f};
    float rowSteps[] = {KB_KEY_STEP, KB_KEY_STEP, isSymbolHit ? 30.0f : (float)KB_KEY_STEP};

    for (int row = 0; row < 3; row++)
    {
        const char* keys = layout[row];
        int count = strlen(keys);
        for (int col = 0; col < count; col++)
        {
            float x = rowXs[row] + col * rowSteps[row];
            if (Hit(x, rowYs[row], KB_KEY_W, KB_KEY_H, tx, ty))
            {
                char c = keys[col];
                if (g_app.kbMode == KB_PINYIN && s_pinyin)
                {
                    // 拼音模式：输入拼音字母
                    ime_input(s_pinyin, c);
                    s_candidateIndex = 0;
                    s_candidatePage = 0;
                }
                else
                {
                    InsertChar(c);
                }
                return false;
            }
        }
    }

    // 删除键
    if (Hit(KB_DEL_X, KB_Y(KB_ROW3_Y), KB_DEL_W, KB_KEY_H, tx, ty))
    {
        if (pinyinActive)
        {
            ime_backspace(s_pinyin);
            s_candidateIndex = 0;
        }
        else
        {
            Backspace();
        }
        return false;
    }

    // 大小写/中英文切换键
    if (Hit(KB_BTN_LANG_X, KB_Y(KB_ACTION_Y), KB_BTN_LANG_W, KB_ACTION_H, tx, ty))
    {
        if (g_app.kbMode == KB_LOWER) g_app.kbMode = KB_UPPER;
        else if (g_app.kbMode == KB_UPPER) g_app.kbMode = KB_PINYIN;
        else if (g_app.kbMode == KB_PINYIN) g_app.kbMode = KB_LOWER;
        else g_app.kbMode = KB_LOWER;
        if (s_pinyin) ime_clear(s_pinyin);
        s_candidateIndex = 0;
        return false;
    }

    // 符号切换
    if (Hit(KB_BTN_SYM_X, KB_Y(KB_ACTION_Y), KB_BTN_SYM_W, KB_ACTION_H, tx, ty))
    {
        g_app.kbMode = (g_app.kbMode == KB_SYMBOL) ? KB_LOWER : KB_SYMBOL;
        if (s_pinyin) ime_clear(s_pinyin);
        s_candidateIndex = 0;
        return false;
    }

    // 空格
    if (Hit(KB_BTN_SPACE_X, KB_Y(KB_ACTION_Y), KB_BTN_SPACE_W, KB_ACTION_H, tx, ty))
    {
        if (pinyinActive)
        {
            // 空格提交第一个候选词
            CommitCandidate(0);
        }
        else
        {
            InsertChar(' ');
        }
        return false;
    }

    // 取消
    if (Hit(KB_BTN_CANCEL_X, KB_Y(KB_ACTION_Y), KB_BTN_CANCEL_W, KB_ACTION_H, tx, ty))
    {
        g_app.keyboardActive = false;
        return true;
    }

    // 确定
    if (Hit(KB_BTN_OK_X, KB_Y(KB_ACTION_Y), KB_BTN_OK_W, KB_ACTION_H, tx, ty))
    {
        if (pinyinActive)
        {
            CommitCandidate(0);
        }
        g_app.keyboardActive = false;
        return true;
    }

    return false;
}

// ========== 绘制候选词栏 ==========
static void DrawCandidates(void)
{
    if (!s_pinyin || !ime_active(s_pinyin)) return;

    // 背景
    UI_DrawRect(0, KB_CAND_Y, 320, KB_CAND_H, C2D_Color32(0xF5, 0xEE, 0xD8, 0xFF));
    UI_DrawRect(0, KB_CAND_Y + KB_CAND_H - 1, 320, 1, COLOR_BORDER_GRAY);

    // 显示当前拼音
    const char* py = ime_buffer(s_pinyin);
    UI_DrawText(8, 8, 0.45f, COLOR_BLUE, py);

    // 显示候选词
    int count = ime_candidate_count(s_pinyin);
    int startX = 8 + (int)strlen(py) * 8 + 8;
    for (int i = 0; i < count && i < 7; i++)
    {
        const char* cand = ime_candidate(s_pinyin, i);
        if (!cand) break;
        int cx = startX + i * 40;
        if (cx > 300) break;

        // 选中高亮
        if (i == s_candidateIndex)
        {
            UI_DrawRoundRectR(cx - 2, 4, 36, 24, 4, COLOR_SELECTED_BG, COLOR_BLUE);
        }

        // 数字标号
        char num[4];
        snprintf(num, sizeof(num), "%d.", i + 1);
        UI_DrawText(cx, 8, 0.35f, COLOR_TEXT_SECONDARY, num);
        // 候选词
        UI_DrawText(cx + 12, 6, 0.5f, COLOR_TEXT_PRIMARY, cand);
    }
}

// ========== 绘制 ==========
// 绘制多行文本到输入区（按\n分行，自动换行）
static void DrawMultilineInput(void)
{
    int inputH = KB_INPUT_H_ACTUAL();
    int lineH = 16; // 每行高度
    int maxLines = (inputH - 6) / lineH;
    if (maxLines < 1) maxLines = 1;

    // 找到光标所在行
    int lineCount = 1;
    int cursorLine = 0;
    int lineStart[64];
    lineStart[0] = 0;

    for (int i = 0; g_app.kbBuffer[i] && lineCount < 64; i++)
    {
        if (g_app.kbBuffer[i] == '\n')
        {
            lineStart[lineCount] = i + 1;
            if (i < g_app.kbCursor) cursorLine = lineCount;
            lineCount++;
        }
    }

    // 计算滚动偏移（让光标所在行可见）
    int scrollLine = 0;
    if (cursorLine >= maxLines)
        scrollLine = cursorLine - maxLines + 1;

    // 绘制可见行
    int drawY = KB_INPUT_Y + 4;
    for (int i = scrollLine; i < lineCount && i < scrollLine + maxLines; i++)
    {
        int start = lineStart[i];
        int end;
        if (i < lineCount - 1)
        {
            end = lineStart[i + 1] - 1; // -1 to skip \n
        }
        else
        {
            end = strlen(g_app.kbBuffer);
        }

        int len = end - start;
        if (len > 38) len = 38; // 每行最多38字符

        char lineBuf[40];
        if (len > 0)
        {
            strncpy(lineBuf, g_app.kbBuffer + start, len);
            lineBuf[len] = 0;
        }
        else
        {
            lineBuf[0] = 0;
        }

        UI_DrawText(10, drawY, 0.45f, COLOR_TEXT_PRIMARY, lineBuf);

        // 如果光标在这一行，绘制光标
        if (i == cursorLine)
        {
            int cursorPos = g_app.kbCursor - start;
            if (cursorPos > len) cursorPos = len;
            UI_DrawRect(10 + cursorPos * 7, drawY - 1, 1, 14, COLOR_BLUE);
        }

        drawY += lineH;
    }
}

void Keyboard_Draw(void)
{
    if (!g_app.keyboardActive) return;

    // 背景
    C2D_TargetClear(botTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(botTarget);

    int inputH = KB_INPUT_H_ACTUAL();

    // 候选词栏（拼音模式下）
    if (g_app.kbMode == KB_PINYIN)
    {
        DrawCandidates();
    }
    else
    {
        // 普通模式顶部标题栏
        UI_DrawRect(0, 0, 320, 32, C2D_Color32(0xF5, 0xEE, 0xD8, 0xFF));
        UI_DrawRect(0, 31, 320, 1, COLOR_BORDER_GRAY);

        const char* title;
        if (g_app.kbIsPassword) title = "密码";
        else if (s_multiline) title = "正文";
        else title = "输入";
        UI_DrawText(8, 8, 0.5f, COLOR_TEXT_SECONDARY, title);

        char countBuf[24];
        int len = strlen(g_app.kbBuffer);
        snprintf(countBuf, sizeof(countBuf), "%d/%d", len, g_app.kbMaxLen - 1);
        UI_DrawText(260, 8, 0.45f, COLOR_TEXT_SECONDARY, countBuf);
    }

    // 输入显示区
    UI_DrawRoundRectR(4, KB_INPUT_Y, 312, inputH, 5, COLOR_WHITE, COLOR_BORDER_GRAY);

    if (s_multiline)
    {
        // 多行模式
        DrawMultilineInput();
    }
    else if (g_app.kbIsPassword)
    {
        int maxChars = 38;
        int showLen = strlen(g_app.kbBuffer);
        if (showLen > maxChars) showLen = maxChars;
        char stars[64];
        for (int i = 0; i < showLen; i++) stars[i] = '*';
        stars[showLen] = 0;
        UI_DrawText(10, KB_INPUT_Y + 7, 0.5f, COLOR_TEXT_PRIMARY, stars);
    }
    else
    {
        int maxChars = 38;
        int len = strlen(g_app.kbBuffer);
        int start = 0;
        if (len > maxChars)
        {
            if (g_app.kbCursor > maxChars - 5)
                start = g_app.kbCursor - (maxChars - 5);
        }
        int visibleLen = len - start;
        if (visibleLen > maxChars) visibleLen = maxChars;

        char display[64];
        strncpy(display, g_app.kbBuffer + start, visibleLen);
        display[visibleLen] = 0;
        UI_DrawText(10, KB_INPUT_Y + 7, 0.5f, COLOR_TEXT_PRIMARY, display);

        int drawCursor = g_app.kbCursor - start;
        if (drawCursor < 0) drawCursor = 0;
        if (drawCursor > visibleLen) drawCursor = visibleLen;
        UI_DrawRect(10 + drawCursor * 8, KB_INPUT_Y + 5, 1, 18, COLOR_BLUE);
    }

    // 三行按键（使用动态Y）
    const char* const* layout = GetCurrentLayout();
    bool isSymbol = (g_app.kbMode == KB_SYMBOL);
    // 符号模式：三行都不缩进，第三行9键紧凑排列（步长30）给删除键留位置
    float rowXs[] = {4, isSymbol ? 4.0f : 19.0f, isSymbol ? 0.0f : 50.0f};
    float rowSteps[] = {KB_KEY_STEP, KB_KEY_STEP, isSymbol ? 30.0f : (float)KB_KEY_STEP};
    float rowYs[] = {KB_Y(KB_ROW1_Y), KB_Y(KB_ROW2_Y), KB_Y(KB_ROW3_Y)};

    for (int row = 0; row < 3; row++)
    {
        const char* keys = layout[row];
        int count = strlen(keys);
        for (int col = 0; col < count; col++)
        {
            float x = rowXs[row] + col * rowSteps[row];
            char buf[2] = {keys[col], 0};
            DrawKey(x, rowYs[row], KB_KEY_W, KB_KEY_H, buf, false, false);
        }
    }

    // 删除键
    DrawKey(KB_DEL_X, KB_Y(KB_ROW3_Y), KB_DEL_W, KB_KEY_H, "删除", false, true);

    // 底部功能键
    const char* langLabel;
    if (g_app.kbMode == KB_LOWER) langLabel = "abc";
    else if (g_app.kbMode == KB_UPPER) langLabel = "ABC";
    else if (g_app.kbMode == KB_PINYIN) langLabel = "中文";
    else langLabel = "abc";

    int actionY = KB_Y(KB_ACTION_Y);
    DrawKey(KB_BTN_LANG_X, actionY, KB_BTN_LANG_W, KB_ACTION_H, langLabel,
            g_app.kbMode == KB_PINYIN, true);
    DrawKey(KB_BTN_SYM_X, actionY, KB_BTN_SYM_W, KB_ACTION_H,
            g_app.kbMode == KB_SYMBOL ? "abc" : "123", false, true);
    DrawKey(KB_BTN_SPACE_X, actionY, KB_BTN_SPACE_W, KB_ACTION_H, "空格", false, true);
    DrawKey(KB_BTN_CANCEL_X, actionY, KB_BTN_CANCEL_W, KB_ACTION_H, "取消", false, true);
    DrawKey(KB_BTN_OK_X, actionY, KB_BTN_OK_W, KB_ACTION_H, "确定", true, false);

    // 底部提示
    const char* hint;
    if (s_multiline)
        hint = "Y切换 X清空 B退格 A换行 START确定";
    else if (g_app.kbMode == KB_PINYIN)
        hint = "Y切换 X清空 左右选词 A确认 空格选词";
    else
        hint = "Y切换 X清空 B退格 START确定";
    UI_DrawText(8, actionY + KB_ACTION_H + 3, 0.35f, COLOR_TEXT_SECONDARY, hint);
}
