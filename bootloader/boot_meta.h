#ifndef __BOOT_META_H__
#define __BOOT_META_H__

#include "boot_layout.h"

/*
 * 元数据是 OTA 的"真相来源"：谁有效、谁待确认、试了几次。
 * 采用日志式追加写，避免"擦除-写入"窗口掉电导致配置全丢。
 */

/* 读取最新一条有效记录；失败返回 -1，out 被填成安全默认值 */
int boot_meta_load(ota_meta_t *out);

/* 追加写入一条记录（seq 自动 +1），成功返回 0 */
int boot_meta_save(const ota_meta_t *in);

/* 便捷：只改 pending / active */
int boot_meta_set_pending(uint32_t slot, uint32_t version, uint32_t crc, uint32_t size);
int boot_meta_confirm(uint32_t slot);
int boot_meta_bump_attempts(void);

#endif
