// accounts.h - 账户管理
#ifndef ACCOUNTS_H
#define ACCOUNTS_H

#include "common.h"

void Accounts_Init(void);
int Accounts_GetCount(void);
int Accounts_GetFirstEmpty(void);
bool Accounts_Add(const char* email, const char* password);
void Accounts_Remove(int index);
AccountType Accounts_DetectType(const char* email);
void Accounts_GetServerInfo(AccountType type, const char** imap, const char** smtp, int* imapPort, int* smtpPort);

// 配置文件读写
bool Accounts_LoadConfig(void);
bool Accounts_SaveConfig(void);

// 设置持久化（sdmc:/3ds/Mail3DS/settings.ini）
void Settings_Load(void);
void Settings_Save(void);

#endif
