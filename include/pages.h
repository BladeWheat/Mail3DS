// pages.h - 页面函数
#ifndef PAGES_H
#define PAGES_H

#include "common.h"

// 加载/网络错误/无账户
void Page_LoadDraw(void);
void Page_LoadUpdate(u32 kDown);
void Page_NetworkErrorDraw(void);
void Page_NetworkErrorUpdate(touchPosition* touch, u32 kDown);
void Page_NoAccountDraw(void);
void Page_NoAccountUpdate(touchPosition* touch, u32 kDown);

// 账户列表/新增账户
void Page_AccountListDraw(void);
void Page_AccountListUpdate(touchPosition* touch, u32 kDown);
void Page_AddAccountDraw(void);
void Page_AddAccountUpdate(touchPosition* touch, u32 kDown);

// 主界面/写邮件/已发送
void Page_MainDraw(void);
void Page_MainUpdate(touchPosition* touch, u32 kDown);
void Page_ComposeDraw(void);
void Page_ComposeUpdate(touchPosition* touch, u32 kDown);
void Page_SentListDraw(void);
void Page_SentListUpdate(touchPosition* touch, u32 kDown);

// 设置
void Page_SettingsDraw(void);
void Page_SettingsUpdate(touchPosition* touch, u32 kDown);
void Page_NotifSettingsDraw(void);
void Page_NotifSettingsUpdate(touchPosition* touch, u32 kDown);
void Page_AutoCheckDraw(void);
void Page_AutoCheckUpdate(touchPosition* touch, u32 kDown);
void Page_AboutDraw(void);
void Page_AboutUpdate(touchPosition* touch, u32 kDown);

#endif
