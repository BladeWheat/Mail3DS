// pages_load.c - 加载页、网络错误页、无账户弹窗
#include "pages.h"
#include "ui.h"
#include "network.h"
#include "accounts.h"
#include "email.h"
#include <sys/stat.h>
#include <math.h>

// 加载步骤
#define LOAD_STEPS 5
static bool s_stepDone[LOAD_STEPS] = {false};
static bool s_stepOk[LOAD_STEPS] = {false};

// 弹窗类型
typedef enum {
    POPUP_NONE = 0,
    POPUP_NO_ACCOUNT,
    POPUP_NETWORK_ERROR
} PopupType;

static PopupType s_popup = POPUP_NONE;

// 标题D图标
static C2D_SpriteSheet s_logoSheet = NULL;
static C2D_Image s_logoIcon;

static void LoadLogo(void)
{
    if (!s_logoSheet)
    {
        s_logoSheet = C2D_SpriteSheetLoad("romfs:/logo_d.t3x");
        if (s_logoSheet)
            s_logoIcon = C2D_SpriteSheetGetImage(s_logoSheet, 0);
    }
}
static int s_popupStep = 0; // 弹窗动画进度0-15

// 执行加载步骤
static void ExecuteStep(int step)
{
    if (s_stepDone[step]) return;
    s_stepDone[step] = true;

    switch (step)
    {
        case 0: s_stepOk[step] = true; break;
        case 1:
            mkdir("sdmc:/3ds", 0777);
            mkdir("sdmc:/3ds/Mail3DS", 0777);
            {
                struct stat st;
                s_stepOk[step] = (stat("sdmc:/3ds/Mail3DS", &st) == 0 && (st.st_mode & S_IFDIR));
            }
            break;
        case 2:
            Accounts_Init();
            s_stepOk[step] = Accounts_LoadConfig();
            if (!s_stepOk[step]) s_stepOk[step] = true;
            break;
        case 3:
            {
                FILE* f = fopen("romfs:/ui-menu-font.bcfnt", "rb");
                if (f) { fclose(f); s_stepOk[step] = true; }
                else { s_stepOk[step] = false; }
            }
            break;
        case 4:
            s_stepOk[step] = Network_Init();
            break;
    }
}

// 绘制旋转spinner（8个点围成圆圈）
static void DrawSpinner(float cx, float cy, float radius)
{
    int frame = g_app.frameCount % 48; // 48帧一圈
    for (int i = 0; i < 8; i++)
    {
        float angle = (i * 45.0f - 90.0f) * 3.14159f / 180.0f;
        float px = cx + cosf(angle) * radius;
        float py = cy + sinf(angle) * radius;
        // 根据当前帧计算透明度，形成旋转效果
        int diff = (i - (frame / 6) + 8) % 8;
        int alpha;
        if (diff == 0) alpha = 220;
        else if (diff == 1) alpha = 180;
        else if (diff == 2) alpha = 140;
        else if (diff == 3) alpha = 100;
        else if (diff == 4) alpha = 70;
        else alpha = 40;
        u32 color = C2D_Color32(0x42, 0x42, 0x42, alpha);
        C2D_DrawRectSolid(px - 2, py - 2, 0.5f, 4, 4, color);
    }
}

// 绘制上屏标题（加载和弹窗共用）
static void DrawTopScreen(void)
{
    C2D_TargetClear(topTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(topTarget);

    LoadLogo();

    // "3" 放大两倍（scale=2.6），红色粗体
    float scale3 = 2.6f;
    float scaleRest = 1.3f;
    const char* text3 = "3";
    const char* textS = "S";
    const char* textRest = "电子邮箱";

    // 测量各部分宽度
    float w3 = UI_MeasureText(scale3, text3);
    float wS = UI_MeasureText(scaleRest, textS);
    float wRest = UI_MeasureText(scaleRest, textRest);
    float iconSize = 48.0f;
    float gap = 4.0f;
    float totalW = w3 + gap + iconSize + gap + wS + wRest;
    float startX = (SCREEN_TOP_W - totalW) / 2;

    // 画"3"（粗体，红色，y=65）
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++)
            UI_DrawText(startX + dx, 65 + dy, scale3, COLOR_RED, text3);

    // 画D图标（y=72）
    if (s_logoSheet && s_logoIcon.subtex)
        C2D_DrawImageAt(s_logoIcon, startX + w3 + gap, 72, 0.5f, NULL, iconSize/48, iconSize/48);

    // 画"S"（粗体，黑色，y=80）
    float sX = startX + w3 + gap + iconSize + gap;
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++)
            UI_DrawText(sX + dx, 80 + dy, scaleRest, COLOR_TEXT_PRIMARY, textS);

    // 画"电子邮箱"（粗体，黑色，y=90）
    for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++)
            UI_DrawText(sX + wS + dx, 90 + dy, scaleRest, COLOR_TEXT_PRIMARY, textRest);
}

// 绘制下屏加载中卡片
static void DrawLoadingCard(void)
{
    // 米黄色圆角卡片
    float cardX = 12, cardY = 20, cardW = 296, cardH = 200;
    UI_DrawShadow(cardX, cardY, cardW, cardH, 8);
    UI_DrawRoundRectR(cardX, cardY, cardW, cardH, 8, C2D_Color32(0xF8, 0xF3, 0xE1, 0xFF), C2D_Color32(0xD7, 0xCE, 0xB0, 0xFF));

    // spinner
    DrawSpinner(SCREEN_BOT_W / 2, 95, 20);

    // 加载文字
    UI_DrawTextCenter(SCREEN_BOT_W/2, 130, 0.5f, COLOR_TEXT_PRIMARY, "网络连接");
    // 动画点
    int dotPhase = (g_app.frameCount / 20) % 4;
    char dots[5] = "";
    for (int i = 0; i < dotPhase; i++) dots[i] = '.';
    char loadText[32];
    snprintf(loadText, sizeof(loadText), "加载邮件中%s", dots);
    UI_DrawTextCenter(SCREEN_BOT_W/2, 155, 0.45f, COLOR_TEXT_SECONDARY, loadText);
}

// 绘制无账户弹窗
static void DrawNoAccountPopup(void)
{
    // 灰色半透明遮罩
    C2D_DrawRectSolid(0, 0, 0, SCREEN_BOT_W, SCREEN_BOT_H, C2D_Color32(0, 0, 0, 100));

    // 弹窗动画（淡入+轻微缩放）
    if (s_popupStep < 16) s_popupStep++;
    float scale = 0.9f + (s_popupStep / 16.0f) * 0.1f;
    int alpha = (int)(255 * s_popupStep / 16);

    float cardW = 260 * scale;
    float cardH = 160 * scale;
    float cardX = (SCREEN_BOT_W - cardW) / 2;
    float cardY = (SCREEN_BOT_H - cardH) / 2;

    // 阴影
    UI_DrawShadow(cardX, cardY, cardW, cardH, 8);
    // 卡片
    u32 cardBg = C2D_Color32(0xF8, 0xF3, 0xE1, alpha);
    u32 cardBorder = C2D_Color32(0xD7, 0xCE, 0xB0, alpha);
    UI_DrawRoundRectR(cardX, cardY, cardW, cardH, 8, cardBg, cardBorder);

    // 文字
    u32 textColor = C2D_Color32(0x21, 0x21, 0x21, alpha);
    u32 subTextColor = C2D_Color32(0x75, 0x75, 0x75, alpha);
    UI_DrawTextCenter(SCREEN_BOT_W/2, cardY + 40*scale, 0.5f, textColor, "您没有添加任何账户");
    UI_DrawTextCenter(SCREEN_BOT_W/2, cardY + 65*scale, 0.45f, subTextColor, "请添加电子邮件账户");

    // 添加按钮
    float btnW = 100, btnH = 36;
    float btnX = (SCREEN_BOT_W - btnW) / 2;
    float btnY = cardY + 100*scale;
    UI_DrawShadow(btnX, btnY, btnW, btnH, 6);
    UI_DrawRoundRectR(btnX, btnY, btnW, btnH, 6, COLOR_BLUE, COLOR_BLUE_DARK);
    UI_DrawTextCenter(SCREEN_BOT_W/2, btnY + 10, 0.5f, COLOR_WHITE, "添加");
}

// ========== 加载页 ==========
void Page_LoadDraw(void)
{
    DrawTopScreen();

    // 下屏
    C2D_TargetClear(botTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(botTarget);

    if (s_popup == POPUP_NO_ACCOUNT)
    {
        // 无账户弹窗（背景是白色，弹窗覆盖）
        DrawNoAccountPopup();
    }
    else if (s_popup == POPUP_NETWORK_ERROR)
    {
        // 网络错误弹窗
        C2D_DrawRectSolid(0, 0, 0, SCREEN_BOT_W, SCREEN_BOT_H, C2D_Color32(0, 0, 0, 100));
        if (s_popupStep < 16) s_popupStep++;
        float cardW = 260, cardH = 140;
        float cardX = (SCREEN_BOT_W - cardW)/2;
        float cardY = (SCREEN_BOT_H - cardH)/2;
        UI_DrawShadow(cardX, cardY, cardW, cardH, 8);
        UI_DrawRoundRectR(cardX, cardY, cardW, cardH, 8, COLOR_WHITE, COLOR_BORDER_GRAY);
        UI_DrawTextCenter(SCREEN_BOT_W/2, cardY+35, 0.55f, COLOR_RED, "网络连接错误");
        UI_DrawTextCenter(SCREEN_BOT_W/2, cardY+60, 0.4f, COLOR_TEXT_SECONDARY, "请检查网络后重试");
        float btnW=80, btnH=32, btnX=(SCREEN_BOT_W-btnW)/2, btnY=cardY+90;
        UI_DrawShadow(btnX, btnY, btnW, btnH, 6);
        UI_DrawRoundRectR(btnX, btnY, btnW, btnH, 6, COLOR_BLUE, COLOR_BLUE_DARK);
        UI_DrawTextCenter(SCREEN_BOT_W/2, btnY+8, 0.45f, COLOR_WHITE, "重试");
    }
    else
    {
        // 加载中
        DrawLoadingCard();
    }
}

void Page_LoadUpdate(u32 kDown)
{
    // 如果有弹窗显示，处理弹窗输入
    if (s_popup == POPUP_NO_ACCOUNT)
    {
        // A键=添加账户
        if (kDown & KEY_A)
        {
            g_app.currentPage = PAGE_ADD_ACCOUNT;
            g_app.addEmailBuf[0] = 0;
            g_app.addPassBuf[0] = 0;
            g_app.activeInputField = -1;
            return;
        }
        if (hidKeysHeld() & KEY_TOUCH)
        {
            touchPosition touch;
            hidTouchRead(&touch);
            float cardH = 160;
            float cardY = (SCREEN_BOT_H - cardH)/2;
            float btnW = 100, btnH = 36;
            float btnX = (SCREEN_BOT_W - btnW)/2;
            float btnY = cardY + 100;
            if (touch.px >= btnX && touch.px <= btnX+btnW &&
                touch.py >= btnY && touch.py <= btnY+btnH)
            {
                g_app.currentPage = PAGE_ADD_ACCOUNT;
                g_app.addEmailBuf[0] = 0;
                g_app.addPassBuf[0] = 0;
                g_app.activeInputField = -1;
            }
        }
        return;
    }

    if (s_popup == POPUP_NETWORK_ERROR)
    {
        if (kDown & KEY_A)
        {
            s_popup = POPUP_NONE;
            s_popupStep = 0;
            g_app.loadProgress = 0;
            g_app.loadStep = 0;
            for (int i = 0; i < LOAD_STEPS; i++) { s_stepDone[i]=false; s_stepOk[i]=false; }
            return;
        }
        if (hidKeysHeld() & KEY_TOUCH)
        {
            touchPosition touch;
            hidTouchRead(&touch);
            float cardH=140, cardY=(SCREEN_BOT_H-cardH)/2;
            float btnW=80, btnH=32, btnX=(SCREEN_BOT_W-btnW)/2, btnY=cardY+90;
            if (touch.px >= btnX && touch.px <= btnX+btnW &&
                touch.py >= btnY && touch.py <= btnY+btnH)
            {
                s_popup = POPUP_NONE;
                s_popupStep = 0;
                g_app.loadProgress = 0;
                g_app.loadStep = 0;
                for (int i = 0; i < LOAD_STEPS; i++) { s_stepDone[i]=false; s_stepOk[i]=false; }
            }
        }
        return;
    }

    // 执行加载步骤
    if (g_app.loadStep >= 0 && g_app.loadStep < LOAD_STEPS)
    {
        ExecuteStep(g_app.loadStep);
        if (s_stepDone[g_app.loadStep] && !s_stepOk[g_app.loadStep])
        {
            if (g_app.loadStep == 3)
            {
                g_app.loadError = true;
                s_popup = POPUP_NETWORK_ERROR;
                s_popupStep = 0;
                return;
            }
        }
    }

    g_app.loadProgress++;
    int newStep = g_app.loadProgress * LOAD_STEPS / 240;
    if (newStep >= LOAD_STEPS) newStep = LOAD_STEPS - 1;
    g_app.loadStep = newStep;

    // 加载完成（4秒）
    if (g_app.loadProgress >= 240)
    {
        g_app.loadProgress = 240;
        ExecuteStep(LOAD_STEPS - 1);

        if (g_app.loadError)
        {
            s_popup = POPUP_NETWORK_ERROR;
            s_popupStep = 0;
            return;
        }

        if (Accounts_GetCount() == 0)
        {
            s_popup = POPUP_NO_ACCOUNT;
            s_popupStep = 0;
        }
        else
        {
            g_app.fetchFinished = false;  // 触发主界面拉取真实邮件
            g_app.currentPage = PAGE_MAIN;
        }
    }
}

// ========== 网络错误页（保留兼容，但实际用弹窗） ==========
void Page_NetworkErrorDraw(void)
{
    DrawTopScreen();
    C2D_TargetClear(botTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(botTarget);
    DrawLoadingCard();
}

void Page_NetworkErrorUpdate(touchPosition* touch, u32 kDown)
{
    (void)touch;
    Page_LoadUpdate(kDown);
}

// ========== 无账户页（保留兼容，但实际用弹窗） ==========
void Page_NoAccountDraw(void)
{
    DrawTopScreen();
    C2D_TargetClear(botTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(botTarget);
    DrawNoAccountPopup();
}

void Page_NoAccountUpdate(touchPosition* touch, u32 kDown)
{
    (void)touch;
    (void)kDown;
    // 弹窗处理在Page_LoadUpdate中
}
