// cache.c - 本地缓存管理
// header_cache/<UID>.hdr  — 邮件基础信息（JSON格式）
// body_cache/<UID>.txt    — 邮件正文（纯文本）
#include "cache.h"
#include "cJSON.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ========== 辅助函数 ==========

static int mkdir_recursive(const char* path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777);
}

void Cache_Log(const char* fmt, ...)
{
    // 发布版本禁用SD卡日志写入以提升速度
    (void)fmt;
}

static char* read_file_all(const char* path, long* outLen)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 4 * 1024 * 1024) { fclose(fp); return NULL; }
    char* buf = (char*)malloc(fsize + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, fsize, fp);
    fclose(fp);
    buf[rd] = 0;
    if (outLen) *outLen = (long)rd;
    return buf;
}

static int write_file_atomic(const char* path, const char* data, int len)
{
    char tmpPath[512];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    FILE* fp = fopen(tmpPath, "wb");
    if (!fp) return -1;
    if (len > 0) fwrite(data, 1, len, fp);
    fflush(fp);
    fclose(fp);
    remove(path);
    if (rename(tmpPath, path) != 0)
    {
        rename(tmpPath, path);
        return -1;
    }
    return 0;
}

// ========== 路径 ==========

// account_map.ini 路径
static void get_map_path(char* out, int outSize)
{
    snprintf(out, outSize, "sdmc:/3ds/Mail3DS/account_map.ini");
}

// 从 account_map.ini 查找 email 对应的序号，找不到则分配新序号并写入
static int get_account_index(const char* email)
{
    char mapPath[256];
    get_map_path(mapPath, sizeof(mapPath));

    // 确保根目录存在
    mkdir_recursive("sdmc:/3ds/Mail3DS");

    // 读取现有映射
    FILE* fp = fopen(mapPath, "r");
    if (fp)
    {
        char line[512];
        int maxIdx = -1;
        while (fgets(line, sizeof(line), fp))
        {
            // 格式：account_N=email@example.com
            int idx;
            char addr[256];
            if (sscanf(line, "account_%d=%255s", &idx, addr) == 2)
            {
                if (strcmp(addr, email) == 0)
                {
                    fclose(fp);
                    return idx;
                }
                if (idx > maxIdx) maxIdx = idx;
            }
        }
        fclose(fp);

        // 没找到，分配新序号，追加写入
        int newIdx = maxIdx + 1;
        fp = fopen(mapPath, "a");
        if (fp)
        {
            // 检查文件末尾是否有换行符，没有则先补一个
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            if (fsize > 0)
            {
                fseek(fp, -1, SEEK_END);
                int lastCh = fgetc(fp);
                if (lastCh != '\n' && lastCh != '\r')
                    fputc('\n', fp);
            }
            fprintf(fp, "account_%d=%s\n", newIdx, email);
            fclose(fp);
        }
        return newIdx;
    }

    // 文件不存在，创建并写入第一个映射
    fp = fopen(mapPath, "w");
    if (fp)
    {
        fprintf(fp, "account_0=%s\n", email);
        fclose(fp);
    }
    return 0;
}

void Cache_GetAccountPath(const char* email, char* out, int outSize)
{
    int idx = get_account_index(email);
    snprintf(out, outSize, "sdmc:/3ds/Mail3DS/account_%d", idx);
}

static void get_header_path(const char* email, u32 uid, char* out, int outSize)
{
    char base[256];
    Cache_GetAccountPath(email, base, sizeof(base));
    snprintf(out, outSize, "%s/header_cache/%lu.hdr", base, (unsigned long)uid);
}

// 正文缓存目录：body_cache/<UID>/
static void get_body_dir(const char* email, u32 uid, char* out, int outSize)
{
    char base[256];
    Cache_GetAccountPath(email, base, sizeof(base));
    snprintf(out, outSize, "%s/body_cache/%lu", base, (unsigned long)uid);
}

static void get_meta_path(const char* email, u32 uid, char* out, int outSize)
{
    char dir[256];
    get_body_dir(email, uid, dir, sizeof(dir));
    snprintf(out, outSize, "%s/meta.dat", dir);
}

static void get_plain_path(const char* email, u32 uid, char* out, int outSize)
{
    char dir[256];
    get_body_dir(email, uid, dir, sizeof(dir));
    snprintf(out, outSize, "%s/body_plain.txt", dir);
}

static void get_html_path(const char* email, u32 uid, char* out, int outSize)
{
    char dir[256];
    get_body_dir(email, uid, dir, sizeof(dir));
    snprintf(out, outSize, "%s/body_html.html", dir);
}

// 前向声明
static void dir_clear_files(const char* path);

// ========== 初始化 ==========

int Cache_InitAccount(const char* email)
{
    char base[256];
    Cache_GetAccountPath(email, base, sizeof(base));
    Cache_Log("InitAccount: %s", base);

    mkdir_recursive(base);

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/header_cache", base);
    mkdir_recursive(dir);
    snprintf(dir, sizeof(dir), "%s/body_cache", base);
    mkdir_recursive(dir);

    Cache_Log("InitAccount: directories created OK");
    return 0;
}

// ========== 邮件基础信息（JSON） ==========

int Cache_SaveHeader(const char* email, const CacheEntry* entry)
{
    // 确保目录存在
    char base[256];
    Cache_GetAccountPath(email, base, sizeof(base));
    mkdir_recursive(base);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/header_cache", base);
    mkdir_recursive(dir);

    cJSON* root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "uid", (double)entry->imapUid);
    cJSON_AddNumberToObject(root, "seq", entry->imapSeq);
    cJSON_AddStringToObject(root, "sender", entry->sender);
    cJSON_AddStringToObject(root, "from", entry->fromAddr);
    cJSON_AddStringToObject(root, "subject", entry->subject);
    cJSON_AddStringToObject(root, "contentType", entry->contentType);
    cJSON_AddStringToObject(root, "preview", entry->preview);
    cJSON_AddStringToObject(root, "date", "");
    cJSON_AddNumberToObject(root, "year", entry->year);
    cJSON_AddNumberToObject(root, "month", entry->month);
    cJSON_AddNumberToObject(root, "day", entry->day);
    cJSON_AddNumberToObject(root, "hour", entry->hour);
    cJSON_AddNumberToObject(root, "minute", entry->minute);
    cJSON_AddNumberToObject(root, "timestamp", (double)entry->timestamp);
    cJSON_AddStringToObject(root, "raw_date", entry->rawDate);
    cJSON_AddBoolToObject(root, "unread", entry->unread ? 1 : 0);
    cJSON_AddBoolToObject(root, "hasAttachment", entry->hasAttachment ? 1 : 0);
    cJSON_AddBoolToObject(root, "bodyCached", entry->bodyCached ? 1 : 0);

    char* jsonStr = cJSON_Print(root);
    cJSON_Delete(root);
    if (!jsonStr) return -1;

    char path[512];
    get_header_path(email, entry->imapUid, path, sizeof(path));
    int ret = write_file_atomic(path, jsonStr, (int)strlen(jsonStr));
    free(jsonStr);

    if (ret == 0)
        Cache_Log("SaveHeader: OK uid=%lu", (unsigned long)entry->imapUid);
    else
        Cache_Log("SaveHeader: FAILED uid=%lu", (unsigned long)entry->imapUid);
    return ret;
}

int Cache_LoadHeader(const char* email, u32 uid, CacheEntry* out)
{
    char path[512];
    get_header_path(email, uid, path, sizeof(path));

    long len = 0;
    char* data = read_file_all(path, &len);
    if (!data) return -1;

    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) return -1;

    memset(out, 0, sizeof(*out));
    cJSON* j;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "uid")) && cJSON_IsNumber(j))
        out->imapUid = (u32)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "seq")) && cJSON_IsNumber(j))
        out->imapSeq = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "sender")) && cJSON_IsString(j))
        strncpy(out->sender, j->valuestring, sizeof(out->sender) - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "from")) && cJSON_IsString(j))
        strncpy(out->fromAddr, j->valuestring, sizeof(out->fromAddr) - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "subject")) && cJSON_IsString(j))
        strncpy(out->subject, j->valuestring, sizeof(out->subject) - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "contentType")) && cJSON_IsString(j))
        strncpy(out->contentType, j->valuestring, sizeof(out->contentType) - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "preview")) && cJSON_IsString(j))
        strncpy(out->preview, j->valuestring, sizeof(out->preview) - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "date")) && cJSON_IsString(j))
        strncpy(out->date, j->valuestring, sizeof(out->date) - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "year")) && cJSON_IsNumber(j))
        out->year = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "month")) && cJSON_IsNumber(j))
        out->month = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "day")) && cJSON_IsNumber(j))
        out->day = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "hour")) && cJSON_IsNumber(j))
        out->hour = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "minute")) && cJSON_IsNumber(j))
        out->minute = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "timestamp")) && cJSON_IsNumber(j))
        out->timestamp = (long long)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "raw_date")) && cJSON_IsString(j))
        strncpy(out->rawDate, j->valuestring, sizeof(out->rawDate) - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "unread")) && cJSON_IsBool(j))
        out->unread = cJSON_IsTrue(j) ? 1 : 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "hasAttachment")) && cJSON_IsBool(j))
        out->hasAttachment = cJSON_IsTrue(j) ? 1 : 0;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "bodyCached")) && cJSON_IsBool(j))
        out->bodyCached = cJSON_IsTrue(j) ? 1 : 0;

    cJSON_Delete(root);
    return 0;
}

// ========== 邮件正文（纯文本） ==========

int Cache_SaveBody(const char* email, u32 uid, const char* body, int bodyLen)
{
    // 构造简化版元数据
    BodyMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = BODY_META_MAGIC;
    meta.uid = uid;
    meta.cache_time = (u64)time(NULL);
    meta.plain_size = bodyLen;
    meta.html_size = 0;
    strncpy(meta.charset, "UTF-8", sizeof(meta.charset) - 1);
    meta.image_count = 0;

    return Cache_SaveBodyEx(email, uid, &meta, body, NULL);
}

int Cache_SaveBodyEx(const char* email, u32 uid, const BodyMeta* meta,
                     const char* plainBody, const char* htmlBody)
{
    if (!meta) return -1;

    // 创建 body_cache/<UID>/ 目录
    char dir[512];
    get_body_dir(email, uid, dir, sizeof(dir));
    mkdir_recursive(dir);

    // 写 meta.dat
    char metaPath[512];
    get_meta_path(email, uid, metaPath, sizeof(metaPath));
    int ret = write_file_atomic(metaPath, (const char*)meta, sizeof(BodyMeta));
    if (ret != 0)
    {
        Cache_Log("SaveBodyEx: meta.dat FAILED uid=%lu", (unsigned long)uid);
        return ret;
    }

    // 写 body_plain.txt
    if (plainBody && meta->plain_size > 0)
    {
        char plainPath[512];
        get_plain_path(email, uid, plainPath, sizeof(plainPath));
        ret = write_file_atomic(plainPath, plainBody, meta->plain_size);
        if (ret != 0)
        {
            Cache_Log("SaveBodyEx: body_plain.txt FAILED uid=%lu", (unsigned long)uid);
            return ret;
        }
    }

    // 写 body_html.html（如果有）
    if (htmlBody && meta->html_size > 0)
    {
        char htmlPath[512];
        get_html_path(email, uid, htmlPath, sizeof(htmlPath));
        ret = write_file_atomic(htmlPath, htmlBody, meta->html_size);
        if (ret != 0)
        {
            Cache_Log("SaveBodyEx: body_html.html FAILED uid=%lu", (unsigned long)uid);
            return ret;
        }
    }

    Cache_Log("SaveBodyEx: OK uid=%lu (plain=%d, html=%d, imgs=%d)",
              (unsigned long)uid, meta->plain_size, meta->html_size, meta->image_count);
    return 0;
}

int Cache_LoadBodyMeta(const char* email, u32 uid, BodyMeta* outMeta)
{
    if (!outMeta) return -1;

    char metaPath[512];
    get_meta_path(email, uid, metaPath, sizeof(metaPath));

    FILE* fp = fopen(metaPath, "rb");
    if (!fp) return -1;

    BodyMeta meta;
    size_t rd = fread(&meta, 1, sizeof(BodyMeta), fp);
    fclose(fp);

    if (rd != sizeof(BodyMeta) || meta.magic != BODY_META_MAGIC)
    {
        Cache_Log("LoadBodyMeta: invalid magic or size uid=%lu", (unsigned long)uid);
        // 缓存失效，删除目录
        char dir[512];
        get_body_dir(email, uid, dir, sizeof(dir));
        dir_clear_files(dir);
        rmdir(dir);
        return -1;
    }

    *outMeta = meta;
    return 0;
}

char* Cache_LoadBody(const char* email, u32 uid)
{
    // 先校验 meta.dat
    BodyMeta meta;
    if (Cache_LoadBodyMeta(email, uid, &meta) != 0)
        return NULL;

    // 优先读 HTML（阶段二使用），阶段一读 plain
    char path[512];
    if (meta.html_size > 0)
    {
        get_html_path(email, uid, path, sizeof(path));
        char* html = read_file_all(path, NULL);
        if (html) return html;
    }

    get_plain_path(email, uid, path, sizeof(path));
    return read_file_all(path, NULL);
}

// 只加载纯文本正文（不加载HTML）
char* Cache_LoadBodyPlain(const char* email, u32 uid)
{
    BodyMeta meta;
    if (Cache_LoadBodyMeta(email, uid, &meta) != 0)
        return NULL;
    char path[512];
    get_plain_path(email, uid, path, sizeof(path));
    return read_file_all(path, NULL);
}

// 递归计算目录下所有文件大小（字节）
static long long dir_size_recursive(const char* path)
{
    long long total = 0;
    DIR* d = opendir(path);
    if (!d) return 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
                total += dir_size_recursive(full);
            else
                total += st.st_size;
        }
    }
    closedir(d);
    return total;
}

// 递归删除目录下所有文件（保留目录本身）
static void dir_clear_files(const char* path)
{
    DIR* d = opendir(path);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
            {
                dir_clear_files(full);
                rmdir(full);
            }
            else
            {
                remove(full);
            }
        }
    }
    closedir(d);
}

static float s_cachedTotalSizeMB = -1.0f; // -1表示需要重新计算

void Cache_RefreshTotalSize(void)
{
    s_cachedTotalSizeMB = -1.0f;
}

float Cache_GetTotalSizeMB(void)
{
    if (s_cachedTotalSizeMB >= 0.0f)
        return s_cachedTotalSizeMB;

    long long total = 0;
    const char* root = "sdmc:/3ds/Mail3DS";
    DIR* d = opendir(root);
    if (!d)
    {
        Cache_Log("GetSize: opendir failed for %s", root);
        s_cachedTotalSizeMB = 0.0f;
        return 0.0f;
    }
    int fileCount = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", root, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
            {
                long long sub = dir_size_recursive(full);
                total += sub;
            }
            else
            {
                total += st.st_size;
                fileCount++;
            }
        }
    }
    closedir(d);
    s_cachedTotalSizeMB = (float)total / (1024.0f * 1024.0f);
    Cache_Log("GetSize: total=%lld bytes, rootFiles=%d", total, fileCount);
    return s_cachedTotalSizeMB;
}

void Cache_ClearAll(void)
{
    const char* root = "sdmc:/3ds/Mail3DS";
    DIR* d = opendir(root);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        // 只处理账户文件夹（account_开头）
        if (strncmp(ent->d_name, "account_", 8) != 0)
            continue;
        char acctDir[512];
        snprintf(acctDir, sizeof(acctDir), "%s/%s", root, ent->d_name);
        struct stat st;
        if (stat(acctDir, &st) == 0 && S_ISDIR(st.st_mode))
        {
            char sub[512];
            snprintf(sub, sizeof(sub), "%s/header_cache", acctDir);
            dir_clear_files(sub);
            snprintf(sub, sizeof(sub), "%s/body_cache", acctDir);
            dir_clear_files(sub);
        }
    }
    closedir(d);
    Cache_Log("Cache_ClearAll: done");
}

// 保存账户UID列表到本地文件
int Cache_SaveUidList(const char* email, const u32* uids, int count)
{
    if (!email || !uids || count <= 0) return -1;
    char base[256];
    Cache_GetAccountPath(email, base, sizeof(base));
    char path[512];
    snprintf(path, sizeof(path), "%s/uid_list.dat", base);

    // 确保目录存在
    mkdir(base, 0777);

    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    // 先写临时文件再重命名，防止写入中断损坏
    char tmpPath[512];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    fclose(f);
    f = fopen(tmpPath, "wb");
    if (!f) return -1;

    // 格式：4字节魔数 + 4字节数量 + 4字节*count
    // 魔数 "UID2" 表示按日期排序的UID列表（旧版"UID1"按UID排序，已废弃）
    u32 magic = 0x55494432; // "UID2"
    fwrite(&magic, 4, 1, f);
    fwrite(&count, 4, 1, f);
    fwrite(uids, 4, count, f);
    fclose(f);

    rename(tmpPath, path);
    Cache_Log("SaveUidList: saved %d UIDs for %s", count, email);
    return 0;
}

// 从本地文件加载账户UID列表
int Cache_LoadUidList(const char* email, u32* uids, int outCap)
{
    if (!email || !uids || outCap <= 0) return -1;
    char base[256];
    Cache_GetAccountPath(email, base, sizeof(base));
    char path[512];
    snprintf(path, sizeof(path), "%s/uid_list.dat", base);

    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    u32 magic = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x55494432)
    {
        fclose(f);
        return -1;
    }

    int count = 0;
    if (fread(&count, 4, 1, f) != 1 || count <= 0)
    {
        fclose(f);
        return -1;
    }

    if (count > outCap)
    {
        // 缓冲区不够，需要扩容
        fclose(f);
        return -1;
    }

    if (fread(uids, 4, count, f) != (size_t)count)
    {
        fclose(f);
        return -1;
    }

    fclose(f);
    Cache_Log("LoadUidList: loaded %d UIDs for %s", count, email);
    return count;
}
