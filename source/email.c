// email.c - 邮件数据
#include "email.h"
#include <time.h>
#include <stdio.h>

// 根据邮件时间格式化显示日期
// 当天：HH:MM（24小时制，两位）
// 同年不同天：M/D
// 不同年：M/D/YYYY
void Email_FormatDate(int year, int month, int day, int hour, int minute,
                      char* out, int outSize)
{
    if (year <= 0 || month <= 0 || day <= 0)
    {
        snprintf(out, outSize, "未知时间");
        return;
    }

    // “今天”按北京时间（UTC+8）判断，与 hdr 中存储的年月日保持一致
    time_t now = time(NULL) + 8 * 3600;
    struct tm* lt = gmtime(&now);
    int curYear = lt ? lt->tm_year + 1900 : 0;
    int curMonth = lt ? lt->tm_mon + 1 : 0;
    int curDay = lt ? lt->tm_mday : 0;

    if (year == curYear && month == curMonth && day == curDay)
        snprintf(out, outSize, "%02d:%02d", hour, minute);
    else if (year == curYear)
        snprintf(out, outSize, "%d/%d", month, day);
    else
        snprintf(out, outSize, "%d/%d/%d", month, day, year);
}

void Email_Init(void)
{
    g_app.emailCount = 0;
    g_app.selectedEmail = -1;
    g_app.selectedFilter = -1;
    g_app.sentCount = 0;
}

void Email_AddSampleData(void)
{
    // 示例邮件数据（30封，分布在3个账户）
    static const struct {
        int account;
        const char* sender;
        const char* subject;
        const char* body;
        const char* date;
        bool unread;
        bool hasAtt;
    } samples[] = {
        {0, "GitHub", "仓库安全提醒",
         "我们在您的一个仓库中发现了潜在的安全问题。\n\n请检查您的安全设置，如果尚未启用双重身份验证，请尽快启用。\n\n—— GitHub 安全团队",
         "10:30", true, false},
        {0, "任天堂", "您的 3DS eShop 收据",
         "感谢您的购买！\n\n您已成功购买：\n- 精灵宝可梦 究极之日\n\n游戏现已可以下载。\n\n—— 任天堂 eShop",
         "昨天", true, true},
        {0, "小张", "周末一起玩吗？",
         "嘿！\n\n这周六有空吗？我买了新的马里奥赛车 DLC，咱们可以联机一起玩。\n\n等你消息！",
         "昨天", false, false},
        {0, "科技周刊", "本周科技动态 - 3DS 自制软件",
         "本周热点：\n\n1. Citro2D 发布新版本\n2. 自制软件浏览器大更新\n3. 3DS 联机服务社区复兴\n\n订阅获取更多每周资讯！",
         "周一", false, false},
        {0, "开发团队", "自定义键盘完成！",
         "恭喜你完成了自定义软键盘！\n\n现在你可以输入带 @ 和 . 的邮箱地址了，完全没有问题。\n\n键盘支持：\n- 小写字母\n- 大写字母\n- 符号和数字\n\n继续加油！",
         "周日", true, false},
        {0, "QQ邮箱团队", "登录提醒",
         "您的QQ邮箱于今日 09:15 在新设备上登录。\n\n登录地点：北京\n设备：3DS Mail Client\n\n如果这不是您本人操作，请及时修改密码。",
         "09:15", true, false},
        {0, "小李", "聚会照片",
         "昨天聚会的照片我整理好了，放在附件里了！\n\n大家玩得很开心，下次再约～",
         "周六", false, true},
        {0, "Steam", "您的愿望单游戏打折了",
         "您愿望单中的以下游戏正在促销：\n\n- 塞尔达传说：王国之泪 -30%\n- 星露谷物语 -50%\n\n促销截止到下周一。",
         "周五", false, false},
        {0, "Bilibili", "您关注的UP主更新了",
         "您关注的UP主「3DS改造大师」发布了新视频：\n\n《3DS自制软件开发教程 #5 - 网络通信篇》\n\n快去看看吧！",
         "周四", true, false},
        {0, "支付宝", "账单提醒",
         "您本月账单已生成：\n\n消费总额：¥328.50\n- 餐饮：¥156.00\n- 购物：¥128.50\n- 其他：¥44.00\n\n还款日为每月10日。",
         "周三", false, false},
        {1, "163邮箱", "邮箱容量提醒",
         "您的163邮箱容量已使用 65%。\n\n已用：3.25GB / 5GB\n\n建议及时清理不需要的邮件。",
         "08:00", true, false},
        {1, "老板", "项目进度汇报",
         "小王，\n\n下周一之前把项目进度报告发给我，重点说明：\n1. 本周完成的工作\n2. 遇到的问题\n3. 下周计划\n\n辛苦了。",
         "昨天", true, false},
        {1, "人事部", "工资条",
         "您2026年7月工资条已生成，请查收附件。\n\n如有疑问请在3个工作日内联系人事部。",
         "8/5", false, true},
        {1, "同事-老王", "会议纪要",
         "今天下午会议纪要如下：\n\n1. 新项目下周启动\n2. 你负责前端模块\n3. 周五前提交技术方案\n\n详见附件文档。",
         "8/4", false, true},
        {1, "快递100", "您的包裹已签收",
         "您的快递已签收：\n\n运单号：SF1234567890\n签收人：本人\n签收时间：今日 14:30\n\n感谢使用顺丰速运。",
         "8/3", false, false},
        {1, "网易云音乐", "每日推荐",
         "根据您的口味，今日为您推荐：\n\n1. 《晴天》- 周杰伦\n2. 《光年之外》- 邓紫棋\n3. 《起风了》- 买辣椒也用券\n\n打开APP发现更多好音乐。",
         "8/2", false, false},
        {1, "京东", "订单发货通知",
         "您的订单已发货：\n\n商品：SanDisk 32GB microSD卡\n订单号：JD2026080112345\n预计送达：8月3日\n\n快递员：张师傅 138****5678",
         "8/1", true, false},
        {1, "美团外卖", "订单完成",
         "您的外卖订单已完成：\n\n商家：黄焖鸡米饭\n商品：黄焖鸡+米饭\n金额：¥22.00\n\n满意的话给个五星好评吧～",
         "7/31", false, false},
        {1, "中国移动", "话费余额提醒",
         "尊敬的客户，您的话费余额不足20元：\n\n当前余额：¥12.50\n套餐：58元畅享套餐\n\n请及时充值以免停机。",
         "7/30", false, false},
        {1, "豆瓣", "电影推荐",
         "本周新片推荐：\n\n《星际穿越2》豆瓣评分 9.2\n《夏日大作战》重映 豆瓣评分 8.9\n\n点击查看附近影院排片。",
         "7/29", true, false},
        {2, "Gmail Team", "Security alert",
         "A new sign-in to your Google Account:\n\nDevice: Nintendo 3DS\nLocation: Beijing, China\nTime: Aug 8, 2026, 10:20 AM\n\nIf this was you, no action is needed.",
         "10:20", true, false},
        {2, "YouTube", "New subscriber",
         "Congratulations! Your channel gained 100 new subscribers this week.\n\nTotal subscribers: 1,247\n\nKeep creating great content!",
         "昨天", false, false},
        {2, "Google Play", "App update available",
         "Updates available for 3 apps:\n\n- Chrome (126.0.6478.71)\n- Gmail (2026.08.04)\n- YouTube (19.05.36)\n\nTap to update.",
         "周一", false, false},
        {2, "Amazon", "Your order has shipped",
         "Your order has shipped:\n\nItem: USB-C Charging Cable\nOrder: #112-3456789\nEstimated delivery: Aug 10\n\nTrack your package in the Amazon app.",
         "周日", false, false},
        {2, "GitHub", "Pull request merged",
         "Your pull request #42 has been merged:\n\nRepository: 3ds-mail-client\nTitle: Add custom soft keyboard\nMerged by: @maintainer\n\nGreat work!",
         "周六", true, false},
        {2, "Stack Overflow", "New answer to your question",
         "Your question \"How to render CJK text with citro2d?\" has a new answer:\n\n\"You need to use BCFNT fonts generated with mkbcfnt...\"\n\nView the full answer on Stack Overflow.",
         "周五", false, false},
        {2, "Twitter", "Verify your email",
         "Please verify your email address to complete your Twitter account setup.\n\nClick the link below to verify:\nverify.twitter.com/abc123",
         "周四", false, false},
        {2, "Dropbox", "Storage almost full",
         "Your Dropbox is 90% full:\n\n2.7 GB of 3 GB used\n\nUpgrade to Dropbox Plus for 2 TB of storage.",
         "周三", true, false},
        {2, "Spotify", "Your weekly mix is ready",
         "Your Discover Weekly playlist is ready!\n\nFeaturing artists like:\n- 米津玄師\n- YOASOBI\n- Official髭男dism\n\nListen now on Spotify.",
         "周二", false, false},
        {2, "Reddit", "Top posts from r/3dshacks",
         "Today's top posts:\n\n1. [Release] New FTP client for 3DS\n2. [Tutorial] Installing custom themes\n3. [Discussion] Best homebrew apps 2026\n\nJoin the discussion!",
         "8/6", false, false},
    };

    int count = sizeof(samples) / sizeof(samples[0]);
    for (int i = 0; i < count && i < MAX_EMAILS; i++)
    {
        Email* e = &g_app.emails[i];
        e->id = i + 1;
        e->accountIndex = samples[i].account;
        strncpy(e->sender, samples[i].sender, sizeof(e->sender) - 1);
        strncpy(e->subject, samples[i].subject, sizeof(e->subject) - 1);
        strncpy(e->body, samples[i].body, sizeof(e->body) - 1);
        strncpy(e->date, samples[i].date, sizeof(e->date) - 1);
        e->unread = samples[i].unread;
        e->hasAttachment = samples[i].hasAtt;
    }

    g_app.emailCount = count;
}

void Email_MarkAsRead(int index)
{
    if (index >= 0 && index < g_app.emailCount)
    {
        g_app.emails[index].unread = false;
    }
}

int Email_GetFilteredCount(int filter)
{
    int count = 0;
    for (int i = 0; i < g_app.emailCount; i++)
    {
        if (filter < 0 || g_app.emails[i].accountIndex == filter)
            count++;
    }
    return count;
}
