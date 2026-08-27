// email.h - 邮件数据
#ifndef EMAIL_H
#define EMAIL_H

#include "common.h"

void Email_Init(void);
void Email_AddSampleData(void);
void Email_MarkAsRead(int index);
int Email_GetFilteredCount(int filter);

// 格式化邮件日期显示
void Email_FormatDate(int year, int month, int day, int hour, int minute,
                      char* out, int outSize);

#endif
