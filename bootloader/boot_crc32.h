#ifndef __BOOT_CRC32_H__
#define __BOOT_CRC32_H__

#include <stdint.h>

/*
 * CRC-32，与 zlib.crc32 / Python 的 zlib.crc32 完全一致（反射算法）：
 *   poly   0xEDB88320（0x04C11DB7 的反射）
 *   init   0xFFFFFFFF
 *   final  ^0xFFFFFFFF
 *
 * 分块计算时：
 *   uint32_t c = 0xFFFFFFFF;
 *   c = boot_crc32_update(c, buf1, n1);
 *   c = boot_crc32_update(c, buf2, n2);
 *   result = boot_crc32_final(c);
 *
 * 打包脚本必须用 zlib.crc32 算 payload，两边才能对上。
 */
uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
uint32_t boot_crc32_final(uint32_t crc);
uint32_t boot_crc32(const uint8_t *data, uint32_t len);

#endif
