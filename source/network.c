// network.c - 网络模块（IMAP/SMTP via libcurl）
#include "network.h"
#include "accounts.h"
#include "cache.h"
#include "email.h"
#include <3ds.h>
#include <curl/curl.h>
#include <mbedtls/base64.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <iconv.h>
#include <errno.h>

static bool s_socInitialized = false;
static bool s_curlInitialized = false;

// ========== 辅助函数 ==========

// 不区分大小写的字符串搜索（devkitARM没有stristr）
static const char* stristr(const char* haystack, const char* needle)
{
    if (!haystack || !needle || !*needle) return haystack;
    for (; *haystack; haystack++)
    {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n))
        { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

// ========== 初始化/清理 ==========

bool Network_Init(void)
{
    if (s_socInitialized) return true;

    acInit();

    void* socBuffer = memalign(0x1000, 0x100000);
    if (socBuffer)
    {
        Result rc = socInit((u32*)socBuffer, 0x100000);
        if (R_SUCCEEDED(rc))
            s_socInitialized = true;
        else
            free(socBuffer);
    }

    // 初始化libcurl
    if (!s_curlInitialized)
    {
        if (curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK)
            s_curlInitialized = true;
    }

    return true;
}

void Network_Shutdown(void)
{
    if (s_curlInitialized)
    {
        curl_global_cleanup();
        s_curlInitialized = false;
    }
    if (s_socInitialized)
    {
        socExit();
        s_socInitialized = false;
    }
    acExit();
}

bool Network_CheckConnection(void)
{
    u32 status = 0;
    Result rc = ACU_GetWifiStatus(&status);
    return (R_SUCCEEDED(rc) && status != 0);
}

// ========== libcurl写入回调 ==========

struct WriteBuf {
    char* data;
    size_t size;
    size_t capacity;
};

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    struct WriteBuf* buf = (struct WriteBuf*)userdata;
    size_t total = size * nmemb;
    // 最大接收1MB，防止大邮件导致内存不足
    if (buf->size + total + 1 > 1024 * 1024)
        total = 1024 * 1024 - buf->size - 1;
    if (total == 0) return 0;
    if (buf->size + total + 1 > buf->capacity)
    {
        size_t newCap = buf->capacity * 2 + total + 256;
        if (newCap > 1024 * 1024) newCap = 1024 * 1024;
        char* newData = realloc(buf->data, newCap);
        if (!newData) return 0;
        buf->data = newData;
        buf->capacity = newCap;
    }
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = 0;
    return total;
}

static void buf_init(struct WriteBuf* buf)
{
    buf->capacity = 8192;
    buf->data = malloc(buf->capacity);
    buf->size = 0;
    if (buf->data) buf->data[0] = 0;
}

static void buf_free(struct WriteBuf* buf)
{
    if (buf->data)
    {
        memset(buf->data, 0, buf->size);
        free(buf->data);
        buf->data = NULL;
    }
}

static void buf_append(struct WriteBuf* buf, const char* data, size_t len)
{
    if (buf->size + len + 1 > buf->capacity)
    {
        size_t newCap = buf->capacity * 2 + len + 256;
        if (newCap > 4*1024*1024) newCap = 4*1024*1024;
        char* newData = realloc(buf->data, newCap);
        if (!newData) return;
        buf->data = newData;
        buf->capacity = newCap;
    }
    memcpy(buf->data + buf->size, data, len);
    buf->size += len;
    buf->data[buf->size] = 0;
}

// 静态日志宏（发布版本禁用SD卡日志写入以提升速度）
#define IMAP_LOG_STATIC(...)

// ========== 通用curl设置 ==========

static void setup_curl_common(CURL* curl, const char* url,
                              const char* user, const char* pass,
                              struct WriteBuf* buf)
{
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, pass);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    // 3DS上暂时禁用证书验证（后续可添加cacert.pem）
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_TRY);
    // 允许详细调试（发布时关闭）
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
}

// ========== 连接测试 ==========

// 前向声明：原始IMAP连接函数（定义在后面）
typedef struct { CURL* curl; int tagCounter; } ImapConn;
static int imap_connect(ImapConn* ic, const char* server, int port,
                        const char* user, const char* pass);
static void imap_disconnect(ImapConn* ic);
static int imap_send_recv(ImapConn* ic, const char* command,
                          char** responseBuf, size_t* responseSize);

void Network_TestConnection(const char* imapServer, int imapPort,
                            const char* smtpServer, int smtpPort,
                            const char* email, const char* password,
                            ConnectTestResult* result)
{
    memset(result, 0, sizeof(ConnectTestResult));
    result->finished = false;

    if (!s_curlInitialized)
    {
        snprintf(result->error, sizeof(result->error), "网络未初始化");
        result->finished = true;
        return;
    }

    // 测试IMAP连接：使用原始IMAP命令验证登录+SELECT+SEARCH
    {
        ImapConn ic;
        if (imap_connect(&ic, imapServer, imapPort, email, password) != 0)
        {
            result->imapOk = false;
            snprintf(result->error, sizeof(result->error), "IMAP连接失败，请检查邮箱地址和授权码");
        }
        else
        {
            // SELECT INBOX
            char* resp = NULL;
            size_t respLen = 0;
            int rc = imap_send_recv(&ic, "SELECT INBOX", &resp, &respLen);
            if (rc != 0)
            {
                result->imapOk = false;
                snprintf(result->error, sizeof(result->error), "无法打开收件箱");
            }
            else
            {
                free(resp); resp = NULL;
                // UID SEARCH ALL 验证可以获取邮件列表
                rc = imap_send_recv(&ic, "UID SEARCH ALL", &resp, &respLen);
                result->imapOk = (rc == 0);
                if (!result->imapOk)
                    snprintf(result->error, sizeof(result->error), "无法获取邮件列表");
            }
            if (resp) free(resp);
            imap_disconnect(&ic);
        }
    }

    // IMAP成功后测试SMTP
    if (result->imapOk)
    {
        CURL* curl = curl_easy_init();
        if (curl)
        {
            struct WriteBuf buf;
            buf_init(&buf);
            char url[256];
            snprintf(url, sizeof(url), "smtps://%s:%d", smtpServer, smtpPort);
            setup_curl_common(curl, url, email, password, &buf);
            // SMTP只连接不发送
            curl_easy_setopt(curl, CURLOPT_MAIL_FROM, NULL);
            struct curl_slist* recipients = NULL;
            recipients = curl_slist_append(recipients, email);
            curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
            // 提供空邮件数据
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
            curl_easy_setopt(curl, CURLOPT_READDATA, NULL);
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        // 使用NOOP验证连接而不实际发送
        CURLcode res = curl_easy_perform(curl);
        // SMTP连接测试：CURLE_OK或部分错误（如没有收件人）也算连接成功
        result->smtpOk = (res == CURLE_OK || res == CURLE_RECV_ERROR ||
                          res == CURLE_GOT_NOTHING);
        if (!result->smtpOk)
        {
            // SMTP失败不阻断，只记录警告
            snprintf(result->error, sizeof(result->error),
                     "SMTP: %s（收件仍可正常使用）", curl_easy_strerror(res));
        }
        curl_slist_free_all(recipients);
        buf_free(&buf);
        curl_easy_cleanup(curl);
        }
    }

    result->finished = true;
}

// ========== 邮件拉取 ==========

// 自实现Base64解码（不依赖mbedtls，避免3DS平台兼容性问题）
// 返回解码后的字节数，-1表示失败
static int base64_decode_simple(const char* in, int inLen, unsigned char* out, int outMax)
{
    static const signed char table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    int outLen = 0;
    int val = 0;
    int bits = 0;
    for (int i = 0; i < inLen; i++)
    {
        unsigned char c = (unsigned char)in[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
        signed char v = table[c];
        if (v < 0) continue; // 跳过非法字符
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (outLen >= outMax) return -1;
            out[outLen++] = (unsigned char)((val >> bits) & 0xFF);
        }
    }
    return outLen;
}

// 解析RFC 2822邮件日期，转换为本地时间
// 返回0成功，-1失败；结果写入year/month/day/hour/minute/timestamp
static int parse_mail_date(const char* rawDate,
                           int* outYear, int* outMonth, int* outDay,
                           int* outHour, int* outMin, long long* outTimestamp)
{
    *outYear = *outMonth = *outDay = *outHour = *outMin = 0;
    *outTimestamp = 0;
    if (!rawDate || !*rawDate) return -1;

    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    char mon[8] = {0};
    int tzSign = 1, tzHours = 0, tzMins = 0;
    int monthNum = 0;
    static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};

    const char* p = rawDate;
    // 跳过开头空白
    while (*p == ' ' || *p == '\t') p++;
    // 如果以字母开头（星期几），跳过到逗号后
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
    {
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    while (*p == ' ' || *p == '\t') p++;

    // 支持ISO 8601格式：2026-08-16T12:01:00+08:00
    if (p[0] >= '0' && p[0] <= '9' && p[4] == '-' && p[7] == '-')
    {
        year = atoi(p);
        monthNum = atoi(p + 5);
        day = atoi(p + 8);
        const char* tp = strchr(p, 'T');
        if (!tp) tp = strchr(p, 't');
        if (!tp) tp = strchr(p, ' ');
        if (tp)
        {
            hour = atoi(tp + 1);
            const char* colon = strchr(tp + 1, ':');
            if (colon) min = atoi(colon + 1);
        }
        // 解析时区
        const char* tzp = strchr(p, '+');
        if (!tzp) tzp = strchr(p, '-');
        if (tzp && tzp > p + 9)
        {
            tzSign = (*tzp == '-') ? -1 : 1;
            tzp++;
            if (tzp[0] >= '0' && tzp[0] <= '9' && tzp[1] >= '0' && tzp[1] <= '9')
                tzHours = (tzp[0]-'0')*10 + (tzp[1]-'0');
            if (tzp[2] == ':' && tzp[3] >= '0' && tzp[4] >= '0')
                tzMins = (tzp[3]-'0')*10 + (tzp[4]-'0');
            else if (tzp[2] >= '0' && tzp[3] >= '0')
                tzMins = (tzp[2]-'0')*10 + (tzp[3]-'0');
        }
        goto parse_date_compute;
    }

    // 解析日
    day = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ') p++;

    // 解析月
    int mi = 0;
    while (*p && *p != ' ' && mi < 3) mon[mi++] = *p++;
    mon[mi] = 0;
    while (*p == ' ') p++;

    // 解析年
    year = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ') p++;

    // 解析时:分:秒
    hour = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    if (*p == ':') p++;
    min = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    if (*p == ':')
    {
        p++;
        sec = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    }
    while (*p == ' ') p++;

    // 解析时区偏移（如 +0800 或 -0400）
    if (*p == '+' || *p == '-')
    {
        tzSign = (*p == '-') ? -1 : 1;
        p++;
        // 手动解析4位数字时区，避免atoi("0800")返回800的bug
        if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
            p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9')
        {
            tzHours = (p[0]-'0')*10 + (p[1]-'0');
            tzMins  = (p[2]-'0')*10 + (p[3]-'0');
        }
    }
    // 支持命名时区（如 "CST"、"EST"、"GMT"、"UTC"）
    if (tzHours == 0 && tzMins == 0 && *p != '+' && *p != '-')
    {
        // 跳过空格
        while (*p == ' ') p++;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
        {
            char tzName[8] = {0};
            int ti = 0;
            while (*p && *p != ' ' && *p != '\r' && *p != '\n' && ti < 7)
                tzName[ti++] = *p++;
            tzName[ti] = 0;
            // 常见时区缩写表（相对于UTC的小时偏移）
            static const struct { const char* abbr; int offsetHours; } tzTable[] = {
                {"UTC", 0}, {"GMT", 0}, {"UT", 0}, {"Z", 0},
                {"CST", 8}, {"HKT", 8}, {"SGT", 8}, {"AWST", 8}, {"PHT", 8}, {"MYT", 8},
                {"JST", 9}, {"KST", 9},
                {"EST", -5}, {"EDT", -4}, {"CDT", -5},
                {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7},
                {"CET", 1}, {"CEST", 2}, {"EET", 2}, {"EEST", 3},
                {"IST", 5}, {"MSK", 3}, {"AEST", 10}, {"AEDT", 11},
                {"NZST", 12}, {"NZDT", 13}, {"HST", -10}, {"AKST", -9},
                {NULL, 0}
            };
            // 中国标准时间CST=+8优先匹配（中文邮件常见）
            // 简单处理：如果tzName是CST，默认按+8（中国标准时间）
            for (int ti2 = 0; tzTable[ti2].abbr; ti2++)
            {
                if (strcasecmp(tzName, tzTable[ti2].abbr) == 0)
                {
                    tzSign = tzTable[ti2].offsetHours >= 0 ? 1 : -1;
                    tzHours = tzTable[ti2].offsetHours >= 0 ? tzTable[ti2].offsetHours : -tzTable[ti2].offsetHours;
                    break;
                }
            }
        }
    }

    for (int m = 0; m < 12; m++)
        if (strncasecmp(mon, months[m], 3) == 0) { monthNum = m + 1; break; }

parse_date_compute:
    if (year < 1970 || monthNum < 1 || day < 1) return -1;

    // 计算UTC时间戳（手动计算，不依赖timegm）

    // 计算UTC时间戳（手动计算，不依赖timegm）
    // 先计算从1970到该年1月1日的秒数
    long long timestamp = 0;
    for (int y = 1970; y < year; y++)
    {
        int leap = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 1 : 0;
        timestamp += (long long)(365 + leap) * 86400;
    }
    // 加上该年1月1日到该月1日的天数
    static const int mDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leapYear = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) ? 1 : 0;
    for (int m = 0; m < monthNum - 1; m++)
    {
        timestamp += (long long)mDays[m] * 86400;
        if (m == 1 && leapYear) timestamp += 86400;
    }
    // 加上日、时、分、秒
    timestamp += (long long)(day - 1) * 86400;
    timestamp += (long long)hour * 3600;
    timestamp += (long long)min * 60;
    timestamp += sec;

    // timestamp现在是邮件发送方时区的本地时间戳
    // 转换为UTC：减去发送方时区偏移
    int tzOffsetSec = tzSign * (tzHours * 3600 + tzMins * 60);
    long long utcTimestamp = timestamp - tzOffsetSec;

    // 获取设备本地时区偏移（秒）
    // 用当前时间的localtime/gmtime差值计算，只算一次
    long long localOffset = 0;
    {
        time_t now = time(NULL);
        struct tm ltBuf, gtBuf;
        struct tm* ltTmp = localtime(&now);
        if (ltTmp) { ltBuf = *ltTmp; } else { memset(&ltBuf, 0, sizeof(ltBuf)); }
        struct tm* gtTmp = gmtime(&now);
        if (gtTmp) { gtBuf = *gtTmp; } else { memset(&gtBuf, 0, sizeof(gtBuf)); }
        if (ltTmp && gtTmp)
        {
            localOffset = (long long)(ltBuf.tm_hour - gtBuf.tm_hour) * 3600
                        + (long long)(ltBuf.tm_min - gtBuf.tm_min) * 60;
            // 处理日期跨越
            int ydayDiff = ltBuf.tm_yday - gtBuf.tm_yday;
            int yearDiff = ltBuf.tm_year - gtBuf.tm_year;
            if (yearDiff != 0)
                ydayDiff += yearDiff * 365;
            if (ydayDiff > 1 || ydayDiff < -1)
                localOffset += (ydayDiff > 0 ? 1 : -1) * 86400;
            else if (ydayDiff != 0)
                localOffset += ydayDiff * 86400;
        }
        else
        {
            localOffset = 8 * 3600; // 默认UTC+8（中国标准时间）
        }
    }

    long long localTimestamp = utcTimestamp + localOffset;

    // 手动从时间戳计算年月日时分，不依赖localtime（避免3DS newlib兼容问题）
    {
        long long ts = localTimestamp;
        long long days = ts / 86400;
        long long rem = ts % 86400;
        if (rem < 0) { rem += 86400; days--; }
        int hour = (int)(rem / 3600);
        int minute = (int)((rem % 3600) / 60);

        // 从1970-01-01开始计算年
        int y = 1970;
        while (1)
        {
            int leap = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 1 : 0;
            long long ydays = 365 + leap;
            if (days < ydays) break;
            days -= ydays;
            y++;
        }
        int leapYear = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 1 : 0;
        static const int mDaysArr[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        int mo = 0;
        for (mo = 0; mo < 12; mo++)
        {
            int md = mDaysArr[mo] + (mo == 1 && leapYear ? 1 : 0);
            if (days < md) break;
            days -= md;
        }
        *outYear = y;
        *outMonth = mo + 1;
        *outDay = (int)days + 1;
        *outHour = hour;
        *outMin = minute;
        *outTimestamp = localTimestamp;
        return 0;
    }
}

// MIME编码字符串解码：=?charset?B?base64?= 或 =?charset?Q?qp?=
// 将指定编码的字节转换为UTF-8，返回转换后的长度（写入out）
// 解码出错时使用 U+FFFD（�）作为占位符，而非问号
static int convert_to_utf8(const char* charset, const char* in, int inLen,
                           char* out, int outSize)
{
    if (!charset || !in || inLen <= 0 || !out || outSize <= 0) return 0;

    // U+FFFD 替换字符的UTF-8编码
    static const char REPLACEMENT[3] = {(char)0xEF, (char)0xBF, (char)0xBD};

    // UTF-8和ASCII不需要转换，但需要验证并替换无效字节
    if (strcasecmp(charset, "UTF-8") == 0 ||
        strcasecmp(charset, "US-ASCII") == 0 ||
        strcasecmp(charset, "ASCII") == 0)
    {
        int outPos = 0;
        int i = 0;
        while (i < inLen && outPos < outSize - 1)
        {
            unsigned char c = (unsigned char)in[i];
            int seqLen = 0;
            // 判断UTF-8序列长度
            if (c < 0x80) seqLen = 1;
            else if ((c & 0xE0) == 0xC0) seqLen = 2;
            else if ((c & 0xF0) == 0xE0) seqLen = 3;
            else if ((c & 0xF8) == 0xF0) seqLen = 4;
            else {
                // 无效起始字节，插入U+FFFD
                if (outPos + 3 >= outSize) break;
                memcpy(out + outPos, REPLACEMENT, 3);
                outPos += 3;
                i++;
                continue;
            }
            // 检查序列是否完整且后续字节合法
            if (i + seqLen > inLen)
            {
                // 不完整序列，插入U+FFFD
                if (outPos + 3 >= outSize) break;
                memcpy(out + outPos, REPLACEMENT, 3);
                outPos += 3;
                break;
            }
            bool valid = true;
            for (int j = 1; j < seqLen; j++)
            {
                if (((unsigned char)in[i+j] & 0xC0) != 0x80)
                { valid = false; break; }
            }
            if (!valid)
            {
                if (outPos + 3 >= outSize) break;
                memcpy(out + outPos, REPLACEMENT, 3);
                outPos += 3;
                i++;
                continue;
            }
            // 合法UTF-8序列，直接复制
            if (outPos + seqLen >= outSize) break;
            memcpy(out + outPos, in + i, seqLen);
            outPos += seqLen;
            i += seqLen;
        }
        out[outPos] = 0;
        return outPos;
    }

    iconv_t cd = iconv_open("UTF-8", charset);
    if (cd == (iconv_t)-1)
    {
        // 转换不支持，直接复制原始字节（不插入替换符，保留原始数据）
        int copyLen = inLen < outSize - 1 ? inLen : outSize - 1;
        memcpy(out, in, copyLen);
        out[copyLen] = 0;
        return copyLen;
    }

    char* inBuf = (char*)in;
    size_t inLeft = inLen;
    char* outBuf = out;
    size_t outLeft = outSize - 1;

    while (inLeft > 0)
    {
        size_t ret = iconv(cd, &inBuf, &inLeft, &outBuf, &outLeft);
        if (ret == (size_t)-1)
        {
            if (errno == EILSEQ)
            {
                // 无效多字节序列：跳过1个输入字节，插入U+FFFD
                if (outLeft < 3) break;
                memcpy(outBuf, REPLACEMENT, 3);
                outBuf += 3;
                outLeft -= 3;
                inBuf++;
                inLeft--;
            }
            else if (errno == EINVAL)
            {
                // 末尾不完整序列：插入U+FFFD
                if (outLeft < 3) break;
                memcpy(outBuf, REPLACEMENT, 3);
                outBuf += 3;
                outLeft -= 3;
                break;
            }
            else if (errno == E2BIG)
            {
                // 输出缓冲区满
                break;
            }
            else
            {
                // 其他错误，跳过1字节继续
                inBuf++;
                inLeft--;
            }
        }
    }

    *outBuf = 0;
    int result = (int)(outBuf - out);
    iconv_close(cd);
    return result;
}

static void decode_mime_string(const char* in, char* out, int outSize)
{
    if (!in || !out || outSize <= 0) return;
    out[0] = 0;
    int outPos = 0;
    const char* p = in;

    while (*p && outPos < outSize - 1)
    {
        const char* encStart = strstr(p, "=?");
        if (!encStart)
        {
            // 剩余部分直接复制
            while (*p && outPos < outSize - 1)
                out[outPos++] = *p++;
            break;
        }
        // 复制编码部分前的普通文本
        while (p < encStart && outPos < outSize - 1)
            out[outPos++] = *p++;

        // 解析 =?charset?encoding?data?=
        const char* q1 = strchr(encStart + 2, '?');
        if (!q1) break;
        const char* q2 = strchr(q1 + 1, '?');
        if (!q2) break;
        // q2+1指向数据开始，找数据结束的'?'（后面必须跟'='）
        const char* q3 = strchr(q2 + 1, '?');
        if (!q3 || q3[1] != '=') break;
        const char* encEnd = q3;  // encEnd指向"?="中的'?'

        // 提取charset：从encStart+2到q1
        char charset[64] = {0};
        int csLen = (int)(q1 - (encStart + 2));
        if (csLen >= (int)sizeof(charset)) csLen = (int)sizeof(charset) - 1;
        strncpy(charset, encStart + 2, csLen);
        charset[csLen] = 0;

        // encoding是q1后面的单个字符（B或Q）
        char encoding = toupper((unsigned char)q1[1]);
        int dataLen = (int)(encEnd - (q2 + 1));
        char* data = (char*)malloc(dataLen + 1);
        if (!data) break;
        strncpy(data, q2 + 1, dataLen);
        data[dataLen] = 0;

        // 解码到临时缓冲区
        unsigned char* dec = (unsigned char*)malloc(4096);
        int decLen = 0;
        if (dec)
        {
            if (encoding == 'B')
            {
                // 去除Base64数据中的空白字符（换行、空格、制表符）
                char* clean = (char*)malloc(dataLen + 1);
                if (clean)
                {
                    int ci = 0;
                    for (int j = 0; j < dataLen; j++)
                    {
                        char c = data[j];
                        if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
                            clean[ci++] = c;
                    }
                    clean[ci] = 0;
                    // 使用自实现Base64解码（不依赖mbedtls）
                    decLen = base64_decode_simple(clean, ci, dec, 4095);
                    if (decLen < 0) decLen = 0;
                    free(clean);
                }
            }
            else if (encoding == 'Q')
            {
                for (int i = 0; data[i] && decLen < 4095; i++)
                {
                    if (data[i] == '_')
                        dec[decLen++] = ' ';
                    else if (data[i] == '=' && i + 2 < dataLen)
                    {
                        char hex[3] = {data[i+1], data[i+2], 0};
                        dec[decLen++] = (unsigned char)strtol(hex, NULL, 16);
                        i += 2;
                    }
                    else
                        dec[decLen++] = (unsigned char)data[i];
                }
            }

            if (decLen > 0)
            {
                // 转换为UTF-8并复制到输出
                int converted = convert_to_utf8(charset, (const char*)dec, decLen,
                                                out + outPos, outSize - outPos);
                outPos += converted;
            }
            free(dec);
        }
        free(data);
        p = encEnd + 2;
    }
    out[outPos] = 0;
    // 去掉末尾空白
    while (outPos > 0 && (out[outPos-1] == ' ' || out[outPos-1] == '\t' ||
           out[outPos-1] == '\r' || out[outPos-1] == '\n'))
        out[--outPos] = 0;
}

// 从头部中提取指定字段值（返回指向值开头的指针，设置lineEnd）
static const char* find_header(const char* headers, const char* name, const char** lineEnd)
{
    const char* p = headers;
    size_t nameLen = strlen(name);
    while (*p)
    {
        if (strncasecmp(p, name, nameLen) == 0 && p[nameLen] == ':')
        {
            const char* val = p + nameLen + 1;
            while (*val == ' ' || *val == '\t') val++;
            // 找到值的结束位置（包括折叠的后续行）
            const char* end = val;
            while (*end && *end != '\n')
            {
                end++;
                // 如果下一行以空格/tab开头，是折叠头，继续包含
                if (*end == '\n' && (end[1] == ' ' || end[1] == '\t'))
                    end++;
            }
            if (lineEnd) *lineEnd = end;
            return val;
        }
        // 跳到下一行
        const char* nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return NULL;
}

// 从邮件数据中提取正文预览
static void extract_preview(const char* contentType, const char* cte,
                            const char* body, int bodyLen,
                            char* preview, int previewSize)
{
    preview[0] = 0;
    if (!body || bodyLen <= 0) return;

    const char* textBody = body;
    int textLen = bodyLen;

    if (contentType && stristr(contentType, "multipart/"))
    {
        const char* bnd = stristr(contentType, "boundary=");
        if (bnd)
        {
            bnd += 9;
            char boundary[128] = {0};
            int bi = 0;
            if (*bnd == '"')
            {
                bnd++;
                while (*bnd && *bnd != '"' && bi < (int)sizeof(boundary) - 1)
                    boundary[bi++] = *bnd++;
            }
            else
            {
                while (*bnd && *bnd != ';' && *bnd != '\r' && *bnd != '\n' && bi < (int)sizeof(boundary) - 1)
                    boundary[bi++] = *bnd++;
            }
            while (bi > 0 && boundary[bi-1] == ' ') boundary[--bi] = 0;

            if (boundary[0])
            {
                char delim[140];
                snprintf(delim, sizeof(delim), "--%s", boundary);
                const char* part = strstr(body, delim);
                if (part)
                {
                    part += strlen(delim);
                    if (*part == '\r') part++;
                    if (*part == '\n') part++;

                    const char* partHeaderEnd = strstr(part, "\r\n\r\n");
                    if (!partHeaderEnd) partHeaderEnd = strstr(part, "\n\n");
                    if (partHeaderEnd)
                    {
                        char partCT[512] = {0};
                        int hdrLen = (int)(partHeaderEnd - part);
                        if (hdrLen > (int)sizeof(partCT) - 1) hdrLen = (int)sizeof(partCT) - 1;
                        strncpy(partCT, part, hdrLen);
                        partCT[hdrLen] = 0;

                        const char* partCTVal = find_header(partCT, "Content-Type", NULL);
                        const char* partCTEVal = find_header(partCT, "Content-Transfer-Encoding", NULL);

                        if (partCTVal && !stristr(partCTVal, "text/plain"))
                        {
                            const char* nextPart = strstr(partHeaderEnd, delim);
                            if (nextPart)
                            {
                                part = nextPart + strlen(delim);
                                if (*part == '\r') part++;
                                if (*part == '\n') part++;
                                partHeaderEnd = strstr(part, "\r\n\r\n");
                                if (!partHeaderEnd) partHeaderEnd = strstr(part, "\n\n");
                            }
                        }

                        if (partHeaderEnd)
                        {
                            textBody = partHeaderEnd + (partHeaderEnd[1] == '\n' ? 2 : 4);
                            const char* nextDelim = strstr(textBody, delim);
                            textLen = nextDelim ? (int)(nextDelim - textBody) : (int)(body + bodyLen - textBody);
                            if (partCTEVal) cte = partCTEVal;
                        }
                    }
                }
            }
        }
    }

    char* decoded = (char*)malloc(16384);
    char* b64 = (char*)malloc(16384);
    if (!decoded || !b64) {
        if (decoded) free(decoded);
        if (b64) free(b64);
        return;
    }
    if (cte && stristr(cte, "base64"))
    {
        int bi = 0;
        for (int i = 0; i < textLen && bi < 16383; i++)
        {
            char c = textBody[i];
            if (c != '\r' && c != '\n' && c != ' ')
                b64[bi++] = c;
        }
        b64[bi] = 0;
        int decLen = base64_decode_simple(b64, bi, (unsigned char*)decoded, 16383);
        if (decLen > 0)
        {
            decoded[decLen] = 0;
            textBody = decoded;
            textLen = decLen;
        }
        else { textBody = ""; textLen = 0; }
    }
    else if (cte && stristr(cte, "quoted-printable"))
    {
        int di = 0;
        for (int i = 0; i < textLen && di < (int)16383; i++)
        {
            if (textBody[i] == '=' && i + 2 < textLen)
            {
                char hex[3] = {textBody[i+1], textBody[i+2], 0};
                if (isxdigit((unsigned char)hex[0]) && isxdigit((unsigned char)hex[1]))
                {
                    decoded[di++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                    continue;
                }
            }
            decoded[di++] = textBody[i];
        }
        decoded[di] = 0;
        textBody = decoded;
        textLen = di;
    }

    // 根据Content-Type中的charset转换为UTF-8
    if (contentType)
    {
        const char* cs = stristr(contentType, "charset=");
        if (cs)
        {
            cs += 8;
            char charset[64] = {0};
            int ci = 0;
            if (*cs == '"') cs++;
            while (*cs && *cs != '"' && *cs != ';' && *cs != '\r' && *cs != '\n' && *cs != ' ' && ci < 63)
                charset[ci++] = *cs++;
            charset[ci] = 0;
            if (charset[0] && strcasecmp(charset, "UTF-8") != 0 &&
                strcasecmp(charset, "US-ASCII") != 0 && strcasecmp(charset, "ASCII") != 0)
            {
                char* converted = (char*)malloc(16384);
                if (converted)
                {
                    int convLen = convert_to_utf8(charset, textBody, textLen, converted, 16384);
                    if (convLen > 0)
                    {
                        // 如果textBody指向decoded，把转换结果复制回decoded
                        if (textBody == decoded)
                        {
                            memcpy(decoded, converted, convLen);
                            decoded[convLen] = 0;
                            textLen = convLen;
                        }
                        else
                        {
                            memcpy(decoded, converted, convLen);
                            decoded[convLen] = 0;
                            textBody = decoded;
                            textLen = convLen;
                        }
                    }
                    free(converted);
                }
            }
        }
    }

    char* noHtml = (char*)malloc(16384);
    if (noHtml && contentType && stristr(contentType, "text/html"))
    {
        int di = 0;
        bool inTag = false;
        for (int i = 0; i < textLen && di < 16383; i++)
        {
            if (textBody[i] == '<') inTag = true;
            else if (textBody[i] == '>') inTag = false;
            else if (!inTag) noHtml[di++] = textBody[i];
        }
        noHtml[di] = 0;
        textBody = noHtml;
        textLen = di;
    }

    int pi = 0;
    bool lastSpace = false;
    for (int i = 0; i < textLen && pi < previewSize - 2; i++)
    {
        char c = textBody[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ')
        {
            if (!lastSpace && pi > 0) preview[pi++] = ' ';
            lastSpace = true;
        }
        else if (c >= 32 || (unsigned char)c >= 0x80)
        {
            preview[pi++] = c;
            lastSpace = false;
        }
    }
    while (pi > 0 && preview[pi-1] == ' ') pi--;
    preview[pi] = 0;
    free(decoded);
    free(b64);
    if (noHtml) free(noHtml);
}

static void parse_imap_headers(const char* data, int accountIndex, int seqStart)
{
    if (!data) return;

    g_app.emailCount = 0;
    const char* dataEnd = data + strlen(data);
    const char* p = data;
    int mailIdx = 0;

    while (*p && mailIdx < MAX_EMAILS)
    {
        // 查找 "FETCH (" 标记
        const char* fetchMark = strstr(p, "FETCH (");
        if (!fetchMark) break;

        // 从FETCH往前找序号
        int imapSeq = seqStart + mailIdx;
        {
            const char* sp = fetchMark - 1;
            while (sp > p && *sp == ' ') sp--;
            while (sp > p && *sp >= '0' && *sp <= '9') sp--;
            if (*sp == ' ' || *sp == '*')
                imapSeq = atoi(sp + 1);
        }

        // 从 "FETCH (" 之后开始，用括号深度跟踪解析
        const char* scan = fetchMark + 6; // 指向 "(" 之后
        int depth = 1; // 已经在 "(" 内

        const char* hdrData = NULL;
        int hdrSize = 0;
        const char* bodyData = NULL;
        int bodySize = 0;

        // 记录上一个看到的BODY标记类型：0=none, 1=HEADER.FIELDS, 2=TEXT
        int lastBodyType = 0;
        bool seenHeaderFields = false;
        bool seenText = false;
        bool inQuotes = false;

        while (scan < dataEnd && depth > 0)
        {
            char c = *scan;

            // 跳过引号内的内容（引号内的括号/花括号不计入深度）
            if (inQuotes)
            {
                if (c == '"' && (scan == fetchMark + 6 || *(scan-1) != '\\'))
                    inQuotes = false;
                scan++;
                continue;
            }
            if (c == '"')
            {
                inQuotes = true;
                scan++;
                continue;
            }

            if (c == '{')
            {
                // 解析字面量大小
                int litSize = atoi(scan + 1);
                const char* braceClose = strchr(scan, '}');
                if (!braceClose) break;
                if (litSize < 0) litSize = 0;
                // 限制单个字面量最大256KB，防止异常响应
                if (litSize > 256 * 1024) litSize = 256 * 1024;

                // 字面量数据在 "}\r\n" 之后
                const char* litData = braceClose + 1;
                if (litData < dataEnd && *litData == '\r') litData++;
                if (litData < dataEnd && *litData == '\n') litData++;

                // 根据lastBodyType判断这是头部还是正文
                if (lastBodyType == 1 && !seenHeaderFields)
                {
                    hdrData = litData;
                    hdrSize = litSize;
                    seenHeaderFields = true;
                }
                else if (lastBodyType == 2 && !seenText)
                {
                    bodyData = litData;
                    bodySize = litSize;
                    seenText = true;
                }

                // 跳过字面量数据
                scan = litData + litSize;
                lastBodyType = 0;
                continue;
            }
            else if (c == '(')
            {
                depth++;
            }
            else if (c == ')')
            {
                depth--;
            }
            else if ((c == 'B' || c == 'b') && scan + 5 < dataEnd &&
                     (scan[1] == 'O' || scan[1] == 'o') &&
                     (scan[2] == 'D' || scan[2] == 'd') &&
                     (scan[3] == 'Y' || scan[3] == 'y') &&
                     scan[4] == '[')
            {
                // 精确检测 BODY[ （不区分大小写，[必须紧跟BODY之后）
                const char* bracket = scan + 4;
                const char* bracketEnd = strchr(bracket, ']');
                if (bracketEnd && bracketEnd < dataEnd)
                {
                    // 在 [] 内查找 HEADER.FIELDS 或 TEXT
                    bool isHeader = false, isText = false;
                    for (const char* s = bracket + 1; s < bracketEnd; s++)
                    {
                        if (!isHeader && strncasecmp(s, "HEADER.FIELDS", 13) == 0)
                            isHeader = true;
                        if (!isText && strncasecmp(s, "TEXT", 4) == 0)
                            isText = true;
                    }
                    if (isHeader) lastBodyType = 1;
                    else if (isText) lastBodyType = 2;
                }
            }

            scan++;
        }

        // scan现在指向FETCH响应结束后的位置（")"之后）
        p = scan;

        // 限制大小
        if (hdrSize > 16384) hdrSize = 16384;
        if (bodySize > 32768) bodySize = 32768;
        if (hdrData && hdrData + hdrSize > dataEnd)
            hdrSize = (int)(dataEnd - hdrData);
        if (bodyData && bodyData + bodySize > dataEnd)
            bodySize = (int)(dataEnd - bodyData);

        Email* mail = &g_app.emails[mailIdx];
        memset(mail, 0, sizeof(Email));
        mail->id = mailIdx + 1;
        mail->imapSeq = imapSeq;
        mail->accountIndex = accountIndex;
        mail->unread = true;
        mail->hasAttachment = false;

        // 检查FLAGS中是否有\Seen（在FETCH属性列表中，第一个字面量之前）
        {
            const char* regionEnd = hdrData ? hdrData - 2 : fetchMark + 200;
            if (regionEnd > dataEnd) regionEnd = dataEnd;
            if (regionEnd > fetchMark)
            {
                int regionLen = (int)(regionEnd - fetchMark);
                if (regionLen > 6)
                {
                    for (int s = 0; s < regionLen - 5; s++)
                    {
                        if (fetchMark[s] == '\\' &&
                            (fetchMark[s+1] == 'S' || fetchMark[s+1] == 's') &&
                            (fetchMark[s+2] == 'e' || fetchMark[s+2] == 'E') &&
                            (fetchMark[s+3] == 'e' || fetchMark[s+3] == 'E') &&
                            (fetchMark[s+4] == 'n' || fetchMark[s+4] == 'N'))
                        {
                            mail->unread = false;
                            break;
                        }
                    }
                }
            }
        }

        // 解析UID
        {
            const char* regionEnd = hdrData ? hdrData - 2 : fetchMark + 200;
            if (regionEnd > dataEnd) regionEnd = dataEnd;
            const char* uidMark = stristr(fetchMark, "UID ");
            if (uidMark && uidMark < regionEnd)
                mail->uid = (u32)strtoul(uidMark + 4, NULL, 10);
            if (mail->uid == 0)
                mail->uid = (u32)imapSeq;
        }

        // 解析头部
        if (hdrData && hdrSize > 0)
        {
            char* headers = (char*)malloc(hdrSize + 1);
            if (headers)
            {
                strncpy(headers, hdrData, hdrSize);
                headers[hdrSize] = 0;

                // From
                const char* le;
                const char* fromVal = find_header(headers, "From", &le);
                if (fromVal && le)
                {
                    char rawFrom[256] = {0};
                    int fl = (int)(le - fromVal);
                    if (fl >= (int)sizeof(rawFrom)) fl = (int)sizeof(rawFrom) - 1;
                    strncpy(rawFrom, fromVal, fl);
                    rawFrom[fl] = 0;
                    int rl = (int)strlen(rawFrom);
                    while (rl > 0 && (rawFrom[rl-1] == '\r' || rawFrom[rl-1] == ' '))
                        rawFrom[--rl] = 0;

                    char decodedFrom[256];
                    decode_mime_string(rawFrom, decodedFrom, sizeof(decodedFrom));

                    char* lt = strchr(decodedFrom, '<');
                    char* gt = strchr(decodedFrom, '>');
                    if (lt && gt && lt < gt)
                    {
                        *gt = 0;
                        strncpy(mail->fromAddr, lt + 1, sizeof(mail->fromAddr) - 1);
                        *lt = 0;
                        char* name = decodedFrom;
                        while (*name == ' ' || *name == '"') name++;
                        int nlen = (int)strlen(name);
                        while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '"'))
                            name[--nlen] = 0;
                        if (nlen > 0)
                            strncpy(mail->sender, name, sizeof(mail->sender) - 1);
                        else
                        {
                            char* at = strchr(mail->fromAddr, '@');
                            if (at) { *at = 0; strcpy(mail->sender, mail->fromAddr); *at = '@'; }
                            else strcpy(mail->sender, mail->fromAddr);
                        }
                    }
                    else
                    {
                        strncpy(mail->fromAddr, decodedFrom, sizeof(mail->fromAddr) - 1);
                        char* at = strchr(mail->fromAddr, '@');
                        if (at) { *at = 0; strcpy(mail->sender, mail->fromAddr); *at = '@'; }
                        else strcpy(mail->sender, mail->fromAddr);
                    }
                }

                // Subject
                const char* subjVal = find_header(headers, "Subject", &le);
                if (subjVal && le)
                {
                    char rawSubj[512] = {0};
                    int sl = (int)(le - subjVal);
                    if (sl >= (int)sizeof(rawSubj)) sl = (int)sizeof(rawSubj) - 1;
                    strncpy(rawSubj, subjVal, sl);
                    rawSubj[sl] = 0;
                    decode_mime_string(rawSubj, mail->subject, sizeof(mail->subject));
                }

                // Date
                const char* dateVal = find_header(headers, "Date", &le);
                if (dateVal && le)
                {
                    char rawDate[64] = {0};
                    int dl = (int)(le - dateVal);
                    if (dl >= (int)sizeof(rawDate)) dl = (int)sizeof(rawDate) - 1;
                    strncpy(rawDate, dateVal, dl);
                    rawDate[dl] = 0;

                    int day = 0, hour = 0, min = 0, year = 0;
                    char mon[8] = {0};
                    const char* dp = rawDate;
                    while (*dp && *dp != ',') dp++;
                    if (*dp == ',') dp++;
                    while (*dp == ' ') dp++;
                    day = atoi(dp);
                    while (*dp >= '0' && *dp <= '9') dp++;
                    while (*dp == ' ') dp++;
                    int mi = 0;
                    while (*dp && *dp != ' ' && mi < 3) mon[mi++] = *dp++;
                    mon[mi] = 0;
                    while (*dp == ' ') dp++;
                    year = atoi(dp);
                    while (*dp >= '0' && *dp <= '9') dp++;
                    while (*dp == ' ') dp++;
                    hour = atoi(dp);
                    while (*dp >= '0' && *dp <= '9') dp++;
                    if (*dp == ':') dp++;
                    min = atoi(dp);

                    static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
                    int monthNum = 0;
                    for (int m = 0; m < 12; m++)
                        if (strncasecmp(mon, months[m], 3) == 0) { monthNum = m + 1; break; }

                    bool isToday = false;
                    {
                        time_t now = time(NULL);
                        struct tm* lt2 = localtime(&now);
                        if (lt2 && lt2->tm_year + 1900 == year &&
                            lt2->tm_mon + 1 == monthNum && lt2->tm_mday == day)
                            isToday = true;
                    }
                    if (isToday)
                        snprintf(mail->date, sizeof(mail->date), "%02d:%02d", hour, min);
                    else if (monthNum > 0)
                        snprintf(mail->date, sizeof(mail->date), "%d/%d", monthNum, day);
                    else
                        strncpy(mail->date, rawDate, sizeof(mail->date) - 1);
                }

                // 附件检测
                if (stristr(headers, "Content-Disposition: attachment") ||
                    stristr(headers, "multipart/mixed"))
                {
                    mail->hasAttachment = true;
                }

                // Content-Type（从邮件头部获取，不依赖bodyData）
                const char* ctVal = find_header(headers, "Content-Type", NULL);
                if (ctVal)
                    strncpy(mail->contentType, ctVal, sizeof(mail->contentType) - 1);

                // 正文预览
                if (bodyData && bodySize > 0)
                {
                    const char* cteVal = find_header(headers, "Content-Transfer-Encoding", NULL);
                    extract_preview(ctVal, cteVal, bodyData, bodySize, mail->preview, sizeof(mail->preview));
                }

                free(headers);
            }
        }

        if (mail->subject[0] == 0)
            strcpy(mail->subject, "(无主题)");
        if (mail->sender[0] == 0)
            strcpy(mail->sender, "(未知发件人)");

        strcpy(mail->body, "");
        mail->bodyLoaded = false;

        mailIdx++;
    }

    g_app.emailCount = mailIdx;
}
static int parse_total_mails(const char* data)
{
    if (!data) return 0;
    // 查找 "MESSAGES N" 模式
    const char* p = stristr(data, "MESSAGES");
    if (!p) return 0;
    p += 8;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

// 只获取账户总邮件数（用于检测新邮件），返回-1表示失败
int Network_GetTotalMails(int accountIndex)
{
    if (accountIndex < 0 || accountIndex >= MAX_ACCOUNTS) return -1;
    Account* acc = &g_app.accounts[accountIndex];
    if (!acc->added) return -1;

    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    struct WriteBuf buf;
    buf_init(&buf);
    char url[256];
    snprintf(url, sizeof(url), "imaps://%s:%d/INBOX", acc->imapServer, acc->imapPort);
    setup_curl_common(curl, url, acc->email, acc->password, &buf);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "STATUS INBOX (MESSAGES)");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !buf.data) { buf_free(&buf); return -1; }

    int total = -1;
    const char* p = strstr(buf.data, "MESSAGES");
    if (p)
    {
        p += 8;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        total = atoi(p);
    }
    buf_free(&buf);
    return total;
}

// ========== 原始IMAP通信（CURLOPT_CONNECT_ONLY方式） ==========
// 解决某些邮箱（如163）在CUSTOMREQUEST模式下重复登录被拒的问题

// 建立IMAP连接并登录
static int imap_connect(ImapConn* ic, const char* server, int port,
                        const char* user, const char* pass)
{
    memset(ic, 0, sizeof(*ic));
    ic->curl = curl_easy_init();
    if (!ic->curl) return -1;

    char url[256];
    snprintf(url, sizeof(url), "imaps://%s:%d/INBOX", server, port);
    curl_easy_setopt(ic->curl, CURLOPT_URL, url);
    curl_easy_setopt(ic->curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(ic->curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(ic->curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(ic->curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(ic->curl, CURLOPT_CONNECT_ONLY, 1L);

    CURLcode rc = curl_easy_perform(ic->curl);
    if (rc != CURLE_OK)
    {
        IMAP_LOG_STATIC("imap_connect failed: %d (%s)\n", (int)rc, curl_easy_strerror(rc));
        curl_easy_cleanup(ic->curl);
        ic->curl = NULL;
        return -1;
    }

    // CONNECT_ONLY模式下libcurl只建立TLS连接，不自动登录
    // 需要先读取服务器greeting，再手动发送LOGIN命令
    {
        char recvBuf[4096];
        size_t nread = 0;
        int waited = 0;
        // 等待greeting（最多5秒）
        while (waited < 50)
        {
            rc = curl_easy_recv(ic->curl, recvBuf, sizeof(recvBuf) - 1, &nread);
            if (rc == CURLE_OK && nread > 0)
            {
                recvBuf[nread] = 0;
                IMAP_LOG_STATIC("IMAP greeting: %s\n", recvBuf);
                break;
            }
            if (rc == CURLE_AGAIN)
            {
                svcSleepThread(100 * 1000000LL); // 100ms
                waited++;
                continue;
            }
            break;
        }

        // 发送LOGIN命令
        char loginCmd[512];
        snprintf(loginCmd, sizeof(loginCmd), "A001 LOGIN \"%s\" \"%s\"\r\n", user, pass);
        size_t sent = 0;
        rc = curl_easy_send(ic->curl, loginCmd, strlen(loginCmd), &sent);
        if (rc != CURLE_OK || sent != strlen(loginCmd))
        {
            IMAP_LOG_STATIC("imap LOGIN send failed: %d\n", (int)rc);
            curl_easy_cleanup(ic->curl);
            ic->curl = NULL;
            return -1;
        }

        // 读取LOGIN响应
        struct WriteBuf loginResp;
        buf_init(&loginResp);
        int maxWait = 100;
        int loginOk = 0;
        while (maxWait-- > 0)
        {
            nread = 0;
            rc = curl_easy_recv(ic->curl, recvBuf, sizeof(recvBuf) - 1, &nread);
            if (rc == CURLE_OK && nread > 0)
            {
                recvBuf[nread] = 0;
                if (loginResp.size + nread + 1 > loginResp.capacity)
                {
                    size_t newCap = loginResp.capacity * 2 + nread + 256;
                    char* newData = realloc(loginResp.data, newCap);
                    if (!newData) break;
                    loginResp.data = newData;
                    loginResp.capacity = newCap;
                }
                memcpy(loginResp.data + loginResp.size, recvBuf, nread);
                loginResp.size += nread;
                loginResp.data[loginResp.size] = 0;

                if (strstr(loginResp.data, "A001 OK"))
                {
                    loginOk = 1;
                    break;
                }
                if (strstr(loginResp.data, "A001 NO") || strstr(loginResp.data, "A001 BAD"))
                {
                    IMAP_LOG_STATIC("imap LOGIN denied: %s\n", loginResp.data);
                    break;
                }
            }
            else if (rc == CURLE_AGAIN)
            {
                svcSleepThread(100 * 1000000LL);
            }
            else break;
        }
        IMAP_LOG_STATIC("imap LOGIN response: %s\n",
                        loginResp.data ? loginResp.data : "(none)");
        buf_free(&loginResp);

        if (!loginOk)
        {
            curl_easy_cleanup(ic->curl);
            ic->curl = NULL;
            return -1;
        }
    }

    ic->tagCounter = 1;
    return 0;
}

static void imap_disconnect(ImapConn* ic)
{
    if (ic->curl)
    {
        curl_easy_cleanup(ic->curl);
        ic->curl = NULL;
    }
}

// 发送IMAP命令并读取完整响应
// responseBuf: 存储响应的缓冲区（调用者负责释放）
// 返回: 0=成功(OK), -1=失败(NO/BAD/错误), 1=等待更多数据（字面量）
static int imap_send_recv(ImapConn* ic, const char* command,
                          char** responseBuf, size_t* responseSize)
{
    char tag[16];
    snprintf(tag, sizeof(tag), "A%03d", ++ic->tagCounter);

    // 构造完整命令
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s %s\r\n", tag, command);

    // 发送命令
    size_t sent = 0;
    CURLcode rc = curl_easy_send(ic->curl, cmd, strlen(cmd), &sent);
    if (rc != CURLE_OK || sent != strlen(cmd))
    {
        IMAP_LOG_STATIC("imap_send failed: %d\n", (int)rc);
        return -1;
    }

    // 读取响应，直到找到 "tag OK"/"tag NO"/"tag BAD"
    struct WriteBuf resp;
    buf_init(&resp);
    char recvBuf[4096];
    char tagOk[32], tagNo[32], tagBad[32];
    snprintf(tagOk, sizeof(tagOk), "%s OK", tag);
    snprintf(tagNo, sizeof(tagNo), "%s NO", tag);
    snprintf(tagBad, sizeof(tagBad), "%s BAD", tag);

    int maxWait = 300; // 最多等待300*100ms = 30秒
    while (maxWait-- > 0)
    {
        size_t nread = 0;
        rc = curl_easy_recv(ic->curl, recvBuf, sizeof(recvBuf) - 1, &nread);
        if (rc == CURLE_OK && nread > 0)
        {
            recvBuf[nread] = 0;
            if (resp.size + nread + 1 > resp.capacity)
            {
                size_t newCap = resp.capacity * 2 + nread + 256;
                if (newCap > 4*1024*1024) newCap = 4*1024*1024;
                if (resp.size + nread + 1 > newCap) break; // 缓冲区已满
                char* newData = realloc(resp.data, newCap);
                if (!newData) break;
                resp.data = newData;
                resp.capacity = newCap;
            }
            memcpy(resp.data + resp.size, recvBuf, nread);
            resp.size += nread;
            resp.data[resp.size] = 0;

            // 检查是否收到完整响应
            if (strstr(resp.data, tagOk))
            {
                *responseBuf = resp.data;
                *responseSize = resp.size;
                return 0;
            }
            if (strstr(resp.data, tagNo) || strstr(resp.data, tagBad))
            {
                IMAP_LOG_STATIC("IMAP command denied: %.*s\n",
                                resp.size > 512 ? 512 : (int)resp.size, resp.data);
                buf_free(&resp);
                *responseBuf = NULL;
                *responseSize = 0;
                return -1;
            }
        }
        else if (rc == CURLE_AGAIN)
        {
            // 暂时没有数据，等待
            svcSleepThread(100 * 1000 * 1000ULL); // 100ms
        }
        else
        {
            // 错误
            break;
        }
    }

    buf_free(&resp);
    return -1;
}

// 发送IMAP命令并读取字面量响应（用于FETCH BODY[HEADER]等返回{ N }字面量的命令）
// 返回的responseBuf只包含字面量数据（纯头部），不包含FETCH前缀和tag完成行
static int imap_send_recv_literal(ImapConn* ic, const char* command,
                                  char** responseBuf, size_t* responseSize)
{
    char tag[16];
    snprintf(tag, sizeof(tag), "A%03d", ++ic->tagCounter);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s %s\r\n", tag, command);

    size_t sent = 0;
    CURLcode rc = curl_easy_send(ic->curl, cmd, strlen(cmd), &sent);
    if (rc != CURLE_OK || sent != strlen(cmd)) return -1;

    struct WriteBuf resp;
    buf_init(&resp);
    char recvBuf[8192];
    char tagOk[32], tagNo[32], tagBad[32];
    snprintf(tagOk, sizeof(tagOk), "%s OK", tag);
    snprintf(tagNo, sizeof(tagNo), "%s NO", tag);
    snprintf(tagBad, sizeof(tagBad), "%s BAD", tag);

    // 持续读取直到收到tag完成
    int maxWait = 300;
    int foundLiteral = 0;
    size_t litSize = 0;
    size_t litStartOffset = 0; // 字面量数据在resp中的起始偏移

    while (maxWait-- > 0)
    {
        int done = 0;
        if (strstr(resp.data, tagOk)) done = 1;
        else if (strstr(resp.data, tagNo) || strstr(resp.data, tagBad))
        {
            buf_free(&resp);
            return -1;
        }
        if (done) break;

        size_t nread = 0;
        rc = curl_easy_recv(ic->curl, recvBuf, sizeof(recvBuf) - 1, &nread);
        if (rc == CURLE_OK && nread > 0)
        {
            buf_append(&resp, recvBuf, nread);

            // 查找 {N} 字面量标记（只在第一次找到时记录）
            if (!foundLiteral)
            {
                // 查找 "{" 后面跟数字和 "}" 的模式
                for (size_t ci = 0; ci + 2 < resp.size; ci++)
                {
                    if (resp.data[ci] == '{')
                    {
                        size_t di = ci + 1;
                        size_t numStart = di;
                        while (di < resp.size && resp.data[di] >= '0' && resp.data[di] <= '9') di++;
                        if (di > numStart && di < resp.size && resp.data[di] == '}')
                        {
                            litSize = (size_t)atol(resp.data + numStart);
                            // 找到 "}" 后的 \r\n，字面量数据从 \n 之后开始
                            size_t afterBrace = di + 1;
                            if (afterBrace < resp.size && resp.data[afterBrace] == '\r') afterBrace++;
                            if (afterBrace < resp.size && resp.data[afterBrace] == '\n') afterBrace++;
                            litStartOffset = afterBrace;
                            foundLiteral = 1;
                            break;
                        }
                    }
                }
            }
        }
        else if (rc == CURLE_AGAIN)
        {
            svcSleepThread(100 * 1000 * 1000ULL);
        }
        else break;
    }

    if (!foundLiteral || litSize == 0)
    {
        buf_free(&resp);
        return -1;
    }

    // 检查是否收到了足够的字面量数据
    if (resp.size < litStartOffset + litSize)
    {
        // 数据不完整
        buf_free(&resp);
        return -1;
    }

    // 只提取字面量数据（纯头部）
    char* literal = (char*)malloc(litSize + 1);
    if (!literal)
    {
        buf_free(&resp);
        return -1;
    }
    memcpy(literal, resp.data + litStartOffset, litSize);
    literal[litSize] = 0;
    buf_free(&resp);

    *responseBuf = literal;
    *responseSize = litSize;
    return 0;
}

// 解析原始邮件头部数据到Email结构体（批量FETCH和单封FETCH共用）
static void parse_raw_header_to_email(Email* mail, const char* hdrBuf, size_t hdrLen)
{
    (void)hdrLen;
    const char* le;
    const char* fromVal = find_header(hdrBuf, "From", &le);
    const char* fromLe = le;
    const char* subjVal = find_header(hdrBuf, "Subject", &le);
    const char* subjLe = le;

    if (fromVal && fromLe)
    {
        char rawFrom[256] = {0};
        int fl = (int)(fromLe - fromVal);
        if (fl >= (int)sizeof(rawFrom)) fl = (int)sizeof(rawFrom) - 1;
        strncpy(rawFrom, fromVal, fl);
        rawFrom[fl] = 0;
        // 展开折叠头
        {
            char* cp = rawFrom;
            while ((cp = strstr(cp, "\r\n")) != NULL)
            {
                char* nxt = cp + 2;
                if (*nxt == ' ' || *nxt == '\t')
                {
                    int rem = (int)strlen(nxt + 1) + 1;
                    memmove(cp + 1, nxt + 1, rem);
                    *cp = ' ';
                }
                else break;
            }
        }
        int rl = (int)strlen(rawFrom);
        while (rl > 0 && (rawFrom[rl-1] == '\r' || rawFrom[rl-1] == ' '))
            rawFrom[--rl] = 0;
        char decodedFrom[256];
        decode_mime_string(rawFrom, decodedFrom, sizeof(decodedFrom));
        char* lt = strchr(decodedFrom, '<');
        char* gt = strchr(decodedFrom, '>');
        if (lt && gt && lt < gt)
        {
            *gt = 0;
            strncpy(mail->fromAddr, lt + 1, sizeof(mail->fromAddr) - 1);
            *lt = 0;
            char* name = decodedFrom;
            while (*name == ' ' || *name == '"') name++;
            int nlen = (int)strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '"'))
                name[--nlen] = 0;
            if (nlen > 0)
                strncpy(mail->sender, name, sizeof(mail->sender) - 1);
            else
            {
                char* at = strchr(mail->fromAddr, '@');
                if (at) { *at = 0; strcpy(mail->sender, mail->fromAddr); *at = '@'; }
                else strcpy(mail->sender, mail->fromAddr);
            }
        }
        else
        {
            strncpy(mail->fromAddr, decodedFrom, sizeof(mail->fromAddr) - 1);
            char* at = strchr(mail->fromAddr, '@');
            if (at) { *at = 0; strcpy(mail->sender, mail->fromAddr); *at = '@'; }
            else strcpy(mail->sender, mail->fromAddr);
        }
    }

    if (subjVal && subjLe)
    {
        char rawSubj[512] = {0};
        int sl = (int)(subjLe - subjVal);
        if (sl >= (int)sizeof(rawSubj)) sl = (int)sizeof(rawSubj) - 1;
        strncpy(rawSubj, subjVal, sl);
        rawSubj[sl] = 0;
        // 展开折叠头
        {
            char* cp = rawSubj;
            while ((cp = strstr(cp, "\r\n")) != NULL)
            {
                char* nxt = cp + 2;
                if (*nxt == ' ' || *nxt == '\t')
                {
                    int rem = (int)strlen(nxt + 1) + 1;
                    memmove(cp + 1, nxt + 1, rem);
                    *cp = ' ';
                }
                else break;
            }
        }
        decode_mime_string(rawSubj, mail->subject, sizeof(mail->subject));
    }

    const char* dateVal = find_header(hdrBuf, "Date", &le);
    if (dateVal && le)
    {
        char rawDate[128] = {0};
        int dl = (int)(le - dateVal);
        if (dl >= (int)sizeof(rawDate)) dl = (int)sizeof(rawDate) - 1;
        strncpy(rawDate, dateVal, dl);
        rawDate[dl] = 0;
        for (int ci = 0; rawDate[ci]; ci++)
        {
            if (rawDate[ci] == '\r' || rawDate[ci] == '\n' || rawDate[ci] == '\t')
                rawDate[ci] = ' ';
        }
        char* ds = rawDate;
        while (*ds == ' ') ds++;
        char* de = ds + strlen(ds) - 1;
        while (de > ds && *de == ' ') *de-- = 0;
        strncpy(mail->rawDate, ds, sizeof(mail->rawDate) - 1);
        parse_mail_date(ds, &mail->year, &mail->month, &mail->day,
                        &mail->hour, &mail->minute, &mail->timestamp);
        mail->date[0] = 0;
    }

    // Content-Type
    {
        const char* ctLe;
        const char* ctVal = find_header(hdrBuf, "Content-Type", &ctLe);
        if (ctVal && ctLe)
        {
            int ctl = (int)(ctLe - ctVal);
            if (ctl >= (int)sizeof(mail->contentType)) ctl = (int)sizeof(mail->contentType) - 1;
            strncpy(mail->contentType, ctVal, ctl);
            mail->contentType[ctl] = 0;
            int ctl2 = (int)strlen(mail->contentType);
            while (ctl2 > 0 && (mail->contentType[ctl2-1] == '\r' || mail->contentType[ctl2-1] == ' ' || mail->contentType[ctl2-1] == '\n'))
                mail->contentType[--ctl2] = 0;
        }
    }

    if (stristr(hdrBuf, "Content-Disposition: attachment") ||
        stristr(hdrBuf, "multipart/mixed"))
        mail->hasAttachment = true;

    if (mail->subject[0] == 0)
        strcpy(mail->subject, "(无主题)");
    if (mail->sender[0] == 0)
        strcpy(mail->sender, "(未知发件人)");
    strcpy(mail->body, "");
    mail->bodyLoaded = false;

    Email_FormatDate(mail->year, mail->month, mail->day,
                     mail->hour, mail->minute,
                     mail->date, sizeof(mail->date));
}

int Network_FetchMailsPage(int accountIndex, int page, int perPage, FetchResult* result)
{
    memset(result, 0, sizeof(FetchResult));
    result->finished = false;

    if (accountIndex < 0 || accountIndex >= MAX_ACCOUNTS ||
        !g_app.accounts[accountIndex].added)
    {
        snprintf(result->error, sizeof(result->error), "无效账户");
        result->finished = true;
        return -1;
    }

    Account* acc = &g_app.accounts[accountIndex];

    // 不清空邮件列表：后台补齐时保持已缓存的邮件继续显示
    // g_app.emailCount 仅在拉取成功后才更新

    // 调试日志已禁用以提升加载速度
    #define IMAP_LOG(...)

    // 记录本次拉取的账户信息（在curl初始化之前，确保一定能写入）
    IMAP_LOG("\n========== Fetch Start: %s @ %s:%d, page=%d, perPage=%d ==========\n",
             acc->email, acc->imapServer, acc->imapPort, page, perPage);

    // 第一步：优先用 UID SEARCH ALL 获取全部UID（同时得到邮件总数）
    int totalMails = 0;
    int endSeq = 0, startSeq = 0, seqCount = 0;
    u32 uidMap[64];
    bool seenMap[64];
    memset(uidMap, 0, sizeof(uidMap));
    memset(seenMap, 0, sizeof(seenMap));
    int fetchedUidCount = 0;

    // 使用原始IMAP连接（CURLOPT_CONNECT_ONLY），避免libcurl重复登录问题
    ImapConn ic;
    if (imap_connect(&ic, acc->imapServer, acc->imapPort, acc->email, acc->password) != 0)
    {
        IMAP_LOG("FATAL: imap_connect failed for %s\n", acc->email);
        snprintf(result->error, sizeof(result->error), "连接邮件服务器失败");
        result->success = false;
        result->finished = true;
        return -1;
    }

    // 发送SELECT INBOX
    {
        char* selResp = NULL;
        size_t selLen = 0;
        int rc = imap_send_recv(&ic, "SELECT INBOX", &selResp, &selLen);
        IMAP_LOG("SELECT INBOX: rc=%d, size=%d\n", rc, (int)selLen);
        if (rc != 0)
        {
            IMAP_LOG("FATAL: SELECT INBOX failed for %s\n", acc->email);
            snprintf(result->error, sizeof(result->error), "无法打开收件箱");
            result->success = false;
            result->finished = true;
            if (selResp) free(selResp);
            imap_disconnect(&ic);
            return -1;
        }
        if (selResp) free(selResp);
    }

    // 检查内存中是否已有该账户的完整UID列表
    if (g_app.uidListLoaded && g_app.uidListAccount == accountIndex &&
        g_app.uidListCount > 0 && g_app.uidList != NULL)
    {
        totalMails = g_app.uidListCount;
        endSeq = totalMails - page * perPage;
        startSeq = endSeq - perPage + 1;
        if (startSeq < 1) startSeq = 1;
        if (endSeq < 1) endSeq = 1;
        if (startSeq > endSeq) startSeq = endSeq;
        seqCount = endSeq - startSeq + 1;
        for (int i = 0; i < seqCount && fetchedUidCount < 64; i++)
        {
            int uidIdx = startSeq + i - 1;
            if (uidIdx >= 0 && uidIdx < g_app.uidListCount && g_app.uidList[uidIdx] != 0)
            {
                uidMap[fetchedUidCount] = g_app.uidList[uidIdx];
                fetchedUidCount++;
            }
        }
        IMAP_LOG("Using in-memory UID list: total=%d, page=%d, fetched=%d\n",
                 totalMails, page, fetchedUidCount);
    }

    // 内存中没有UID列表，优先用SORT扩展按日期排序，回退到SEARCH+INTERNALDATE客户端排序
    if (fetchedUidCount == 0)
    {
        // 确保uidList已分配
        if (g_app.uidList == NULL)
        {
            g_app.uidListCap = 4096;
            g_app.uidList = (u32*)malloc(g_app.uidListCap * sizeof(u32));
        }

        bool uidListReady = false;

        // 方法1：IMAP SORT扩展（按日期排序，一次命令返回有序UID列表，从旧到新）
        if (g_app.uidList)
        {
            char* sortResp = NULL;
            size_t sortLen = 0;
            int rcSort = imap_send_recv(&ic, "UID SORT (DATE) UTF-8 ALL", &sortResp, &sortLen);
            if (rcSort == 0 && sortResp && sortLen > 0)
            {
                const char* sortMarker = strstr(sortResp, "* SORT");
                if (sortMarker)
                {
                    const char* p = sortMarker + 6;
                    int allCount = 0;
                    while (*p && allCount < 65536)
                    {
                        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
                        if (*p >= '0' && *p <= '9')
                        {
                            if (allCount >= g_app.uidListCap)
                            {
                                int newCap = g_app.uidListCap * 2;
                                if (newCap > 65536) newCap = 65536;
                                if (allCount >= newCap) break;
                                u32* newList = (u32*)realloc(g_app.uidList, newCap * sizeof(u32));
                                if (!newList) break;
                                g_app.uidList = newList;
                                g_app.uidListCap = newCap;
                            }
                            g_app.uidList[allCount] = (u32)strtoul(p, NULL, 10);
                            allCount++;
                            while (*p >= '0' && *p <= '9') p++;
                        }
                        else if (*p) p++;
                        else break;
                    }
                    if (allCount > 0)
                    {
                        g_app.uidListCount = allCount;
                        g_app.uidListAccount = accountIndex;
                        g_app.uidListLoaded = true;
                        totalMails = allCount;
                        Cache_SaveUidList(acc->email, g_app.uidList, allCount);
                        uidListReady = true;
                    }
                }
            }
            if (sortResp) free(sortResp);
        }

        // 方法2：SORT不可用，回退到 SEARCH ALL + FETCH INTERNALDATE 客户端排序
        if (!uidListReady && g_app.uidList)
        {
            char* searchResp = NULL;
            size_t searchLen = 0;
            int rc = imap_send_recv(&ic, "UID SEARCH ALL", &searchResp, &searchLen);

            if (rc == 0 && searchResp && searchLen > 0)
            {
                int allCount = 0;
                const char* p = searchResp;
                const char* sm = stristr(searchResp, "SEARCH");
                if (sm) p = sm + 6;
                while (*p && allCount < 65536)
                {
                    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t' ||
                           *p == '(' || *p == ')' || *p == ',') p++;
                    if (*p >= '0' && *p <= '9')
                    {
                        if (allCount >= g_app.uidListCap)
                        {
                            int newCap = g_app.uidListCap * 2;
                            if (newCap > 65536) newCap = 65536;
                            if (allCount >= newCap) break;
                            u32* newList = (u32*)realloc(g_app.uidList, newCap * sizeof(u32));
                            if (!newList) break;
                            g_app.uidList = newList;
                            g_app.uidListCap = newCap;
                        }
                        g_app.uidList[allCount] = (u32)strtoul(p, NULL, 10);
                        allCount++;
                        while (*p >= '0' && *p <= '9') p++;
                    }
                    else if (*p) p++;
                    else break;
                }

                if (allCount > 0)
                {
                    // 批量FETCH所有邮件的INTERNALDATE（服务器接收时间）
                    char* dateResp = NULL;
                    size_t dateLen = 0;
                    int rcDate = imap_send_recv(&ic, "UID FETCH 1:* (UID INTERNALDATE)",
                                                &dateResp, &dateLen);

                    if (rcDate == 0 && dateResp && dateLen > 0)
                    {
                        // UID-时间戳配对数组
                        typedef struct { u32 uid; long long ts; } UidTs;
                        UidTs* pairs = (UidTs*)malloc(allCount * sizeof(UidTs));
                        if (pairs)
                        {
                            for (int i = 0; i < allCount; i++)
                            {
                                pairs[i].uid = g_app.uidList[i];
                                pairs[i].ts = 0;
                            }

                            // 解析FETCH响应中的每个INTERNALDATE
                            const char* resp = dateResp;
                            while (resp && *resp)
                            {
                                const char* datePtr = strstr(resp, "INTERNALDATE \"");
                                if (!datePtr) break;
                                datePtr += 14;
                                const char* dateEnd = strchr(datePtr, '"');
                                if (!dateEnd) break;

                                // 向前查找同一行的UID
                                const char* lineStart = datePtr;
                                while (lineStart > dateResp && lineStart[-1] != '\n') lineStart--;
                                const char* uidPtr = strstr(lineStart, "UID ");
                                if (uidPtr && uidPtr < datePtr)
                                {
                                    uidPtr += 4;
                                    u32 uid = (u32)strtoul(uidPtr, NULL, 10);

                                    char dateBuf[64];
                                    int dl = (int)(dateEnd - datePtr);
                                    if (dl >= (int)sizeof(dateBuf)) dl = sizeof(dateBuf) - 1;
                                    strncpy(dateBuf, datePtr, dl);
                                    dateBuf[dl] = 0;

                                    int y, mo, d, h, mi;
                                    long long ts;
                                    if (parse_mail_date(dateBuf, &y, &mo, &d, &h, &mi, &ts) == 0)
                                    {
                                        // 二分查找UID在pairs中的位置（SEARCH返回的UID升序）
                                        int lo = 0, hi = allCount - 1, found = -1;
                                        while (lo <= hi)
                                        {
                                            int mid = (lo + hi) / 2;
                                            if (pairs[mid].uid == uid) { found = mid; break; }
                                            if (pairs[mid].uid < uid) lo = mid + 1;
                                            else hi = mid - 1;
                                        }
                                        if (found >= 0) pairs[found].ts = ts;
                                    }
                                }
                                resp = dateEnd + 1;
                            }

                            // 按时间戳升序排序（旧的在前，时间戳为0的排最后）
                            int cmp_uid_ts(const void* a, const void* b)
                            {
                                const UidTs* ta = (const UidTs*)a;
                                const UidTs* tb = (const UidTs*)b;
                                if (ta->ts == 0 && tb->ts == 0)
                                    return (ta->uid < tb->uid) ? -1 : 1;
                                if (ta->ts == 0) return 1;
                                if (tb->ts == 0) return -1;
                                if (ta->ts != tb->ts)
                                    return (ta->ts < tb->ts) ? -1 : 1;
                                return 0;
                            }
                            qsort(pairs, allCount, sizeof(UidTs), cmp_uid_ts);

                            // 写回排序后的UID列表
                            for (int i = 0; i < allCount; i++)
                                g_app.uidList[i] = pairs[i].uid;

                            free(pairs);
                        }
                    }
                    if (dateResp) free(dateResp);

                    g_app.uidListCount = allCount;
                    g_app.uidListAccount = accountIndex;
                    g_app.uidListLoaded = true;
                    totalMails = allCount;
                    Cache_SaveUidList(acc->email, g_app.uidList, allCount);
                    uidListReady = true;
                }
            }
            if (searchResp) free(searchResp);
        }

        // 根据排序后的UID列表计算分页
        if (uidListReady && totalMails > 0)
        {
            endSeq = totalMails - page * perPage;
            startSeq = endSeq - perPage + 1;
            if (startSeq < 1) startSeq = 1;
            if (endSeq < 1) endSeq = 1;
            if (startSeq > endSeq) startSeq = endSeq;
            seqCount = endSeq - startSeq + 1;
            fetchedUidCount = 0;
            for (int i = 0; i < seqCount && fetchedUidCount < 64; i++)
            {
                int seqPos = startSeq + i - 1;
                if (seqPos >= 0 && seqPos < g_app.uidListCount)
                {
                    uidMap[fetchedUidCount] = g_app.uidList[seqPos];
                    fetchedUidCount++;
                }
            }
        }
    }

    if (totalMails <= 0 || fetchedUidCount == 0)
    {
        // 拉取失败：不清空已缓存的邮件列表，保持显示缓存数据
        result->success = false;
        result->count = g_app.emailCount;
        result->totalMails = g_app.totalMails;
        snprintf(result->error, sizeof(result->error),
                 "无法获取邮件列表（服务器拒绝登录或不支持IMAP）");
        result->finished = true;
        imap_disconnect(&ic);
        return -1;
    }

    // 批量获取FLAGS（已读/未读状态）
    {
        char uidList[512];
        int ulen = 0;
        for (int i = 0; i < fetchedUidCount && i < 64; i++)
        {
            if (uidMap[i] == 0) continue;
            int n = snprintf(uidList + ulen, sizeof(uidList) - ulen, "%s%lu",
                             (ulen > 0 ? "," : ""), (unsigned long)uidMap[i]);
            if (n > 0) ulen += n;
            if (ulen >= (int)sizeof(uidList) - 16) break;
        }
        if (ulen > 0)
        {
            char cmd[640];
            snprintf(cmd, sizeof(cmd), "UID FETCH %s (UID FLAGS)", uidList);
            char* flagsResp = NULL;
            size_t flagsLen = 0;
            int rcFlags = imap_send_recv(&ic, cmd, &flagsResp, &flagsLen);
            IMAP_LOG("FLAGS FETCH: rc=%d, size=%d\n", rcFlags, (int)flagsLen);
            if (rcFlags == 0 && flagsResp)
            {
                for (int i = 0; i < fetchedUidCount && i < 64; i++)
                {
                    if (uidMap[i] == 0) continue;
                    char uidStr[32];
                    snprintf(uidStr, sizeof(uidStr), "UID %lu", (unsigned long)uidMap[i]);
                    const char* p = strstr(flagsResp, uidStr);
                    if (p)
                    {
                        const char* lineEnd = strchr(p, '\n');
                        if (!lineEnd) lineEnd = p + strlen(p);
                        seenMap[i] = false;
                        for (const char* s = p; s < lineEnd - 5; s++)
                        {
                            if (s[0] == '\\' &&
                                (s[1] == 'S' || s[1] == 's') &&
                                (s[2] == 'e' || s[2] == 'E') &&
                                (s[3] == 'e' || s[3] == 'E') &&
                                (s[4] == 'n' || s[4] == 'N'))
                            {
                                seenMap[i] = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (flagsResp) free(flagsResp);
        }
    }

    // 加载邮件 — 优先从本地缓存读取，无缓存的批量FETCH（一次IMAP命令获取多封邮件头部）
    int newCount = 0;

    // 需要联网获取的UID列表（缓存未命中的邮件）
    u32 fetchUidList[64];
    int fetchUidIdx[64]; // 对应uidMap中的索引
    int fetchCount = 0;

    for (int i = 0; i < fetchedUidCount && i < 64; i++)
    {
        if (uidMap[i] == 0) continue;

        int idx = newCount;
        if (idx >= MAX_EMAILS) break;
        Email* mail = &g_app.emails[idx];
        memset(mail, 0, sizeof(Email));
        mail->id = idx + 1;
        mail->imapSeq = startSeq + i;
        mail->uid = uidMap[i];
        mail->accountIndex = accountIndex;
        mail->unread = !seenMap[i];
        mail->hasAttachment = false;

        // 先尝试从本地缓存加载（timestamp=0的旧缓存也直接使用，不重复联网）
        CacheEntry ce;
        memset(&ce, 0, sizeof(ce));
        int cached = (Cache_LoadHeader(acc->email, uidMap[i], &ce) == 0);
        if (cached)
        {
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
            newCount++;
        }
        else
        {
            // 缓存未命中，加入批量获取列表
            if (fetchCount < 64)
            {
                fetchUidList[fetchCount] = uidMap[i];
                fetchUidIdx[fetchCount] = i;
                fetchCount++;
            }
        }
    }

    // 批量FETCH未缓存的邮件头部（一次IMAP命令，大幅减少网络往返）
    if (fetchCount > 0)
    {
        // 构建UID列表字符串
        char uidListStr[768];
        int ulen = 0;
        for (int fi = 0; fi < fetchCount; fi++)
        {
            int n = snprintf(uidListStr + ulen, sizeof(uidListStr) - ulen, "%s%lu",
                             (ulen > 0 ? "," : ""), (unsigned long)fetchUidList[fi]);
            if (n > 0) ulen += n;
            if (ulen >= (int)sizeof(uidListStr) - 16) break;
        }

        // 发送批量FETCH命令（只获取需要的头部字段，减少数据传输）
        char batchCmd[1024];
        snprintf(batchCmd, sizeof(batchCmd),
                 "UID FETCH %s (BODY[HEADER.FIELDS (FROM SUBJECT DATE CONTENT-TYPE CONTENT-DISPOSITION)])",
                 uidListStr);

        char* batchResp = NULL;
        size_t batchLen = 0;
        int rcBatch = imap_send_recv(&ic, batchCmd, &batchResp, &batchLen);
        IMAP_LOG("Batch FETCH: rc=%d, size=%d, count=%d\n",
                 rcBatch, (int)batchLen, fetchCount);

        if (rcBatch == 0 && batchResp && batchLen > 0)
        {
            // 解析批量响应：每个邮件以 "* N FETCH (UID <uid> ... {size}\r\n<headers>\r\n)" 分隔
            const char* resp = batchResp;
            const char* respEnd = batchResp + batchLen;

            for (int fi = 0; fi < fetchCount; fi++)
            {
                u32 targetUid = fetchUidList[fi];
                int mapIdx = fetchUidIdx[fi];

                // 在响应中查找 "UID <targetUid>"
                char uidMarker[32];
                snprintf(uidMarker, sizeof(uidMarker), "UID %lu", (unsigned long)targetUid);
                const char* p = strstr(resp, uidMarker);
                if (!p) continue;

                // 从UID标记位置向后查找字面量 {N}
                const char* litStart = NULL;
                size_t litSize = 0;
                const char* scan = p;
                while (scan < respEnd - 2)
                {
                    if (*scan == '{')
                    {
                        const char* numStart = scan + 1;
                        const char* numEnd = numStart;
                        while (numEnd < respEnd && *numEnd >= '0' && *numEnd <= '9') numEnd++;
                        if (numEnd > numStart && numEnd < respEnd && *numEnd == '}')
                        {
                            litSize = (size_t)atol(numStart);
                            const char* afterBrace = numEnd + 1;
                            if (afterBrace < respEnd && *afterBrace == '\r') afterBrace++;
                            if (afterBrace < respEnd && *afterBrace == '\n') afterBrace++;
                            litStart = afterBrace;
                            break;
                        }
                    }
                    scan++;
                }

                if (!litStart || litSize == 0 || litStart + litSize > respEnd)
                    continue;

                // 提取头部数据并确保null终止
                char* hdrBuf = (char*)malloc(litSize + 1);
                if (!hdrBuf) continue;
                memcpy(hdrBuf, litStart, litSize);
                hdrBuf[litSize] = 0;

                // 填充Email结构体
                int idx = newCount;
                if (idx < MAX_EMAILS)
                {
                    Email* mail = &g_app.emails[idx];
                    memset(mail, 0, sizeof(Email));
                    mail->id = idx + 1;
                    mail->imapSeq = startSeq + mapIdx;
                    mail->uid = targetUid;
                    mail->accountIndex = accountIndex;
                    mail->unread = !seenMap[mapIdx];
                    mail->hasAttachment = false;

                    parse_raw_header_to_email(mail, hdrBuf, litSize);
                    newCount++;
                }

                free(hdrBuf);

                // 移动响应指针，避免重复匹配同一位置
                resp = litStart + litSize;
            }
        }
        else
        {
            IMAP_LOG("Batch FETCH failed, falling back to per-email FETCH\n");
            // 批量FETCH失败，回退到逐封FETCH
            for (int fi = 0; fi < fetchCount; fi++)
            {
                u32 uid = fetchUidList[fi];
                int mapIdx = fetchUidIdx[fi];
                char* hdrData = NULL;
                size_t hdrLen = 0;
                char fetchCmd[256];
                snprintf(fetchCmd, sizeof(fetchCmd), "UID FETCH %lu (BODY[HEADER])",
                         (unsigned long)uid);
                int rcHdr = imap_send_recv_literal(&ic, fetchCmd, &hdrData, &hdrLen);

                if (rcHdr == 0 && hdrData && hdrLen > 0)
                {
                    int idx = newCount;
                    if (idx < MAX_EMAILS)
                    {
                        Email* mail = &g_app.emails[idx];
                        memset(mail, 0, sizeof(Email));
                        mail->id = idx + 1;
                        mail->imapSeq = startSeq + mapIdx;
                        mail->uid = uid;
                        mail->accountIndex = accountIndex;
                        mail->unread = !seenMap[mapIdx];
                        mail->hasAttachment = false;
                        parse_raw_header_to_email(mail, hdrData, hdrLen);
                        newCount++;
                    }
                }
                if (hdrData) free(hdrData);
            }
        }
        if (batchResp) free(batchResp);
    }

    // 按时间戳降序排序（时间戳为0的排最后，UID作为兜底排序依据）
    if (newCount > 1)
    {
        int cmp_email_by_time(const void* a, const void* b)
        {
            const Email* ea = (const Email*)a;
            const Email* eb = (const Email*)b;
            if (ea->timestamp != 0 && eb->timestamp != 0)
            {
                if (ea->timestamp != eb->timestamp)
                    return (ea->timestamp > eb->timestamp) ? -1 : 1;
            }
            else if (ea->timestamp != 0) return -1;
            else if (eb->timestamp != 0) return 1;
            if (ea->uid != eb->uid)
                return (ea->uid > eb->uid) ? -1 : 1;
            return 0;
        }
        qsort(g_app.emails, newCount, sizeof(Email), cmp_email_by_time);
        for (int i = 0; i < newCount; i++)
            g_app.emails[i].id = i + 1;
    }

    imap_disconnect(&ic);

    if (newCount == 0)
    {
        // 拉取失败：保持已缓存的邮件列表不变
        snprintf(result->error, sizeof(result->error), "未解析到邮件");
        result->success = false;
        result->finished = true;
        return -1;
    }

    // 拉取成功，更新邮件列表
    g_app.emailCount = newCount;
    result->success = true;
    result->count = newCount;
    result->totalMails = totalMails;
    result->finished = true;
    #undef IMAP_LOG
    return newCount;
}

// ========== HTML 转纯文本 ==========

#define HTML_BUF_SIZE (128*1024)

// 解码常见HTML实体
static void html_decode_entity(const char** pp, char* out, int* outLen, int outCap)
{
    const char* p = *pp;
    if (*p != '&') { if (*outLen < outCap) out[(*outLen)++] = *p; return; }
    p++; // skip &
    if (strncasecmp(p, "nbsp;", 5) == 0) { if (*outLen < outCap) out[(*outLen)++] = ' '; p += 5; }
    else if (strncasecmp(p, "amp;", 4) == 0) { if (*outLen < outCap) out[(*outLen)++] = '&'; p += 4; }
    else if (strncasecmp(p, "lt;", 3) == 0) { if (*outLen < outCap) out[(*outLen)++] = '<'; p += 3; }
    else if (strncasecmp(p, "gt;", 3) == 0) { if (*outLen < outCap) out[(*outLen)++] = '>'; p += 3; }
    else if (strncasecmp(p, "quot;", 5) == 0) { if (*outLen < outCap) out[(*outLen)++] = '"'; p += 5; }
    else if (strncasecmp(p, "apos;", 5) == 0) { if (*outLen < outCap) out[(*outLen)++] = '\''; p += 5; }
    else if (strncasecmp(p, "copy;", 5) == 0) { if (*outLen < outCap-1) { out[(*outLen)++] = 0xC2; out[(*outLen)++] = 0xA9; } p += 5; }
    else if (strncasecmp(p, "reg;", 4) == 0) { if (*outLen < outCap-1) { out[(*outLen)++] = 0xC2; out[(*outLen)++] = 0xAE; } p += 4; }
    else if (strncasecmp(p, "mdash;", 6) == 0) { if (*outLen < outCap-2) { out[(*outLen)++] = 0xE2; out[(*outLen)++] = 0x80; out[(*outLen)++] = 0x94; } p += 6; }
    else if (strncasecmp(p, "ndash;", 6) == 0) { if (*outLen < outCap-2) { out[(*outLen)++] = 0xE2; out[(*outLen)++] = 0x80; out[(*outLen)++] = 0x93; } p += 6; }
    else if (strncasecmp(p, "hellip;", 7) == 0) { if (*outLen < outCap-2) { out[(*outLen)++] = 0xE2; out[(*outLen)++] = 0x80; out[(*outLen)++] = 0xA6; } p += 7; }
    else if (*p == '#')
    {
        // 数字实体 &#NNN; 或 &#xHH;
        p++;
        int code = 0;
        if (*p == 'x' || *p == 'X')
        {
            p++;
            while (*p && *p != ';' && isxdigit((unsigned char)*p)) { code = code * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10)); p++; }
        }
        else
        {
            while (*p && *p != ';' && isdigit((unsigned char)*p)) { code = code * 10 + (*p - '0'); p++; }
        }
        if (*p == ';') p++;
        // UTF-8编码
        if (code > 0 && code < 0x110000)
        {
            if (code < 0x80) { if (*outLen < outCap) out[(*outLen)++] = (char)code; }
            else if (code < 0x800) { if (*outLen < outCap-1) { out[(*outLen)++] = (char)(0xC0 | (code >> 6)); out[(*outLen)++] = (char)(0x80 | (code & 0x3F)); } }
            else if (code < 0x10000) { if (*outLen < outCap-2) { out[(*outLen)++] = (char)(0xE0 | (code >> 12)); out[(*outLen)++] = (char)(0x80 | ((code >> 6) & 0x3F)); out[(*outLen)++] = (char)(0x80 | (code & 0x3F)); } }
            else { if (*outLen < outCap-3) { out[(*outLen)++] = (char)(0xF0 | (code >> 18)); out[(*outLen)++] = (char)(0x80 | ((code >> 12) & 0x3F)); out[(*outLen)++] = (char)(0x80 | ((code >> 6) & 0x3F)); out[(*outLen)++] = (char)(0x80 | (code & 0x3F)); } }
        }
        *pp = p;
        return;
    }
    else
    {
        // 未知实体，原样输出
        if (*outLen < outCap) out[(*outLen)++] = '&';
        *pp = p;
        return;
    }
    if (*p == ';') p++;
    *pp = p;
}

// 将HTML转换为可读纯文本
// - 移除<style>/<script>/<!-- -->内容
// - <img>替换为[图片]
// - <br>/<p>/<div>/<h1-6>/<li>/<tr>等转换为换行
// - 解码HTML实体
// - 连续空行压缩为一个
static int html_to_text(const char* html, int htmlLen, char* out, int outCap)
{
    int outLen = 0;
    int i = 0;
    bool lastWasNewline = false;

    // 跳过DOCTYPE
    if (htmlLen > 10 && strncasecmp(html, "<!DOCTYPE", 9) == 0)
    {
        while (i < htmlLen && html[i] != '>') i++;
        if (i < htmlLen) i++;
    }

    while (i < htmlLen && outLen < outCap - 1)
    {
        if (html[i] == '<')
        {
            // 检查是否为注释
            if (i + 3 < htmlLen && html[i+1] == '!' && html[i+2] == '-' && html[i+3] == '-')
            {
                // 跳过注释
                i += 4;
                while (i + 2 < htmlLen && !(html[i] == '-' && html[i+1] == '-' && html[i+2] == '>')) i++;
                i += 3;
                continue;
            }

            // 提取标签名
            char tag[32] = {0};
            int tl = 0;
            int ti = i + 1;
            while (ti < htmlLen && html[ti] == ' ') ti++;
            bool isClose = (ti < htmlLen && html[ti] == '/');
            if (isClose) ti++;
            while (ti < htmlLen && tl < 31 && (isalnum((unsigned char)html[ti]) || html[ti] == '-'))
                tag[tl++] = tolower((unsigned char)html[ti++]);
            tag[tl] = 0;

            // 找到标签结束位置
            int tagEnd = ti;
            while (tagEnd < htmlLen && html[tagEnd] != '>') tagEnd++;

            // 处理需要跳过内容的标签
            if (!isClose && (strcasecmp(tag, "style") == 0 || strcasecmp(tag, "script") == 0 ||
                strcasecmp(tag, "head") == 0 || strcasecmp(tag, "title") == 0))
            {
                // 简单查找</tag>
                char closePattern[40];
                snprintf(closePattern, sizeof(closePattern), "</%s", tag);
                const char* cp = html + i + 1;
                while (cp < html + htmlLen)
                {
                    cp = strstr(cp, closePattern);
                    if (!cp) { cp = html + htmlLen; break; }
                    // 确认是</tag>而不是</tagxxx>
                    const char* after = cp + 2 + strlen(tag);
                    while (after < html + htmlLen && *after == ' ') after++;
                    if (after < html + htmlLen && *after == '>') { cp = after + 1; break; }
                    cp++;
                }
                i = (int)(cp - html);
                continue;
            }

            // 处理<img>标签
            if (!isClose && strcasecmp(tag, "img") == 0)
            {
                if (outLen < outCap - 6)
                {
                    memcpy(out + outLen, "[图片]", 12 > (outCap - outLen - 1) ? (outCap - outLen - 1) : 12);
                    outLen += strlen("[图片]");
                    if (outLen >= outCap) outLen = outCap - 1;
                }
                i = tagEnd + 1;
                lastWasNewline = false;
                continue;
            }

            // 处理换行标签
            if (!isClose && (strcasecmp(tag, "br") == 0 || strcasecmp(tag, "hr") == 0))
            {
                if (outLen > 0 && out[outLen-1] != '\n' && outLen < outCap - 1)
                    out[outLen++] = '\n';
                i = tagEnd + 1;
                lastWasNewline = true;
                continue;
            }

            // 块级标签：开标签和闭标签都产生换行
            if (strcasecmp(tag, "p") == 0 || strcasecmp(tag, "div") == 0 ||
                strcasecmp(tag, "h1") == 0 || strcasecmp(tag, "h2") == 0 ||
                strcasecmp(tag, "h3") == 0 || strcasecmp(tag, "h4") == 0 ||
                strcasecmp(tag, "h5") == 0 || strcasecmp(tag, "h6") == 0 ||
                strcasecmp(tag, "li") == 0 || strcasecmp(tag, "tr") == 0 ||
                strcasecmp(tag, "blockquote") == 0 || strcasecmp(tag, "pre") == 0 ||
                strcasecmp(tag, "table") == 0 || strcasecmp(tag, "ul") == 0 ||
                strcasecmp(tag, "ol") == 0 || strcasecmp(tag, "center") == 0)
            {
                if (isClose)
                {
                    if (outLen > 0 && out[outLen-1] != '\n' && outLen < outCap - 1)
                        out[outLen++] = '\n';
                    lastWasNewline = true;
                }
                else
                {
                    if (outLen > 0 && out[outLen-1] != '\n' && outLen < outCap - 1)
                        out[outLen++] = '\n';
                    lastWasNewline = true;
                }
                i = tagEnd + 1;
                continue;
            }

            // 其他标签直接跳过
            i = tagEnd + 1;
        }
        else if (html[i] == '&')
        {
            const char* pp = html + i;
            html_decode_entity(&pp, out, &outLen, outCap);
            i = (int)(pp - html);
            lastWasNewline = false;
        }
        else if (html[i] == '\r')
        {
            i++; // 跳过\r
        }
        else if (html[i] == '\n')
        {
            // HTML中的换行在文本中当作空格（除非在<pre>中，简化处理）
            if (outLen > 0 && out[outLen-1] != ' ' && out[outLen-1] != '\n' && outLen < outCap - 1)
                out[outLen++] = ' ';
            i++;
        }
        else
        {
            if (outLen < outCap - 1)
                out[outLen++] = html[i];
            i++;
            lastWasNewline = (html[i-1] == '\n');
        }
    }

    out[outLen] = 0;

    // 后处理：压缩连续空行为一个，去除行首行尾空格（用堆分配避免栈溢出）
    char* final = (char*)malloc(outCap);
    if (!final) return outLen; // 内存不足，返回未后处理的结果
    int fi = 0;
    int si = 0;
    int blankCount = 0;
    while (si < outLen)
    {
        // 读取一行
        int lineStart = si;
        while (si < outLen && out[si] != '\n') si++;
        int lineEnd = si;
        if (si < outLen) si++; // skip \n

        // 去除行首空格
        while (lineStart < lineEnd && (out[lineStart] == ' ' || out[lineStart] == '\t')) lineStart++;
        // 去除行尾空格
        while (lineEnd > lineStart && (out[lineEnd-1] == ' ' || out[lineEnd-1] == '\t')) lineEnd--;

        if (lineStart >= lineEnd)
        {
            blankCount++;
            if (blankCount <= 1 && fi > 0 && fi < outCap - 1)
                final[fi++] = '\n';
        }
        else
        {
            blankCount = 0;
            int copyLen = lineEnd - lineStart;
            if (fi + copyLen >= outCap) copyLen = outCap - fi - 1;
            if (copyLen > 0)
            {
                memcpy(final + fi, out + lineStart, copyLen);
                fi += copyLen;
            }
            if (fi < outCap - 1)
                final[fi++] = '\n';
        }
    }
    // 去除末尾换行
    while (fi > 0 && final[fi-1] == '\n') fi--;
    final[fi] = 0;

    memcpy(out, final, fi + 1);
    free(final);
    return fi;
}

// ========== 原始邮件解析（curl兜底路径用） ==========

// 检测内容是否为base64编码（启发式：字符集和长度特征）
static int looks_like_base64(const char* data, int len)
{
    if (len < 8) return 0;
    int b64chars = 0, total = 0, lines = 0;
    int lineLen = 0;
    for (int i = 0; i < len; i++)
    {
        char c = data[i];
        if (c == '\r' || c == '\n')
        {
            if (lineLen > 0) { lines++; if (lineLen % 4 != 0) return 0; lineLen = 0; }
            continue;
        }
        if (c == ' ' || c == '\t') continue;
        total++;
        lineLen++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')
            b64chars++;
        else
            return 0; // 非base64字符
    }
    if (lineLen > 0) { lines++; if (lineLen % 4 != 0) return 0; }
    if (total < 16 || lines < 1) return 0;
    return (b64chars == total) ? 1 : 0;
}

// 从头部中提取boundary参数
static void extract_boundary(const char* headers, char* boundary, int bSize)
{
    boundary[0] = 0;
    const char* ct = stristr(headers, "Content-Type:");
    if (!ct) ct = stristr(headers, "Content-type:");
    if (!ct) return;
    const char* b = stristr(ct, "boundary=");
    if (!b) return;
    b += 9;
    while (*b == ' ' || *b == '\t') b++;
    if (*b == '"')
    {
        b++;
        int bi = 0;
        while (*b && *b != '"' && bi < bSize - 1)
            boundary[bi++] = *b++;
        boundary[bi] = 0;
    }
    else
    {
        int bi = 0;
        while (*b && *b != '\r' && *b != '\n' && *b != ';' && bi < bSize - 1)
            boundary[bi++] = *b++;
        boundary[bi] = 0;
    }
}

// 解码单个part的内容（QP/base64/7bit + charset转换）
// 返回解码后的长度，结果写入outBuf（调用者保证outBuf至少BODY_BUF_SIZE大小）
static int decode_part_content(const char* data, int dataLen,
                                const char* cte, const char* contentType,
                                char* outBuf, int outCap)
{
    if (dataLen <= 0 || !outBuf || outCap <= 0) return 0;

    // 先复制到临时缓冲区
    char* tmp = (char*)malloc(outCap);
    if (!tmp) return 0;
    int tmpLen = 0;

    if (cte && stristr(cte, "base64"))
    {
        // base64解码
        char* b64buf = (char*)malloc(dataLen + 1);
        if (b64buf)
        {
            int bi = 0;
            for (int i = 0; i < dataLen; i++)
            {
                char c = data[i];
                if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
                    b64buf[bi++] = c;
            }
            b64buf[bi] = 0;
            tmpLen = base64_decode_simple(b64buf, bi, (unsigned char*)tmp, outCap - 1);
            if (tmpLen > 0) tmp[tmpLen] = 0;
            free(b64buf);
        }
    }
    else if (cte && stristr(cte, "quoted-printable"))
    {
        // QP解码
        for (int i = 0; i < dataLen && tmpLen < outCap - 1; i++)
        {
            if (data[i] == '=' && i + 1 < dataLen)
            {
                if (data[i+1] == '\r' || data[i+1] == '\n')
                {
                    i++;
                    if (i < dataLen && data[i] == '\r') i++;
                    if (i < dataLen && data[i] == '\n') i++;
                    i--;
                    continue;
                }
                if (i + 2 < dataLen)
                {
                    char hx[3] = {data[i+1], data[i+2], 0};
                    if (isxdigit((unsigned char)hx[0]) && isxdigit((unsigned char)hx[1]))
                    {
                        tmp[tmpLen++] = (char)strtol(hx, NULL, 16);
                        i += 2;
                        continue;
                    }
                }
            }
            tmp[tmpLen++] = data[i];
        }
        tmp[tmpLen] = 0;
    }
    else
    {
        // 7bit/8bit/binary 直接复制
        tmpLen = dataLen;
        if (tmpLen > outCap - 1) tmpLen = outCap - 1;
        memcpy(tmp, data, tmpLen);
        tmp[tmpLen] = 0;
    }

    if (tmpLen <= 0) { free(tmp); return 0; }

    // charset转换
    if (contentType)
    {
        const char* cs = stristr(contentType, "charset=");
        if (cs)
        {
            cs += 8;
            char chs[64] = {0};
            int ci = 0;
            if (*cs == '"' || *cs == '\'') cs++;
            while (*cs && *cs != '"' && *cs != '\'' && *cs != ';' &&
                   *cs != '\r' && *cs != '\n' && *cs != ' ' && ci < 63)
                chs[ci++] = *cs++;
            chs[ci] = 0;
            if (chs[0] && strcasecmp(chs, "UTF-8") != 0 && strcasecmp(chs, "utf8") != 0 &&
                strcasecmp(chs, "US-ASCII") != 0 && strcasecmp(chs, "ASCII") != 0 &&
                strcasecmp(chs, "ISO-8859-1") != 0 && strcasecmp(chs, "latin1") != 0)
            {
                char* conv = (char*)malloc(outCap);
                if (conv)
                {
                    int cl = convert_to_utf8(chs, tmp, tmpLen, conv, outCap);
                    if (cl > 0)
                    {
                        memcpy(tmp, conv, cl);
                        tmp[cl] = 0;
                        tmpLen = cl;
                    }
                    free(conv);
                }
            }
        }
    }

    memcpy(outBuf, tmp, tmpLen + 1);
    free(tmp);
    return tmpLen;
}

// 从单个MIME part（头部+正文）中提取文本
// 返回0=未找到文本，1=找到text/plain，2=找到text/html
static int parse_single_part(const char* partData, int partLen,
                              char* plainOut, int* plainLenOut,
                              char* htmlOut, int* htmlLenOut, int outCap)
{
    // 分离头部和正文
    const char* hdrEnd = strstr(partData, "\r\n\r\n");
    int sep = 4;
    if (!hdrEnd) { hdrEnd = strstr(partData, "\n\n"); sep = 2; }

    const char* bodyStart = partData;
    int bodyLen = partLen;
    char hdrBuf[2048] = {0};

    if (hdrEnd)
    {
        int hdrLen = (int)(hdrEnd - partData);
        if (hdrLen > 2047) hdrLen = 2047;
        memcpy(hdrBuf, partData, hdrLen);
        bodyStart = hdrEnd + sep;
        bodyLen = partLen - (int)(bodyStart - partData);
        while (bodyLen > 0 && (bodyStart[bodyLen-1] == '\n' || bodyStart[bodyLen-1] == '\r'))
            bodyLen--;
    }

    // 查找Content-Type和Content-Transfer-Encoding
    char ctBuf[512] = {0};
    const char* ct = find_header(hdrBuf, "Content-Type", NULL);
    if (ct) strncpy(ctBuf, ct, sizeof(ctBuf) - 1);
    else strcpy(ctBuf, "text/plain");

    const char* cte = find_header(hdrBuf, "Content-Transfer-Encoding", NULL);

    // QP/Base64 自动检测
    char cteBuf[64] = {0};
    if (!cte && bodyLen > 10)
    {
        int qpHits = 0, softBreaks = 0;
        int chkLen = bodyLen > 4096 ? 4096 : bodyLen;
        for (int qi = 0; qi < chkLen - 2; qi++)
        {
            if (bodyStart[qi] == '=' && bodyStart[qi+1] == '\r' && bodyStart[qi+2] == '\n')
                softBreaks++;
            else if (bodyStart[qi] == '=' && bodyStart[qi+1] == '\n')
                softBreaks++;
            else if (bodyStart[qi] == '=' &&
                     isxdigit((unsigned char)bodyStart[qi+1]) &&
                     isxdigit((unsigned char)bodyStart[qi+2]))
                qpHits++;
        }
        if (qpHits > 5 || softBreaks > 2)
        {
            strcpy(cteBuf, "quoted-printable");
            cte = cteBuf;
        }
        else if (looks_like_base64(bodyStart, chkLen))
        {
            strcpy(cteBuf, "base64");
            cte = cteBuf;
        }
    }

    if (stristr(ctBuf, "text/html"))
    {
        *htmlLenOut = decode_part_content(bodyStart, bodyLen, cte, ctBuf, htmlOut, outCap);
        return *htmlLenOut > 0 ? 2 : 0;
    }
    else if (stristr(ctBuf, "text/"))
    {
        *plainLenOut = decode_part_content(bodyStart, bodyLen, cte, ctBuf, plainOut, outCap);
        return *plainLenOut > 0 ? 1 : 0;
    }
    return 0;
}

// 解析原始RFC822邮件数据，提取text/plain和text/html
static void parse_raw_email(const char* data, int dataLen,
                             char* plainOut, int* plainLenOut,
                             char* htmlOut, int* htmlLenOut, int outCap)
{
    *plainLenOut = 0;
    *htmlLenOut = 0;
    plainOut[0] = 0;
    htmlOut[0] = 0;

    if (!data || dataLen <= 0) return;

    const char* bodyStart = data;
    int bodyLen = dataLen;
    char topHdr[2048] = {0};

    // 检查内容是否以MIME boundary开头（libcurl可能不返回顶层头部）
    int startsWithBoundary = 0;
    {
        const char* p = data;
        while (p < data + dataLen && (*p == '\r' || *p == '\n')) p++;
        if (p < data + dataLen - 2 && p[0] == '-' && p[1] == '-')
            startsWithBoundary = 1;
    }

    if (!startsWithBoundary)
    {
        // 分离邮件头部和正文
        const char* hdrEnd = strstr(data, "\r\n\r\n");
        int sep = 4;
        if (!hdrEnd) { hdrEnd = strstr(data, "\n\n"); sep = 2; }
        if (hdrEnd)
        {
            int topHdrLen = (int)(hdrEnd - data);
            // 检查"头部"是否真的是邮件头部（必须包含冒号，否则可能是误分割）
            int looksLikeHeaders = 0;
            {
                const char* hp = data;
                const char* he = hdrEnd;
                while (hp < he)
                {
                    if (*hp == ':') { looksLikeHeaders = 1; break; }
                    hp++;
                }
            }
            if (looksLikeHeaders)
            {
                if (topHdrLen > 2047) topHdrLen = 2047;
                memcpy(topHdr, data, topHdrLen);
                bodyStart = hdrEnd + sep;
                bodyLen = dataLen - (int)(bodyStart - data);
            }
            // 如果不像头部，整个内容当正文处理（boundary扫描会处理）
        }
    }

    // 检查是否multipart：先从头部找boundary，找不到则从正文扫描
    char boundary[256] = {0};
    extract_boundary(topHdr, boundary, sizeof(boundary));

    if (!boundary[0])
    {
        // 从正文扫描boundary：找以 "--" 开头的行
        const char* p = bodyStart;
        const char* end = data + dataLen;
        while (p < end - 2)
        {
            if (p[0] == '-' && p[1] == '-' &&
                (p == bodyStart || p[-1] == '\n'))
            {
                // 提取boundary字符串
                const char* bs = p + 2;
                const char* be = bs;
                while (be < end && *be != '\r' && *be != '\n' && *be != ' ') be++;
                int bl = (int)(be - bs);
                if (bl > 5 && bl < 250)
                {
                    // 验证这个boundary是否出现至少2次
                    int count = 0;
                    char probe[280];
                    snprintf(probe, sizeof(probe), "--%.*s", bl, bs);
                    const char* pp = bodyStart;
                    while ((pp = strstr(pp, probe)) != NULL)
                    {
                        count++;
                        pp += strlen(probe);
                        if (count >= 2) break;
                    }
                    if (count >= 2)
                    {
                        memcpy(boundary, bs, bl);
                        boundary[bl] = 0;
                        break;
                    }
                }
            }
            p++;
        }
    }

    if (boundary[0])
    {
        // multipart：按boundary分割
        char bnd[280];
        snprintf(bnd, sizeof(bnd), "--%s", boundary);
        int bndLen = (int)strlen(bnd);

        const char* p = bodyStart;
        const char* end = data + dataLen;

        // 检查preamble（第一个boundary之前的内容），如果看起来是base64，作为text/plain解码
        const char* firstB = strstr(p, bnd);
        if (firstB && firstB > p)
        {
            int preLen = (int)(firstB - p);
            // 去掉尾部空白
            while (preLen > 0 && (p[preLen-1] == '\r' || p[preLen-1] == '\n' ||
                                  p[preLen-1] == ' ' || p[preLen-1] == '\t'))
                preLen--;
            if (preLen > 16 && looks_like_base64(p, preLen))
            {
                char ctDummy[32] = "text/plain";
                *plainLenOut = decode_part_content(p, preLen, "base64", ctDummy,
                                                    plainOut, outCap);
            }
        }
        if (firstB) p = firstB;

        while (p < end)
        {
            // 找到下一个boundary
            const char* b = strstr(p, bnd);
            if (!b) break;

            // 跳过boundary行
            const char* partStart = b + bndLen;
            if (partStart < end && *partStart == '-') break; // 结束boundary
            while (partStart < end && *partStart != '\n') partStart++;
            if (partStart < end) partStart++;
            if (partStart < end && *partStart == '\r') partStart++;

            // 找下一个boundary
            const char* nextB = strstr(partStart, bnd);
            int partLen;
            if (nextB)
            {
                partLen = (int)(nextB - partStart);
                // 去掉part末尾的\r\n
                while (partLen > 0 && (partStart[partLen-1] == '\n' || partStart[partLen-1] == '\r'))
                    partLen--;
            }
            else
            {
                partLen = (int)(end - partStart);
            }

            if (partLen > 0)
            {
                // 检查这个part是否嵌套multipart
                char partHdr[1024] = {0};
                const char* pHdrEnd = strstr(partStart, "\r\n\r\n");
                if (!pHdrEnd) pHdrEnd = strstr(partStart, "\n\n");
                if (pHdrEnd)
                {
                    int pHdrLen = (int)(pHdrEnd - partStart);
                    if (pHdrLen > 1023) pHdrLen = 1023;
                    memcpy(partHdr, partStart, pHdrLen);
                }

                char subBoundary[256] = {0};
                extract_boundary(partHdr, subBoundary, sizeof(subBoundary));

                if (subBoundary[0])
                {
                    // 嵌套multipart，递归解析
                    int subPlain = 0, subHtml = 0;
                    char* subPlainBuf = (char*)malloc(outCap);
                    char* subHtmlBuf = (char*)malloc(outCap);
                    if (subPlainBuf && subHtmlBuf)
                    {
                        parse_raw_email(partStart, partLen,
                                        subPlainBuf, &subPlain,
                                        subHtmlBuf, &subHtml, outCap);
                        if (subPlain > 0 && *plainLenOut == 0)
                        {
                            memcpy(plainOut, subPlainBuf, subPlain + 1);
                            *plainLenOut = subPlain;
                        }
                        if (subHtml > 0 && *htmlLenOut == 0)
                        {
                            memcpy(htmlOut, subHtmlBuf, subHtml + 1);
                            *htmlLenOut = subHtml;
                        }
                    }
                    if (subPlainBuf) free(subPlainBuf);
                    if (subHtmlBuf) free(subHtmlBuf);
                }
                else
                {
                    // 单个part
                    char* tmpPlain = (char*)malloc(outCap);
                    char* tmpHtml = (char*)malloc(outCap);
                    if (tmpPlain && tmpHtml)
                    {
                        int tp = 0, th = 0;
                        int ret = parse_single_part(partStart, partLen,
                                                    tmpPlain, &tp,
                                                    tmpHtml, &th, outCap);
                        if (ret == 1 && *plainLenOut == 0)
                        {
                            memcpy(plainOut, tmpPlain, tp + 1);
                            *plainLenOut = tp;
                        }
                        else if (ret == 2 && *htmlLenOut == 0)
                        {
                            memcpy(htmlOut, tmpHtml, th + 1);
                            *htmlLenOut = th;
                        }
                    }
                    if (tmpPlain) free(tmpPlain);
                    if (tmpHtml) free(tmpHtml);
                }
            }

            if (!nextB) break;
            p = nextB;
        }
    }
    else
    {
        // 单part邮件
        parse_single_part(bodyStart, bodyLen, plainOut, plainLenOut, htmlOut, htmlLenOut, outCap);
    }
}

// ========== BODYSTRUCTURE 解析：只找 text/plain 和 text/html 的节号 ==========

typedef struct {
    char plain[32];  // 如 "1.1"，空表示未找到
    char html[32];   // 如 "1.2"
    int  isMultipart; // 顶层是否为 multipart
} TextSections;

// 跳过引号字符串（p 指向开头引号）
static const char* bs_skip_quoted(const char* p, const char* end) {
    p++;
    while (p < end) {
        if (*p == '\\' && p + 1 < end) { p += 2; continue; }
        if (*p == '"') { p++; break; }
        p++;
    }
    return p;
}

// 跳过括号组（p 指向 '('）
static const char* bs_skip_paren(const char* p, const char* end) {
    int depth = 0;
    while (p < end) {
        if (*p == '"') { p = bs_skip_quoted(p, end); continue; }
        if (*p == '(') depth++;
        if (*p == ')') { depth--; if (depth == 0) { p++; break; } }
        p++;
    }
    return p;
}

// 递归解析 BODYSTRUCTURE
// p 指向当前层级的 '('
// prefix: 当前节号前缀（如 "1."，顶层为空字符串）
static const char* parse_bodystructure(const char* p, const char* end,
                                        const char* prefix, TextSections* sec) {
    p++; // 跳过 '('
    int partNum = 1;

    while (p < end && *p != ')') {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= end || *p == ')') break;

        if (*p == '(') {
            // 判断是 multipart 还是单 part
            const char* q = p + 1;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;

            if (q < end && *q == '(') {
                // multipart：第一个子元素还是 '('，递归
                char newPrefix[32];
                if (prefix[0])
                    snprintf(newPrefix, sizeof(newPrefix), "%s%d.", prefix, partNum);
                else
                    snprintf(newPrefix, sizeof(newPrefix), "%d.", partNum);
                p = parse_bodystructure(p, end, newPrefix, sec);
            } else if (q < end && *q == '"') {
                // 单 part：("type" "subtype" ...)
                const char* typeStart = q + 1;
                const char* typeEnd = NULL;
                for (const char* t = typeStart; t < end; t++) {
                    if (*t == '"') { typeEnd = t; break; }
                }
                if (typeEnd) {
                    const char* sq = typeEnd + 1;
                    while (sq < end && (*sq == ' ' || *sq == '\t')) sq++;
                    if (sq < end && *sq == '"') {
                        const char* subStart = sq + 1;
                        const char* subEnd = NULL;
                        for (const char* t = subStart; t < end; t++) {
                            if (*t == '"') { subEnd = t; break; }
                        }
                        if (subEnd) {
                            char type[32], subtype[32];
                            int tl = typeEnd - typeStart;
                            int sl = subEnd - subStart;
                            if (tl > 31) tl = 31;
                            if (sl > 31) sl = 31;
                            memcpy(type, typeStart, tl); type[tl] = 0;
                            memcpy(subtype, subStart, sl); subtype[sl] = 0;

                            if (strcasecmp(type, "text") == 0) {
                                char section[32];
                                if (prefix[0])
                                    snprintf(section, sizeof(section), "%s%d", prefix, partNum);
                                else
                                    snprintf(section, sizeof(section), "%d", partNum);

                                if (strcasecmp(subtype, "plain") == 0 && sec->plain[0] == 0)
                                    strncpy(sec->plain, section, sizeof(sec->plain) - 1);
                                else if (strcasecmp(subtype, "html") == 0 && sec->html[0] == 0)
                                    strncpy(sec->html, section, sizeof(sec->html) - 1);
                            }
                        }
                    }
                }
                p = bs_skip_paren(p, end);
            } else {
                p = bs_skip_paren(p, end);
            }
            partNum++;
        } else if (*p == '"') {
            p = bs_skip_quoted(p, end);
        } else if (*p == ')') {
            break;
        } else {
            p++;
        }
    }

    if (p < end && *p == ')') p++;
    return p;
}

// 从 IMAP FETCH 响应中提取 BODYSTRUCTURE 并解析
static int extract_text_sections(const char* resp, size_t respLen, TextSections* sec) {
    memset(sec, 0, sizeof(*sec));

    const char* bs = strstr(resp, "BODYSTRUCTURE");
    if (!bs) bs = strstr(resp, "BODY");
    if (!bs) return -1;

    // 找到 BODYSTRUCTURE 后的第一个 '('
    const char* p = bs + 12;
    if (strncmp(bs, "BODY", 4) == 0 && strncmp(bs, "BODYSTRUCTURE", 13) != 0)
        p = bs + 4;
    const char* end = resp + respLen;
    while (p < end && *p != '(') p++;
    if (p >= end) return -1;

    // 判断是否 multipart：第一个 '(' 后的第一个非空白字符
    const char* q = p + 1;
    while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
    if (q < end && *q == '(')
        sec->isMultipart = 1;
    else
        sec->isMultipart = 0;

    parse_bodystructure(p, end, "", sec);
    return 0;
}

bool Network_FetchMailBody(int accountIndex, int imapSeq)
{
    if (accountIndex < 0 || accountIndex >= MAX_ACCOUNTS ||
        !g_app.accounts[accountIndex].added || imapSeq <= 0)
        return false;

    Account* acc = &g_app.accounts[accountIndex];

    // 找到对应邮件
    int mailIdx = -1;
    for (int i = 0; i < g_app.emailCount; i++)
    {
        if (g_app.emails[i].imapSeq == imapSeq) { mailIdx = i; break; }
    }
    if (mailIdx < 0) return false;

    Email* mail = &g_app.emails[mailIdx];

    // 1. 先查本地正文缓存
    if (mail->uid > 0)
    {
        char* cached = Cache_LoadBodyPlain(acc->email, mail->uid);
        if (cached)
        {
            // 正文已缓存，直接返回（图片统一用[图片]占位，不下载）
            strncpy(mail->body, cached, sizeof(mail->body) - 1);
            mail->body[sizeof(mail->body) - 1] = 0;
            free(cached);
            mail->bodyLoaded = true;
            mail->unread = false;
            return true;
        }
    }

    // 2. 通过 IMAP 连接，先获取 BODYSTRUCTURE，只拉取文本 part（不拉取图片）
    ImapConn ic;
    if (imap_connect(&ic, acc->imapServer, acc->imapPort, acc->email, acc->password) != 0)
    {
        Cache_Log("FetchBody: IMAP connect failed for %s", acc->email);
        return false;
    }

    // 必须先 SELECT INBOX，否则 UID FETCH 会失败
    {
        char* selResp = NULL;
        size_t selLen = 0;
        int rcSel = imap_send_recv(&ic, "SELECT INBOX", &selResp, &selLen);
        if (rcSel != 0)
        {
            Cache_Log("FetchBody: SELECT INBOX failed for %s", acc->email);
            if (selResp) free(selResp);
            imap_disconnect(&ic);
            return false;
        }
        if (selResp) free(selResp);
    }

    // 2a. 获取 BODYSTRUCTURE
    char bsCmd[128];
    snprintf(bsCmd, sizeof(bsCmd), "UID FETCH %lu (BODYSTRUCTURE)",
             (unsigned long)mail->uid);
    char* bsResp = NULL;
    size_t bsLen = 0;
    int rcBs = imap_send_recv(&ic, bsCmd, &bsResp, &bsLen);

    TextSections sections;
    memset(&sections, 0, sizeof(sections));
    if (rcBs == 0 && bsResp && bsLen > 0)
    {
        extract_text_sections(bsResp, bsLen, &sections);
        Cache_Log("FetchBody BODYSTRUCTURE: multipart=%d, plain=[%s], html=[%s]",
                  sections.isMultipart, sections.plain, sections.html);
    }
    if (bsResp) free(bsResp);

    // 使用128KB缓冲区，营销邮件HTML可能很大
    #define BODY_BUF_SIZE (128*1024)
    char* plainBuf = (char*)malloc(BODY_BUF_SIZE);
    char* htmlBuf = (char*)malloc(BODY_BUF_SIZE);
    if (!plainBuf || !htmlBuf)
    {
        if (plainBuf) free(plainBuf);
        if (htmlBuf) free(htmlBuf);
        imap_disconnect(&ic);
        return false;
    }
    plainBuf[0] = 0;
    htmlBuf[0] = 0;
    int plainLen = 0, htmlLen = 0;

    // DECODE_PART2 宏：解码单个 part（含 CTE 解码和 charset 转换）
    #define DECODE_PART2(cdata, clen, cteVal, partCTVal, outBuf, outLen) do { \
        const char* dSrc = cdata; int dLen = clen; \
        char* dTmp = (char*)malloc(BODY_BUF_SIZE); \
        if (dTmp) { \
            if (cteVal && stristr(cteVal, "base64")) { \
                char* b64buf = (char*)malloc(dLen + 1); \
                if (b64buf) { \
                    int bi2 = 0; \
                    for (int ii = 0; ii < dLen; ii++) { \
                        char cc = dSrc[ii]; \
                        if (cc != '\r' && cc != '\n' && cc != ' ' && cc != '\t') b64buf[bi2++] = cc; \
                    } \
                    b64buf[bi2] = 0; \
                    int dl = base64_decode_simple(b64buf, bi2, (unsigned char*)dTmp, BODY_BUF_SIZE-1); \
                    if (dl > 0) { dTmp[dl] = 0; dSrc = dTmp; dLen = dl; } \
                    else { dSrc = NULL; dLen = 0; } \
                    free(b64buf); \
                } \
            } else if (cteVal && stristr(cteVal, "quoted-printable")) { \
                int di = 0; \
                for (int ii = 0; ii < dLen && di < BODY_BUF_SIZE-1; ii++) { \
                    if (dSrc[ii] == '=' && ii + 1 < dLen) { \
                        if (dSrc[ii+1] == '\r' || dSrc[ii+1] == '\n') { \
                            ii++; \
                            if (ii < dLen && dSrc[ii] == '\r') ii++; \
                            if (ii < dLen && dSrc[ii] == '\n') ii++; \
                            ii--; \
                            continue; \
                        } \
                        if (ii + 2 < dLen) { \
                            char hx[3] = {dSrc[ii+1], dSrc[ii+2], 0}; \
                            if (isxdigit((unsigned char)hx[0]) && isxdigit((unsigned char)hx[1])) { \
                                dTmp[di++] = (char)strtol(hx, NULL, 16); ii += 2; continue; \
                            } \
                        } \
                    } \
                    dTmp[di++] = dSrc[ii]; \
                } \
                dTmp[di] = 0; dSrc = dTmp; dLen = di; \
            } else if (cteVal && stristr(cteVal, "7bit")) { \
                if (dLen > BODY_BUF_SIZE-1) dLen = BODY_BUF_SIZE-1; \
                memcpy(dTmp, dSrc, dLen); dTmp[dLen] = 0; dSrc = dTmp; \
            } else { \
                if (dLen > BODY_BUF_SIZE-1) dLen = BODY_BUF_SIZE-1; \
                memcpy(dTmp, dSrc, dLen); dTmp[dLen] = 0; dSrc = dTmp; \
            } \
            if (dSrc && dLen > 0 && partCTVal && partCTVal[0]) { \
                const char* cs2 = stristr(partCTVal, "charset="); \
                if (cs2) { \
                    cs2 += 8; \
                    char chs[64] = {0}; int ci2 = 0; \
                    if (*cs2 == '"' || *cs2 == '\'') cs2++; \
                    while (*cs2 && *cs2 != '"' && *cs2 != '\'' && *cs2 != ';' && *cs2 != '\r' && *cs2 != '\n' && *cs2 != ' ' && ci2 < 63) \
                        chs[ci2++] = *cs2++; \
                    chs[ci2] = 0; \
                    if (chs[0] && strcasecmp(chs, "UTF-8") != 0 && strcasecmp(chs, "utf8") != 0 && \
                        strcasecmp(chs, "US-ASCII") != 0 && strcasecmp(chs, "ASCII") != 0 && \
                        strcasecmp(chs, "ISO-8859-1") != 0 && strcasecmp(chs, "latin1") != 0) { \
                        char* conv = (char*)malloc(BODY_BUF_SIZE); \
                        if (conv) { \
                            int cl = convert_to_utf8(chs, dSrc, dLen, conv, BODY_BUF_SIZE); \
                            if (cl > 0) { memcpy(dTmp, conv, cl); dTmp[cl] = 0; dSrc = dTmp; dLen = cl; } \
                            free(conv); \
                        } \
                    } \
                } \
            } \
            if (dSrc && dLen > 0) { \
                if (dLen > BODY_BUF_SIZE-1) dLen = BODY_BUF_SIZE-1; \
                memcpy(outBuf, dSrc, dLen); outBuf[dLen] = 0; outLen = dLen; \
            } \
            free(dTmp); \
        } \
    } while(0)

    // 2b. 拉取单个文本 part 并解码
    // partData 是 IMAP 返回的字面量数据，包含该 part 的头部和正文
    // partDataLen 是数据总长度（含头部）
    // outBuf/outLen 是输出缓冲区
    // wantType: "text/plain" 或 "text/html"
    int fetch_and_decode_part(const char* section, char* outBuf, int* outLen,
                               const char* wantType) {
        char fetchCmd[256];
        snprintf(fetchCmd, sizeof(fetchCmd), "UID FETCH %lu (BODY[%s])",
                 (unsigned long)mail->uid, section);
        char* partData = NULL;
        size_t partLen = 0;
        int rc = imap_send_recv_literal(&ic, fetchCmd, &partData, &partLen);
        if (rc != 0 || !partData || partLen == 0) {
            if (partData) free(partData);
            return -1;
        }

        // 分离 part 头部和正文
        const char* partHdrEnd = strstr(partData, "\r\n\r\n");
        int partSep = 4;
        if (!partHdrEnd) {
            partHdrEnd = strstr(partData, "\n\n");
            partSep = 2;
        }

        const char* partCT = NULL;
        const char* partCTE = NULL;
        const char* partBodyStart = partData;
        int partBodyLen = (int)partLen;

        if (partHdrEnd) {
            int partHdrLen = (int)(partHdrEnd - partData);
            char partHdr[2048] = {0};
            if (partHdrLen > 2047) partHdrLen = 2047;
            memcpy(partHdr, partData, partHdrLen);
            partCT = find_header(partHdr, "Content-Type", NULL);
            partCTE = find_header(partHdr, "Content-Transfer-Encoding", NULL);
            partBodyStart = partHdrEnd + partSep;
            partBodyLen = (int)(partLen - (size_t)(partBodyStart - partData));
            while (partBodyLen > 0 && (partBodyStart[partBodyLen-1] == '\n' ||
                   partBodyStart[partBodyLen-1] == '\r'))
                partBodyLen--;
        }

        char partCTCopy[512] = {0};
        if (partCT) strncpy(partCTCopy, partCT, sizeof(partCTCopy) - 1);
        else strncpy(partCTCopy, wantType, sizeof(partCTCopy) - 1);

        // QP 自动检测
        char partCteBuf[64] = {0};
        if (!partCTE && partBodyLen > 10) {
            int qpHits = 0, softBreaks = 0;
            int chkLen = partBodyLen > 4096 ? 4096 : partBodyLen;
            for (int qi = 0; qi < chkLen - 2; qi++) {
                if (partBodyStart[qi] == '=' && partBodyStart[qi+1] == '\r' && partBodyStart[qi+2] == '\n')
                    softBreaks++;
                else if (partBodyStart[qi] == '=' && partBodyStart[qi+1] == '\n')
                    softBreaks++;
                else if (partBodyStart[qi] == '=' &&
                         isxdigit((unsigned char)partBodyStart[qi+1]) &&
                         isxdigit((unsigned char)partBodyStart[qi+2]))
                    qpHits++;
            }
            if (qpHits > 5 || softBreaks > 2) {
                strcpy(partCteBuf, "quoted-printable");
                partCTE = partCteBuf;
            }
            else if (looks_like_base64(partBodyStart, chkLen)) {
                strcpy(partCteBuf, "base64");
                partCTE = partCteBuf;
            }
        }

        DECODE_PART2(partBodyStart, partBodyLen, partCTE, partCTCopy, outBuf, *outLen);
        free(partData);
        return *outLen > 0 ? 0 : -1;
    }

    // 2c. 根据 BODYSTRUCTURE 结果拉取文本 part
    if (sections.isMultipart)
    {
        // multipart 邮件：按节号分别拉取 text/plain 和 text/html
        if (sections.plain[0])
        {
            if (fetch_and_decode_part(sections.plain, plainBuf, &plainLen, "text/plain") == 0)
                Cache_Log("FetchBody: fetched text/plain part [%s], len=%d", sections.plain, plainLen);
        }
        if (sections.html[0])
        {
            if (fetch_and_decode_part(sections.html, htmlBuf, &htmlLen, "text/html") == 0)
                Cache_Log("FetchBody: fetched text/html part [%s], len=%d", sections.html, htmlLen);
        }
        // 如果 BODYSTRUCTURE 解析没找到文本 part，回退拉取 BODY[1]
        if (plainLen == 0 && htmlLen == 0)
        {
            Cache_Log("FetchBody: no text parts found in BODYSTRUCTURE, trying BODY[1]");
            if (fetch_and_decode_part("1", plainBuf, &plainLen, "text/plain") != 0)
                fetch_and_decode_part("1", htmlBuf, &htmlLen, "text/html");
        }
    }
    else
    {
        // 单 part 邮件（text/plain 或 text/html）：直接拉取 BODY[]（不含图片，体积小）
        char* singleData = NULL;
        size_t singleLen = 0;
        char singleCmd[128];
        snprintf(singleCmd, sizeof(singleCmd), "UID FETCH %lu (BODY[])",
                 (unsigned long)mail->uid);
        int rcSingle = imap_send_recv_literal(&ic, singleCmd, &singleData, &singleLen);
        if (rcSingle == 0 && singleData && singleLen > 0)
        {
            // 分离头部和正文
            const char* sHdrEnd = strstr(singleData, "\r\n\r\n");
            int sSep = 4;
            if (!sHdrEnd) { sHdrEnd = strstr(singleData, "\n\n"); sSep = 2; }

            const char* sCT = NULL;
            const char* sCTE = NULL;
            const char* sBodyStart = singleData;
            int sBodyLen = (int)singleLen;

            if (sHdrEnd) {
                int sHdrLen = (int)(sHdrEnd - singleData);
                char sHdr[2048] = {0};
                if (sHdrLen > 2047) sHdrLen = 2047;
                memcpy(sHdr, singleData, sHdrLen);
                sCT = find_header(sHdr, "Content-Type", NULL);
                sCTE = find_header(sHdr, "Content-Transfer-Encoding", NULL);
                sBodyStart = sHdrEnd + sSep;
                sBodyLen = (int)(singleLen - (size_t)(sBodyStart - singleData));
                while (sBodyLen > 0 && (sBodyStart[sBodyLen-1] == '\n' ||
                       sBodyStart[sBodyLen-1] == '\r'))
                    sBodyLen--;
            }

            char sCTCopy[512] = {0};
            if (sCT) strncpy(sCTCopy, sCT, sizeof(sCTCopy) - 1);
            else strcpy(sCTCopy, "text/plain");

            char sCteBuf[64] = {0};
            if (!sCTE && sBodyLen > 10) {
                int qpHits = 0, softBreaks = 0;
                int chkLen = sBodyLen > 4096 ? 4096 : sBodyLen;
                for (int qi = 0; qi < chkLen - 2; qi++) {
                    if (sBodyStart[qi] == '=' && sBodyStart[qi+1] == '\r' && sBodyStart[qi+2] == '\n')
                        softBreaks++;
                    else if (sBodyStart[qi] == '=' && sBodyStart[qi+1] == '\n')
                        softBreaks++;
                    else if (sBodyStart[qi] == '=' &&
                             isxdigit((unsigned char)sBodyStart[qi+1]) &&
                             isxdigit((unsigned char)sBodyStart[qi+2]))
                        qpHits++;
                }
                if (qpHits > 5 || softBreaks > 2) {
                    strcpy(sCteBuf, "quoted-printable");
                    sCTE = sCteBuf;
                }
                else if (looks_like_base64(sBodyStart, chkLen)) {
                    strcpy(sCteBuf, "base64");
                    sCTE = sCteBuf;
                }
            }

            if (sCT && stristr(sCT, "text/html"))
                DECODE_PART2(sBodyStart, sBodyLen, sCTE, sCTCopy, htmlBuf, htmlLen);
            else if ((sCT && stristr(sCT, "multipart/")) ||
                     (sBodyLen > 4 && sBodyStart[0] == '-' && sBodyStart[1] == '-'))
            {
                // BODYSTRUCTURE误判为单part但实际是multipart，用parse_raw_email解析
                parse_raw_email(sBodyStart, sBodyLen,
                                plainBuf, &plainLen,
                                htmlBuf, &htmlLen, BODY_BUF_SIZE);
            }
            else
                DECODE_PART2(sBodyStart, sBodyLen, sCTE, sCTCopy, plainBuf, plainLen);
        }
        if (singleData) free(singleData);
    }

    imap_disconnect(&ic);

    // 2d. 如果都拉取失败，再尝试 BODY[] 作为最后兜底（仅限小邮件）
    if (plainLen == 0 && htmlLen == 0)
    {
        Cache_Log("FetchBody: all part fetches failed, trying BODY[] as fallback");
        CURL* curl = curl_easy_init();
        if (curl)
        {
            struct WriteBuf buf;
            buf_init(&buf);
            char url[512];
            snprintf(url, sizeof(url),
                     "imaps://%s:%d/INBOX;UID=%lu",
                     acc->imapServer, acc->imapPort,
                     (unsigned long)mail->uid);
            setup_curl_common(curl, url, acc->email, acc->password, &buf);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            CURLcode res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            if (res == CURLE_OK && buf.data && buf.size > 0)
            {
                // 使用完整MIME解析：分离头部、multipart分割、QP/base64解码、charset转换
                int rawLen = buf.size > BODY_BUF_SIZE-1 ? BODY_BUF_SIZE-1 : (int)buf.size;
                parse_raw_email(buf.data, rawLen,
                                plainBuf, &plainLen,
                                htmlBuf, &htmlLen, BODY_BUF_SIZE);
                Cache_Log("FetchBody fallback: plain=%d, html=%d", plainLen, htmlLen);
            }
            buf_free(&buf);
        }
    }

    // 编码检测：如果plain包含高字节且不是有效UTF-8，尝试GBK转UTF-8
    if (plainLen > 0)
    {
        bool hasHighBytes = false;
        bool validUtf8 = true;
        int i = 0;
        while (i < plainLen)
        {
            unsigned char c = (unsigned char)plainBuf[i];
            if (c >= 0x80)
            {
                hasHighBytes = true;
                // 检查UTF-8多字节序列
                int expected = 0;
                if ((c & 0xE0) == 0xC0) expected = 1;
                else if ((c & 0xF0) == 0xE0) expected = 2;
                else if ((c & 0xF8) == 0xF0) expected = 3;
                else { validUtf8 = false; break; }

                for (int j = 1; j <= expected; j++)
                {
                    if (i + j >= plainLen || ((unsigned char)plainBuf[i+j] & 0xC0) != 0x80)
                    { validUtf8 = false; break; }
                }
                if (!validUtf8) break;
                i += expected + 1;
            }
            else
            {
                i++;
            }
        }

        if (hasHighBytes && !validUtf8)
        {
            Cache_Log("FetchBody: plain not valid UTF-8, trying GBK conversion");
            char* converted = (char*)malloc(BODY_BUF_SIZE);
            if (converted)
            {
                int cl = convert_to_utf8("GBK", plainBuf, plainLen, converted, BODY_BUF_SIZE);
                if (cl > 0)
                {
                    memcpy(plainBuf, converted, cl);
                    plainBuf[cl] = 0;
                    plainLen = cl;
                    Cache_Log("FetchBody: GBK conversion OK, len=%d", cl);
                }
                else
                {
                    // 尝试GB2312
                    cl = convert_to_utf8("GB2312", plainBuf, plainLen, converted, BODY_BUF_SIZE);
                    if (cl > 0)
                    {
                        memcpy(plainBuf, converted, cl);
                        plainBuf[cl] = 0;
                        plainLen = cl;
                        Cache_Log("FetchBody: GB2312 conversion OK, len=%d", cl);
                    }
                }
                free(converted);
            }
        }
    }

    // 同样检测HTML编码
    if (htmlLen > 0)
    {
        bool hasHighBytes = false;
        bool validUtf8 = true;
        int i = 0;
        while (i < htmlLen)
        {
            unsigned char c = (unsigned char)htmlBuf[i];
            if (c >= 0x80)
            {
                hasHighBytes = true;
                int expected = 0;
                if ((c & 0xE0) == 0xC0) expected = 1;
                else if ((c & 0xF0) == 0xE0) expected = 2;
                else if ((c & 0xF8) == 0xF0) expected = 3;
                else { validUtf8 = false; break; }
                for (int j = 1; j <= expected; j++)
                {
                    if (i + j >= htmlLen || ((unsigned char)htmlBuf[i+j] & 0xC0) != 0x80)
                    { validUtf8 = false; break; }
                }
                if (!validUtf8) break;
                i += expected + 1;
            }
            else i++;
        }
        if (hasHighBytes && !validUtf8)
        {
            char* converted = (char*)malloc(BODY_BUF_SIZE);
            if (converted)
            {
                int cl = convert_to_utf8("GBK", htmlBuf, htmlLen, converted, BODY_BUF_SIZE);
                if (cl <= 0)
                    cl = convert_to_utf8("GB2312", htmlBuf, htmlLen, converted, BODY_BUF_SIZE);
                if (cl > 0)
                {
                    memcpy(htmlBuf, converted, cl);
                    htmlBuf[cl] = 0;
                    htmlLen = cl;
                }
                free(converted);
            }
        }
    }

    Cache_Log("FetchBody parsed: plainLen=%d, htmlLen=%d", plainLen, htmlLen);

    // 安全网：如果plainBuf中包含MIME boundary或Content-Type头，说明未正确解析，重新解析
    if (plainLen > 0)
    {
        bool needsReparse = false;
        // 检查是否包含MIME boundary行
        const char* bndCheck = plainBuf;
        while (bndCheck && *bndCheck)
        {
            if (bndCheck[0] == '-' && bndCheck[1] == '-' &&
                (bndCheck == plainBuf || bndCheck[-1] == '\n'))
            {
                needsReparse = true;
                break;
            }
            bndCheck = strchr(bndCheck, '\n');
            if (bndCheck) bndCheck++;
        }
        // 检查是否包含Content-Type头
        if (!needsReparse && stristr(plainBuf, "Content-Type:"))
            needsReparse = true;
        // 检查是否包含Content-Transfer-Encoding头
        if (!needsReparse && stristr(plainBuf, "Content-Transfer-Encoding:"))
            needsReparse = true;

        if (needsReparse)
        {
            Cache_Log("FetchBody: plainBuf contains raw MIME, re-parsing");
            int rePlain = 0, reHtml = 0;
            char* rePlainBuf = (char*)malloc(BODY_BUF_SIZE);
            char* reHtmlBuf = (char*)malloc(BODY_BUF_SIZE);
            if (rePlainBuf && reHtmlBuf)
            {
                parse_raw_email(plainBuf, plainLen,
                                rePlainBuf, &rePlain,
                                reHtmlBuf, &reHtml, BODY_BUF_SIZE);
                if (rePlain > 0)
                {
                    memcpy(plainBuf, rePlainBuf, rePlain + 1);
                    plainLen = rePlain;
                }
                if (reHtml > 0 && htmlLen == 0)
                {
                    memcpy(htmlBuf, reHtmlBuf, reHtml + 1);
                    htmlLen = reHtml;
                }
                Cache_Log("FetchBody re-parse: plainLen=%d, htmlLen=%d", plainLen, htmlLen);
            }
            if (rePlainBuf) free(rePlainBuf);
            if (reHtmlBuf) free(reHtmlBuf);
        }
    }

    // 如果有HTML但没有plain，从HTML生成纯文本
    if (htmlLen > 0 && plainLen == 0)
    {
        plainLen = html_to_text(htmlBuf, htmlLen, plainBuf, BODY_BUF_SIZE);
    }

    // 保存完整正文长度用于缓存文件
    int fullPlainLen = plainLen;
    int fullHtmlLen = htmlLen;

    // 如果plain和html都为空，说明拉取失败，不缓存错误内容
    if (fullPlainLen == 0 && fullHtmlLen == 0)
    {
        Cache_Log("FetchBody: both plain and html empty, fetch failed");
        free(plainBuf);
        free(htmlBuf);
        return false;
    }

    // 复制plain到mail->body（内存中只保留前1023字节用于预览）
    if (plainLen > (int)sizeof(mail->body) - 1)
        plainLen = sizeof(mail->body) - 1;
    if (plainLen > 0)
    {
        memcpy(mail->body, plainBuf, plainLen);
        mail->body[plainLen] = 0;
    }
    else
    {
        // HTML有内容但plain为空（HTML转plain后仍为空的极端情况）
        strcpy(mail->body, "(无文本内容)");
        fullPlainLen = (int)strlen(mail->body);
    }
    mail->bodyLoaded = true;
    mail->unread = false;

    // 保存到本地缓存（meta.dat + body_plain.txt + body_html.html）
    if (mail->uid > 0)
    {
        BodyMeta bmeta;
        memset(&bmeta, 0, sizeof(bmeta));
        bmeta.magic = BODY_META_MAGIC;
        bmeta.uid = mail->uid;
        bmeta.cache_time = (u64)time(NULL);
        bmeta.plain_size = fullPlainLen;
        bmeta.html_size = fullHtmlLen;
        strncpy(bmeta.charset, "UTF-8", sizeof(bmeta.charset) - 1);
        bmeta.image_count = 0;

        int saveRet = Cache_SaveBodyEx(acc->email, mail->uid, &bmeta,
                         fullPlainLen > 0 ? plainBuf : NULL,
                         fullHtmlLen > 0 ? htmlBuf : NULL);
        Cache_Log("FetchBody Cache_SaveBodyEx ret=%d, plainSize=%d, htmlSize=%d",
                  saveRet, fullPlainLen, fullHtmlLen);

        // 更新hdr中的bodyCached标记
        CacheEntry ce;
        memset(&ce, 0, sizeof(ce));
        ce.imapUid = mail->uid;
        ce.imapSeq = mail->imapSeq;
        strncpy(ce.sender, mail->sender, sizeof(ce.sender) - 1);
        strncpy(ce.fromAddr, mail->fromAddr, sizeof(ce.fromAddr) - 1);
        strncpy(ce.subject, mail->subject, sizeof(ce.subject) - 1);
        strncpy(ce.contentType, mail->contentType, sizeof(ce.contentType) - 1);
        strncpy(ce.preview, mail->preview, sizeof(ce.preview) - 1);
        ce.year = mail->year; ce.month = mail->month; ce.day = mail->day;
        ce.hour = mail->hour; ce.minute = mail->minute;
        ce.timestamp = mail->timestamp;
        strncpy(ce.rawDate, mail->rawDate, sizeof(ce.rawDate) - 1);
        ce.unread = mail->unread;
        ce.hasAttachment = mail->hasAttachment;
        ce.bodyCached = 1;
        Cache_SaveHeader(acc->email, &ce);
    }

    free(plainBuf);
    free(htmlBuf);
    return true;
}

// ========== SMTP发送 ==========

struct UploadStatus {
    const char* data;
    size_t size;
    size_t sent;
};

static size_t read_callback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    struct UploadStatus* st = (struct UploadStatus*)userdata;
    size_t available = st->size - st->sent;
    size_t toSend = size * nitems;
    if (toSend > available) toSend = available;
    if (toSend > 0)
    {
        memcpy(buffer, st->data + st->sent, toSend);
        st->sent += toSend;
    }
    return toSend;
}

bool Network_SendMail(int accountIndex, const char* to,
                      const char* subject, const char* body)
{
    if (accountIndex < 0 || accountIndex >= MAX_ACCOUNTS ||
        !g_app.accounts[accountIndex].added)
        return false;

    Account* acc = &g_app.accounts[accountIndex];

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    // 构建RFC 822邮件
    char mailData[2048];
    int len = snprintf(mailData, sizeof(mailData),
        "From: %s\r\n"
        "To: %s\r\n"
        "Subject: %s\r\n"
        "\r\n"
        "%s\r\n",
        acc->email, to, subject, body);

    struct UploadStatus upload;
    upload.data = mailData;
    upload.size = len;
    upload.sent = 0;

    struct WriteBuf buf;
    buf_init(&buf);

    char url[256];
    snprintf(url, sizeof(url), "smtps://%s:%d", acc->smtpServer, acc->smtpPort);
    setup_curl_common(curl, url, acc->email, acc->password, &buf);

    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, acc->email);
    struct curl_slist* recipients = NULL;
    recipients = curl_slist_append(recipients, to);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    buf_free(&buf);
    memset(mailData, 0, sizeof(mailData));

    if (res == CURLE_OK)
    {
        // 保存到已发送
        if (g_app.sentCount < MAX_EMAILS)
        {
            Email* sent = &g_app.sentMails[g_app.sentCount];
            sent->id = g_app.sentCount + 1;
            sent->accountIndex = accountIndex;
            strncpy(sent->sender, to, sizeof(sent->sender) - 1);
            strncpy(sent->subject, subject, sizeof(sent->subject) - 1);
            strncpy(sent->body, body, sizeof(sent->body) - 1);
            strcpy(sent->date, "刚刚");
            sent->unread = false;
            sent->hasAttachment = false;
            g_app.sentCount++;
        }
        return true;
    }
    return false;
}

// 旧接口兼容
void Network_RefreshMails(void)
{
    // 已由Network_FetchMails替代
}
