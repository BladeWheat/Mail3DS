// common.h - 全局定义
#ifndef COMMON_H
#define COMMON_H

#include <3ds.h>
#include <citro2d.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// 屏幕尺寸
#define SCREEN_TOP_W  400
#define SCREEN_TOP_H  240
#define SCREEN_BOT_W  320
#define SCREEN_BOT_H  240

// 最大数量
#define MAX_ACCOUNTS  4
#define MAX_EMAILS    100

// 颜色定义（按UI规范）
#define COLOR_PAGE_BG       C2D_Color32(0xF6, 0xF1, 0xC0, 0xFF)  // 米黄背景 #f6f1c0
#define COLOR_WHITE         C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define COLOR_BLUE          C2D_Color32(0x19, 0x76, 0xD2, 0xFF)  // 主按钮蓝 #1976D2
#define COLOR_BLUE_DARK     C2D_Color32(0x0D, 0x47, 0xA1, 0xFF)  // 按下深蓝 #0D47A1
#define COLOR_BTN_GRAY      C2D_Color32(0x61, 0x61, 0x61, 0xFF)  // 次按钮灰 #616161
#define COLOR_BTN_GRAY_DARK C2D_Color32(0x42, 0x42, 0x42, 0xFF)
#define COLOR_RED           C2D_Color32(0xD3, 0x2F, 0x2F, 0xFF)  // 警示红 #D32F2F
#define COLOR_TEXT_PRIMARY  C2D_Color32(0x21, 0x21, 0x21, 0xFF)  // 主文字 #212121
#define COLOR_TEXT_SECONDARY C2D_Color32(0x75, 0x75, 0x75, 0xFF) // 次文字 #757575
#define COLOR_SELECTED_BG   C2D_Color32(0xBB, 0xDE, 0xFB, 0xFF)  // 选中背景 #BBDEFB
#define COLOR_UNREAD_DOT    C2D_Color32(0xF4, 0x43, 0x36, 0xFF)  // 未读红点 #F44336
#define COLOR_INPUT_FOCUS   C2D_Color32(0xE3, 0xF2, 0xFD, 0xFF)  // 输入框聚焦
#define COLOR_BORDER_GRAY   C2D_Color32(0x9E, 0x9E, 0x9E, 0xFF)  // 虚线边框
#define COLOR_KEY_BG        C2D_Color32(0xEC, 0xE5, 0xD0, 0xFF)  // 按键背景
#define COLOR_KEY_FUNC      C2D_Color32(0x90, 0xA4, 0xAE, 0xFF)  // 功能键背景

// 页面状态
typedef enum {
    PAGE_LOAD,
    PAGE_NETWORK_ERROR,
    PAGE_NO_ACCOUNT,
    PAGE_ACCOUNT_LIST,
    PAGE_ADD_ACCOUNT,
    PAGE_MAIN,
    PAGE_COMPOSE,
    PAGE_SENT_LIST,
    PAGE_SETTINGS,
    PAGE_NOTIF_SETTINGS,
    PAGE_AUTO_CHECK,
    PAGE_ABOUT
} PageState;

// 键盘模式
typedef enum {
    KB_LOWER,
    KB_UPPER,
    KB_SYMBOL,
    KB_PINYIN
} KeyboardMode;

// 账户类型
typedef enum {
    ACC_OTHER,
    ACC_QQ,
    ACC_163,
    ACC_GMAIL,
    ACC_OUTLOOK
} AccountType;

// 账户结构体
typedef struct {
    bool added;
    char email[128];
    char password[128];
    char imapServer[128];
    char smtpServer[128];
    int imapPort;
    int smtpPort;
    AccountType type;
    bool fetchBlocked;  // 非QQ邮箱拉取失败后标记为true，不再重复发起网络请求
} Account;

// 邮件结构体
typedef struct {
    int id;
    int accountIndex;
    int imapSeq;       // IMAP服务器上的邮件序号
    u32 uid;           // IMAP UID（唯一标识，用于缓存）
    char sender[128];  // 发件人显示名（昵称或邮箱前缀）
    char fromAddr[128];// 发件人邮箱地址
    char subject[256];
    char contentType[256]; // Content-Type头部（用于正文解析）
    char preview[128]; // 正文预览
    char body[1024];
    char date[32];     // 旧字段，新代码不再使用
    int year, month, day, hour, minute; // 本地时间
    long long timestamp; // Unix时间戳（秒），排序唯一依据
    char rawDate[64];  // 原始Date头文本（调试用）
    bool unread;
    bool hasAttachment;
    bool bodyLoaded;
} Email;

// 全局应用状态
typedef struct {
    PageState currentPage;

    // 账户
    Account accounts[MAX_ACCOUNTS];
    int accountCount;

    // 邮件
    Email emails[MAX_EMAILS];
    int emailCount;
    int selectedEmail;
    int selectedFilter; // -1=全部, 0-3=对应账户

    // 邮件列表分页
    int emailsPerPage;  // 每页加载数量：10/15/20/25
    int listPage;       // 当前页码（0开始）
    float listScroll;   // 列表像素滚动偏移（顺滑滚动）

    // 通知设置
    bool notifEnabled;     // 通知总开关
    bool notifSound;       // 声音提醒
    bool notifLED;         // 指示灯闪烁

    // 自动检查邮件
    int autoCheckInterval; // 0=关闭 1=5分钟 2=15分钟 3=30分钟 4=1小时

    // 已发送
    Email sentMails[MAX_EMAILS];
    int sentCount;

    // 输入
    char addEmailBuf[128];
    char addPassBuf[128];
    int activeInputField; // 0=邮箱 1=密码, 10=收件人 11=主题 12=正文

    // 写邮件
    char composeTo[128];
    char composeSubject[256];
    char composeBody[2048];

    // 自定义键盘
    bool keyboardActive;
    KeyboardMode kbMode;
    char kbBuffer[2048];
    int kbCursor;
    int kbMaxLen;
    bool kbIsPassword;
    bool kbMultiline;

    // 帧计数
    u64 frameCount;
    u64 lastRefreshTime;

    // 按键焦点是否已激活（页面刚打开时为false，按方向键后为true）
    bool focusActive;

    // 加载状态
    int loadProgress;     // 0-100
    int loadStep;         // 当前加载步骤 0-4
    bool loadError;       // 加载是否出错

    // 邮件拉取状态
    bool fetchingMails;
    bool fetchFinished;
    bool fetchSuccess;
    char fetchError[128];
    int totalMails;     // 服务器上总邮件数
    int totalPages;     // 总页数

    // UID列表缓存（内存中，按IMAP序号顺序，用于从本地缓存加载邮件）
    u32* uidList;       // 动态分配，支持任意数量邮件
    int uidListCap;     // 已分配容量
    int uidListCount;
    int uidListAccount; // 当前UID列表对应的账户索引
    bool uidListLoaded; // UID列表是否已加载

    // 新邮件检测
    int lastTotalMails;      // 上次已知的总邮件数
    u64 lastAutoCheckFrame;  // 上次自动检查的帧计数
    bool needRefresh;        // 开启软件时设为true，强制检测新邮件
} AppState;

extern AppState g_app;
extern C3D_RenderTarget* topTarget;
extern C3D_RenderTarget* botTarget;
extern u32 g_kUp;  // 本帧抬起的按键（用于触摸松开检测）

#endif
