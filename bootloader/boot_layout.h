#ifndef __BOOT_LAYOUT_H__
#define __BOOT_LAYOUT_H__

#include <stdint.h>

/*
 * ============================================================================
 * STM32F407VE (512KB Flash, 8 个扇区) 分区表
 * ----------------------------------------------------------------------------
 *  扇区   起始地址      容量     用途
 *  0     0x08000000    16KB  ┐
 *  1     0x08004000    16KB  ┘ Bootloader            (32KB)
 *  2     0x08008000    16KB    元数据日志区          (16KB)
 *  3     0x0800C000    16KB  ┐
 *  4     0x08010000    64KB  ├ Slot A = 208KB       (0x0800C000 ~ 0x08040000)
 *  5     0x08020000   128KB  ┘
 *  6     0x08040000   128KB  ┐
 *  7     0x08060000   128KB  ┘ Slot B = 256KB       (0x08040000 ~ 0x08080000)
 *
 * 注意：两区容量不等是因为 F407 的扇区大小不相等（16/64/128KB），
 *       取小者作约束：单个镜像最大 SLOT_A_SIZE - IMG_HEADER_SIZE = 207KB。
 *
 * 每个槽前 1KB 是镜像头，真正的中断向量表在 slot_base + 0x400。
 * ============================================================================
 */

#define BOOT_BASE           0x08000000u
#define BOOT_SIZE           (32u * 1024u)

#define META_BASE           0x08008000u
#define META_SECTOR_SIZE    (16u * 1024u)
#define META_RECORD_SIZE    256u
#define META_RECORD_COUNT   (META_SECTOR_SIZE / META_RECORD_SIZE)   /* 64 条 */

#define SLOT_A_BASE         0x0800C000u
#define SLOT_A_SIZE         (208u * 1024u)
#define SLOT_B_BASE         0x08040000u
#define SLOT_B_SIZE         (256u * 1024u)

#define SLOT_COUNT          2u
#define SLOT_NONE           0xFFFFFFFFu

#define IMG_HEADER_SIZE     0x400u          /* 1KB */
#define IMG_MAGIC           0x494D4731u     /* 'IMG1' */
#define IMG_HEADER_VERSION  1u

/* 单个镜像 payload 的上限（两区取小） */
#define OTA_MAX_IMAGE_SIZE  (SLOT_A_SIZE - IMG_HEADER_SIZE)     /* 207KB */

/* 版本号：0xMMmmpp  例如 v1.2.3 -> 0x010203 */
#define IMG_VERSION(major, minor, patch) \
    (((uint32_t)(major) << 16) | ((uint32_t)(minor) << 8) | ((uint32_t)(patch)))

/* --------------------------------------------------------------------------
 * 镜像头：放在槽起始处，占满 IMG_HEADER_SIZE 字节
 * -------------------------------------------------------------------------- */
typedef struct
{
    uint32_t magic;             /* IMG_MAGIC */
    uint32_t header_version;    /* IMG_HEADER_VERSION */
    uint32_t version;           /* 0xMMmmpp */
    uint32_t load_addr;         /* 期望加载地址 = slot_base + 0x400 */
    uint32_t image_size;        /* payload 字节数（不含头） */
    uint32_t image_crc32;       /* payload 的 CRC32 */
    uint32_t build_time;        /* 编译时间戳（秒） */
    uint32_t flags;             /* bit0: 调试版，其余保留 */
    uint8_t  git_hash[8];       /* 可选：前 8 字节 commit */
    uint8_t  reserved[980];
    uint32_t header_crc32;      /* 覆盖前 1020 字节 */
} img_header_t;

/* --------------------------------------------------------------------------
 * 元数据：日志式追加写。每条 256 字节，一条录满 64 条才整扇区擦除一次，
 * 断电时最多丢最新一条，旧记录仍可回退。
 * -------------------------------------------------------------------------- */
#define META_MAGIC          0x4D455441u     /* 'META' */
#define META_DEFAULT_ATTEMPTS  3u

typedef struct
{
    uint32_t magic;
    uint32_t seq;               /* 单调递增，选最大者 */
    uint32_t active_slot;       /* 0=A 1=B：当前"已确认可运行"的槽 */
    uint32_t pending_slot;      /* 0=A 1=B SLOT_NONE：等待确认的新固件 */
    uint32_t attempts;          /* pending 已尝试启动次数 */
    uint32_t max_attempts;      /* 超过则回滚 */
    uint32_t version[SLOT_COUNT];
    uint32_t crc[SLOT_COUNT];
    uint32_t size[SLOT_COUNT];
    uint32_t reserved[51];
    uint32_t crc32;             /* 覆盖前 252 字节 */
} ota_meta_t;

/* 用宏而不是 static inline，兼容 ARMCC5 的非 C99 模式 */
#define SLOT_BASE_OF(slot)   (((slot) == 0u) ? SLOT_A_BASE : SLOT_B_BASE)
#define SLOT_SIZE_OF(slot)   (((slot) == 0u) ? SLOT_A_SIZE : SLOT_B_SIZE)
#define SLOT_IMAGE_OF(slot)  (SLOT_BASE_OF(slot) + IMG_HEADER_SIZE)

#endif /* __BOOT_LAYOUT_H__ */
