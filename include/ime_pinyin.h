// ime_pinyin.h - 拼音输入法头文件
#ifndef IME_PINYIN_H
#define IME_PINYIN_H

#ifdef __cplusplus
extern "C" {
#endif

// 拼音缓冲区最大长度
#define IME_BUFFER_MAX 64
// 最大候选词数量
#define IME_MAX_CANDIDATES 32

// 拼音输入法句柄（前向声明）
typedef struct PinyinIme PinyinIme;

// 创建/销毁
PinyinIme* ime_create(const char* path);
void ime_destroy(PinyinIme* ime);

// 输入操作
void ime_input(PinyinIme* ime, char letter);
void ime_backspace(PinyinIme* ime);
void ime_clear(PinyinIme* ime);

// 状态查询
const char* ime_buffer(const PinyinIme* ime);
int ime_active(const PinyinIme* ime);
int ime_matched_length(const PinyinIme* ime);
int ime_candidate_count(const PinyinIme* ime);
const char* ime_candidate(const PinyinIme* ime, int index);

// 提交候选词（返回UTF-8汉字，并清空拼音）
const char* ime_commit(PinyinIme* ime, int index);

#ifdef __cplusplus
}
#endif

#endif
