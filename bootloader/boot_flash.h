#ifndef __BOOT_FLASH_H__
#define __BOOT_FLASH_H__

#include <stdint.h>

#define BOOT_FLASH_OK      0
#define BOOT_FLASH_ERR    (-1)

/* 解锁/上锁：擦写内部 Flash 必须先解锁 */
void boot_flash_unlock(void);
void boot_flash_lock(void);

/*
 * 按地址范围擦除，内部会自动扩展到扇区边界。
 * 注意：F407 的扇区大小不等（16/64/128KB），一次擦除可能比你请求的大。
 * 返回 0 成功。
 */
int boot_flash_erase(uint32_t addr, uint32_t len);

/* 按地址擦除到区域末尾（addr 必须落在扇区边界内，长度自动覆盖到 len） */
int boot_flash_erase_range(uint32_t addr, uint32_t len);

/*
 * 写数据：addr 必须 4 字节对齐，len 必须 4 字节对齐。
 * 未对齐的数据由调用者自行补齐（0xFF）。
 */
int boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);

/* 读数据（会先失效 ART 数据缓存，保证读到刚写入的内容） */
void boot_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/* 失效 Flash 数据缓存：写完必须调用，否则回读可能是旧值 */
void boot_flash_dcache_reset(void);

/* 该地址是否为空（全 0xFF） */
int boot_flash_is_blank(uint32_t addr, uint32_t len);

#endif /* __BOOT_FLASH_H__ */
