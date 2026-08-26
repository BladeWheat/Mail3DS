#ifndef CACHE_H
#define CACHE_H

#include "common.h"

// 邮件基础信息（内存中使用，序列化为JSON存储在 header_cache/<UID>.hdr）
typedef struct {
    u32 imapUid;          // IMAP UID（唯一标识，同时作为文件名）
    int imapSeq;          // IMAP序号（用于FETCH，每次同步可能变化）
    char sender[128];     // 发件人昵称
    char fromAddr[128];   // 发件人邮箱
    char subject[256];    // 主题
    char contentType[256]; // Content-Type头部
    char preview[128];    // 正文预览
    char date[32];        // 旧字段，新代码不再使用
    int year, month, day, hour, minute; // 本地时间
    long long timestamp;  // Unix时间戳（秒），排序唯一依据
    char rawDate[64];     // 原始Date头文本（调试用）
    u8 unread;            // 未读标记
    u8 hasAttachment;     // 有附件
    u8 bodyCached;        // 正文已缓存
} CacheEntry;

// ========== 正文缓存结构体 ==========

#define BODY_META_MAGIC 0x424F4459  // "BODY"
#define MAX_CID_MAPS 16

// CID图片映射表条目
typedef struct {
    char cid_str[128];   // CID标识字符串
    int img_index;       // 图片序号（对应images/img_N.png）
    int format;          // 0=PNG, 1=JPG
} CidMapEntry;

// 正文元数据（meta.dat，二进制结构体）
typedef struct {
    u32 magic;           // 魔数 0x424F4459
    u32 uid;             // 邮件UID
    u64 cache_time;      // 缓存时间戳（Unix时间秒）
    int plain_size;      // body_plain.txt 大小（字节）
    int html_size;       // body_html.html 大小（字节，0表示无HTML）
    char charset[32];    // 原始字符编码
    int image_count;     // 内嵌图片数量
    CidMapEntry cid_maps[MAX_CID_MAPS]; // CID映射表
} BodyMeta;

// 初始化账户缓存目录（header_cache/ 和 body_cache/）
int Cache_InitAccount(const char* email);

// 获取账户缓存目录路径
void Cache_GetAccountPath(const char* email, char* out, int outSize);

// 保存邮件基础信息到 header_cache/<UID>.hdr（JSON格式）
int Cache_SaveHeader(const char* email, const CacheEntry* entry);

// 从 header_cache/<UID>.hdr 读取邮件基础信息，返回0成功
int Cache_LoadHeader(const char* email, u32 uid, CacheEntry* out);

// 保存正文到 body_cache/<UID>/ 目录（meta.dat + body_plain.txt）
int Cache_SaveBody(const char* email, u32 uid, const char* body, int bodyLen);

// 保存正文（含完整元数据，支持HTML和图片映射）
int Cache_SaveBodyEx(const char* email, u32 uid, const BodyMeta* meta,
                     const char* plainBody, const char* htmlBody);

// 读取正文元数据（meta.dat），返回0成功，-1失败/缓存失效
int Cache_LoadBodyMeta(const char* email, u32 uid, BodyMeta* outMeta);

// 读取纯文本正文，返回malloc分配的字符串（调用者释放），NULL表示无缓存
char* Cache_LoadBody(const char* email, u32 uid);

// 只读取纯文本正文（不读取HTML），返回malloc分配的字符串
char* Cache_LoadBodyPlain(const char* email, u32 uid);

// 计算所有账户缓存文件总大小（MB），结果缓存，不每次扫描SD卡
float Cache_GetTotalSizeMB(void);

// 标记缓存大小需要重新计算（进入设置页/清除缓存后调用）
void Cache_RefreshTotalSize(void);

// 调试日志（写入cache_log.txt）
void Cache_Log(const char* fmt, ...);

// 清除所有账户的缓存文件（header_cache和body_cache内容）
void Cache_ClearAll(void);

// 保存账户UID列表到本地文件（uid_list.dat）
int Cache_SaveUidList(const char* email, const u32* uids, int count);

// 从本地文件加载账户UID列表，返回UID数量，-1表示无缓存
// uids/outCap：调用者提供的缓冲区和容量
int Cache_LoadUidList(const char* email, u32* uids, int outCap);

#endif
