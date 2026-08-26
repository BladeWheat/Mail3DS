// main.c - 主程序入口
#include "common.h"
#include "ui.h"
#include "keyboard.h"
#include "accounts.h"
#include "email.h"
#include "network.h"
#include "cache.h"
#include "pages.h"
#include "html_render.h"

AppState g_app;
C3D_RenderTarget* topTarget;
C3D_RenderTarget* botTarget;
u32 g_kUp = 0;

int main(void)
{
    // 初始化图形（必须最先做，否则无法渲染）
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    // 创建渲染目标
    topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    botTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // 初始化系统服务
    cfguInit();       // 系统配置
    mcuHwcInit();     // MCU硬件（电量检测）
    Result romfsRes = romfsInit();  // romfs文件系统（字体等资源）
    if (R_FAILED(romfsRes))
    {
        // romfs挂载失败不退出程序，UI层会使用备用位图字体并跳过图标绘制
        // 常见原因：3dsx文件中未正确嵌入RomFS
    }

    // 加载中文字体（BCFNT）
    UI_InitFont();
    UI_LoadIcons();

    // 初始化键盘和邮件模块（网络和账户在加载页中初始化）
    Email_Init();
    Keyboard_Init();

    // 初始化应用状态
    memset(&g_app, 0, sizeof(AppState));
    g_app.currentPage = PAGE_LOAD;
    g_app.loadProgress = 0;
    g_app.loadStep = 0;
    g_app.loadError = false;
    g_app.frameCount = 0;
    g_app.lastRefreshTime = 0;
    g_app.selectedEmail = -1;
    g_app.selectedFilter = -1;
    g_app.emailsPerPage = 10;
    g_app.listPage = 0;
    g_app.listScroll = 0;
    g_app.notifEnabled = true;
    g_app.notifSound = true;
    g_app.notifLED = true;
    g_app.autoCheckInterval = 2;  // 默认15分钟
    g_app.fetchingMails = false;
    g_app.fetchFinished = false;
    g_app.fetchSuccess = false;
    g_app.fetchError[0] = 0;
    g_app.totalMails = 0;
    g_app.totalPages = 1;
    g_app.uidListCap = 4096;
    g_app.uidList = (u32*)malloc(g_app.uidListCap * sizeof(u32));
    g_app.uidListLoaded = false;
    g_app.uidListCount = 0;
    g_app.uidListAccount = -1;
    g_app.lastTotalMails = 0;
    g_app.lastAutoCheckFrame = 0;
    g_app.needRefresh = true;  // 开启软件时强制检测新邮件

    // 从SD卡加载用户设置（覆盖默认值）
    Settings_Load();

    int prevPage = PAGE_LOAD;  // 上一帧页面，用于防止页面跳转误触
    bool touchWasDown = false;  // 上一帧触摸是否按下

    // 主循环
    while (aptMainLoop())
    {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        g_kUp = hidKeysUp();

        // 左摇杆方向映射到十字键
        if (kDown & KEY_CPAD_UP)    kDown |= KEY_DUP;
        if (kDown & KEY_CPAD_DOWN)  kDown |= KEY_DDOWN;
        if (kDown & KEY_CPAD_LEFT)  kDown |= KEY_DLEFT;
        if (kDown & KEY_CPAD_RIGHT) kDown |= KEY_DRIGHT;

        touchPosition touch;
        hidTouchRead(&touch);

        g_app.frameCount++;

        // START退出（键盘激活时START用于确认输入，不退出）
        if ((kDown & KEY_START) && !g_app.keyboardActive) break;

        // 防止页面跳转误触：
        // 1. 如果本帧页面刚切换，屏蔽触摸
        // 2. 如果触摸还没松开（从上个页面按住过来），屏蔽触摸
        bool touchDown = (kHeld & KEY_TOUCH) != 0;
        if (g_app.currentPage != prevPage || (touchWasDown && !(kDown & KEY_TOUCH)))
        {
            kDown &= ~KEY_TOUCH;
            touch.px = 0;
            touch.py = 0;
        }
        // 页面切换后需要等触摸松开才能再次响应
        if (g_app.currentPage != prevPage)
        {
            touchWasDown = true;
            g_app.focusActive = false;  // 新页面不默认选中
        }
        else
            touchWasDown = touchDown;
        prevPage = g_app.currentPage;

        // 页面更新
        switch (g_app.currentPage)
        {
            case PAGE_LOAD:
                Page_LoadUpdate(kDown);
                break;
            case PAGE_NETWORK_ERROR:
                Page_NetworkErrorUpdate(&touch, kDown);
                break;
            case PAGE_NO_ACCOUNT:
                Page_NoAccountUpdate(&touch, kDown);
                break;
            case PAGE_ACCOUNT_LIST:
                Page_AccountListUpdate(&touch, kDown);
                break;
            case PAGE_ADD_ACCOUNT:
                Page_AddAccountUpdate(&touch, kDown);
                break;
            case PAGE_MAIN:
                Page_MainUpdate(&touch, kDown);
                break;
            case PAGE_COMPOSE:
                Page_ComposeUpdate(&touch, kDown);
                break;
            case PAGE_SENT_LIST:
                Page_SentListUpdate(&touch, kDown);
                break;
            case PAGE_SETTINGS:
                Page_SettingsUpdate(&touch, kDown);
                break;
            case PAGE_NOTIF_SETTINGS:
                Page_NotifSettingsUpdate(&touch, kDown);
                break;
            case PAGE_AUTO_CHECK:
                Page_AutoCheckUpdate(&touch, kDown);
                break;
            case PAGE_ABOUT:
                Page_AboutUpdate(&touch, kDown);
                break;
        }

        // 渲染帧
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        // 调用各页面Draw
        switch (g_app.currentPage)
        {
            case PAGE_LOAD: Page_LoadDraw(); break;
            case PAGE_NETWORK_ERROR: Page_NetworkErrorDraw(); break;
            case PAGE_NO_ACCOUNT: Page_NoAccountDraw(); break;
            case PAGE_ACCOUNT_LIST: Page_AccountListDraw(); break;
            case PAGE_ADD_ACCOUNT: Page_AddAccountDraw(); break;
            case PAGE_MAIN: Page_MainDraw(); break;
            case PAGE_COMPOSE: Page_ComposeDraw(); break;
            case PAGE_SENT_LIST: Page_SentListDraw(); break;
            case PAGE_SETTINGS: Page_SettingsDraw(); break;
            case PAGE_NOTIF_SETTINGS: Page_NotifSettingsDraw(); break;
            case PAGE_AUTO_CHECK: Page_AutoCheckDraw(); break;
            case PAGE_ABOUT: Page_AboutDraw(); break;
        }

        C3D_FrameEnd(0);
    }

    // 退出前保存邮件缓存到SD卡
    if (g_app.accountCount > 0 && g_app.emailCount > 0)
    {
        int accIdx = -1;
        if (g_app.selectedFilter >= 0 && g_app.selectedFilter < MAX_ACCOUNTS &&
            g_app.accounts[g_app.selectedFilter].added)
            accIdx = g_app.selectedFilter;
        else
        {
            for (int i = 0; i < MAX_ACCOUNTS; i++)
                if (g_app.accounts[i].added) { accIdx = i; break; }
        }
        if (accIdx >= 0)
        {
            for (int i = 0; i < g_app.emailCount; i++)
            {
                Email* m = &g_app.emails[i];
                CacheEntry ce;
                memset(&ce, 0, sizeof(ce));
                ce.imapUid = m->uid;
                ce.imapSeq = m->imapSeq;
                strncpy(ce.sender, m->sender, sizeof(ce.sender) - 1);
                strncpy(ce.fromAddr, m->fromAddr, sizeof(ce.fromAddr) - 1);
                strncpy(ce.subject, m->subject, sizeof(ce.subject) - 1);
                strncpy(ce.contentType, m->contentType, sizeof(ce.contentType) - 1);
                strncpy(ce.preview, m->preview, sizeof(ce.preview) - 1);
                strncpy(ce.date, m->date, sizeof(ce.date) - 1);
                ce.year = m->year;
                ce.month = m->month;
                ce.day = m->day;
                ce.hour = m->hour;
                ce.minute = m->minute;
                ce.timestamp = m->timestamp;
                strncpy(ce.rawDate, m->rawDate, sizeof(ce.rawDate) - 1);
                ce.unread = m->unread;
                ce.hasAttachment = m->hasAttachment;
                ce.bodyCached = m->bodyLoaded;
                Cache_SaveHeader(g_app.accounts[accIdx].email, &ce);
            }
        }
    }

    // 清理
    Network_Shutdown();
    UI_FreeIcons();
    UI_FiniFont();
    romfsExit();
    mcuHwcExit();
    cfguExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    return 0;
}
