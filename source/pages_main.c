// pages_main.c - 主界面和写邮件
#include "pages.h"
#include "ui.h"
#include "keyboard.h"
#include "network.h"
#include "email.h"
#include "accounts.h"
#include "cache.h"
#include "html_render.h"

// 邮件拉取线程
static Thread s_fetchThread = NULL;
static FetchResult s_fetchResult;
static int s_fetchAccountIdx = -1; // 记录当前拉取的账户索引，用于检测账户切换
static bool s_cachePathLogged = false; // 缓存路径日志是否已记录

struct FetchThreadArg {
    int accountIdx;
    int page;
    int perPage;
};

static void fetch_mails_thread(void* arg)
{
    struct FetchThreadArg* fa = (struct FetchThreadArg*)arg;
    int idx = fa->accountIdx;
    int page = fa->page;
    int perPage = fa->perPage;
    free(fa);

    // 调试日志已禁用以提升加载速度

    Network_FetchMailsPage(idx, page, perPage, &s_fetchResult);

}

// 获取当前选中账户索引
#define GET_CUR_ACC() ({ \
    int _idx = -1; \
    if (g_app.selectedFilter >= 0 && g_app.selectedFilter < MAX_ACCOUNTS && \
        g_app.accounts[g_app.selectedFilter].added) \
        _idx = g_app.selectedFilter; \
    else { \
        for (int _i = 0; _i < MAX_ACCOUNTS; _i++) \
            if (g_app.accounts[_i].added) { _idx = _i; break; } \
    } \
    _idx; \
})

// 获取当前用于拉取邮件的账户索引
// 所有邮箱都会正常发起网络请求尝试加载
static int GetFetchAccountIdx(void)
{
    if (g_app.selectedFilter >= 0 && g_app.selectedFilter < MAX_ACCOUNTS &&
        g_app.accounts[g_app.selectedFilter].added)
        return g_app.selectedFilter;
    for (int i = 0; i < MAX_ACCOUNTS; i++)
    {
        if (g_app.accounts[i].added) return i;
    }
    return -1;
}

// 检查当前账户是否为非QQ邮箱（用于fetch失败后显示限制提示）
static bool IsNonQQAccount(int accIdx)
{
    if (accIdx < 0 || accIdx >= MAX_ACCOUNTS) return false;
    return g_app.accounts[accIdx].type != ACC_QQ;
}

// 从内存UID列表+本地缓存加载一页邮件（不联网）
// 返回加载到的邮件数，-1表示UID列表不可用或一封都没缓存
// complete输出：true=该页全部缓存无需联网，false=部分缓存需要后台补齐
static int LoadPageFromCache(int accountIdx, int page, int perPage, bool* complete)
{
    if (complete) *complete = false;

    if (!g_app.uidListLoaded || g_app.uidListCount <= 0 ||
        g_app.uidListAccount != accountIdx || !g_app.uidList)
        return -1;

    int endSeq = g_app.uidListCount - page * perPage;
    int startSeq = endSeq - perPage + 1;
    if (startSeq < 1) startSeq = 1;
    if (endSeq < 1) endSeq = 1;
    if (startSeq > endSeq) startSeq = endSeq;

    // 单次遍历：检查缓存是否完整，同时加载数据
    g_app.emailCount = 0;
    int loaded = 0;
    bool allCached = true;

    for (int seq = endSeq; seq >= startSeq; seq--)
    {
        int uidIdx = seq - 1;
        if (uidIdx < 0 || uidIdx >= g_app.uidListCount) { allCached = false; continue; }
        u32 uid = g_app.uidList[uidIdx];
        if (uid == 0) { allCached = false; continue; }

        CacheEntry ce;
        memset(&ce, 0, sizeof(ce));
        if (Cache_LoadHeader(g_app.accounts[accountIdx].email, uid, &ce) != 0)
        {
            allCached = false;
            continue;
        }

        // 缓存存在，直接加载到邮件列表（即使timestamp=0也加载）
        if (loaded < MAX_EMAILS)
        {
            Email* mail = &g_app.emails[loaded];
            memset(mail, 0, sizeof(Email));
            mail->id = loaded + 1;
            mail->accountIndex = accountIdx;
            mail->imapSeq = seq;
            mail->uid = uid;
            strncpy(mail->sender, ce.sender, sizeof(mail->sender) - 1);
            strncpy(mail->fromAddr, ce.fromAddr, sizeof(mail->fromAddr) - 1);
            strncpy(mail->subject, ce.subject, sizeof(mail->subject) - 1);
            strncpy(mail->contentType, ce.contentType, sizeof(mail->contentType) - 1);
            strncpy(mail->preview, ce.preview, sizeof(mail->preview) - 1);
            mail->year = ce.year;
            mail->month = ce.month;
            mail->day = ce.day;
            mail->hour = ce.hour;
            mail->minute = ce.minute;
            mail->timestamp = ce.timestamp;
            strncpy(mail->rawDate, ce.rawDate, sizeof(mail->rawDate) - 1);
            mail->unread = ce.unread ? true : false;
            mail->hasAttachment = ce.hasAttachment ? true : false;
            mail->bodyLoaded = ce.bodyCached ? true : false;
            Email_FormatDate(mail->year, mail->month, mail->day,
                             mail->hour, mail->minute,
                             mail->date, sizeof(mail->date));
            loaded++;
        }
    }

    // 有缓存的邮件先显示，即使部分未缓存也不阻塞（缺失的邮件由后台刷新补齐）
    if (loaded == 0)
    {
        g_app.emailCount = 0;
        return -1; // 一封都没缓存，需要联网拉取
    }

    if (complete) *complete = allCached;

    // seq从大到小遍历，UID列表已按日期排序（旧到新），从末尾取即最新在前
    g_app.emailCount = loaded;
    return loaded;
}

// 新邮件检测线程
static Thread s_checkThread = NULL;
static struct { bool finished; bool hasNew; int total; } s_checkResult;

static void check_new_mails_thread(void* arg)
{
    int accIdx = (int)(intptr_t)arg;
    int total = Network_GetTotalMails(accIdx);
    s_checkResult.total = total;
    s_checkResult.hasNew = (total > 0 && total != g_app.lastTotalMails);
    s_checkResult.finished = true;
}

static void start_check_new_mails(void)
{
    if (s_checkThread) return;
    int accIdx = GetFetchAccountIdx();
    if (accIdx < 0) return; // 没有可用账户
    s_checkResult.finished = false;
    s_checkResult.hasNew = false;
    s_checkResult.total = -1;
    s_checkThread = threadCreate(check_new_mails_thread,
                                (void*)(intptr_t)accIdx,
                                64*1024, 0x18, -1, false);
}

// 主界面焦点区域：0=标签栏，1=邮件列表，2=底部按钮
static int s_mainFocusArea = 1;
static int s_bottomFocus = 0;  // 底部按钮焦点：0=写邮件，1=刷新，2=账户
static bool s_readingMail = false;  // 正文阅读模式
static int s_lastPerPage = 10;  // 跟踪每页邮件数变化
static bool s_bodyLoading = false;  // 正文正在从服务器加载（用于显示加载提示）
static int s_bodyLoadingIdx = -1;   // 正在加载正文的邮件索引
static int s_bodyScroll = 0;        // 正文滚动行偏移
static bool s_showDevDialog = false; // "开发中"弹窗
static bool s_useHtmlRender = false; // 当前是否使用HTML渲染
static int s_htmlTotalLines = 0;    // HTML渲染总行数
static char* s_fullBody = NULL;     // 完整正文缓存（不受mail->body 1024字节限制）
static int s_fullBodyLen = 0;
static int s_pressedBottomBtn = -1;  // 触摸按下的底部按钮（-1=无）

// 纯文本预换行（避免每帧重新排版导致卡顿）
#define MAX_WRAPPED_LINES 512
static char s_wrappedLines[MAX_WRAPPED_LINES][384];
static int s_wrappedLineCount = 0;
static u8 s_wrapQuote[MAX_WRAPPED_LINES];  // 每行是否引用行
static bool s_wrapDirty = true;            // 正文变化时需要重新换行

// 尝试加载邮件HTML正文并初始化HTML渲染器，成功返回true
static bool try_load_html_render(Email* mail)
{
    s_useHtmlRender = false;

    // 先释放之前的完整正文缓存
    if (s_fullBody) { free(s_fullBody); s_fullBody = NULL; s_fullBodyLen = 0; }

    if (!mail || mail->uid == 0) return false;

    // 读取完整纯文本正文（不受mail->body 1024字节限制）
    char* fullPlain = Cache_LoadBodyPlain(g_app.accounts[mail->accountIndex].email, mail->uid);
    if (fullPlain)
    {
        s_fullBody = fullPlain;
        s_fullBodyLen = (int)strlen(fullPlain);
        s_wrapDirty = true;  // 正文变化，需要重新预换行
    }

    // 尝试加载HTML
    BodyMeta bmeta;
    if (Cache_LoadBodyMeta(g_app.accounts[mail->accountIndex].email,
                           mail->uid, &bmeta) == 0 && bmeta.html_size > 0)
    {
        char htmlPath[512];
        char base[256];
        Cache_GetAccountPath(g_app.accounts[mail->accountIndex].email, base, sizeof(base));
        snprintf(htmlPath, sizeof(htmlPath),
                 "%s/body_cache/%lu/body_html.html",
                 base, (unsigned long)mail->uid);

        FILE* fp = fopen(htmlPath, "rb");
        if (!fp) return false;

        int htmlLen = bmeta.html_size;
        if (htmlLen > 65536) htmlLen = 65536;
        char* htmlData = (char*)malloc(htmlLen + 1);
        if (!htmlData) { fclose(fp); return false; }

        size_t rd = fread(htmlData, 1, htmlLen, fp);
        htmlData[rd] = 0;
        fclose(fp);

        // 设置图片上下文，用于CID图片查找
        HtmlRender_SetEmailContext(g_app.accounts[mail->accountIndex].email, mail->uid);

        int totalLines = HtmlRender_Parse(htmlData, (int)rd, 400 - 32, 0.45f);
        free(htmlData);

        if (totalLines > 0)
        {
            s_useHtmlRender = true;
            s_htmlTotalLines = totalLines;
            return true;
        }
    }
    return false;
}

// 写邮件页焦点：0=收件人，1=主题，2=正文，3=取消，4=发送
static int s_composeFocus = 0;

// 邮件列表滚动条拖动
static bool s_scrollDragging = false;
static int s_scrollDragOffset = 0;
// 邮件列表触摸滑动
static bool s_listTouching = false;
static int s_listTouchPrevY = 0;
static int s_listTouchStartY = 0;
static bool s_listMoved = false;

// 下拉按钮和弹窗
static C2D_SpriteSheet s_dropSheet = NULL;
static C2D_Image s_dropIcon;
// 邮件信封图标
static C2D_SpriteSheet s_mailReadSheet = NULL;
static C2D_SpriteSheet s_mailUnreadSheet = NULL;
static C2D_Image s_mailReadIcon;
static C2D_Image s_mailUnreadIcon;
static bool s_dropdownOpen = false;       // 弹窗是否打开
static float s_dropAngle = 0.0f;          // 当前旋转角度（度）
static int s_dropAnimFrame = -1;          // 动画帧计数（-1=无动画）
static bool s_dropAnimOpening = false;    // 动画方向：true=打开(顺时针), false=关闭(逆时针)
static bool s_pressedDropBtn = false;     // 下拉按钮是否被按下（等待抬起执行）
static int s_dropAction = 0;              // 按下时记录的操作：0=无，1=切换弹窗，2=点外部关闭，3=点菜单项
static int s_dropMenuItem = -1;           // 按下时记录的菜单项索引
#define DROP_ANIM_FRAMES 12               // 0.2秒 @60fps

// 纯文本预换行：只在正文变化时执行一次，避免每帧全文排版卡顿
static void wrap_plain_text(const char* text, float bodyScale, int maxWidth)
{
    s_wrappedLineCount = 0;
    if (!text || !*text) return;

    char line[384];
    int lineLen = 0;
    int lastSpace = -1;
    bool isQuote = false;
    int indent = 0;
    bool lastLineEmpty = false;  // 上一行是否为空行（用于合并连续空行）

    #define ADD_WRAPPED_LINE() do { \
        if (s_wrappedLineCount < MAX_WRAPPED_LINES) { \
            line[lineLen] = 0; \
            strncpy(s_wrappedLines[s_wrappedLineCount], line, 383); \
            s_wrappedLines[s_wrappedLineCount][383] = 0; \
            s_wrapQuote[s_wrappedLineCount] = isQuote ? 1 : 0; \
            s_wrappedLineCount++; \
        } \
        lastLineEmpty = (lineLen == 0); \
        lineLen = 0; \
        lastSpace = -1; \
    } while(0)

    while (*text)
    {
        if (*text == '\r') { text++; continue; }

        if (*text == '\n')
        {
            if (lineLen == 0)
            {
                // 连续空行只保留一个
                if (!lastLineEmpty)
                {
                    isQuote = false;
                    indent = 0;
                    ADD_WRAPPED_LINE(); // 空行
                }
            }
            else
            {
                ADD_WRAPPED_LINE();
            }
            isQuote = false;
            indent = 0;
            text++;
            continue;
        }

        if (lineLen == 0 && *text == '>')
        {
            isQuote = true;
            indent = 12;
            text++;
            while (*text == ' ') text++;
            continue;
        }

        line[lineLen++] = *text;
        line[lineLen] = 0;

        if (*text == ' ')
            lastSpace = lineLen - 1;

        float w = UI_MeasureText(bodyScale, line);
        if (w > maxWidth - indent && lineLen > 1)
        {
            if (lastSpace > 0)
            {
                lineLen = lastSpace;
                line[lineLen] = 0;
                ADD_WRAPPED_LINE();
                text++;
                while (*text == ' ') text++;
            }
            else
            {
                lineLen--;
                line[lineLen] = 0;
                ADD_WRAPPED_LINE();
                text++;
            }
            if (isQuote) indent = 12;
        }
        else
        {
            text++;
        }
    }

    if (lineLen > 0)
        ADD_WRAPPED_LINE();

    #undef ADD_WRAPPED_LINE
}

// ========== 主界面 ==========
void Page_MainDraw(void)
{
    // 上屏：邮件详情（米黄背景）
    C2D_TargetClear(topTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(topTarget);

    // 实时状态栏
    UI_DrawStatusBarMain();

    if (g_app.selectedEmail >= 0 && g_app.selectedEmail < g_app.emailCount)
    {
        Email* mail = &g_app.emails[g_app.selectedEmail];

        if (s_readingMail)
        {
            // 阅读模式：显示邮件基础信息和正文
            // 主题
            UI_DrawText(16, 40, 0.55f, COLOR_TEXT_PRIMARY, mail->subject);

            // 发件人/时间行
            UI_DrawText(16, 64, 0.42f, COLOR_BLUE, mail->sender);
            UI_DrawText(SCREEN_TOP_W - 80, 64, 0.38f, COLOR_TEXT_SECONDARY, mail->date);

            // 分割线
            UI_DrawRect(16, 84, SCREEN_TOP_W - 32, 1, COLOR_BORDER_GRAY);

            // 正文区域：增强排版渲染，可滚动
            if (mail->bodyLoaded && (mail->body[0] || s_useHtmlRender))
            {
                // HTML富文本渲染优先
                if (s_useHtmlRender)
                {
                    int totalLines = 0;
                    HtmlRender_Render(16, 92, SCREEN_TOP_W - 32, 148,
                                      s_bodyScroll, 16.0f, &totalLines);
                    // 滚动范围限制
                    int maxVisible = 148 / 16;
                    int maxScroll = totalLines - maxVisible;
                    if (maxScroll < 0) maxScroll = 0;
                    if (s_bodyScroll > maxScroll) s_bodyScroll = maxScroll;
                }
                else
                {
                // 纯文本渲染：使用预换行结果，避免每帧全文排版
                float bodyScale = 0.45f;
                float lineHeight = 16.0f;
                int startX = 16;
                int startY = 92;
                int maxWidth = SCREEN_TOP_W - 32;
                int maxLines = (240 - startY) / (int)lineHeight;

                const char* text = s_fullBody ? s_fullBody : mail->body;

                // 正文变化时只换行一次
                if (s_wrapDirty)
                {
                    wrap_plain_text(text, bodyScale, maxWidth);
                    s_wrapDirty = false;
                }

                u32 quoteColor = C2D_Color32(0x66, 0x66, 0x66, 0xFF);

                // 只渲染可见行
                int endLine = s_bodyScroll + maxLines;
                if (endLine > s_wrappedLineCount) endLine = s_wrappedLineCount;
                for (int i = s_bodyScroll; i < endLine; i++)
                {
                    int visY = startY + (i - s_bodyScroll) * (int)lineHeight;
                    int x = startX + (s_wrapQuote[i] ? 12 : 0);
                    u32 col = s_wrapQuote[i] ? quoteColor : COLOR_TEXT_PRIMARY;
                    UI_DrawText(x, visY, bodyScale, col, s_wrappedLines[i]);
                }

                int maxScroll = s_wrappedLineCount - maxLines;
                if (maxScroll < 0) maxScroll = 0;
                if (s_bodyScroll > maxScroll) s_bodyScroll = maxScroll;
                } // end else (纯文本渲染)
            }
            else if (!mail->bodyLoaded || s_bodyLoading)
            {
                UI_DrawTextCenter(SCREEN_TOP_W / 2, 122, 0.5f, COLOR_TEXT_SECONDARY,
                    s_bodyLoading ? "正在加载邮件内容..." : "正在加载正文...");
            }
        }
        else
        {
            // 选中但未进入阅读模式：不显示邮件基础信息，只显示提示
            UI_DrawTextCenter(SCREEN_TOP_W / 2, 110, 0.45f, COLOR_TEXT_SECONDARY, "按A键查看正文");
        }
    }
    else
    {
        UI_DrawTextCenter(SCREEN_TOP_W / 2, 110, 0.55f, COLOR_TEXT_SECONDARY, "请选择一封邮件");
    }

    // 上屏底部翻页提示梯形框（非阅读模式显示）
    if (!s_readingMail)
    {
        u32 trapColor = C2D_Color32(0xE0, 0xE0, 0xE0, 0xFF);
        u32 trapBorder = C2D_Color32(0xBD, 0xBD, 0xBD, 0xFF);
        int trapY = 215;
        int trapH = 25;

        // 左侧梯形（直角在左，上下颠倒：顶部窄底部宽）
        for (int py = 0; py < trapH; py++)
        {
            float ratio = (float)py / (float)(trapH - 1);
            int w = 75 + (int)(20 * ratio); // 顶部75，底部95
            C2D_DrawRectSolid(0, trapY + py, 0, w, 1, trapColor);
        }
        // 边框
        C2D_DrawRectSolid(0, trapY, 0, 75, 1, trapBorder);      // 上边框
        C2D_DrawRectSolid(0, trapY, 0, 1, trapH, trapBorder);    // 左边框
        C2D_DrawRectSolid(0, trapY + trapH - 1, 0, 95, 1, trapBorder); // 下边框
        // 文字左右上下居中（用矩形近似梯形区域）
        UI_DrawTextCenterInRectDepth(0, trapY, 95, trapH, 0, 0.4f, COLOR_TEXT_PRIMARY, "L键 上一页");

        // 右侧梯形（直角在右，上下颠倒：顶部窄底部宽）
        for (int py = 0; py < trapH; py++)
        {
            float ratio = (float)py / (float)(trapH - 1);
            int w = 75 + (int)(20 * ratio); // 顶部75，底部95
            int x = SCREEN_TOP_W - w;
            C2D_DrawRectSolid(x, trapY + py, 0, w, 1, trapColor);
        }
        // 边框
        C2D_DrawRectSolid(SCREEN_TOP_W - 75, trapY, 0, 75, 1, trapBorder);      // 上边框
        C2D_DrawRectSolid(SCREEN_TOP_W - 1, trapY, 0, 1, trapH, trapBorder);     // 右边框
        C2D_DrawRectSolid(SCREEN_TOP_W - 95, trapY + trapH - 1, 0, 95, 1, trapBorder); // 下边框
        // 文字左右上下居中（用矩形近似梯形区域）
        UI_DrawTextCenterInRectDepth(SCREEN_TOP_W - 95, trapY, 95, trapH, 0, 0.4f, COLOR_TEXT_PRIMARY, "R键 下一页");
    }

    // 下屏
    C2D_TargetClear(botTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(botTarget);

    u32 barBg = C2D_Color32(0xED, 0xE6, 0xB0, 0xFF);

    // 邮件列表（顺滑像素滚动，先画，上下栏后画以遮挡溢出）
    int filteredIdx[MAX_EMAILS];
    int filteredCount = 0;
    for (int i = 0; i < g_app.emailCount; i++)
    {
        if (g_app.selectedFilter < 0 || g_app.emails[i].accountIndex == g_app.selectedFilter)
            filteredIdx[filteredCount++] = i;
    }

    #define MAIL_ITEM_H 56
    #define MAIL_VIS 3
    int listY = 39;
    float maxScrollF = (filteredCount - MAIL_VIS) * (float)MAIL_ITEM_H;
    if (maxScrollF < 0) maxScrollF = 0;
    float scrollF = g_app.listScroll;
    if (scrollF > maxScrollF) scrollF = maxScrollF;
    if (scrollF < 0) scrollF = 0;
    g_app.listScroll = scrollF;

    int firstItem = (int)(scrollF / MAIL_ITEM_H);
    int lastItem = firstItem + MAIL_VIS + 1;
    if (lastItem > filteredCount) lastItem = filteredCount;

    // 内容区参数
    const float iconX = 8.0f;
    const float iconSize = 32.0f;
    const float contentX = 48.0f;
    const float contentW = 264.0f;  // 312 - 48

    for (int fi = firstItem; fi < lastItem; fi++)
    {
        int i = filteredIdx[fi];
        Email* mail = &g_app.emails[i];
        float y = listY - scrollF + fi * MAIL_ITEM_H;

        // 选中背景
        if (i == g_app.selectedEmail)
        {
            C2D_DrawRectSolid(0, y, 0.0f, SCREEN_BOT_W, MAIL_ITEM_H, COLOR_SELECTED_BG);
        }

        // 左侧信封图标（32x32，垂直居中）
        {
            if (!s_mailReadSheet)
            {
                s_mailReadSheet = C2D_SpriteSheetLoad("romfs:/mail_read.t3x");
                if (s_mailReadSheet) s_mailReadIcon = C2D_SpriteSheetGetImage(s_mailReadSheet, 0);
            }
            if (!s_mailUnreadSheet)
            {
                s_mailUnreadSheet = C2D_SpriteSheetLoad("romfs:/mail_unread.t3x");
                if (s_mailUnreadSheet) s_mailUnreadIcon = C2D_SpriteSheetGetImage(s_mailUnreadSheet, 0);
            }
            float ex = iconX, ey = y + (MAIL_ITEM_H - iconSize) / 2.0f;
            float envScale = iconSize / 48.0f;
            C2D_Image envImg = mail->unread ? s_mailUnreadIcon : s_mailReadIcon;
            C2D_SpriteSheet sheet = mail->unread ? s_mailUnreadSheet : s_mailReadSheet;
            if (sheet && envImg.subtex)
                C2D_DrawImageAt(envImg, ex, ey, 0.0f, NULL, envScale, envScale);
        }

        // 第一行：发件人（最高优先级，15px加粗深色）
        u32 senderColor = mail->unread ? COLOR_TEXT_PRIMARY : COLOR_TEXT_SECONDARY;
        UI_DrawTextTruncated(contentX, y + 5, 0.0f, 0.55f, senderColor,
                             mail->sender, contentW, mail->unread);

        // 第二行：主题（13px常规中灰）
        u32 subjColor = mail->unread ? C2D_Color32(0x33,0x33,0x33,0xFF) : COLOR_TEXT_SECONDARY;
        UI_DrawTextTruncated(contentX, y + 23, 0.0f, 0.48f, subjColor,
                             mail->subject, contentW, false);

        // 第三行：日期 + 正文预览 + 附件
        {
            float tx = contentX;
            // 日期（11px浅灰，完整显示）
            UI_DrawTextDepth(tx, y + 41, 0.0f, 0.4f, COLOR_TEXT_SECONDARY, mail->date);
            tx += UI_MeasureText(0.4f, mail->date);

            // 正文预览
            if (mail->preview[0])
            {
                // 间隔符
                UI_DrawTextDepth(tx, y + 41, 0.0f, 0.4f, COLOR_TEXT_SECONDARY, " · ");
                tx += UI_MeasureText(0.4f, " · ");

                // 附件图标预留宽度
                float attachW = mail->hasAttachment ? 16.0f : 0.0f;
                float previewW = contentW - (tx - contentX) - attachW;
                if (previewW > 10.0f)
                {
                    UI_DrawTextTruncated(tx, y + 41, 0.0f, 0.4f, COLOR_TEXT_SECONDARY,
                                         mail->preview, previewW, false);
                }
            }

            // 附件图标（最右侧，回形针）
            if (mail->hasAttachment)
            {
                float ax = contentX + contentW - 12.0f;
                // 简单绘制回形针形状（竖线+圆弧）
                C2D_DrawRectSolid(ax, y + 42, 0.0f, 2, 10, COLOR_TEXT_SECONDARY);
            }
        }

        // 分隔线
        if (fi < lastItem - 1)
        {
            UI_DrawRect(8, y + MAIL_ITEM_H, 304, 1, COLOR_BORDER_GRAY);
        }
    }

    // 右侧滚动条
    if (filteredCount > MAIL_VIS)
    {
        int trackX = 313, trackY = 43, trackH = 160;
        int thumbH = trackH * MAIL_VIS / filteredCount;
        if (thumbH < 20) thumbH = 20;
        int thumbY = trackY;
        if (maxScrollF > 0)
            thumbY = trackY + (int)((trackH - thumbH) * scrollF / maxScrollF);
        C2D_DrawRectSolid(trackX, trackY, 0, 3, trackH, C2D_Color32(0xE0,0xDC,0xC8,0xFF));
        C2D_DrawRectSolid(trackX, thumbY, 0, 3, thumbH, C2D_Color32(0x9E,0x9E,0x9E,0xFF));
        C2D_DrawRectSolid(trackX, thumbY, 0, 1, thumbH, C2D_Color32(0xBD,0xBD,0xBD,0xFF));
    }

    // 加载/空状态提示（有缓存邮件时不覆盖列表，后台静默补齐）
    if (g_app.fetchingMails && filteredCount == 0)
    {
        UI_DrawTextCenterDepth(SCREEN_BOT_W / 2, 110, 0.0f, 0.45f, COLOR_TEXT_SECONDARY, "正在加载邮件...");
    }
    else if (!g_app.fetchingMails && g_app.fetchFinished && !g_app.fetchSuccess && filteredCount == 0)
    {
        int curAcc = GetFetchAccountIdx();
        if (IsNonQQAccount(curAcc))
        {
            // 非QQ邮箱加载失败：居中显示红色不支持提示，分两行
            UI_DrawTextCenterDepth(SCREEN_BOT_W / 2, 100, 0.0f, 0.45f, COLOR_RED,
                "由于该账户邮箱的安全设置，");
            UI_DrawTextCenterDepth(SCREEN_BOT_W / 2, 124, 0.0f, 0.45f, COLOR_RED,
                "不支持读取邮件");
        }
        else
        {
            UI_DrawTextDepth(16, 100, 0.0f, 0.4f, COLOR_RED, g_app.fetchError);
        }
    }
    else if (filteredCount == 0)
    {
        UI_DrawTextCenterDepth(SCREEN_BOT_W / 2, 110, 0.0f, 0.45f, COLOR_TEXT_SECONDARY, "暂无邮件");
    }

    // 页码显示（右下角，列表区域内）
    if (g_app.fetchFinished && g_app.fetchSuccess && g_app.totalMails > 0)
    {
        char pageInfo[32];
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d页", g_app.listPage + 1, g_app.totalPages);
        UI_DrawTextDepth(270, 193, 0.0f, 0.35f, COLOR_TEXT_SECONDARY, pageInfo);
    }

    // 上栏（中间层 depth=0.5）
    C2D_DrawRectSolid(0, 0, 0.5f, SCREEN_BOT_W, 36, barBg);
    C2D_DrawRectSolid(0, 36, 0.5f, SCREEN_BOT_W, 1, C2D_Color32(0x00,0x00,0x00,0x30));
    C2D_DrawRectSolid(0, 37, 0.5f, SCREEN_BOT_W, 1, C2D_Color32(0x00,0x00,0x00,0x18));
    C2D_DrawRectSolid(0, 38, 0.5f, SCREEN_BOT_W, 1, C2D_Color32(0x00,0x00,0x00,0x0A));

    // 上栏标题
    {
        const char* title = "";
        if (g_app.selectedFilter >= 0 && g_app.selectedFilter < MAX_ACCOUNTS &&
            g_app.accounts[g_app.selectedFilter].added)
            title = g_app.accounts[g_app.selectedFilter].email;
        else
        {
            // 默认选中第一个已添加的账户
            for (int i = 0; i < MAX_ACCOUNTS; i++)
            {
                if (g_app.accounts[i].added)
                {
                    g_app.selectedFilter = i;
                    title = g_app.accounts[i].email;
                    break;
                }
            }
        }
        UI_DrawTextDepth(12, 13, 0.5f, 0.45f, COLOR_TEXT_PRIMARY, title);
    }

    // 下拉按钮
    if (!s_dropSheet)
    {
        s_dropSheet = C2D_SpriteSheetLoad("romfs:/dropdown.t3x");
        if (s_dropSheet) s_dropIcon = C2D_SpriteSheetGetImage(s_dropSheet, 0);
    }
    if (s_dropSheet && s_dropIcon.subtex)
    {
        float rad = s_dropAngle * 3.14159265f / 180.0f;
        float scale = 24.0f / 32.0f;
        C2D_DrawImageAtRotated(s_dropIcon, 302, 18, 0.5f, rad, NULL, scale, scale);
    }

    // 底部按钮栏（中间层 depth=0.5）
    C2D_DrawRectSolid(0, 209, 0.5f, SCREEN_BOT_W, 1, C2D_Color32(0x00,0x00,0x00,0x0A));
    C2D_DrawRectSolid(0, 210, 0.5f, SCREEN_BOT_W, 1, C2D_Color32(0x00,0x00,0x00,0x18));
    C2D_DrawRectSolid(0, 211, 0.5f, SCREEN_BOT_W, 1, C2D_Color32(0x00,0x00,0x00,0x30));
    C2D_DrawRectSolid(0, 212, 0.5f, SCREEN_BOT_W, 28, barBg);

    // 紧凑型按钮
    {
        float by = 212, bh = 28;
        float bw = SCREEN_BOT_W / 3.0f;
        const char* labels[] = {"写邮件", "刷新", "账户"};
        u32 btnNormal = C2D_Color32(0xF5,0xF0,0xD8,0xFF);
        u32 btnPressed = C2D_Color32(0xD8,0xD2,0xB0,0xFF);
        u32 btnTop = C2D_Color32(0xFF,0xFF,0xFF,0x60);
        u32 btnBottom = C2D_Color32(0x00,0x00,0x00,0x20);
        u32 sepColor = C2D_Color32(0xB8,0xB2,0x98,0xFF);

        for (int b = 0; b < 3; b++)
        {
            float bx = b * bw;
            bool pressed = (s_pressedBottomBtn == b);
            u32 bg = pressed ? btnPressed : btnNormal;

            C2D_DrawRectSolid(bx, by, 0.5f, bw, bh, bg);
            C2D_DrawRectSolid(bx, by, 0.5f, bw, 1, btnTop);
            C2D_DrawRectSolid(bx, by + bh - 1, 0.5f, bw, 1, btnBottom);

            if (b < 2)
                C2D_DrawRectSolid(bx + bw - 1, by + 2, 0.5f, 1, bh - 4, sepColor);

            UI_DrawTextCenterDepth(bx + bw / 2, by + 7, 0.5f, 0.45f, COLOR_TEXT_PRIMARY, labels[b]);
        }
    }

    // 下拉弹窗（最上层，最后绘制）
    if (s_dropdownOpen || s_dropAnimFrame >= 0)
    {
        // 构建菜单项
        #define MAX_MENU_ITEMS 10
        const char* menuItems[MAX_MENU_ITEMS];
        int menuCount = 0;
        int menuAccountIdx[MAX_MENU_ITEMS]; // -1=非账户项, >=0=账户索引
        int menuType[MAX_MENU_ITEMS];       // 0=账户, 1=未读, 2=已发送, 3=收藏夹

        for (int i = 0; i < MAX_ACCOUNTS; i++)
        {
            if (g_app.accounts[i].added)
            {
                menuItems[menuCount] = g_app.accounts[i].email;
                menuAccountIdx[menuCount] = i;
                menuType[menuCount] = 0;
                menuCount++;
            }
        }

        // 分隔线位置（在账户项之后）
        int sepIndex = menuCount;

        menuItems[menuCount] = "未读邮件";
        menuAccountIdx[menuCount] = -1;
        menuType[menuCount] = 1;
        menuCount++;

        menuItems[menuCount] = "已发送邮件";
        menuAccountIdx[menuCount] = -1;
        menuType[menuCount] = 2;
        menuCount++;

        menuItems[menuCount] = "收藏夹";
        menuAccountIdx[menuCount] = -1;
        menuType[menuCount] = 3;
        menuCount++;

        float itemH = 26.0f;
        float px = 156, py = 26, pw = 160;
        float ph = menuCount * itemH + 4;

        // 阴影
        C2D_DrawRectSolid(px + 2, py + 2, 1.0f, pw, ph, C2D_Color32(0,0,0,40));
        // 白色背景（depth=1.0f覆盖下层内容）
        C2D_DrawRectSolid(px, py, 1.0f, pw, ph, COLOR_WHITE);
        // 边框
        UI_DrawRect(px, py, pw, ph, COLOR_BORDER_GRAY);

        // 菜单项
        for (int i = 0; i < menuCount; i++)
        {
            float iy = py + 2 + i * itemH;
            bool selected = false;
            if (menuType[i] == 0)
            {
                if (menuAccountIdx[i] >= 0 && g_app.selectedFilter == menuAccountIdx[i]) selected = true;
            }
            if (selected)
                C2D_DrawRectSolid(px + 2, iy, 1.0f, pw - 4, itemH, C2D_Color32(0xE3,0xF2,0xFD,0xFF));

            UI_DrawTextDepth(px + 10, iy + 7, 1.0f, 0.42f, selected ? COLOR_BLUE : COLOR_TEXT_PRIMARY, menuItems[i]);

            // 分隔线
            if (i == sepIndex - 1)
                C2D_DrawRectSolid(px + 6, iy + itemH, 1.0f, pw - 12, 1, COLOR_BORDER_GRAY);
        }
    }

    // "开发中"弹窗
    if (s_showDevDialog)
    {
        // 半透明遮罩（最上层）
        C2D_DrawRectSolid(0, 0, 1.0f, 320, 240, C2D_Color32(0, 0, 0, 128));
        // 弹窗白色背景
        int dw = 240, dh = 80;
        int dx = (320 - dw) / 2, dy = (240 - dh) / 2;
        C2D_DrawRectSolid(dx, dy, 1.0f, dw, dh, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
        // 蓝色边框
        u32 borderColor = COLOR_BLUE;
        C2D_DrawRectSolid(dx, dy, 1.0f, dw, 1, borderColor);
        C2D_DrawRectSolid(dx, dy + dh - 1, 1.0f, dw, 1, borderColor);
        C2D_DrawRectSolid(dx, dy, 1.0f, 1, dh, borderColor);
        C2D_DrawRectSolid(dx + dw - 1, dy, 1.0f, 1, dh, borderColor);
        // 黑色文字，分两行居中
        UI_DrawTextCenterDepth(160, dy + 16, 1.0f, 0.5f, C2D_Color32(0,0,0,0xFF), "还在开发中");
        UI_DrawTextCenterDepth(160, dy + 44, 1.0f, 0.42f, C2D_Color32(0,0,0,0xFF), "该版本不支持该功能");
    }
}

void Page_MainUpdate(touchPosition* touch, u32 kDown)
{
    // 调试状态追踪已禁用以提升性能

    // "开发中"弹窗：按A/B键或触摸关闭
    if (s_showDevDialog)
    {
        if (kDown & (KEY_A | KEY_B | KEY_TOUCH))
            s_showDevDialog = false;
        return;
    }

    // 延迟加载正文：上一帧已显示"正在加载"提示，本帧执行阻塞式网络请求
    if (s_bodyLoading && s_bodyLoadingIdx >= 0 && s_bodyLoadingIdx < g_app.emailCount)
    {
        int loadIdx = s_bodyLoadingIdx;
        Email* mail = &g_app.emails[loadIdx];
        Network_FetchMailBody(mail->accountIndex, mail->imapSeq);
        char* body = Cache_LoadBody(g_app.accounts[mail->accountIndex].email, mail->uid);
        if (body)
        {
            strncpy(mail->body, body, sizeof(mail->body) - 1);
            mail->body[sizeof(mail->body) - 1] = 0;
            mail->bodyLoaded = true;
            free(body);
        }
        s_bodyLoading = false;
        s_bodyLoadingIdx = -1;
        s_wrapDirty = true;
        try_load_html_render(mail);
        return; // 本帧不再处理其他输入
    }

    // 邮件拉取结果轮询
    if (g_app.fetchingMails && s_fetchResult.finished)
    {
        g_app.fetchingMails = false;
        g_app.fetchFinished = true;
        g_app.fetchSuccess = s_fetchResult.success;

        // 检查拉取期间用户是否切换了账户
        int curAccIdx = GetFetchAccountIdx();
        bool accountSwitched = (s_fetchAccountIdx != curAccIdx);

        if (accountSwitched)
        {
            // 账户已切换，不处理本次结果，下一帧会自动触发新拉取
            if (s_fetchThread) { threadJoin(s_fetchThread, U64_MAX); s_fetchThread = NULL; }
            g_app.fetchFinished = false; // 触发新账户的拉取
        }
        else if (s_fetchResult.success)
        {
            g_app.totalMails = s_fetchResult.totalMails;
            g_app.lastTotalMails = s_fetchResult.totalMails;
            g_app.lastAutoCheckFrame = g_app.frameCount;
            int perPage = g_app.emailsPerPage > 0 ? g_app.emailsPerPage : 10;
            g_app.totalPages = (g_app.totalMails + perPage - 1) / perPage;
            if (g_app.totalPages < 1) g_app.totalPages = 1;
            g_app.fetchError[0] = 0;
            g_app.selectedEmail = -1;
            g_app.listScroll = 0;
            s_readingMail = false;
            s_useHtmlRender = false;
            HtmlRender_Clear();
            if (s_fullBody) { free(s_fullBody); s_fullBody = NULL; s_fullBodyLen = 0; }
            s_bodyScroll = 0;
        }
        else
        {
            // 非QQ邮箱加载失败时，显示安全限制提示，并标记不再重复请求
            if (IsNonQQAccount(s_fetchAccountIdx))
            {
                strncpy(g_app.fetchError, "由于该账户邮箱的安全设置，不支持读取邮件",
                        sizeof(g_app.fetchError) - 1);
                if (s_fetchAccountIdx >= 0 && s_fetchAccountIdx < MAX_ACCOUNTS)
                    g_app.accounts[s_fetchAccountIdx].fetchBlocked = true;
            }
            else
            {
                strncpy(g_app.fetchError, s_fetchResult.error, sizeof(g_app.fetchError) - 1);
            }
        }

        // 只有拉取成功才保存到本地缓存（防止失败时残留旧账户邮件被写入新账户缓存）
        if (s_fetchResult.success && g_app.emailCount > 0 && !accountSwitched)
        {
            int accIdx = s_fetchAccountIdx;
            if (accIdx >= 0)
            {
                for (int i = 0; i < g_app.emailCount; i++)
                {
                    Email* m = &g_app.emails[i];
                    m->accountIndex = accIdx;
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
        if (s_fetchThread)
        {
            threadJoin(s_fetchThread, U64_MAX);
            s_fetchThread = NULL;
        }
    }

    // 启动分页拉取的辅助逻辑
    #define START_FETCH(_pg) do { \
        int _idx = GetFetchAccountIdx(); \
        if (_idx >= 0) { \
            s_cachePathLogged = false; \
            g_app.fetchingMails = true; \
            g_app.fetchFinished = false; \
            g_app.listPage = (_pg); \
            s_fetchAccountIdx = _idx; \
            memset(&s_fetchResult, 0, sizeof(s_fetchResult)); \
            struct FetchThreadArg* _arg = malloc(sizeof(struct FetchThreadArg)); \
            if (_arg) { \
                _arg->accountIdx = _idx; \
                _arg->page = (_pg); \
                _arg->perPage = g_app.emailsPerPage > 0 ? g_app.emailsPerPage : 10; \
                s_fetchThread = threadCreate(fetch_mails_thread, _arg, 1024*1024, 0x18, -1, false); \
                if (!s_fetchThread) { \
                    g_app.fetchingMails = false; \
                    g_app.fetchFinished = true; \
                    g_app.fetchSuccess = false; \
                    snprintf(g_app.fetchError, sizeof(g_app.fetchError), "线程创建失败"); \
                } \
            } else { \
                g_app.fetchingMails = false; \
                g_app.fetchFinished = true; \
                g_app.fetchSuccess = false; \
                snprintf(g_app.fetchError, sizeof(g_app.fetchError), "内存分配失败"); \
            } \
        } \
    } while(0)

    // 检测每页邮件数变化：重置分页和拉取状态，强制重新加载
    if (s_lastPerPage != g_app.emailsPerPage)
    {
        s_lastPerPage = g_app.emailsPerPage;
        g_app.fetchFinished = false;
        g_app.fetchSuccess = false;
        g_app.listPage = 0;
        g_app.selectedEmail = -1;
        g_app.listScroll = 0;
        s_readingMail = false;
        s_useHtmlRender = false;
        HtmlRender_Clear();
        if (s_fullBody) { free(s_fullBody); s_fullBody = NULL; s_fullBodyLen = 0; }
        s_bodyScroll = 0;
        int perPage = g_app.emailsPerPage > 0 ? g_app.emailsPerPage : 10;
        g_app.totalPages = (g_app.totalMails + perPage - 1) / perPage;
        if (g_app.totalPages < 1) g_app.totalPages = 1;
    }

    // 进入页面时：
    // - 非QQ邮箱已被标记fetchBlocked：直接显示红字提示，不发起网络请求
    // - needRefresh（开启软件）或无UID列表：联网加载
    // - UID列表已在内存：从本地缓存加载，不联网
    if (!g_app.fetchingMails && !g_app.fetchFinished && g_app.accountCount > 0)
    {
        int accIdx = GetFetchAccountIdx();
        if (accIdx >= 0 && g_app.accounts[accIdx].fetchBlocked)
        {
            // 该非QQ邮箱之前拉取失败，直接显示红字提示，不再发起网络请求
            g_app.fetchFinished = true;
            g_app.fetchSuccess = false;
            g_app.emailCount = 0;
            g_app.totalMails = 0;
            g_app.totalPages = 1;
            strncpy(g_app.fetchError, "由于该账户邮箱的安全设置，不支持读取邮件",
                    sizeof(g_app.fetchError) - 1);
        }
        else if (accIdx >= 0)
        {
            // 先尝试从本地缓存恢复UID列表（切换账户回来时无需联网）
            if (!g_app.uidListLoaded || g_app.uidListAccount != accIdx)
            {
                if (g_app.uidList == NULL)
                {
                    g_app.uidListCap = 4096;
                    g_app.uidList = (u32*)malloc(g_app.uidListCap * sizeof(u32));
                }
                int localCount = Cache_LoadUidList(g_app.accounts[accIdx].email,
                                                   g_app.uidList, g_app.uidListCap);
                if (localCount > 0)
                {
                    g_app.uidListCount = localCount;
                    g_app.uidListAccount = accIdx;
                    g_app.uidListLoaded = true;
                }
            }

            if (g_app.needRefresh || !g_app.uidListLoaded ||
                g_app.uidListAccount != accIdx)
            {
                g_app.needRefresh = false;
                START_FETCH(g_app.listPage);
            }
            else
            {
                // 从缓存加载
                int perPage = g_app.emailsPerPage > 0 ? g_app.emailsPerPage : 10;
                bool complete = false;
                int loaded = LoadPageFromCache(accIdx, g_app.listPage, perPage, &complete);
                if (loaded >= 0)
                {
                    // 有缓存就先显示
                    g_app.fetchFinished = true;
                    g_app.fetchSuccess = true;
                    g_app.fetchError[0] = 0;
                    g_app.selectedEmail = -1;
                    g_app.listScroll = 0;
                    g_app.lastTotalMails = g_app.totalMails;
                    g_app.lastAutoCheckFrame = g_app.frameCount;
                    // 缓存不完整时后台联网补齐（不清空已显示的列表）
                    if (!complete)
                        START_FETCH(g_app.listPage);
                }
                else
                {
                    START_FETCH(g_app.listPage);
                }
            }
        }
    }

    // L/R键翻页：优先从缓存加载，无缓存才联网
    if (!g_app.fetchingMails && g_app.fetchFinished && g_app.fetchSuccess && g_app.totalPages > 1)
    {
        int pageToLoad = -1;
        if (kDown & KEY_R)
        {
            int nextPage = g_app.listPage + 1;
            if (nextPage >= g_app.totalPages) nextPage = 0;
            pageToLoad = nextPage;
        }
        if (kDown & KEY_L)
        {
            int prevPage = g_app.listPage - 1;
            if (prevPage < 0) prevPage = g_app.totalPages - 1;
            pageToLoad = prevPage;
        }
        if (pageToLoad >= 0)
        {
            int accIdx = GetFetchAccountIdx();
            if (accIdx >= 0)
            {
                int perPage = g_app.emailsPerPage > 0 ? g_app.emailsPerPage : 10;
                int loaded = -1;
                bool complete = false;
                if (g_app.uidListLoaded && g_app.uidListAccount == accIdx)
                    loaded = LoadPageFromCache(accIdx, pageToLoad, perPage, &complete);
                if (loaded >= 0)
                {
                    g_app.listPage = pageToLoad;
                    g_app.selectedEmail = -1;
                    g_app.listScroll = 0;
                    s_readingMail = false;
                    s_useHtmlRender = false;
                    HtmlRender_Clear();
                    if (s_fullBody) { free(s_fullBody); s_fullBody = NULL; s_fullBodyLen = 0; }
                    s_bodyScroll = 0;
                    // 缓存不完整时后台联网补齐（不清空已显示的列表）
                    if (!complete)
                        START_FETCH(pageToLoad);
                }
                else
                {
                    START_FETCH(pageToLoad);
                }
            }
        }
    }

    // 自动检查新邮件（按设置间隔）
    if (!g_app.fetchingMails && g_app.accountCount > 0 && g_app.uidListLoaded)
    {
        // 自动检查间隔：0=关闭 1=5分钟 2=15分钟 3=30分钟 4=1小时
        // 3DS约60帧/秒，转换为帧数
        static const u64 intervalFrames[] = {0, 5*60*60, 15*60*60, 30*60*60, 60*60*60};
        int intervalIdx = g_app.autoCheckInterval;
        if (intervalIdx >= 1 && intervalIdx <= 4)
        {
            u64 interval = intervalFrames[intervalIdx];
            if (g_app.frameCount - g_app.lastAutoCheckFrame > interval)
            {
                g_app.lastAutoCheckFrame = g_app.frameCount;
                int accIdx = GetFetchAccountIdx();
                if (accIdx >= 0)
                {
                    int total = Network_GetTotalMails(accIdx);
                    if (total > 0 && total != g_app.lastTotalMails)
                    {
                        // 有新邮件，重新加载
                        g_app.fetchFinished = false;
                        g_app.uidListLoaded = false; // 强制刷新UID列表
                    }
                }
            }
        }
    }

    // 处理新邮件检测结果（刷新按钮或自动检查触发）
    if (s_checkThread && s_checkResult.finished)
    {
        threadJoin(s_checkThread, U64_MAX);
        s_checkThread = NULL;
        if (s_checkResult.hasNew)
        {
            // 有新邮件，重新加载当前页
            g_app.totalMails = s_checkResult.total;
            g_app.lastTotalMails = s_checkResult.total;
            g_app.fetchFinished = false;
            g_app.uidListLoaded = false;
        }
        g_app.lastAutoCheckFrame = g_app.frameCount;
    }

    // 更新下拉按钮旋转动画
    if (s_dropAnimFrame >= 0)
    {
        s_dropAnimFrame++;
        if (s_dropAnimFrame >= DROP_ANIM_FRAMES)
        {
            s_dropAnimFrame = -1;
            s_dropAngle = s_dropAnimOpening ? 180.0f : 0.0f;
        }
        else
        {
            float t = (float)s_dropAnimFrame / DROP_ANIM_FRAMES;
            s_dropAngle = s_dropAnimOpening ? (t * 180.0f) : (180.0f - t * 180.0f);
        }
    }

    // 下拉按钮和弹窗：按下时记录，抬起时执行
    if (kDown & KEY_TOUCH)
    {
        int tx = touch->px, ty = touch->py;
        // 下拉按钮
        if (tx >= 290 && tx <= 314 && ty >= 6 && ty <= 30)
        {
            s_pressedDropBtn = true;
            s_dropAction = 1;
            return;
        }
        // 弹窗内：计算菜单项
        if (s_dropdownOpen)
        {
            // 重新计算菜单项数量
            int mc = 0;
            for (int i = 0; i < MAX_ACCOUNTS; i++)
                if (g_app.accounts[i].added) mc++;
            mc += 3; // 未读、已发送、收藏夹
            float itemH = 26.0f;
            float px = 156, py = 26, pw = 160;
            float ph = mc * itemH + 4;
            if (tx >= px && tx <= px + pw && ty >= py && ty <= py + ph)
            {
                int idx = (int)((ty - py - 2) / itemH);
                if (idx >= 0 && idx < mc)
                {
                    s_pressedDropBtn = true;
                    s_dropAction = 3;
                    s_dropMenuItem = idx;
                    return;
                }
            }
            // 弹窗外部关闭
            if (!(tx >= px && tx <= px + pw && ty >= py && ty <= py + ph))
            {
                s_pressedDropBtn = true;
                s_dropAction = 2;
                return;
            }
        }
    }

    // 按住时检查手指是否移出
    if (hidKeysHeld() & KEY_TOUCH)
    {
        if (s_pressedDropBtn)
        {
            touchPosition curTouch;
            hidTouchRead(&curTouch);
            int tx = curTouch.px, ty = curTouch.py;
            bool onArea = false;
            if (s_dropAction == 1 && tx >= 290 && tx <= 314 && ty >= 6 && ty <= 30)
                onArea = true;
            if (s_dropAction == 2 || s_dropAction == 3)
            {
                int mc = 0;
                for (int i = 0; i < MAX_ACCOUNTS; i++)
                    if (g_app.accounts[i].added) mc++;
                mc += 3;
                float itemH = 26.0f;
                float px = 156, py = 26, pw = 160;
                float ph = mc * itemH + 4;
                if (s_dropAction == 3)
                {
                    int idx = (int)((ty - py - 2) / itemH);
                    if (idx == s_dropMenuItem && tx >= px && tx <= px + pw && ty >= py && ty <= py + ph)
                        onArea = true;
                }
                else
                {
                    if (!(tx >= px && tx <= px + pw && ty >= py && ty <= py + ph))
                        onArea = true;
                }
            }
            if (!onArea) s_pressedDropBtn = false;
        }
    }

    // 抬起时执行
    if (g_kUp & KEY_TOUCH)
    {
        if (s_pressedDropBtn)
        {
            s_pressedDropBtn = false;
            if (s_dropAction == 1)
            {
                s_dropdownOpen = !s_dropdownOpen;
                s_dropAnimOpening = s_dropdownOpen;
                s_dropAnimFrame = 0;
            }
            else if (s_dropAction == 2)
            {
                s_dropdownOpen = false;
                s_dropAnimOpening = false;
                s_dropAnimFrame = 0;
            }
            else if (s_dropAction == 3)
            {
                // 执行菜单项
                int idx = s_dropMenuItem;
                int acctIdx = -1;
                int count = 0;
                for (int i = 0; i < MAX_ACCOUNTS; i++)
                {
                    if (g_app.accounts[i].added)
                    {
                        if (count == idx) { acctIdx = i; break; }
                        count++;
                    }
                }
                if (acctIdx >= 0)
                {
                    g_app.selectedFilter = acctIdx;
                    g_app.selectedEmail = -1;
                    g_app.listPage = 0;
                    g_app.listScroll = 0;
                    g_app.emailCount = 0;  // 清空旧账户的邮件列表
                    s_readingMail = false;
                    s_useHtmlRender = false;
                    HtmlRender_Clear();
                    if (s_fullBody) { free(s_fullBody); s_fullBody = NULL; s_fullBodyLen = 0; }
                    s_bodyScroll = 0;
                    // 切换账户时重置UID列表状态，强制重新获取UID
                    g_app.uidListLoaded = false;
                    g_app.uidListCount = 0;
                    g_app.uidListAccount = -1;
                    g_app.fetchFinished = false;  // 触发重新拉取
                }
                else
                {
                    // 未读/已发送/收藏夹
                    int type = idx - count;
                    if (type == 1) { /* 未读邮件 - TODO */ }
                    if (type == 2) g_app.currentPage = PAGE_SENT_LIST;
                    if (type == 3) { /* 收藏夹 - TODO */ }
                }
                s_dropdownOpen = false;
                s_dropAnimOpening = false;
                s_dropAnimFrame = 0;
            }
            s_dropAction = 0;
            s_dropMenuItem = -1;
            return;
        }
    }

    // 构建过滤后的邮件索引
    int filteredIdx[MAX_EMAILS];
    int filteredCount = 0;
    for (int i = 0; i < g_app.emailCount; i++)
    {
        if (g_app.selectedFilter < 0 || g_app.emails[i].accountIndex == g_app.selectedFilter)
            filteredIdx[filteredCount++] = i;
    }

    #define U_ITEM_H 56
    #define U_VIS 3
    float maxScrollF = (filteredCount - U_VIS) * (float)U_ITEM_H;
    if (maxScrollF < 0) maxScrollF = 0;
    if (g_app.listScroll > maxScrollF) g_app.listScroll = maxScrollF;
    if (g_app.listScroll < 0) g_app.listScroll = 0;

    // 找当前选中邮件在过滤列表中的位置
    int selFiltered = -1;
    for (int i = 0; i < filteredCount; i++)
    {
        if (filteredIdx[i] == g_app.selectedEmail) { selFiltered = i; break; }
    }
    if (selFiltered < 0)
        g_app.selectedEmail = -1;

    // 注意：L/R肩键已用于翻页（见上方分页逻辑），此处不再用于滚动

    // 正文阅读模式：十字键/摇杆上下滚动正文，B键退出
    if (s_readingMail)
    {
        if (kDown & KEY_B)
        {
            s_readingMail = false;
            s_bodyLoading = false;
            s_bodyLoadingIdx = -1;
            s_useHtmlRender = false;
            HtmlRender_Clear();
            HtmlRender_FreeTextures();
            if (s_fullBody) { free(s_fullBody); s_fullBody = NULL; s_fullBodyLen = 0; }
            s_bodyScroll = 0;
        }
        // 十字键上下滚动（每次3行）
        if (kDown & KEY_DUP)
            s_bodyScroll -= 3;
        if (kDown & KEY_DDOWN)
            s_bodyScroll += 3;
        // 摇杆上下滚动
        circlePosition cpos;
        hidCircleRead(&cpos);
        if (cpos.dy > 30) s_bodyScroll += 1;
        if (cpos.dy < -30) s_bodyScroll -= 1;
        if (s_bodyScroll < 0) s_bodyScroll = 0;
        return; // 阅读模式下不处理其他按键
    }

    // 按键导航
    bool dirPressed = (kDown & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) != 0;
    if (dirPressed) g_app.focusActive = true;

    if (g_app.focusActive)
    {
        if (s_mainFocusArea == 0)
        {
            if (kDown & KEY_DDOWN) s_mainFocusArea = 1;
        }
        else if (s_mainFocusArea == 1)
        {
            // 邮件列表：上下切换邮件（不加载正文，按A键才加载）
            if (kDown & KEY_DUP)
            {
                if (selFiltered < 0 && filteredCount > 0)
                {
                    // 没有选中邮件时，按上键选中第一封
                    selFiltered = 0;
                    g_app.selectedEmail = filteredIdx[selFiltered];
                }
                else if (selFiltered > 0)
                {
                    selFiltered--;
                    g_app.selectedEmail = filteredIdx[selFiltered];
                    float itemTop = selFiltered * U_ITEM_H;
                    if (itemTop < g_app.listScroll)
                        g_app.listScroll = itemTop;
                }
            }
            if (kDown & KEY_DDOWN)
            {
                if (selFiltered < 0 && filteredCount > 0)
                {
                    selFiltered = 0;
                    g_app.selectedEmail = filteredIdx[selFiltered];
                }
                else if (selFiltered >= 0 && selFiltered < filteredCount - 1)
                {
                    selFiltered++;
                    g_app.selectedEmail = filteredIdx[selFiltered];
                    float itemBottom = (selFiltered + 1) * U_ITEM_H;
                    float visBottom = g_app.listScroll + U_VIS * U_ITEM_H;
                    if (itemBottom > visBottom)
                        g_app.listScroll = itemBottom - U_VIS * U_ITEM_H;
                }
                else
                    s_mainFocusArea = 2;
            }
            // A键：加载选中邮件正文并进入阅读模式
            if (kDown & KEY_A && g_app.selectedEmail >= 0)
            {
                Email* mail = &g_app.emails[g_app.selectedEmail];
                Email_MarkAsRead(g_app.selectedEmail);
                s_readingMail = true;
                s_bodyScroll = 0;
                s_wrapDirty = true;
                if (!mail->bodyLoaded)
                {
                    // 延迟到下一帧加载，先让"正在加载"提示渲染出来
                    s_bodyLoading = true;
                    s_bodyLoadingIdx = g_app.selectedEmail;
                }
                else
                {
                    try_load_html_render(mail);
                }
            }
        }
        else if (s_mainFocusArea == 2)
        {
            // 底部按钮：左右切换，上→邮件列表，A执行
            if (kDown & KEY_DLEFT)
                s_bottomFocus = (s_bottomFocus + 2) % 3;
            if (kDown & KEY_DRIGHT)
                s_bottomFocus = (s_bottomFocus + 1) % 3;
            if (kDown & KEY_DUP)
                s_mainFocusArea = 1;
            if (kDown & KEY_A)
            {
                if (s_bottomFocus == 0)
                {
                    s_showDevDialog = true;
                }
                else if (s_bottomFocus == 1)
                {
                    start_check_new_mails();
                }
                else if (s_bottomFocus == 2)
                {
                    g_app.currentPage = PAGE_ACCOUNT_LIST;
                    return;
                }
            }
        }
    }
    else
    {
        // 焦点未激活：首次按键选中邮件（不加载正文）
        if (kDown & (KEY_DUP | KEY_DDOWN))
        {
            s_mainFocusArea = 1;
            g_app.focusActive = true;
            if (kDown & KEY_DUP && selFiltered > 0)
            {
                selFiltered--;
                g_app.selectedEmail = filteredIdx[selFiltered];
                float itemTop = selFiltered * U_ITEM_H;
                if (itemTop < g_app.listScroll)
                    g_app.listScroll = itemTop;
            }
            if (kDown & KEY_DDOWN && selFiltered >= 0 && selFiltered < filteredCount - 1)
            {
                selFiltered++;
                g_app.selectedEmail = filteredIdx[selFiltered];
                float itemBottom = (selFiltered + 1) * U_ITEM_H;
                float visBottom = g_app.listScroll + U_VIS * U_ITEM_H;
                if (itemBottom > visBottom)
                    g_app.listScroll = itemBottom - U_VIS * U_ITEM_H;
            }
        }
    }

    // 滚动条拖动（像素级顺滑）
    if (kDown & KEY_TOUCH)
    {
        int tx = touch->px, ty = touch->py;
        if (filteredCount > U_VIS && tx >= 310 && tx <= 318 && ty >= 43 && ty <= 203)
        {
            s_scrollDragging = true;
            int trackH = 160;
            int thumbH = trackH * U_VIS / filteredCount;
            if (thumbH < 20) thumbH = 20;
            int thumbY = 43 + (int)((trackH - thumbH) * g_app.listScroll / maxScrollF);
            s_scrollDragOffset = ty - thumbY;
        }
        // 列表区域触摸滑动
        else if (ty >= 39 && ty <= 209 && tx < 310)
        {
            s_listTouching = true;
            s_listTouchPrevY = ty;
            s_listTouchStartY = ty;
            s_listMoved = false;
        }
    }
    if (s_scrollDragging)
    {
        if (hidKeysHeld() & KEY_TOUCH)
        {
            touchPosition curTouch;
            hidTouchRead(&curTouch);
            int trackH = 160;
            int thumbH = trackH * U_VIS / filteredCount;
            if (thumbH < 20) thumbH = 20;
            int newThumbY = curTouch.py - s_scrollDragOffset;
            if (newThumbY < 43) newThumbY = 43;
            if (newThumbY > 43 + trackH - thumbH) newThumbY = 43 + trackH - thumbH;
            g_app.listScroll = (float)(newThumbY - 43) * maxScrollF / (trackH - thumbH);
        }
        else
        {
            s_scrollDragging = false;
        }
    }

    // 列表区域触摸滑动
    if (s_listTouching)
    {
        if (hidKeysHeld() & KEY_TOUCH)
        {
            touchPosition curTouch;
            hidTouchRead(&curTouch);
            int dy = curTouch.py - s_listTouchPrevY;
            s_listTouchPrevY = curTouch.py;
            if (dy != 0)
            {
                g_app.listScroll -= dy;
                if (g_app.listScroll < 0) g_app.listScroll = 0;
                if (g_app.listScroll > maxScrollF) g_app.listScroll = maxScrollF;
                if (abs(curTouch.py - s_listTouchStartY) > 8)
                    s_listMoved = true;
            }
        }
        else
        {
            // 抬起：如果没移动则视为点击
            if (!s_listMoved)
            {
                int ty = s_listTouchStartY;
                int slot = (ty - 39 + (int)g_app.listScroll) / U_ITEM_H;
                if (slot >= 0 && slot < filteredCount)
                {
                    int tappedIdx = filteredIdx[slot];
                    if (tappedIdx == g_app.selectedEmail && s_readingMail == false)
                    {
                        // 第二次点击已选中的条目：加载正文并进入阅读模式（与A键相同）
                        Email* mail = &g_app.emails[g_app.selectedEmail];
                        Email_MarkAsRead(g_app.selectedEmail);
                        s_readingMail = true;
                        s_bodyScroll = 0;
                        s_wrapDirty = true;
                        if (!mail->bodyLoaded)
                        {
                            // 延迟加载，先显示"正在加载"提示
                            s_bodyLoading = true;
                            s_bodyLoadingIdx = g_app.selectedEmail;
                        }
                        else
                        {
                            try_load_html_render(mail);
                        }
                    }
                    else
                    {
                        // 第一次点击：仅选中（与十字键选择逻辑相同）
                        g_app.selectedEmail = tappedIdx;
                        g_app.focusActive = true;
                        s_readingMail = false;
                    }
                }
            }
            s_listTouching = false;
            s_listMoved = false;
        }
    }

    // 点击（底部按钮）
    if (kDown & KEY_TOUCH && !s_scrollDragging && !s_listTouching)
    {
        g_app.focusActive = true;
        int tx = touch->px;
        int ty = touch->py;
        if (ty >= 212 && ty <= 240)
        {
            float bw = SCREEN_BOT_W / 3.0f;
            if (tx >= 0 && tx < bw)
                s_pressedBottomBtn = 0;
            else if (tx >= bw && tx < bw * 2)
                s_pressedBottomBtn = 1;
            else if (tx >= bw * 2 && tx <= SCREEN_BOT_W)
                s_pressedBottomBtn = 2;
        }
    }

    // 触摸抬起时执行按钮功能
    if (g_kUp & KEY_TOUCH)
    {
        if (s_pressedBottomBtn >= 0)
        {
            int btn = s_pressedBottomBtn;
            s_pressedBottomBtn = -1;
            if (btn == 0)
            {
                s_showDevDialog = true;
            }
            else if (btn == 1)
            {
                start_check_new_mails();
            }
            else if (btn == 2)
            {
                g_app.currentPage = PAGE_ACCOUNT_LIST;
            }
        }
    }

    // 手指移出按钮区域时取消按下状态
    if (hidKeysHeld() & KEY_TOUCH)
    {
        if (s_pressedBottomBtn >= 0)
        {
            touchPosition curTouch;
            hidTouchRead(&curTouch);
            int tx = curTouch.px, ty = curTouch.py;
            bool onBtn = false;
            float bw = SCREEN_BOT_W / 3.0f;
            if (s_pressedBottomBtn == 0 && tx >= 0 && tx < bw && ty >= 212 && ty <= 240) onBtn = true;
            if (s_pressedBottomBtn == 1 && tx >= bw && tx < bw*2 && ty >= 212 && ty <= 240) onBtn = true;
            if (s_pressedBottomBtn == 2 && tx >= bw*2 && tx <= SCREEN_BOT_W && ty >= 212 && ty <= 240) onBtn = true;
            if (!onBtn) s_pressedBottomBtn = -1;
        }
    }
}

// ========== 写邮件页 ==========
static void DrawComposeField(float x, float y, float w, float h, const char* label, const char* value, bool focused)
{
    UI_DrawText(x, y - 18, 0.45f, COLOR_TEXT_SECONDARY, label);
    u32 bg = focused ? COLOR_INPUT_FOCUS : COLOR_WHITE;
    u32 border = focused ? COLOR_BLUE : COLOR_BORDER_GRAY;
    UI_DrawRoundRect(x, y, w, h, bg, border);
    if (strlen(value) > 0)
    {
        UI_DrawText(x + 8, y + 8, 0.5f, COLOR_TEXT_PRIMARY, value);
    }
    else if (!focused)
    {
        UI_DrawText(x + 8, y + 8, 0.45f, C2D_Color32(0xBD, 0xBD, 0xBD, 0xFF), label);
    }
}

static void DrawMultilineText(float x, float y, float lineH, float scale, u32 color, const char* text, int maxLines)
{
    const char* p = text;
    int line = 0;
    while (*p && line < maxLines)
    {
        const char* eol = strchr(p, '\n');
        int len;
        if (eol) len = (int)(eol - p);
        else len = (int)strlen(p);

        char lineBuf[256];
        if (len > 255) len = 255;
        memcpy(lineBuf, p, len);
        lineBuf[len] = 0;
        UI_DrawText(x, y + line * lineH, scale, color, lineBuf);

        line++;
        if (eol) p = eol + 1;
        else break;
    }
}

void Page_ComposeDraw(void)
{
    // 上屏：预览
    C2D_TargetClear(topTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(topTarget);

    UI_DrawStatusBarTop();
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 36, 0.5f, COLOR_BLUE, "写邮件");

    UI_DrawText(16, 60, 0.45f, COLOR_TEXT_SECONDARY, "收件人：");
    UI_DrawText(80, 60, 0.5f, COLOR_TEXT_PRIMARY, g_app.composeTo);

    UI_DrawText(16, 84, 0.45f, COLOR_TEXT_SECONDARY, "主题：");
    UI_DrawText(70, 84, 0.5f, COLOR_TEXT_PRIMARY, g_app.composeSubject);

    UI_DrawRect(16, 108, SCREEN_TOP_W - 32, 1, COLOR_BORDER_GRAY);

    if (strlen(g_app.composeBody) > 0)
    {
        DrawMultilineText(16, 120, 16, 0.45f, COLOR_TEXT_PRIMARY, g_app.composeBody, 8);
    }
    else
    {
        UI_DrawText(16, 120, 0.45f, C2D_Color32(0xBD, 0xBD, 0xBD, 0xFF), "输入邮件内容...");
    }

    // 下屏
    if (Keyboard_IsActive())
    {
        Keyboard_Draw();
        return;
    }

    C2D_TargetClear(botTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(botTarget);

    DrawComposeField(16, 28, 288, 32, "收件人", g_app.composeTo,
        (g_app.activeInputField == 10) || (g_app.activeInputField < 0 && g_app.focusActive && s_composeFocus == 0));
    DrawComposeField(16, 68, 288, 32, "主题", g_app.composeSubject,
        (g_app.activeInputField == 11) || (g_app.activeInputField < 0 && g_app.focusActive && s_composeFocus == 1));

    // 正文框（多行，圆角）
    UI_DrawText(16, 108, 0.45f, COLOR_TEXT_SECONDARY, "正文");
    bool bodyFocused = (g_app.activeInputField == 12) || (g_app.activeInputField < 0 && g_app.focusActive && s_composeFocus == 2);
    u32 bodyBg = bodyFocused ? COLOR_INPUT_FOCUS : COLOR_WHITE;
    u32 bodyBorder = bodyFocused ? COLOR_BLUE : COLOR_BORDER_GRAY;
    UI_DrawRoundRect(16, 122, 288, 68, bodyBg, bodyBorder);
    if (strlen(g_app.composeBody) > 0)
    {
        DrawMultilineText(22, 128, 16, 0.42f, COLOR_TEXT_PRIMARY, g_app.composeBody, 4);
    }
    else if (!bodyFocused)
    {
        UI_DrawText(22, 128, 0.4f, C2D_Color32(0xBD, 0xBD, 0xBD, 0xFF), "输入邮件内容...");
    }

    // 按钮
    bool canSend = (strlen(g_app.composeTo) > 0 && strlen(g_app.composeSubject) > 0);
    u32 cancelBg = (g_app.focusActive && s_composeFocus == 3) ? COLOR_BTN_GRAY_DARK : COLOR_BTN_GRAY;
    UI_DrawButton(16, 198, 140, 34, "取消", cancelBg, COLOR_BTN_GRAY_DARK);
    if (canSend)
    {
        u32 sendBg = (g_app.focusActive && s_composeFocus == 4) ? COLOR_BLUE_DARK : COLOR_BLUE;
        UI_DrawButton(172, 198, 132, 34, "发送", sendBg, COLOR_BLUE_DARK);
    }
    else
    {
        UI_DrawButtonDisabled(172, 198, 132, 34, "发送");
    }
}

static void CommitComposeInput(void)
{
    const char* text = Keyboard_GetText();
    if (g_app.activeInputField == 10)
        strcpy(g_app.composeTo, text);
    else if (g_app.activeInputField == 11)
        strcpy(g_app.composeSubject, text);
    else if (g_app.activeInputField == 12)
        strcpy(g_app.composeBody, text);
    g_app.activeInputField = -1;
}

void Page_ComposeUpdate(touchPosition* touch, u32 kDown)
{
    if (Keyboard_IsActive())
    {
        u32 kHeld = hidKeysHeld();
        bool kbClosed = Keyboard_HandleInput(kDown, kHeld);
        if (!kbClosed && (kDown & KEY_TOUCH))
        {
            kbClosed = Keyboard_HandleTouch(touch);
        }
        if (kbClosed)
        {
            CommitComposeInput();
        }
        return;
    }

    if (kDown & KEY_B)
    {
        g_app.currentPage = PAGE_MAIN;
        return;
    }

    // 按键导航：上下纵向切换，左右横向切换
    bool canSend = (strlen(g_app.composeTo) > 0 && strlen(g_app.composeSubject) > 0);
    // 焦点：0=收件人，1=主题，2=正文，3=取消，4=发送
    // 布局：收件人(整行) → 主题(整行) → 正文(整行) → [取消 | 发送]
    if (kDown & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT))
        g_app.focusActive = true;
    if (kDown & KEY_DUP)
    {
        if (s_composeFocus == 0) s_composeFocus = canSend ? 4 : 3;
        else if (s_composeFocus <= 2) s_composeFocus--;
        else s_composeFocus = 2;  // 按钮上 → 正文
    }
    if (kDown & KEY_DDOWN)
    {
        if (s_composeFocus < 2) s_composeFocus++;
        else if (s_composeFocus == 2) s_composeFocus = 3;
        else s_composeFocus = 0;  // 按钮下 → 收件人
    }
    if (kDown & KEY_DLEFT)
    {
        if (s_composeFocus == 0) s_composeFocus = canSend ? 4 : 3;
        else if (s_composeFocus <= 2) s_composeFocus--;
        else if (s_composeFocus == 3) s_composeFocus = 2;
        else if (s_composeFocus == 4) s_composeFocus = 3;
    }
    if (kDown & KEY_DRIGHT)
    {
        if (s_composeFocus < 2) s_composeFocus++;
        else if (s_composeFocus == 2) s_composeFocus = 3;
        else if (s_composeFocus == 3) s_composeFocus = canSend ? 4 : 0;
        else if (s_composeFocus == 4) s_composeFocus = 0;
    }

    // A键确认（需要焦点已激活）
    if (kDown & KEY_A && g_app.focusActive)
    {
        if (s_composeFocus == 0)
        {
            g_app.activeInputField = 10;
            Keyboard_Open(g_app.composeTo, sizeof(g_app.composeTo), false, false);
            return;
        }
        if (s_composeFocus == 1)
        {
            g_app.activeInputField = 11;
            Keyboard_Open(g_app.composeSubject, sizeof(g_app.composeSubject), false, false);
            return;
        }
        if (s_composeFocus == 2)
        {
            g_app.activeInputField = 12;
            Keyboard_Open(g_app.composeBody, sizeof(g_app.composeBody), false, true);
            return;
        }
        if (s_composeFocus == 3)
        {
            g_app.currentPage = PAGE_MAIN;
            return;
        }
        if (s_composeFocus == 4 && canSend)
        {
            Network_SendMail(g_app.selectedFilter >= 0 ? g_app.selectedFilter : 0, g_app.composeTo, g_app.composeSubject, g_app.composeBody);
            g_app.currentPage = PAGE_MAIN;
            return;
        }
    }

    if (hidKeysHeld() & KEY_TOUCH)
    {
        g_app.focusActive = true;
        int tx = touch->px;
        int ty = touch->py;

        if (ty >= 28 && ty <= 60 && tx >= 16 && tx <= 304)
        {
            s_composeFocus = 0;
            g_app.activeInputField = 10;
            Keyboard_Open(g_app.composeTo, sizeof(g_app.composeTo), false, false);
            return;
        }
        if (ty >= 68 && ty <= 100 && tx >= 16 && tx <= 304)
        {
            s_composeFocus = 1;
            g_app.activeInputField = 11;
            Keyboard_Open(g_app.composeSubject, sizeof(g_app.composeSubject), false, false);
            return;
        }
        if (ty >= 122 && ty <= 190 && tx >= 16 && tx <= 304)
        {
            s_composeFocus = 2;
            g_app.activeInputField = 12;
            Keyboard_Open(g_app.composeBody, sizeof(g_app.composeBody), false, true);
            return;
        }

        // 按钮
        if (ty >= 198 && ty <= 232)
        {
            if (tx >= 16 && tx <= 156)
            {
                s_composeFocus = 3;
                g_app.currentPage = PAGE_MAIN;
                return;
            }
            if (canSend && tx >= 172 && tx <= 304)
            {
                s_composeFocus = 4;
                Network_SendMail(g_app.selectedFilter >= 0 ? g_app.selectedFilter : 0, g_app.composeTo, g_app.composeSubject, g_app.composeBody);
                g_app.currentPage = PAGE_MAIN;
                return;
            }
        }
    }
}

// ========== 已发送列表 ==========
void Page_SentListDraw(void)
{
    C2D_TargetClear(topTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(topTarget);

    UI_DrawStatusBarTop();
    UI_DrawTextCenter(SCREEN_TOP_W / 2, 36, 0.55f, COLOR_BLUE, "已发送邮件");

    if (g_app.sentCount == 0)
    {
        UI_DrawTextCenter(SCREEN_TOP_W / 2, 122, 0.5f, COLOR_TEXT_SECONDARY, "暂无已发送邮件");
    }
    else
    {
        Email* mail = &g_app.sentMails[g_app.sentCount - 1];
        UI_DrawText(16, 62, 0.5f, COLOR_TEXT_PRIMARY, mail->subject);
        UI_DrawText(16, 86, 0.42f, COLOR_TEXT_SECONDARY, "收件人：");
        UI_DrawText(80, 86, 0.42f, COLOR_BLUE, mail->sender);
        UI_DrawRect(16, 108, SCREEN_TOP_W - 32, 1, COLOR_BORDER_GRAY);
        UI_DrawText(16, 118, 0.45f, COLOR_TEXT_PRIMARY, mail->body);
    }

    C2D_TargetClear(botTarget, COLOR_PAGE_BG);
    C2D_SceneBegin(botTarget);

    UI_DrawRect(0, 0, SCREEN_BOT_W, 28, COLOR_WHITE);
    UI_DrawRect(0, 27, SCREEN_BOT_W, 1, COLOR_BORDER_GRAY);
    UI_DrawTextCenter(SCREEN_BOT_W / 2, 7, 0.5f, COLOR_TEXT_PRIMARY, "已发送");

    int listY = 28;
    int itemH = 34;
    for (int i = g_app.sentCount - 1; i >= 0 && (g_app.sentCount - 1 - i) < 5; i--)
    {
        Email* mail = &g_app.sentMails[i];
        int idx = g_app.sentCount - 1 - i;
        int y = listY + idx * itemH;

        UI_DrawText(12, y + 4, 0.45f, COLOR_TEXT_PRIMARY, mail->sender);
        UI_DrawText(12, y + 19, 0.4f, COLOR_TEXT_SECONDARY, mail->subject);
        UI_DrawText(SCREEN_BOT_W - 50, y + 4, 0.38f, COLOR_TEXT_SECONDARY, mail->date);

        if (idx < 4)
            UI_DrawRect(8, y + itemH, SCREEN_BOT_W - 16, 1, COLOR_BORDER_GRAY);
    }

    UI_DrawButton(110, 200, 100, 34, "返回", COLOR_BTN_GRAY, COLOR_BTN_GRAY_DARK);
}

void Page_SentListUpdate(touchPosition* touch, u32 kDown)
{
    (void)touch;
    if (kDown & KEY_B || kDown & KEY_A)
    {
        g_app.currentPage = PAGE_MAIN;
        return;
    }
    if (hidKeysHeld() & KEY_TOUCH)
    {
        int tx = touch->px;
        int ty = touch->py;
        if (tx >= 110 && tx <= 210 && ty >= 200 && ty <= 234)
        {
            g_app.currentPage = PAGE_MAIN;
        }
    }
}
