// network.h - 网络模块（IMAP/SMTP via libcurl）
#ifndef NETWORK_H
#define NETWORK_H

#include "common.h"

// 连接测试结果
typedef struct {
    bool finished;
    bool imapOk;
    bool smtpOk;
    char error[128];
} ConnectTestResult;

// 邮件拉取结果
typedef struct {
    bool finished;
    bool success;
    int count;       // 本页拉取到的邮件数
    int totalMails;  // 账户总邮件数
    char error[128];
} FetchResult;

bool Network_Init(void);
void Network_Shutdown(void);
bool Network_CheckConnection(void);

void Network_TestConnection(const char* imapServer, int imapPort,
                            const char* smtpServer, int smtpPort,
                            const char* email, const char* password,
                            ConnectTestResult* result);

// 分页拉取邮件：page从0开始，perPage为每页邮件数
int Network_FetchMailsPage(int accountIndex, int page, int perPage, FetchResult* result);

// 只获取账户总邮件数（用于检测新邮件），返回-1失败
int Network_GetTotalMails(int accountIndex);

bool Network_FetchMailBody(int accountIndex, int imapSeq);

bool Network_SendMail(int accountIndex, const char* to,
                      const char* subject, const char* body);

void Network_RefreshMails(void);

#endif
