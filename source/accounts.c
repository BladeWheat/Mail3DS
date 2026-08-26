// accounts.c - 账户管理
#include "accounts.h"
#include "cache.h"
#include <stdio.h>
#include <sys/stat.h>

#define CONFIG_PATH "sdmc:/3ds/Mail3DS/accounts.dat"
#define SETTINGS_PATH "sdmc:/3ds/Mail3DS/settings.ini"
#define CONFIG_VERSION 2

// 简单XOR加密密钥（存储时混淆授权码，非强加密）
static const u8 XOR_KEY[] = {0x3D, 0x53, 0x45, 0x6D, 0x61, 0x69, 0x6C, 0x32, 0x30, 0x32, 0x34, 0x21};

static void xor_encrypt(char* data, int len)
{
    for (int i = 0; i < len; i++)
        data[i] ^= XOR_KEY[i % (sizeof(XOR_KEY))];
}

void Accounts_Init(void)
{
    for (int i = 0; i < MAX_ACCOUNTS; i++)
    {
        g_app.accounts[i].added = false;
        memset(g_app.accounts[i].email, 0, sizeof(g_app.accounts[i].email));
        memset(g_app.accounts[i].password, 0, sizeof(g_app.accounts[i].password));
        memset(g_app.accounts[i].imapServer, 0, sizeof(g_app.accounts[i].imapServer));
        memset(g_app.accounts[i].smtpServer, 0, sizeof(g_app.accounts[i].smtpServer));
        g_app.accounts[i].imapPort = 993;
        g_app.accounts[i].smtpPort = 465;
        g_app.accounts[i].type = ACC_OTHER;
    }
    g_app.accountCount = 0;
}

int Accounts_GetCount(void)
{
    int count = 0;
    for (int i = 0; i < MAX_ACCOUNTS; i++)
    {
        if (g_app.accounts[i].added) count++;
    }
    return count;
}

int Accounts_GetFirstEmpty(void)
{
    for (int i = 0; i < MAX_ACCOUNTS; i++)
    {
        if (!g_app.accounts[i].added) return i;
    }
    return -1;
}

AccountType Accounts_DetectType(const char* email)
{
    if (strstr(email, "@qq.com")) return ACC_QQ;
    if (strstr(email, "@163.com") || strstr(email, "@126.com")) return ACC_163;
    if (strstr(email, "@gmail.com")) return ACC_GMAIL;
    if (strstr(email, "@outlook.com") || strstr(email, "@hotmail.com") || strstr(email, "@live.com")) return ACC_OUTLOOK;
    return ACC_OTHER;
}

void Accounts_GetServerInfo(AccountType type, const char** imap, const char** smtp, int* imapPort, int* smtpPort)
{
    switch (type)
    {
        case ACC_QQ:
            *imap = "imap.qq.com"; *smtp = "smtp.qq.com";
            *imapPort = 993; *smtpPort = 465;
            break;
        case ACC_163:
            *imap = "imap.163.com"; *smtp = "smtp.163.com";
            *imapPort = 993; *smtpPort = 465;
            break;
        case ACC_GMAIL:
            *imap = "imap.gmail.com"; *smtp = "smtp.gmail.com";
            *imapPort = 993; *smtpPort = 465;
            break;
        case ACC_OUTLOOK:
            *imap = "outlook.office365.com"; *smtp = "smtp.office365.com";
            *imapPort = 993; *smtpPort = 587;
            break;
        default:
            *imap = "imap.example.com"; *smtp = "smtp.example.com";
            *imapPort = 993; *smtpPort = 465;
            break;
    }
}

bool Accounts_Add(const char* email, const char* password)
{
    int idx = Accounts_GetFirstEmpty();
    if (idx < 0) return false;

    Account* acc = &g_app.accounts[idx];
    acc->added = true;
    acc->fetchBlocked = false;
    strncpy(acc->email, email, sizeof(acc->email) - 1);
    strncpy(acc->password, password, sizeof(acc->password) - 1);
    acc->type = Accounts_DetectType(email);

    const char *imap, *smtp;
    Accounts_GetServerInfo(acc->type, &imap, &smtp, &acc->imapPort, &acc->smtpPort);
    strncpy(acc->imapServer, imap, sizeof(acc->imapServer) - 1);
    strncpy(acc->smtpServer, smtp, sizeof(acc->smtpServer) - 1);

    g_app.accountCount = Accounts_GetCount();

    // 保存到文件
    Accounts_SaveConfig();

    // 创建该账户的缓存目录结构
    Cache_InitAccount(email);

    return true;
}

void Accounts_Remove(int index)
{
    if (index < 0 || index >= MAX_ACCOUNTS) return;
    g_app.accounts[index].added = false;
    memset(g_app.accounts[index].email, 0, sizeof(g_app.accounts[index].email));
    memset(g_app.accounts[index].password, 0, sizeof(g_app.accounts[index].password));
    g_app.accountCount = Accounts_GetCount();
    Accounts_SaveConfig();
}

// 从SD卡加载账户配置
bool Accounts_LoadConfig(void)
{
    FILE* f = fopen(CONFIG_PATH, "rb");
    if (!f) return false;

    // 读取魔数验证
    char magic[4];
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return false; }
    if (magic[0] != 'M' || magic[1] != '3' || magic[2] != 'D' || magic[3] != 'S')
    {
        fclose(f);
        return false;
    }

    // 读取版本
    u8 version;
    if (fread(&version, 1, 1, f) != 1) { fclose(f); return false; }

    // 读取账户数量
    u8 count;
    if (fread(&count, 1, 1, f) != 1) { fclose(f); return false; }

    // 读取每个账户
    for (int i = 0; i < count && i < MAX_ACCOUNTS; i++)
    {
        Account acc;
        memset(&acc, 0, sizeof(acc));

        // added标志
        u8 added;
        if (fread(&added, 1, 1, f) != 1) break;
        acc.added = (added != 0);

        if (acc.added)
        {
            // 邮箱
            u8 len;
            if (fread(&len, 1, 1, f) != 1) break;
            if (len > 0 && len < sizeof(acc.email))
                fread(acc.email, 1, len, f);
            // 密码
            if (fread(&len, 1, 1, f) != 1) break;
            if (len > 0 && len < sizeof(acc.password))
            {
                fread(acc.password, 1, len, f);
                acc.password[len] = 0;
                // v2及以上版本密码是加密的，需要解密
                if (version >= CONFIG_VERSION)
                    xor_encrypt(acc.password, len);
            }

            acc.type = Accounts_DetectType(acc.email);
            const char *imap, *smtp;
            Accounts_GetServerInfo(acc.type, &imap, &smtp, &acc.imapPort, &acc.smtpPort);
            strncpy(acc.imapServer, imap, sizeof(acc.imapServer) - 1);
            strncpy(acc.smtpServer, smtp, sizeof(acc.smtpServer) - 1);

            g_app.accounts[i] = acc;
        }
    }

    fclose(f);
    g_app.accountCount = Accounts_GetCount();
    return true;
}

// 保存账户配置到SD卡
bool Accounts_SaveConfig(void)
{
    FILE* f = fopen(CONFIG_PATH, "wb");
    if (!f) return false;

    // 魔数 "M3DS"
    fwrite("M3DS", 1, 4, f);
    // 版本
    u8 version = CONFIG_VERSION;
    fwrite(&version, 1, 1, f);
    // 账户数量
    u8 count = MAX_ACCOUNTS;
    fwrite(&count, 1, 1, f);

    // 写入每个账户
    for (int i = 0; i < MAX_ACCOUNTS; i++)
    {
        Account* acc = &g_app.accounts[i];
        u8 added = acc->added ? 1 : 0;
        fwrite(&added, 1, 1, f);

        if (acc->added)
        {
            u8 len;
            // 邮箱（明文）
            len = (u8)strlen(acc->email);
            fwrite(&len, 1, 1, f);
            fwrite(acc->email, 1, len, f);
            // 密码（XOR加密后保存）
            char encPass[128];
            int passLen = (int)strlen(acc->password);
            strncpy(encPass, acc->password, sizeof(encPass) - 1);
            encPass[passLen] = 0;
            xor_encrypt(encPass, passLen);
            len = (u8)passLen;
            fwrite(&len, 1, 1, f);
            fwrite(encPass, 1, len, f);
        }
    }

    fclose(f);
    return true;
}

// ========== 设置持久化 ==========

// 从SD卡加载设置，覆盖默认值
void Settings_Load(void)
{
    FILE* f = fopen(SETTINGS_PATH, "r");
    if (!f) return; // 文件不存在，保持默认值

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        // 去除行尾换行
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
            line[--len] = 0;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line;
        const char* val = eq + 1;

        if (strcmp(key, "emailsPerPage") == 0)
        {
            int v = atoi(val);
            // 只允许合法值：7/10/15/20
            if (v == 7 || v == 10 || v == 15 || v == 20)
                g_app.emailsPerPage = v;
        }
        else if (strcmp(key, "notifEnabled") == 0)
            g_app.notifEnabled = (atoi(val) != 0);
        else if (strcmp(key, "notifSound") == 0)
            g_app.notifSound = (atoi(val) != 0);
        else if (strcmp(key, "notifLED") == 0)
            g_app.notifLED = (atoi(val) != 0);
        else if (strcmp(key, "autoCheckInterval") == 0)
        {
            int v = atoi(val);
            if (v >= 0 && v <= 4)
                g_app.autoCheckInterval = v;
        }
    }
    fclose(f);
}

// 保存设置到SD卡
void Settings_Save(void)
{
    // 确保目录存在
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/Mail3DS", 0777);

    FILE* f = fopen(SETTINGS_PATH, "w");
    if (!f) return;

    fprintf(f, "emailsPerPage=%d\n", g_app.emailsPerPage);
    fprintf(f, "notifEnabled=%d\n", g_app.notifEnabled ? 1 : 0);
    fprintf(f, "notifSound=%d\n", g_app.notifSound ? 1 : 0);
    fprintf(f, "notifLED=%d\n", g_app.notifLED ? 1 : 0);
    fprintf(f, "autoCheckInterval=%d\n", g_app.autoCheckInterval);

    fclose(f);
}
