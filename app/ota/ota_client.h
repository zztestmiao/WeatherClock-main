#ifndef __OTA_CLIENT_H__
#define __OTA_CLIENT_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * App 侧 OTA 客户端
 * ----------------------------------------------------------------------------
 * 设计原则：Bootloader 只管"选哪个槽启动"，所有下载/写 Flash 都在 App 里做。
 * 这样即使升级过程断电、下载到一半的残废镜像，也不会影响 Bootloader 本身。
 *
 * 用法（在 workqueue 任务里调用，阻塞整个升级过程）：
 *
 *   if (ota_upgrade_from_url("http://192.168.1.100/fw/app_ota.bin")) {
 *       // 内部已写元数据并 NVIC_SystemReset()
 *   }
 *
 * App 启动成功后再调用 ota_confirm_self() 把 pending 转成 active，
 * 否则下次上电 Bootloader 会认为这次启动"没成功"。
 */

/* 返回 true 表示升级成功并已重启（通常不会返回） */
bool ota_upgrade_from_url(const char *url);

/* 当前运行在哪个槽（0=A 1=B），通过 VTOR 判断 */
uint32_t ota_running_slot(void);

/* 启动成功后调用：把 pending 槽确认为 active，清除回滚计数 */
void ota_confirm_self(void);

/* 内部状态，便于 UI 显示进度 */
typedef enum
{
    OTA_IDLE = 0,
    OTA_CONNECTING,
    OTA_DOWNLOADING,
    OTA_VERIFYING,
    OTA_FAILED
} ota_state_t;

ota_state_t ota_get_state(void);
uint32_t    ota_get_progress(void);      /* 已下载字节数 */
uint32_t    ota_get_total(void);         /* 总字节数，未知时为 0 */

#endif /* __OTA_CLIENT_H__ */
