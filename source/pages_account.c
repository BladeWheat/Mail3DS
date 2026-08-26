// pages_account.c - 账户列表和新增账户页
#include "pages.h"
#include "ui.h"
#include "keyboard.h"
#include "accounts.h"
#include "email.h"
#include "network.h"
#include "cache.h"
#include "html_render.h"
#include <math.h>
#include <strings.h>

// 标题图标精灵
static C2D_SpriteSheet s_titleSheet = NULL;
static C2D_Image s_titleIcon;

// 密码显示/隐藏图标
static C2D_SpriteSheet s_eyeOnSheet = NULL;
static C2D_SpriteSheet s_eyeOffSheet = NULL;
static C2D_Image s_eyeOnIcon;
static C2D_Image s_eyeOffIcon;
static bool s_showPassword = false;

static void LoadTitleIcon(void)
{
    if (!s_titleSheet)
    {
        s_titleSheet = C2D_SpriteSheetLoad("romfs:/title_icon.t3x");
        if (s_titleSheet)
            s_titleIcon = C2D_SpriteSheetGetImage(s_titleSheet, 0);
    }
}

static void LoadEyeIcons(void)
{
    if (!s_eyeOnSheet)
    {
        s_eyeOnSheet = C2D_SpriteSheetLoad("romfs:/eye_on.t3x");
        if (s_eyeOnSheet)
            s_eyeOnIcon = C2D_SpriteSheetGetImage(s_eyeOnSheet, 0);
    }
    if (!s_eyeOffSheet)
    {
        s_eyeOffSheet = C2D_SpriteSheetLoad("romfs:/eye_off.t3x");
        if (s_eyeOffSheet)
            s_eyeOffIcon = C2D_SpriteSheetGetImage(s_eyeOffSheet, 0);
    }
}

// ========== 配色（严格按HTML） ==========
// 上屏/下屏背景
#define COLOR_SCREEN_BG     C2D_Color32(0xF6, 0xF1, 0xC0, 0xFF)  // #f6f1c0
// 标题
#define COLOR_TITLE_BLUE    C2D_Color32(0x54, 0x82, 0x99, 0xFF)  // #548299
// 蓝色账户按钮
#define COLOR_ACC_BTN_BG    C2D_Color32(0x48, 0x84, 0xBB, 0xFF)  // #4884bb
#define COLOR_ACC_BTN_BD    C2D_Color32(0x2B, 0x54, 0x7C, 0xFF)  // #2b547c
// 添加账户大按钮
#define COLOR_BIG_BTN_BG    C2D_Color32(0xF0, 0xEC, 0xE0, 0xFF)  // #f0ece0
#define COLOR_BIG_BTN_BD    C2D_Color32(0xD3, 0xCD, 0xBC, 0xFF)  // #d3cdbc
#define COLOR_BIG_BTN_TX    C2D_Color32(0x55, 0x55, 0x55, 0xFF)  // #555555
#define COLOR_BIG_BTN_DOWN  C2D_Color32(0xDD, 0xD9, 0xCE, 0xFF)  // :active #ddd9ce
// 空槽位
#define COLOR_EMPTY_BG      C2D_Color32(0xE9, 0xE4, 0xB9, 0xFF)  // #e9e4b9
#define COLOR_EMPTY_BD      C2D_Color32(0xC2, 0xBC, 0x96, 0xFF)  // #c2bc96（虚线）
#define COLOR_EMPTY_TEXT    C2D_Color32(0xB0, 0xAA, 0x84, 0xFF)  // #b0aa84
// 状态文字
#define COLOR_STATUS_TEXT   C2D_Color32(0x44, 0x44, 0x44, 0xFF)  // #444444
// 分割线
#define COLOR_DIVIDER       C2D_Color32(0xC9, 0xC3, 0xB0, 0xFF)  // #c9c3b0
#define COLOR_ITEM_LINE     C2D_Color32(0xD8, 0xD8, 0xD8, 0xFF)  // #d8d8d8
// WiFi灰色条
#define COLOR_WIFI_BAR_BG   C2D_Color32(0xDD, 0xDD, 0xDD, 0xFF)  // #dddddd

static int s_deleteConfirm = -1;
static int s_listFocus = 0;  // 账户列表焦点：0=添加按钮，1~4=对应账户删除按钮
static int s_pressedListBtn = -1;  // 触摸按下的按钮（-1=无）
static int s_lastDownX = 0, s_lastDownY = 0;  // 最后按下位置
static bool s_hasDown = false;  // 是否有有效按下记录
static int s_delFocus = 0;   // 删除确认焦点：0=取消，1=确认删除
static int s_addFocus = 0;   // 添加账户页焦点：0=邮箱，1=密码，2=测试连接，3=保存账户
static int s_pressedAddBtn = -1;  // 触摸按下的底部按钮
static char s_addError[64] = "";  // 添加账户错误提示

// 连接测试状态
static bool s_testing = false;
static ConnectTestResult s_testResult;
static Thread s_testThread = NULL;

// 连接测试线程参数
struct TestThreadArg {
    char imapServer[128];
    int imapPort;
    char smtpServer[128];
    int smtpPort;
    char email[128];
    char password[128];
};

static void test_connection_thread(void* arg)
{
    struct TestThreadArg* ta = (struct TestThreadArg*)arg;
    Network_TestConnection(ta->imapServer, ta->imapPort,
                           ta->smtpServer, ta->smtpPort,
                           ta->email, ta->password, &s_testResult);
    // 清除敏感数据
    memset(ta->password, 0, sizeof(ta->password));
    free(ta);
}

// 邮箱格式验证：返回true表示格式合规
static bool IsValidEmail(const char* email)
{
    if (!email || !*email) return false;
    // 找@
    const char* at = strchr(email, '@');
    if (!at || at == email) return false;  // 没有@或@在开头
    if (strchr(at + 1, '@')) return false;  // 多个@
    // @后面必须有.
    const char* dot = strchr(at + 1, '.');
    if (!dot || dot == at + 1) return false;  // 没有.或.紧跟@
    // .后面必须有字符
    if (!*(dot + 1)) return false;
    // 不允许空格
    for (const char* p = email; *p; p++)
        if (*p == ' ' || *p == '\t') return false;
    return true;
}

// 检查邮箱是否已被添加
static bool IsEmailDuplicate(const char* email)
{
    for (int i = 0; i < MAX_ACCOUNTS; i++)
    {
        if (g_app.accounts[i].added && strcasecmp(g_app.accounts[i].email, email) == 0)
            return true;
    }
    return false;
}

// ========== 辅助函数 ==========
// 2px粗虚线圆角边框
static void DrawDashedBorder2px(float x, float y, float w, float h, int r, u32 color)
{
    // 横边：6空5实（先空6px再实5px循环）
    float hStart = x + r, hEnd = x + w - r;
    float pos = hStart;
    bool hGap = true;
    while (pos < hEnd)
    {
        float seg = hGap ? 6 : 5;
        if (pos + seg > hEnd) seg = hEnd - pos;
        if (!hGap)
        {
            C2D_DrawRectSolid(pos, y, 0, seg, 2, color);
            C2D_DrawRectSolid(pos, y + h - 2, 0, seg, 2, color);
        }
        pos += seg;
        hGap = !hGap;
    }
    // 竖边：7实8空（先实7px再空8px循环）
    float vStart = y + r, vEnd = y + h - r;
    pos = vStart;
    bool vGap = false;
    while (pos < vEnd)
    {
        float seg = vGap ? 8 : 7;
        if (pos + seg > vEnd) seg = vEnd - pos;
        if (!vGap)
        {
            C2D_DrawRectSolid(x, pos, 0, 2, seg, color);
            C2D_DrawRectSolid(x + w - 2, pos, 0, 2, seg, color);
        }
        pos += seg;
        vGap = !vGap;
    }
    // 四角（圆角处也画虚线，2px实3px空）
    for (int dy = 0; dy < r; dy++)
    {
        int dx = (int)sqrtf((float)(r*r - (r-1-dy)*(r-1-dy)));
        // 在圆弧上每隔5px画2px实段
        if ((dy % 5) < 2)
        {
            for (int t = 0; t < 2; t++)
            {
                C2D_DrawRectSolid(x + r - dx, y + dy + t, 0, 2, 1, color);
                C2D_DrawRectSolid(x + w - r + dx - 2, y + dy + t, 0, 2, 1, color);
                C2D_DrawRectSolid(x + r - dx, y + h - 2 - dy - t, 0, 2, 1, color);
                C2D_DrawRectSolid(x + w - r + dx - 2, y + h - 2 - dy - t, 0, 2, 1, color);
            }
        }
    }
}

// 2px边框圆角按钮（外层border色，内层fill色内缩2px）
static void DrawBtn2px(float x, float y, float w, float h, int r, u32 fill, u32 border)
{
    UI_DrawRoundRectR(x, y, w, h, r, border, border);
    int ir = r - 2; if (ir < 1) ir = 1;
    UI_DrawRoundRectR(x + 2, y + 2, w - 4, h - 4, ir, fill, fill);
}

// ========== 账户列表页（严格按HTML） ==========
void Page_AccountListDraw(void)
{
    LoadTitleIcon();

    // ===== 上屏 400×240 =====
    C2D_TargetClear(topTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(topTarget);

    // 统一状态栏 34px
    UI_DrawStatusBarTop();

    // 标题行：y=38，图标22px + gap6px + 文字22px粗体，居中
    float iconSize = 22;
    float gap = 6;
    float titleScale = 0.7f;
    float titleTextW = UI_MeasureText(titleScale, "电子邮件账户设置");
    float titleTotalW = iconSize + gap + titleTextW;
    float titleX = (SCREEN_TOP_W - titleTotalW) / 2;
    // 用户提供的标题图标 22×22
    if (s_titleSheet && s_titleIcon.subtex)
        C2D_DrawImageAt(s_titleIcon, titleX, 40, 0.5f, NULL, 22.0f/48, 22.0f/48);
    // 粗体文字 y=40
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++)
            UI_DrawText(titleX + iconSize + gap + dx, 40 + dy, titleScale, COLOR_TITLE_BLUE, "电子邮件账户设置");

    // 4个账户项：按钮96×36，Y=74/112/150/188，X=12
    float itemY[4] = {74, 112, 150, 188};
    float btnW = 96, btnH = 36;
    for (int i = 0; i < MAX_ACCOUNTS; i++)
    {
        float y = itemY[i];

        // 蓝色按钮 82×36 radius4px 2px边框
        UI_DrawShadow(12, y, btnW, btnH, 4);
        DrawBtn2px(12, y, btnW, btnH, 4, COLOR_ACC_BTN_BG, COLOR_ACC_BTN_BD);
        char label[16];
        snprintf(label, sizeof(label), "账户%d", i + 1);
        UI_DrawTextCenterInRect(12, y, btnW, btnH, 0.65f, COLOR_WHITE, label);

        // 状态文字 X=116 20px 左对齐垂直居中
        float statusX = 116;
        float textBaseY = y + btnH/2 - 7; // 20px字height≈20, *0.35≈7
        if (g_app.accounts[i].added)
        {
            char emailShort[40];
            strncpy(emailShort, g_app.accounts[i].email, 30);
            emailShort[30] = 0;
            if (strlen(g_app.accounts[i].email) > 28)
            { emailShort[27]='.'; emailShort[28]='.'; emailShort[29]='.'; emailShort[30]=0; }
            UI_DrawText(statusX, textBaseY, 0.65f, COLOR_STATUS_TEXT, emailShort);
        }
        else
        {
            UI_DrawText(statusX, textBaseY, 0.65f, COLOR_STATUS_TEXT, "未添加");
        }

        // 底边线 1px #d8d8d8，最后一项不画
        if (i < MAX_ACCOUNTS - 1)
            UI_DrawRect(12, y + btnH, SCREEN_TOP_W - 24, 1, COLOR_ITEM_LINE);
    }

    // ===== 下屏 320×240 =====
    C2D_TargetClear(botTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(botTarget);

    if (s_deleteConfirm >= 0)
    {
        // 删除确认
        char msg[64];
        snprintf(msg, sizeof(msg), "确定删除账户%d？", s_deleteConfirm + 1);
        UI_DrawTextCenter(SCREEN_BOT_W / 2, 30, 0.5f, COLOR_RED, msg);
        UI_DrawTextCenter(SCREEN_BOT_W / 2, 52, 0.4f, COLOR_TEXT_SECONDARY, "删除后该账户邮件将不再显示");
        // 取消按钮（焦点高亮）
        u32 cancelBg = (s_delFocus == 0) ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY;
        UI_DrawButton(30, 80, 120, 40, "取消", cancelBg, COLOR_BTN_GRAY_DARK);
        if (s_delFocus == 0)
            UI_DrawRoundRectR(28, 78, 124, 44, 6, 0, COLOR_BLUE);
        // 确认删除按钮（焦点高亮）
        u32 confirmBg = (s_delFocus == 1) ? C2D_Color32(0xB7,0x1C,0x1C,0xFF) : COLOR_RED;
        UI_DrawButton(170, 80, 120, 40, "确认删除", confirmBg, C2D_Color32(0xB7,0x1C,0x1C,0xFF));
        if (s_delFocus == 1)
            UI_DrawRoundRectR(168, 78, 124, 44, 6, 0, COLOR_BLUE);
        int x = 20 + s_deleteConfirm * 70;
        int y = 140;
        UI_DrawShadow(x, y, 60, 60, 6);
        UI_DrawRoundRectR(x, y, 60, 60, 6, COLOR_RED, C2D_Color32(0xB7,0x1C,0x1C,0xFF));
        char num[16];
        snprintf(num, sizeof(num), "%d", s_deleteConfirm + 1);
        UI_DrawTextCenter(x + 30, y + 18, 0.7f, COLOR_WHITE, num);
        UI_DrawTextCenter(x + 30, y + 42, 0.35f, C2D_Color32(0xFF,0xCD,0xD2,0xFF), "删除");
    }
    else
    {
        // 顶部大按钮"添加账户" x=12,y=16,w=296,h=52,radius=12 2px边框
        float addX = 12, addY = 16, addW = 296, addH = 52;
        bool canAdd = (Accounts_GetCount() < MAX_ACCOUNTS);
        if (canAdd)
        {
            UI_DrawShadow(addX, addY, addW, addH, 12);
            u32 addBg = (s_pressedListBtn == 0 || (g_app.focusActive && s_listFocus == 0)) ? COLOR_BIG_BTN_DOWN : COLOR_BIG_BTN_BG;
            DrawBtn2px(addX, addY, addW, addH, 12, addBg, COLOR_BIG_BTN_BD);
            UI_DrawTextCenterInRect(addX, addY, addW, addH, 0.8f, COLOR_BIG_BTN_TX, "添加账户");
        }
        else
        {
            DrawBtn2px(addX, addY, addW, addH, 12, COLOR_BIG_BTN_DOWN, COLOR_BIG_BTN_BD);
            UI_DrawTextCenterInRect(addX, addY, addW, addH, 0.8f, COLOR_TEXT_SECONDARY, "添加账户");
        }

        // 分割线 y=84 h=1 #c9c3b0
        UI_DrawRect(12, 84, 296, 1, COLOR_DIVIDER);

        // 4个虚线框 x=12/88/164/240 y=100 w=68 h=88 gap=8 radius=10
        float slotY = 100, slotW = 68, slotH = 88, gap = 8;
        for (int i = 0; i < MAX_ACCOUNTS; i++)
        {
            float x = 12 + i * (slotW + gap);
            if (g_app.accounts[i].added)
            {
                // 已添加：蓝色实心框2px边框 + 白色文字 + 红色删除按钮
                UI_DrawShadow(x, slotY, slotW, slotH, 10);
                DrawBtn2px(x, slotY, slotW, slotH, 10, COLOR_ACC_BTN_BG, COLOR_ACC_BTN_BD);
                UI_DrawTextCenterInRect(x, slotY + 16, slotW, 28, 0.65f, C2D_Color32(0xBB,0xDE,0xFB,0xFF), "账户");
                char num[8];
                snprintf(num, sizeof(num), "%d", i + 1);
                UI_DrawTextCenterInRect(x, slotY + 44, slotW, 32, 0.9f, COLOR_WHITE, num);
                // 右上角红色删除按钮 20×20
                UI_DrawShadow(x + slotW - 16, slotY - 6, 20, 20, 5);
                UI_DrawRoundRectR(x + slotW - 16, slotY - 6, 20, 20, 5, COLOR_RED, C2D_Color32(0xB7,0x1C,0x1C,0xFF));
                UI_DrawTextCenterInRect(x + slotW - 16, slotY - 6, 20, 20, 0.45f, COLOR_WHITE, "×");
                // 焦点高亮
                if (g_app.focusActive && s_listFocus == i + 1)
                    UI_DrawRoundRectR(x + slotW - 18, slotY - 8, 24, 24, 7, 0, COLOR_BLUE);
            }
            else
            {
                // 未添加：虚线空框2px虚线
                UI_DrawRoundRectR(x, slotY, slotW, slotH, 10, COLOR_EMPTY_BG, COLOR_EMPTY_BG);
                DrawDashedBorder2px(x, slotY, slotW, slotH, 10, COLOR_EMPTY_BD);
                UI_DrawTextCenterInRect(x, slotY + 16, slotW, 28, 0.65f, COLOR_EMPTY_TEXT, "账户");
                char num[8];
                snprintf(num, sizeof(num), "%d", i + 1);
                UI_DrawTextCenterInRect(x, slotY + 44, slotW, 32, 0.9f, COLOR_EMPTY_TEXT, num);
            }
        }

        // 左下角"返回"按钮：120x30, x=0,y=210, 右上角圆角30
        {
            float bx = 0, by = 210, bw = 120, bh = 30, br = 30;
            // 检查是否有已添加账户
            int addedCount = 0;
            for (int i = 0; i < MAX_ACCOUNTS; i++)
                if (g_app.accounts[i].added) addedCount++;
            bool backEnabled = (addedCount > 0);
            bool backFocused = backEnabled && (s_pressedListBtn == 5 || (g_app.focusActive && s_listFocus == 5));

            // 有账户：原来的深灰样式；无账户：浅灰禁用样式
            u32 backBg, backBorder, textColor;
            u32 borderHalf;
            if (!backEnabled)
            {
                backBg = C2D_Color32(0xD8,0xD8,0xD8,0xFF);
                backBorder = C2D_Color32(0xA0,0xA0,0xA0,0xFF);
                textColor = C2D_Color32(0xA0,0xA0,0xA0,0xFF);
                borderHalf = C2D_Color32(0xA0,0xA0,0xA0,0x80);
            }
            else
            {
                backBg = backFocused ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY;
                backBorder = COLOR_BTN_GRAY_DARK;
                textColor = COLOR_WHITE;
                borderHalf = C2D_Color32(0x42,0x42,0x42,0x80);
            }

            float cx = bx + bw - br;
            float cy = by + br;
            for (int py = (int)by; py < by + bh; py++)
            {
                float rightEdge;
                if (py < cy)
                {
                    float dy = cy - py;
                    rightEdge = cx + sqrtf(br * br - dy * dy);
                }
                else
                {
                    rightEdge = bx + bw;
                }
                C2D_DrawRectSolid(bx, py, 0, rightEdge - bx, 1, backBg);
            }

            C2D_DrawRectSolid(bx, by, 0, cx - bx, 2, backBorder);
            C2D_DrawRectSolid(bx, by + 2, 0, cx - bx, 1, borderHalf);

            for (float a = 270.0f; a <= 360.0f; a += 0.5f)
            {
                float rad = a * 3.14159265f / 180.0f;
                for (int t = 0; t <= 1; t++)
                {
                    float r = br - 1.5f + t;
                    float px = cx + r * cosf(rad);
                    float py = cy + r * sinf(rad);
                    C2D_DrawRectSolid(px - 0.5f, py - 0.5f, 0, 1, 1, backBorder);
                }
            }

            C2D_DrawRectSolid(bx + bw - 2, cy, 0, 2, by + bh - cy, backBorder);
            C2D_DrawRectSolid(bx + bw - 3, cy, 0, 1, by + bh - cy, borderHalf);

            UI_DrawTextCenterInRect(bx - 5, by, bw, bh, 0.5f, textColor, "返回");
        }

        // 右下角"设置"按钮：120x30, x=200,y=210, 左上角圆角30（与返回按钮左右对称）
        {
            float bx = 200, by = 210, bw = 120, bh = 30, br = 30;
            bool setFocused = (s_pressedListBtn == 6 || (g_app.focusActive && s_listFocus == 6));
            u32 setBg = setFocused ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY;
            u32 setBorder = COLOR_BTN_GRAY_DARK;
            u32 borderHalf = C2D_Color32(0x42,0x42,0x42,0x80);

            // 左上角圆角圆心
            float cx = bx + br;
            float cy = by + br;
            for (int py = (int)by; py < by + bh; py++)
            {
                float leftEdge;
                if (py < cy)
                {
                    float dy = cy - py;
                    leftEdge = cx - sqrtf(br * br - dy * dy);
                }
                else
                {
                    leftEdge = bx;
                }
                C2D_DrawRectSolid(leftEdge, py, 0, bx + bw - leftEdge, 1, setBg);
            }

            // 上边框（2.5px）
            C2D_DrawRectSolid(cx, by, 0, bx + bw - cx, 2, setBorder);
            C2D_DrawRectSolid(cx, by + 2, 0, bx + bw - cx, 1, borderHalf);

            // 圆角处边框（左上角，角度180~270）
            for (float a = 180.0f; a <= 270.0f; a += 0.5f)
            {
                float rad = a * 3.14159265f / 180.0f;
                for (int t = 0; t <= 1; t++)
                {
                    float r = br - 1.5f + t;
                    float px = cx + r * cosf(rad);
                    float py = cy + r * sinf(rad);
                    C2D_DrawRectSolid(px - 0.5f, py - 0.5f, 0, 1, 1, setBorder);
                }
            }

            // 左边框直线
            C2D_DrawRectSolid(bx, cy, 0, 2, by + bh - cy, setBorder);
            C2D_DrawRectSolid(bx + 1, cy, 0, 1, by + bh - cy, borderHalf);

            UI_DrawTextCenterInRect(bx + 5, by, bw, bh, 0.5f, COLOR_WHITE, "设置");
        }
    }
}

// ========== 新增账户页 ==========
static void DrawInputField(float x, float y, float w, float h, const char* label, const char* value, bool focused, bool isPassword)
{
    UI_DrawText(25, y - 18, 0.45f, COLOR_TEXT_SECONDARY, label);
    u32 bg = focused ? COLOR_INPUT_FOCUS : COLOR_WHITE;
    u32 border = focused ? COLOR_BLUE : COLOR_BORDER_GRAY;
    UI_DrawRoundRect(x, y, w, h, bg, border);

    if (strlen(value) > 0)
    {
        if (isPassword)
        {
            char stars[128];
            int len = strlen(value);
            for (int i = 0; i < len && i < 30; i++) stars[i] = '*';
            stars[len < 30 ? len : 30] = 0;
            UI_DrawText(x + 8, y + 9, 0.5f, COLOR_TEXT_PRIMARY, stars);
        }
        else
        {
            UI_DrawText(x + 8, y + 9, 0.5f, COLOR_TEXT_PRIMARY, value);
        }
    }
    else if (!focused)
    {
        UI_DrawText(x + 8, y + 9, 0.45f, C2D_Color32(0xBD,0xBD,0xBD,0xFF), label);
    }
}

void Page_AccountListUpdate(touchPosition* touch, u32 kDown)
{
    if (s_deleteConfirm >= 0)
    {
        if (kDown & KEY_DLEFT || kDown & KEY_DRIGHT)
            s_delFocus = 1 - s_delFocus;
        if (kDown & KEY_A)
        {
            if (s_delFocus == 1)
                Accounts_Remove(s_deleteConfirm);
            s_deleteConfirm = -1;
            return;
        }
        if (kDown & KEY_B) { s_deleteConfirm = -1; return; }
        if (kDown & KEY_TOUCH)
        {
            int tx = touch->px, ty = touch->py;
            if (tx >= 30 && tx <= 150 && ty >= 80 && ty <= 120) { s_deleteConfirm = -1; return; }
            if (tx >= 170 && tx <= 290 && ty >= 80 && ty <= 120)
            {
                Accounts_Remove(s_deleteConfirm);
                s_deleteConfirm = -1;
                return;
            }
        }
        return;
    }

    // ===== 按键处理 =====
    if (kDown & KEY_B)
    {
        g_app.currentPage = Accounts_GetCount() > 0 ? PAGE_MAIN : PAGE_LOAD;
        return;
    }

    if (kDown & KEY_DLEFT || kDown & KEY_DRIGHT)
    {
        g_app.focusActive = true;
        int focusables[8];
        int count = 0;
        focusables[count++] = 0;
        for (int i = 0; i < MAX_ACCOUNTS; i++)
            if (g_app.accounts[i].added)
                focusables[count++] = i + 1;
        int addedCount = 0;
        for (int i = 0; i < MAX_ACCOUNTS; i++)
            if (g_app.accounts[i].added) addedCount++;
        if (addedCount > 0)
            focusables[count++] = 5;
        focusables[count++] = 6;
        int curIdx = 0;
        for (int i = 0; i < count; i++)
            if (focusables[i] == s_listFocus) { curIdx = i; break; }
        if (kDown & KEY_DRIGHT)
            curIdx = (curIdx + 1) % count;
        else
            curIdx = (curIdx - 1 + count) % count;
        s_listFocus = focusables[curIdx];
    }

    if (kDown & KEY_A && g_app.focusActive)
    {
        if (s_listFocus == 0)
        {
            if (Accounts_GetCount() < MAX_ACCOUNTS)
            {
                g_app.currentPage = PAGE_ADD_ACCOUNT;
                g_app.addEmailBuf[0] = 0;
                g_app.addPassBuf[0] = 0;
                g_app.activeInputField = -1;
                s_showPassword = false;
            }
        }
        else if (s_listFocus >= 1 && s_listFocus <= 4)
        {
            s_deleteConfirm = s_listFocus - 1;
            s_delFocus = 0;
        }
        else if (s_listFocus == 5)
        {
            int addedCount = 0;
            for (int i = 0; i < MAX_ACCOUNTS; i++)
                if (g_app.accounts[i].added) addedCount++;
            if (addedCount > 0)
                g_app.currentPage = PAGE_MAIN;
        }
        else if (s_listFocus == 6)
        {
            Cache_RefreshTotalSize();
            g_app.currentPage = PAGE_SETTINGS;
        }
        return;
    }

    // ===== 统一触摸处理：按下记录、按住跟踪移出、抬起执行 =====

    // 辅助函数：判断坐标是否在指定按钮上
    // btn: 0=添加 1~4=删除 5=返回 6=设置
    // margin: 触摸区域外扩像素
    #define TOUCH_MARGIN 6

    // 抬起执行
    if (g_kUp & KEY_TOUCH)
    {
        int btn = s_pressedListBtn;
        s_pressedListBtn = -1;
        s_hasDown = false;
        if (btn == 0)
        {
            if (Accounts_GetCount() < MAX_ACCOUNTS)
            {
                g_app.currentPage = PAGE_ADD_ACCOUNT;
                g_app.addEmailBuf[0] = 0;
                g_app.addPassBuf[0] = 0;
                g_app.activeInputField = -1;
                s_showPassword = false;
            }
        }
        else if (btn >= 1 && btn <= 4)
        {
            s_deleteConfirm = btn - 1;
            s_delFocus = 0;
        }
        else if (btn == 5)
        {
            int addedCount = 0;
            for (int i = 0; i < MAX_ACCOUNTS; i++)
                if (g_app.accounts[i].added) addedCount++;
            if (addedCount > 0) g_app.currentPage = PAGE_MAIN;
        }
        else if (btn == 6)
        {
            Cache_RefreshTotalSize();
            g_app.currentPage = PAGE_SETTINGS;
        }
    }

    // 按住期间跟踪：更新位置，检测移出
    if ((hidKeysHeld() & KEY_TOUCH) && s_pressedListBtn >= 0 && !(kDown & KEY_TOUCH))
    {
        touchPosition curTouch; hidTouchRead(&curTouch);
        int tx = curTouch.px, ty = curTouch.py;
        s_lastDownX = tx; s_lastDownY = ty;
        bool onBtn = false;
        int m = TOUCH_MARGIN;
        if (s_pressedListBtn == 0)
            onBtn = (ty >= 16 - m && ty <= 68 + m && tx >= 12 - m && tx <= 308 + m);
        else if (s_pressedListBtn >= 1 && s_pressedListBtn <= 4)
        {
            int i = s_pressedListBtn - 1;
            float x = 12 + i * 76;
            onBtn = (g_app.accounts[i].added &&
                     tx >= x + 52 - m && tx <= x + 72 + m &&
                     ty >= 94 - m && ty <= 114 + m);
        }
        else if (s_pressedListBtn == 5)
            onBtn = (ty >= 209 - m && ty <= 240 && tx >= 0 - m && tx <= 120 + m);
        else if (s_pressedListBtn == 6)
            onBtn = (ty >= 209 - m && ty <= 240 && tx >= 200 - m && tx <= 320 + m);
        if (!onBtn) s_pressedListBtn = -1;
    }

    // 按下记录
    if (kDown & KEY_TOUCH)
    {
        int tx = touch->px, ty = touch->py;
        s_lastDownX = tx; s_lastDownY = ty;
        s_hasDown = true;

        if (ty >= 16 && ty <= 68 && tx >= 12 && tx <= 308)
        { s_pressedListBtn = 0; }
        else
        {
            float slotW = 68, slotY = 100, gap = 8;
            for (int i = 0; i < MAX_ACCOUNTS; i++)
            {
                float x = 12 + i * (slotW + gap);
                if (g_app.accounts[i].added &&
                    tx >= x + slotW - 16 - TOUCH_MARGIN && tx <= x + slotW + 4 + TOUCH_MARGIN &&
                    ty >= slotY - 6 - TOUCH_MARGIN && ty <= slotY + 14 + TOUCH_MARGIN)
                { s_pressedListBtn = i + 1; break; }
            }
        }
        if (s_pressedListBtn < 0 && ty >= 209 && ty <= 240)
        {
            if (tx >= 0 && tx <= 120)
            {
                int addedCount = 0;
                for (int i = 0; i < MAX_ACCOUNTS; i++)
                    if (g_app.accounts[i].added) addedCount++;
                if (addedCount > 0) s_pressedListBtn = 5;
            }
            else if (tx >= 200 && tx <= 320)
            {
                s_pressedListBtn = 6;
            }
        }
    }
}

void Page_AddAccountDraw(void)
{
    // 上屏
    C2D_TargetClear(topTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(topTarget);
    UI_DrawStatusBarTop();
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 57, 0.8f, COLOR_BLUE, "新增电子邮件账户");
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 97, 0.5f, COLOR_TEXT_PRIMARY, "请在下屏填写邮箱信息");
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 137, 0.45f, COLOR_RED, "QQ/163/126/新浪邮箱请填写授权码");
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 162, 0.45f, COLOR_RED, "海外邮箱请填写应用专用密码");

    if (Keyboard_IsActive()) { Keyboard_Draw(); return; }

    // 下屏
    C2D_TargetClear(botTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(botTarget);
    LoadEyeIcons();
    bool emailFocused = (g_app.activeInputField == 0) || (g_app.activeInputField < 0 && g_app.focusActive && s_addFocus == 0);
    bool passFocused = (g_app.activeInputField == 1) || (g_app.activeInputField < 0 && g_app.focusActive && s_addFocus == 1);
    DrawInputField(16, 45, 288, 36, "电子邮件地址", g_app.addEmailBuf, emailFocused, false);
    DrawInputField(16, 111, 288, 36, "密码/授权码", g_app.addPassBuf, passFocused, !s_showPassword);

    // 密码显示/隐藏切换图标
    {
        float eyeX = 282, eyeY = 88, eyeSize = 20;
        C2D_Image eyeImg = s_showPassword ? s_eyeOnIcon : s_eyeOffIcon;
        if ((s_showPassword ? s_eyeOnSheet : s_eyeOffSheet) && eyeImg.subtex)
            C2D_DrawImageAt(eyeImg, eyeX, eyeY, 0.5f, NULL, eyeSize / 256.0f, eyeSize / 256.0f);
    }

    AccountType type = Accounts_DetectType(g_app.addEmailBuf);
    const char *imap, *smtp;
    int imapPort, smtpPort;
    Accounts_GetServerInfo(type, &imap, &smtp, &imapPort, &smtpPort);

    // 验证邮箱
    bool emailValid = IsValidEmail(g_app.addEmailBuf);
    bool emailDup = IsEmailDuplicate(g_app.addEmailBuf);
    bool canSave = (!s_testing && emailValid && !emailDup && strlen(g_app.addPassBuf) > 0);

    // 错误提示或服务器信息
    if (s_testing)
    {
        UI_DrawTextCenter(SCREEN_BOT_W / 2, 170, 0.45f, COLOR_BLUE, "正在验证账户连接，请稍候...");
    }
    else if (s_addError[0])
    {
        UI_DrawText(25, 163, 0.4f, COLOR_RED, s_addError);
    }
    else if (strlen(g_app.addEmailBuf) > 0 && !emailValid)
    {
        UI_DrawText(25, 163, 0.4f, COLOR_RED, "电子邮件地址格式不正确");
    }
    else if (emailDup)
    {
        UI_DrawText(25, 163, 0.4f, COLOR_RED, "该电子邮件地址已被添加");
    }
    else
    {
        char info[128];
        snprintf(info, sizeof(info), "IMAP: %s:%d", imap, imapPort);
        UI_DrawText(25, 163, 0.4f, COLOR_TEXT_SECONDARY, info);
        snprintf(info, sizeof(info), "SMTP: %s:%d", smtp, smtpPort);
        UI_DrawText(25, 181, 0.4f, COLOR_TEXT_SECONDARY, info);
    }

        // 左下角“测试连接”按钮：120x30, x=0,y=210, 右上角圆角30
    {
        float bx = 0, by = 210, bw = 120, bh = 30, br = 30;
        float cx = bx + bw - br;
        float cy = by + br;
        bool focused = canSave && ((g_app.focusActive && s_addFocus == 2) || s_pressedAddBtn == 2);
        u32 bg = !canSave ? C2D_Color32(0xBD,0xBD,0xBD,0xFF) : (focused ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY);
        u32 border = !canSave ? C2D_Color32(0x9E,0x9E,0x9E,0xFF) : COLOR_BTN_GRAY_DARK;
        u32 borderHalf = C2D_Color32(border >> 16 & 0xFF, border >> 8 & 0xFF, border & 0xFF, 0x80);
        u32 textColor = !canSave ? C2D_Color32(0x75,0x75,0x75,0xFF) : COLOR_WHITE;

        for (int py = (int)by; py < by + bh; py++)
        {
            float rightEdge;
            if (py < cy)
            {
                float dy = cy - py;
                rightEdge = cx + sqrtf(br * br - dy * dy);
            }
            else
                rightEdge = bx + bw;
            C2D_DrawRectSolid(bx, py, 0, rightEdge - bx, 1, bg);
        }

        C2D_DrawRectSolid(bx, by, 0, cx - bx, 2, border);
        C2D_DrawRectSolid(bx, by + 2, 0, cx - bx, 1, borderHalf);

        for (float a = 270.0f; a <= 360.0f; a += 0.5f)
        {
            float rad = a * 3.14159265f / 180.0f;
            for (int t = 0; t <= 1; t++)
            {
                float r = br - 1.5f + t;
                float px = cx + r * cosf(rad);
                float py = cy + r * sinf(rad);
                C2D_DrawRectSolid(px - 0.5f, py - 0.5f, 0, 1, 1, border);
            }
        }

        UI_DrawTextCenterInRect(bx - 5, by, bw, bh, 0.5f, textColor, "测试连接");
    }

    // 右下角“保存账户”按钮：120x30, x=200,y=210, 左上角圆角30
    {
        float bx = 200, by = 210, bw = 120, bh = 30, br = 30;
        float cx = bx + br;
        float cy = by + br;
        bool focused = canSave && ((g_app.focusActive && s_addFocus == 3) || s_pressedAddBtn == 3);
        u32 bg = !canSave ? C2D_Color32(0xBD,0xBD,0xBD,0xFF) : (focused ? COLOR_BLUE_DARK : COLOR_BLUE);
        u32 border = !canSave ? C2D_Color32(0x9E,0x9E,0x9E,0xFF) : COLOR_BLUE_DARK;
        u32 borderHalf = C2D_Color32(border >> 16 & 0xFF, border >> 8 & 0xFF, border & 0xFF, 0x80);
        u32 textColor = !canSave ? C2D_Color32(0x75,0x75,0x75,0xFF) : COLOR_WHITE;

        for (int py = (int)by; py < by + bh; py++)
        {
            float leftEdge;
            if (py < cy)
            {
                float dy = cy - py;
                leftEdge = cx - sqrtf(br * br - dy * dy);
            }
            else
                leftEdge = bx;
            C2D_DrawRectSolid(leftEdge, py, 0, bx + bw - leftEdge, 1, bg);
        }

        C2D_DrawRectSolid(cx, by, 0, bx + bw - cx, 2, border);
        C2D_DrawRectSolid(cx, by + 2, 0, bx + bw - cx, 1, borderHalf);

        for (float a = 180.0f; a <= 270.0f; a += 0.5f)
        {
            float rad = a * 3.14159265f / 180.0f;
            for (int t = 0; t <= 1; t++)
            {
                float r = br - 1.5f + t;
                float px = cx + r * cosf(rad);
                float py = cy + r * sinf(rad);
                C2D_DrawRectSolid(px - 0.5f, py - 0.5f, 0, 1, 1, border);
            }
        }

        UI_DrawTextCenterInRect(bx + 5, by, bw, bh, 0.5f, textColor, "保存账户");
    }
}

void Page_AddAccountUpdate(touchPosition* touch, u32 kDown)
{
    if (Keyboard_IsActive())
    {
        u32 kHeld = hidKeysHeld();
        bool kbClosed = Keyboard_HandleInput(kDown, kHeld);
        if (!kbClosed && (kDown & KEY_TOUCH))
            kbClosed = Keyboard_HandleTouch(touch);
        if (kbClosed)
        {
            if (g_app.activeInputField == 0)
                strcpy(g_app.addEmailBuf, Keyboard_GetText());
            else if (g_app.activeInputField == 1)
                strcpy(g_app.addPassBuf, Keyboard_GetText());
            g_app.activeInputField = -1;
            s_addError[0] = 0;  // 修改输入后清除错误
        }
        return;
    }

    if (kDown & KEY_B) { g_app.currentPage = PAGE_ACCOUNT_LIST; return; }

    // 按键导航：上下纵向切换，左右横向切换
    bool emailValid = IsValidEmail(g_app.addEmailBuf);
    bool emailDup = IsEmailDuplicate(g_app.addEmailBuf);
    bool canSave = (!s_testing && emailValid && !emailDup && strlen(g_app.addPassBuf) > 0);

    // 测试中：轮询结果，禁止其他操作
    if (s_testing)
    {
        if (s_testResult.finished)
        {
            s_testing = false;
            if (s_testThread)
            {
                threadJoin(s_testThread, U64_MAX);
                s_testThread = NULL;
            }
            if (s_testResult.imapOk)
            {
                // 验证通过，保存账户
                Accounts_Add(g_app.addEmailBuf, g_app.addPassBuf);
                s_addError[0] = 0;
                g_app.currentPage = PAGE_MAIN;
                // 触发邮件拉取：重置所有拉取状态，确保新账户邮件被加载
                g_app.emailCount = 0;
                g_app.selectedEmail = -1;
                g_app.listScroll = 0;
                g_app.listPage = 0;
                g_app.fetchingMails = false;
                g_app.fetchFinished = false;
                g_app.fetchSuccess = false;
                g_app.uidListLoaded = false;
                g_app.uidListCount = 0;
                g_app.uidListAccount = -1;
                g_app.totalMails = 0;
                g_app.totalPages = 1;
                return;
            }
            else
            {
                // 验证失败
                strncpy(s_addError, s_testResult.error, sizeof(s_addError) - 1);
                s_addError[sizeof(s_addError) - 1] = 0;
            }
        }
        return;  // 测试中禁止其他操作
    }
    // 焦点：0=邮箱，1=密码，2=测试连接，3=保存账户
    // 布局：邮箱(整行) → 密码(整行) → [测试连接 | 保存账户]
    if (kDown & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT))
        g_app.focusActive = true;
    if (kDown & KEY_DUP)
    {
        if (s_addFocus == 0) s_addFocus = canSave ? 3 : 1;
        else if (s_addFocus == 1) s_addFocus = 0;
        else s_addFocus = 1;  // 按钮上 → 密码
    }
    if (kDown & KEY_DDOWN)
    {
        if (s_addFocus == 0) s_addFocus = 1;
        else if (s_addFocus == 1) s_addFocus = canSave ? 2 : 0;
        else s_addFocus = 0;  // 按钮下 → 邮箱
    }
    if (kDown & KEY_DLEFT)
    {
        if (s_addFocus == 0) s_addFocus = canSave ? 3 : 1;
        else if (s_addFocus == 1) s_addFocus = 0;
        else if (s_addFocus == 2) s_addFocus = 1;
        else if (s_addFocus == 3) s_addFocus = 2;
    }
    if (kDown & KEY_DRIGHT)
    {
        if (s_addFocus == 0) s_addFocus = 1;
        else if (s_addFocus == 1) s_addFocus = canSave ? 2 : 0;
        else if (s_addFocus == 2) s_addFocus = 3;
        else if (s_addFocus == 3) s_addFocus = 0;
    }

    // A键确认（需要焦点已激活）
    if (kDown & KEY_A && g_app.focusActive)
    {
        if (s_addFocus == 0)
        {
            g_app.activeInputField = 0;
            Keyboard_Open(g_app.addEmailBuf, sizeof(g_app.addEmailBuf), false, false);
            return;
        }
        if (s_addFocus == 1)
        {
            g_app.activeInputField = 1;
            Keyboard_Open(g_app.addPassBuf, sizeof(g_app.addPassBuf), true, false);
            return;
        }
        if (s_addFocus == 3 && canSave)
        {
            // 启动连接测试
            s_addError[0] = 0;
            s_testing = true;
            memset(&s_testResult, 0, sizeof(s_testResult));
            s_testResult.finished = false;

            struct TestThreadArg* ta = calloc(1, sizeof(struct TestThreadArg));
            if (ta)
            {
                AccountType type = Accounts_DetectType(g_app.addEmailBuf);
                const char *imap, *smtp;
                int imapPort, smtpPort;
                Accounts_GetServerInfo(type, &imap, &smtp, &imapPort, &smtpPort);
                strncpy(ta->imapServer, imap, sizeof(ta->imapServer) - 1);
                ta->imapPort = imapPort;
                strncpy(ta->smtpServer, smtp, sizeof(ta->smtpServer) - 1);
                ta->smtpPort = smtpPort;
                strncpy(ta->email, g_app.addEmailBuf, sizeof(ta->email) - 1);
                strncpy(ta->password, g_app.addPassBuf, sizeof(ta->password) - 1);

                s_testThread = threadCreate(test_connection_thread, ta,
                    1024 * 1024, 0x18, -1, false);
                if (!s_testThread)
                {
                    s_testing = false;
                    strcpy(s_addError, "无法启动连接测试");
                    free(ta);
                }
            }
            else
            {
                s_testing = false;
                strcpy(s_addError, "内存不足");
            }
            return;
        }
        // s_addFocus == 2 测试连接：暂无实际功能
    }

    if (hidKeysHeld() & KEY_TOUCH)
    {
        g_app.focusActive = true;
        int tx = touch->px, ty = touch->py;

        // 密码显示/隐藏切换图标
        if (tx >= 278 && tx <= 306 && ty >= 84 && ty <= 112)
        {
            s_showPassword = !s_showPassword;
            return;
        }

        if (ty >= 45 && ty <= 81 && tx >= 16 && tx <= 304)
        {
            s_addFocus = 0;
            g_app.activeInputField = 0;
            Keyboard_Open(g_app.addEmailBuf, sizeof(g_app.addEmailBuf), false, false);
            return;
        }
        if (ty >= 111 && ty <= 147 && tx >= 16 && tx <= 304)
        {
            s_addFocus = 1;
            g_app.activeInputField = 1;
            Keyboard_Open(g_app.addPassBuf, sizeof(g_app.addPassBuf), true, false);
            return;
        }

        if (canSave && ty >= 210 && ty <= 240)
        {
            if (tx >= 0 && tx <= 120)
                s_pressedAddBtn = 2;
            if (tx >= 200 && tx <= 320)
                s_pressedAddBtn = 3;
        }
    }

    // 手指移出按钮时取消
    if (hidKeysHeld() & KEY_TOUCH)
    {
        if (s_pressedAddBtn >= 0)
        {
            touchPosition curTouch;
            hidTouchRead(&curTouch);
            int tx = curTouch.px, ty = curTouch.py;
            bool onBtn = false;
            if (s_pressedAddBtn == 2 && canSave && tx >= 0 && tx <= 120 && ty >= 210 && ty <= 240) onBtn = true;
            if (s_pressedAddBtn == 3 && canSave && tx >= 200 && tx <= 320 && ty >= 210 && ty <= 240) onBtn = true;
            if (!onBtn) s_pressedAddBtn = -1;
        }
    }

    // 触摸抬起时执行
    if (g_kUp & KEY_TOUCH)
    {
        if (s_pressedAddBtn >= 0)
        {
            int btn = s_pressedAddBtn;
            s_pressedAddBtn = -1;
            if (btn == 3 && canSave)
            {
                // 启动连接测试
                s_addError[0] = 0;
                s_testing = true;
                memset(&s_testResult, 0, sizeof(s_testResult));
                s_testResult.finished = false;

                struct TestThreadArg* ta = calloc(1, sizeof(struct TestThreadArg));
                if (ta)
                {
                    AccountType type = Accounts_DetectType(g_app.addEmailBuf);
                    const char *imap, *smtp;
                    int imapPort, smtpPort;
                    Accounts_GetServerInfo(type, &imap, &smtp, &imapPort, &smtpPort);
                    strncpy(ta->imapServer, imap, sizeof(ta->imapServer) - 1);
                    ta->imapPort = imapPort;
                    strncpy(ta->smtpServer, smtp, sizeof(ta->smtpServer) - 1);
                    ta->smtpPort = smtpPort;
                    strncpy(ta->email, g_app.addEmailBuf, sizeof(ta->email) - 1);
                    strncpy(ta->password, g_app.addPassBuf, sizeof(ta->password) - 1);

                    s_testThread = threadCreate(test_connection_thread, ta,
                        1024 * 1024, 0x18, -1, false);
                    if (!s_testThread)
                    {
                        s_testing = false;
                        strcpy(s_addError, "无法启动连接测试");
                        free(ta);
                    }
                }
                else
                {
                    s_testing = false;
                    strcpy(s_addError, "内存不足");
                }
                return;
            }
            // btn == 2 测试连接暂无实际功能
        }
    }
}

// ========== 设置页面 ==========
static int s_setFocus = 0;
static int s_setPressed = -1;
static bool s_setBackPressed = false;
static bool s_showClearCacheDialog = false;
static int s_clearCacheStep = 0; // 0=未显示, 1=确认弹窗
#define SET_ITEM_COUNT 5
// 0=每页邮件数 1=通知设置 2=自动检查邮件 3=清除缓存 4=关于
static const int s_perPageOptions[] = {7, 10, 15, 20};
#define PER_PAGE_OPT_COUNT 4

void Page_SettingsDraw(void)
{
    // 上屏
    C2D_TargetClear(topTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(topTarget);
    UI_DrawStatusBarTop();
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 100, 0.8f, COLOR_BLUE, "设置");

    // 下屏
    C2D_TargetClear(botTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(botTarget);
    C2D_DrawRectSolid(0, 0, 0, SCREEN_BOT_W, SCREEN_BOT_H, COLOR_PAGE_BG);

    static const char* labels[SET_ITEM_COUNT] = {
        "每页邮件数", "通知设置", "自动检查邮件", "清除缓存", "关于"
    };

    for (int i = 0; i < SET_ITEM_COUNT; i++)
    {
        float iy = 23 + i * 33;
        bool focused = (g_app.focusActive && s_setFocus == i);
        bool pressed = (s_setPressed == i);

        if (focused || pressed)
        {
            C2D_DrawRectSolid(8, iy, 0, 304, 31, COLOR_SELECTED_BG);
            UI_DrawRoundRectR(8, iy, 304, 31, 4, 0, COLOR_BLUE);
        }

        u32 color = focused ? COLOR_BLUE : COLOR_TEXT_PRIMARY;
        UI_DrawText(20, iy + 8, 0.45f, color, labels[i]);

        if (i == 0)
        {
            char val[16];
            snprintf(val, sizeof(val), "%d封", g_app.emailsPerPage);
            UI_DrawText(250, iy + 8, 0.45f, COLOR_TEXT_SECONDARY, val);
        }
        else if (i == 3)
        {
            // 清除缓存：显示缓存大小
            char sizeStr[32];
            float cacheMB = Cache_GetTotalSizeMB();
            if (cacheMB < 0.5f)
                snprintf(sizeStr, sizeof(sizeStr), "%dKB", (int)(cacheMB * 1024.0f));
            else
                snprintf(sizeStr, sizeof(sizeStr), "%.2fMB", cacheMB);
            UI_DrawText(240, iy + 8, 0.45f, COLOR_TEXT_SECONDARY, sizeStr);
        }
        else
        {
            UI_DrawText(296, iy + 8, 0.45f, COLOR_TEXT_SECONDARY, ">");
        }

        if (i < SET_ITEM_COUNT - 1)
            C2D_DrawRectSolid(20, iy + 32, 0, 280, 1, C2D_Color32(0xD0,0xCC,0xB8,0xFF));
    }

    // 左下角返回按钮（与账户设置界面相同样式）
    {
        float bx = 0, by = 210, bw = 120, bh = 30, br = 30;
        bool backFocused = s_setBackPressed;
        u32 backBg = backFocused ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY;
        u32 borderHalf = C2D_Color32(0x42,0x42,0x42,0x80);

        float cx = bx + bw - br;
        float cy = by + br;
        for (int py = (int)by; py < by + bh; py++)
        {
            float rightEdge;
            if (py < cy) { float dy = cy - py; rightEdge = cx + sqrtf(br*br - dy*dy); }
            else rightEdge = bx + bw;
            C2D_DrawRectSolid(bx, py, 0, rightEdge - bx, 1, backBg);
        }
        C2D_DrawRectSolid(bx, by, 0, cx - bx, 2, COLOR_BTN_GRAY_DARK);
        C2D_DrawRectSolid(bx, by + 2, 0, cx - bx, 1, borderHalf);
        for (float a = 270.0f; a <= 360.0f; a += 0.5f)
        {
            float rad = a * 3.14159265f / 180.0f;
            for (int t = 0; t <= 1; t++)
            {
                float r = br - 1.5f + t;
                C2D_DrawRectSolid(cx + r*cosf(rad) - 0.5f, cy + r*sinf(rad) - 0.5f, 0, 1, 1, COLOR_BTN_GRAY_DARK);
            }
        }
        C2D_DrawRectSolid(bx + bw - 2, cy, 0, 2, by + bh - cy, COLOR_BTN_GRAY_DARK);
        C2D_DrawRectSolid(bx + bw - 3, cy, 0, 1, by + bh - cy, borderHalf);
        UI_DrawTextCenterInRect(bx - 5, by, bw, bh, 0.5f, COLOR_WHITE, "返回");
    }

    // 清除缓存确认弹窗
    if (s_showClearCacheDialog)
    {
        // 半透明遮罩（最上层）
        C2D_DrawRectSolid(0, 0, 1.0f, 320, 240, C2D_Color32(0, 0, 0, 128));
        int dw = 260, dh = 100;
        int dx = (320 - dw) / 2, dy = (240 - dh) / 2;
        // 白色背景
        C2D_DrawRectSolid(dx, dy, 1.0f, dw, dh, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        // 蓝色边框
        u32 bc = COLOR_BLUE;
        C2D_DrawRectSolid(dx, dy, 1.0f, dw, 1, bc);
        C2D_DrawRectSolid(dx, dy + dh - 1, 1.0f, dw, 1, bc);
        C2D_DrawRectSolid(dx, dy, 1.0f, 1, dh, bc);
        C2D_DrawRectSolid(dx + dw - 1, dy, 1.0f, 1, dh, bc);
        // 黑色文字
        UI_DrawTextCenterDepth(160, dy + 15, 1.0f, 0.5f, C2D_Color32(0,0,0,0xFF), "是否确定清除");
        UI_DrawTextCenterDepth(160, dy + 40, 1.0f, 0.42f, C2D_Color32(0,0,0,0xFF), "清除后邮件将重新加载");
        // 取消/清除按钮
        int btnW = 100, btnH = 28;
        int btnY = dy + dh - 36;
        int btn1X = dx + 20;
        int btn2X = dx + dw - 20 - btnW;
        // 取消按钮（灰色背景，黑色文字）
        C2D_DrawRectSolid(btn1X, btnY, 1.0f, btnW, btnH, C2D_Color32(0xE0,0xE0,0xE0,0xFF));
        UI_DrawTextCenterDepth(btn1X + btnW/2, btnY + 10, 1.0f, 0.45f, C2D_Color32(0,0,0,0xFF), "取消");
        // 清除按钮（蓝色背景，白色文字）
        C2D_DrawRectSolid(btn2X, btnY, 1.0f, btnW, btnH, COLOR_BLUE);
        UI_DrawTextCenterDepth(btn2X + btnW/2, btnY + 10, 1.0f, 0.45f, C2D_Color32(0xFF,0xFF,0xFF,0xFF), "清除");
    }
}

void Page_SettingsUpdate(touchPosition* touch, u32 kDown)
{
    // 清除缓存确认弹窗处理
    if (s_showClearCacheDialog)
    {
        if (kDown & KEY_B)
        {
            s_showClearCacheDialog = false;
            return;
        }
        if (kDown & KEY_A)
        {
            // 确定清除，清除后停留在设置界面
            Cache_ClearAll();
            Cache_RefreshTotalSize();
            s_showClearCacheDialog = false;
            g_app.uidListLoaded = false;
            g_app.needRefresh = true;
            g_app.fetchFinished = false;
            g_app.emailCount = 0;
            g_app.selectedEmail = -1;
            return;
        }
        // 触摸处理
        if (kDown & KEY_TOUCH)
        {
            int tx = touch->px, ty = touch->py;
            int dw = 260, dh = 100;
            int dx = (320 - dw) / 2, dy = (240 - dh) / 2;
            int btnW = 100, btnH = 28;
            int btnY = dy + dh - 36;
            int btn1X = dx + 20;
            int btn2X = dx + dw - 20 - btnW;
            if (ty >= btnY && ty <= btnY + btnH)
            {
                if (tx >= btn1X && tx <= btn1X + btnW)
                    s_showClearCacheDialog = false; // 取消
                else if (tx >= btn2X && tx <= btn2X + btnW)
                {
                    // 确定，清除后停留在设置界面
                    Cache_ClearAll();
                    s_showClearCacheDialog = false;
                    g_app.uidListLoaded = false;
                    g_app.needRefresh = true;
                    g_app.fetchFinished = false;
                    g_app.emailCount = 0;
                    g_app.selectedEmail = -1;
                }
            }
        }
        return;
    }

    if (kDown & KEY_B) { g_app.currentPage = PAGE_ACCOUNT_LIST; return; }

    if (kDown & KEY_DUP)
    {
        g_app.focusActive = true;
        if (s_setFocus > 0) s_setFocus--;
        else s_setFocus = SET_ITEM_COUNT - 1;
    }
    if (kDown & KEY_DDOWN)
    {
        g_app.focusActive = true;
        if (s_setFocus < SET_ITEM_COUNT - 1) s_setFocus++;
        else s_setFocus = 0;
    }

    // 每页邮件数：左右键切换
    if (s_setFocus == 0)
    {
        if (kDown & KEY_DLEFT)
        {
            g_app.focusActive = true;
            int idx = 0;
            for (int i = 0; i < PER_PAGE_OPT_COUNT; i++)
                if (s_perPageOptions[i] == g_app.emailsPerPage) idx = i;
            idx = (idx - 1 + PER_PAGE_OPT_COUNT) % PER_PAGE_OPT_COUNT;
            g_app.emailsPerPage = s_perPageOptions[idx];
            Settings_Save();
        }
        if (kDown & KEY_DRIGHT)
        {
            g_app.focusActive = true;
            int idx = 0;
            for (int i = 0; i < PER_PAGE_OPT_COUNT; i++)
                if (s_perPageOptions[i] == g_app.emailsPerPage) idx = i;
            idx = (idx + 1) % PER_PAGE_OPT_COUNT;
            g_app.emailsPerPage = s_perPageOptions[idx];
            Settings_Save();
        }
    }

    // A键：进入子页面或显示清除缓存弹窗
    if (kDown & KEY_A && g_app.focusActive)
    {
        if (s_setFocus == 1) g_app.currentPage = PAGE_NOTIF_SETTINGS;
        else if (s_setFocus == 2) g_app.currentPage = PAGE_AUTO_CHECK;
        else if (s_setFocus == 3) s_showClearCacheDialog = true;
        else if (s_setFocus == 4) g_app.currentPage = PAGE_ABOUT;
    }

    if (kDown & KEY_TOUCH)
    {
        int tx = touch->px, ty = touch->py;
        for (int i = 0; i < SET_ITEM_COUNT; i++)
        {
            float iy = 23 + i * 33;
            if (ty >= iy && ty <= iy + 31 && tx >= 8 && tx <= 312)
            { s_setPressed = i; g_app.focusActive = true; s_setFocus = i; return; }
        }
        // 返回按钮
        if (ty >= 209 && ty <= 240 && tx >= 0 && tx <= 120)
        {
            s_setBackPressed = true;
            return;
        }
    }

    if (hidKeysHeld() & KEY_TOUCH)
    {
        if (s_setBackPressed)
        {
            touchPosition curTouch; hidTouchRead(&curTouch);
            if (!(curTouch.py >= 209 && curTouch.py <= 240 && curTouch.px >= 0 && curTouch.px <= 120))
                s_setBackPressed = false;
        }
    }

    if (g_kUp & KEY_TOUCH)
    {
        if (s_setPressed >= 0)
        {
            int btn = s_setPressed;
            s_setPressed = -1;
            if (btn == 1) g_app.currentPage = PAGE_NOTIF_SETTINGS;
            else if (btn == 2) g_app.currentPage = PAGE_AUTO_CHECK;
            else if (btn == 3) s_showClearCacheDialog = true;
            else if (btn == 4) g_app.currentPage = PAGE_ABOUT;
        }
        if (s_setBackPressed)
        {
            s_setBackPressed = false;
            g_app.currentPage = PAGE_ACCOUNT_LIST;
        }
    }
}

// ========== 通知设置页面 ==========
static int s_notifFocus = 0;
static int s_notifPressed = -1;
static bool s_notifBackPressed = false;
#define NOTIF_ITEM_COUNT 3

void Page_NotifSettingsDraw(void)
{
    // 上屏
    C2D_TargetClear(topTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(topTarget);
    UI_DrawStatusBarTop();
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 100, 0.8f, COLOR_BLUE, "通知设置");

    // 下屏
    C2D_TargetClear(botTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(botTarget);
    C2D_DrawRectSolid(0, 0, 0, SCREEN_BOT_W, SCREEN_BOT_H, COLOR_PAGE_BG);

    static const char* labels[NOTIF_ITEM_COUNT] = {
        "通知", "声音提醒", "指示灯闪烁"
    };
    bool values[NOTIF_ITEM_COUNT] = {
        g_app.notifEnabled, g_app.notifSound, g_app.notifLED
    };
    bool enabled[NOTIF_ITEM_COUNT] = {
        true, g_app.notifEnabled, g_app.notifEnabled
    };

    for (int i = 0; i < NOTIF_ITEM_COUNT; i++)
    {
        float iy = 30 + i * 44;
        bool focused = (g_app.focusActive && s_notifFocus == i && enabled[i]);
        bool pressed = (s_notifPressed == i);

        if (focused || pressed)
        {
            C2D_DrawRectSolid(8, iy, 0, 304, 40, COLOR_SELECTED_BG);
            UI_DrawRoundRectR(8, iy, 304, 40, 4, 0, COLOR_BLUE);
        }

        u32 color = enabled[i] ? (focused ? COLOR_BLUE : COLOR_TEXT_PRIMARY) : COLOR_TEXT_SECONDARY;
        UI_DrawText(20, iy + 13, 0.45f, color, labels[i]);

        // 开关状态
        const char* status = values[i] ? "开" : "关";
        u32 statusColor = values[i] ? COLOR_BLUE : COLOR_TEXT_SECONDARY;
        if (!enabled[i]) statusColor = C2D_Color32(0xBD,0xBD,0xBD,0xFF);
        UI_DrawText(280, iy + 13, 0.45f, statusColor, status);

        if (i < NOTIF_ITEM_COUNT - 1)
            C2D_DrawRectSolid(20, iy + 42, 0, 280, 1, C2D_Color32(0xD0,0xCC,0xB8,0xFF));
    }

    // 左下角返回按钮
    {
        float bx = 0, by = 210, bw = 120, bh = 30, br = 30;
        bool backFocused = s_notifBackPressed;
        u32 backBg = backFocused ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY;
        u32 borderHalf = C2D_Color32(0x42,0x42,0x42,0x80);

        float cx = bx + bw - br;
        float cy = by + br;
        for (int py = (int)by; py < by + bh; py++)
        {
            float rightEdge;
            if (py < cy) { float dy = cy - py; rightEdge = cx + sqrtf(br*br - dy*dy); }
            else rightEdge = bx + bw;
            C2D_DrawRectSolid(bx, py, 0, rightEdge - bx, 1, backBg);
        }
        C2D_DrawRectSolid(bx, by, 0, cx - bx, 2, COLOR_BTN_GRAY_DARK);
        C2D_DrawRectSolid(bx, by + 2, 0, cx - bx, 1, borderHalf);
        for (float a = 270.0f; a <= 360.0f; a += 0.5f)
        {
            float rad = a * 3.14159265f / 180.0f;
            for (int t = 0; t <= 1; t++)
            {
                float r = br - 1.5f + t;
                C2D_DrawRectSolid(cx + r*cosf(rad) - 0.5f, cy + r*sinf(rad) - 0.5f, 0, 1, 1, COLOR_BTN_GRAY_DARK);
            }
        }
        C2D_DrawRectSolid(bx + bw - 2, cy, 0, 2, by + bh - cy, COLOR_BTN_GRAY_DARK);
        C2D_DrawRectSolid(bx + bw - 3, cy, 0, 1, by + bh - cy, borderHalf);
        UI_DrawTextCenterInRect(bx - 5, by, bw, bh, 0.5f, COLOR_WHITE, "返回");
    }
}

void Page_NotifSettingsUpdate(touchPosition* touch, u32 kDown)
{
    if (kDown & KEY_B) { g_app.currentPage = PAGE_SETTINGS; return; }

    if (kDown & KEY_DUP)
    {
        g_app.focusActive = true;
        if (s_notifFocus > 0) s_notifFocus--;
        else s_notifFocus = NOTIF_ITEM_COUNT - 1;
        // 跳过禁用项
        if (s_notifFocus == 1 && !g_app.notifEnabled) s_notifFocus = 0;
        if (s_notifFocus == 2 && !g_app.notifEnabled) s_notifFocus = 0;
    }
    if (kDown & KEY_DDOWN)
    {
        g_app.focusActive = true;
        if (s_notifFocus < NOTIF_ITEM_COUNT - 1) s_notifFocus++;
        else s_notifFocus = 0;
        if (s_notifFocus == 1 && !g_app.notifEnabled) s_notifFocus = 2;
        if (s_notifFocus == 2 && !g_app.notifEnabled) s_notifFocus = 0;
    }

    if (kDown & KEY_A && g_app.focusActive)
    {
        if (s_notifFocus == 0)
            g_app.notifEnabled = !g_app.notifEnabled;
        else if (s_notifFocus == 1 && g_app.notifEnabled)
            g_app.notifSound = !g_app.notifSound;
        else if (s_notifFocus == 2 && g_app.notifEnabled)
            g_app.notifLED = !g_app.notifLED;
        Settings_Save();
        return;
    }

    if (kDown & KEY_TOUCH)
    {
        int tx = touch->px, ty = touch->py;
        for (int i = 0; i < NOTIF_ITEM_COUNT; i++)
        {
            float iy = 30 + i * 44;
            bool itemEnabled = (i == 0) || g_app.notifEnabled;
            if (itemEnabled && ty >= iy && ty <= iy + 40 && tx >= 8 && tx <= 312)
            { s_notifPressed = i; g_app.focusActive = true; s_notifFocus = i; return; }
        }
        if (ty >= 209 && ty <= 240 && tx >= 0 && tx <= 120)
        { s_notifBackPressed = true; return; }
    }

    if (hidKeysHeld() & KEY_TOUCH)
    {
        if (s_notifBackPressed)
        {
            touchPosition curTouch; hidTouchRead(&curTouch);
            if (!(curTouch.py >= 209 && curTouch.py <= 240 && curTouch.px >= 0 && curTouch.px <= 120))
                s_notifBackPressed = false;
        }
    }

    if (g_kUp & KEY_TOUCH)
    {
        if (s_notifPressed >= 0)
        {
            int btn = s_notifPressed; s_notifPressed = -1;
            if (btn == 0) g_app.notifEnabled = !g_app.notifEnabled;
            else if (btn == 1 && g_app.notifEnabled) g_app.notifSound = !g_app.notifSound;
            else if (btn == 2 && g_app.notifEnabled) g_app.notifLED = !g_app.notifLED;
            Settings_Save();
        }
        if (s_notifBackPressed)
        {
            s_notifBackPressed = false;
            g_app.currentPage = PAGE_SETTINGS;
        }
    }
}


// ========== 自动检查邮件页面 ==========
static int s_autoFocus = 0;
static int s_autoPressed = -1;
static bool s_autoBackPressed = false;
#define AUTO_ITEM_COUNT 5
static const char* s_autoLabels[AUTO_ITEM_COUNT] = {
    "关闭", "每5分钟", "每15分钟", "每30分钟", "每1小时"
};

void Page_AutoCheckDraw(void)
{
    C2D_TargetClear(topTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(topTarget);
    UI_DrawStatusBarTop();
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 100, 0.8f, COLOR_BLUE, "自动检查邮件");

    C2D_TargetClear(botTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(botTarget);
    C2D_DrawRectSolid(0, 0, 0, SCREEN_BOT_W, SCREEN_BOT_H, COLOR_PAGE_BG);

    for (int i = 0; i < AUTO_ITEM_COUNT; i++)
    {
        float iy = 30 + i * 36;
        bool focused = (g_app.focusActive && s_autoFocus == i);
        bool pressed = (s_autoPressed == i);
        bool selected = (g_app.autoCheckInterval == i);

        if (focused || pressed)
        {
            C2D_DrawRectSolid(8, iy, 0, 304, 32, COLOR_SELECTED_BG);
            UI_DrawRoundRectR(8, iy, 304, 32, 4, 0, COLOR_BLUE);
        }

        u32 color = focused ? COLOR_BLUE : COLOR_TEXT_PRIMARY;
        UI_DrawText(20, iy + 9, 0.45f, color, s_autoLabels[i]);

        if (selected)
            UI_DrawText(280, iy + 9, 0.45f, COLOR_BLUE, "\xe2\x97\x8f");

        if (i < AUTO_ITEM_COUNT - 1)
            C2D_DrawRectSolid(20, iy + 34, 0, 280, 1, C2D_Color32(0xD0,0xCC,0xB8,0xFF));
    }

    // 返回按钮
    {
        float bx = 0, by = 210, bw = 120, bh = 30, br = 30;
        bool backFocused = s_autoBackPressed;
        u32 backBg = backFocused ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY;
        u32 borderHalf = C2D_Color32(0x42,0x42,0x42,0x80);
        float cx = bx + bw - br, cy = by + br;
        for (int py = (int)by; py < by + bh; py++)
        {
            float rightEdge;
            if (py < cy) { float dy = cy - py; rightEdge = cx + sqrtf(br*br - dy*dy); }
            else rightEdge = bx + bw;
            C2D_DrawRectSolid(bx, py, 0, rightEdge - bx, 1, backBg);
        }
        C2D_DrawRectSolid(bx, by, 0, cx - bx, 2, COLOR_BTN_GRAY_DARK);
        C2D_DrawRectSolid(bx, by + 2, 0, cx - bx, 1, borderHalf);
        for (float a = 270.0f; a <= 360.0f; a += 0.5f)
        {
            float rad = a * 3.14159265f / 180.0f;
            for (int t = 0; t <= 1; t++)
            {
                float r = br - 1.5f + t;
                C2D_DrawRectSolid(cx + r*cosf(rad) - 0.5f, cy + r*sinf(rad) - 0.5f, 0, 1, 1, COLOR_BTN_GRAY_DARK);
            }
        }
        C2D_DrawRectSolid(bx + bw - 2, cy, 0, 2, by + bh - cy, COLOR_BTN_GRAY_DARK);
        C2D_DrawRectSolid(bx + bw - 3, cy, 0, 1, by + bh - cy, borderHalf);
        UI_DrawTextCenterInRect(bx - 5, by, bw, bh, 0.5f, COLOR_WHITE, "返回");
    }
}

void Page_AutoCheckUpdate(touchPosition* touch, u32 kDown)
{
    if (kDown & KEY_B) { g_app.currentPage = PAGE_SETTINGS; return; }

    if (kDown & KEY_DUP)
    {
        g_app.focusActive = true;
        if (s_autoFocus > 0) s_autoFocus--;
        else s_autoFocus = AUTO_ITEM_COUNT - 1;
    }
    if (kDown & KEY_DDOWN)
    {
        g_app.focusActive = true;
        if (s_autoFocus < AUTO_ITEM_COUNT - 1) s_autoFocus++;
        else s_autoFocus = 0;
    }

    if (kDown & KEY_A && g_app.focusActive)
    {
        g_app.autoCheckInterval = s_autoFocus;
        Settings_Save();
        return;
    }

    if (kDown & KEY_TOUCH)
    {
        int tx = touch->px, ty = touch->py;
        for (int i = 0; i < AUTO_ITEM_COUNT; i++)
        {
            float iy = 30 + i * 36;
            if (ty >= iy && ty <= iy + 32 && tx >= 8 && tx <= 312)
            { s_autoPressed = i; g_app.focusActive = true; s_autoFocus = i; return; }
        }
        if (ty >= 209 && ty <= 240 && tx >= 0 && tx <= 120)
        { s_autoBackPressed = true; return; }
    }

    if (hidKeysHeld() & KEY_TOUCH)
    {
        if (s_autoBackPressed)
        {
            touchPosition curTouch; hidTouchRead(&curTouch);
            if (!(curTouch.py >= 209 && curTouch.py <= 240 && curTouch.px >= 0 && curTouch.px <= 120))
                s_autoBackPressed = false;
        }
    }

    if (g_kUp & KEY_TOUCH)
    {
        if (s_autoPressed >= 0)
        {
            g_app.autoCheckInterval = s_autoPressed;
            s_autoPressed = -1;
            Settings_Save();
        }
        if (s_autoBackPressed)
        {
            s_autoBackPressed = false;
            g_app.currentPage = PAGE_SETTINGS;
        }
    }
}

// ========== 关于页面 ==========
static bool s_aboutBackPressed = false;

void Page_AboutDraw(void)
{
    C2D_TargetClear(topTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(topTarget);
    UI_DrawStatusBarTop();
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 100, 0.8f, COLOR_BLUE, "关于");

    C2D_TargetClear(botTarget, COLOR_SCREEN_BG);
    C2D_SceneBegin(botTarget);
    C2D_DrawRectSolid(0, 0, 0, SCREEN_BOT_W, SCREEN_BOT_H, COLOR_PAGE_BG);

    UI_DrawTextCenter(SCREEN_BOT_W / 2, 30, 0.7f, COLOR_BLUE, "3DS 电子邮箱");
    UI_DrawTextCenter(SCREEN_BOT_W / 2, 60, 0.45f, COLOR_TEXT_SECONDARY, "版本 v1.1.0");

    C2D_DrawRectSolid(40, 85, 0, 240, 1, C2D_Color32(0xD0,0xCC,0xB8,0xFF));

    UI_DrawText(30, 100, 0.42f, COLOR_TEXT_PRIMARY, "作者：Wheat");
    UI_DrawText(30, 125, 0.42f, COLOR_TEXT_PRIMARY, "协议：");
    UI_DrawText(30, 150, 0.42f, COLOR_TEXT_PRIMARY, "致谢：");

    // 返回按钮
    {
        float bx = 0, by = 210, bw = 120, bh = 30, br = 30;
        bool backFocused = s_aboutBackPressed;
        u32 backBg = backFocused ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY;
        u32 borderHalf = C2D_Color32(0x42,0x42,0x42,0x80);
        float cx = bx + bw - br, cy = by + br;
        for (int py = (int)by; py < by + bh; py++)
        {
            float rightEdge;
            if (py < cy) { float dy = cy - py; rightEdge = cx + sqrtf(br*br - dy*dy); }
            else rightEdge = bx + bw;
            C2D_DrawRectSolid(bx, py, 0, rightEdge - bx, 1, backBg);
        }
        C2D_DrawRectSolid(bx, by, 0, cx - bx, 2, COLOR_BTN_GRAY_DARK);
        C2D_DrawRectSolid(bx, by + 2, 0, cx - bx, 1, borderHalf);
        for (float a = 270.0f; a <= 360.0f; a += 0.5f)
        {
            float rad = a * 3.14159265f / 180.0f;
            for (int t = 0; t <= 1; t++)
            {
                float r = br - 1.5f + t;
                C2D_DrawRectSolid(cx + r*cosf(rad) - 0.5f, cy + r*sinf(rad) - 0.5f, 0, 1, 1, COLOR_BTN_GRAY_DARK);
            }
        }
        C2D_DrawRectSolid(bx + bw - 2, cy, 0, 2, by + bh - cy, COLOR_BTN_GRAY_DARK);
        C2D_DrawRectSolid(bx + bw - 3, cy, 0, 1, by + bh - cy, borderHalf);
        UI_DrawTextCenterInRect(bx - 5, by, bw, bh, 0.5f, COLOR_WHITE, "返回");
    }
}

void Page_AboutUpdate(touchPosition* touch, u32 kDown)
{
    if (kDown & KEY_B) { g_app.currentPage = PAGE_SETTINGS; return; }

    if (kDown & KEY_TOUCH)
    {
        int tx = touch->px, ty = touch->py;
        if (ty >= 209 && ty <= 240 && tx >= 0 && tx <= 120)
        { s_aboutBackPressed = true; return; }
    }

    if (hidKeysHeld() & KEY_TOUCH)
    {
        if (s_aboutBackPressed)
        {
            touchPosition curTouch; hidTouchRead(&curTouch);
            if (!(curTouch.py >= 209 && curTouch.py <= 240 && curTouch.px >= 0 && curTouch.px <= 120))
                s_aboutBackPressed = false;
        }
    }

    if (g_kUp & KEY_TOUCH)
    {
        if (s_aboutBackPressed)
        {
            s_aboutBackPressed = false;
            g_app.currentPage = PAGE_SETTINGS;
        }
    }
}
