#include <string.h>
#include "boot_meta.h"
#include "boot_flash.h"
#include "boot_crc32.h"

#define CRC_SPAN    (sizeof(ota_meta_t) - sizeof(uint32_t))

static uint32_t record_addr(uint32_t idx)
{
    return META_BASE + idx * META_RECORD_SIZE;
}

static int record_is_valid(const ota_meta_t *m)
{
    if (m->magic != META_MAGIC)
        return 0;
    uint32_t crc = boot_crc32((const uint8_t *)m, CRC_SPAN);
    return crc == m->crc32;
}

static void meta_defaults(ota_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic        = META_MAGIC;
    m->seq          = 0u;
    m->active_slot  = 0u;               /* 出厂：A 区 */
    m->pending_slot = SLOT_NONE;
    m->attempts     = 0u;
    m->max_attempts = META_DEFAULT_ATTEMPTS;
}

int boot_meta_load(ota_meta_t *out)
{
    ota_meta_t tmp;
    ota_meta_t best;
    int found = 0;

    meta_defaults(&best);

    for (uint32_t i = 0u; i < META_RECORD_COUNT; i++) {
        boot_flash_read(record_addr(i), (uint8_t *)&tmp, sizeof(tmp));
        if (!record_is_valid(&tmp))
            continue;
        if (!found || tmp.seq > best.seq) {
            best  = tmp;
            found = 1;
        }
    }

    *out = best;
    return found ? 0 : -1;
}

static int write_record(uint32_t idx, const ota_meta_t *m)
{
    ota_meta_t tmp = *m;
    tmp.crc32 = boot_crc32((const uint8_t *)&tmp, CRC_SPAN);

    if (boot_flash_write(record_addr(idx), (const uint8_t *)&tmp, sizeof(tmp)) != BOOT_FLASH_OK)
        return -1;

    /* 回读确认：掉电可能写了一半，这里能立刻发现 */
    ota_meta_t rb;
    boot_flash_read(record_addr(idx), (uint8_t *)&rb, sizeof(rb));
    return record_is_valid(&rb) ? 0 : -1;
}

int boot_meta_save(const ota_meta_t *in)
{
    ota_meta_t cur;
    uint32_t last_idx = 0u;
    int found = boot_meta_load(&cur) == 0;

    if (found) {
        /* 找到当前最新记录所在的下标 */
        ota_meta_t tmp;
        for (uint32_t i = 0u; i < META_RECORD_COUNT; i++) {
            boot_flash_read(record_addr(i), (uint8_t *)&tmp, sizeof(tmp));
            if (record_is_valid(&tmp) && tmp.seq == cur.seq) {
                last_idx = i;
                break;
            }
        }
    }

    ota_meta_t next = *in;
    next.magic = META_MAGIC;
    next.seq   = found ? (cur.seq + 1u) : 1u;
    if (next.max_attempts == 0u)
        next.max_attempts = META_DEFAULT_ATTEMPTS;

    /* 先尝试追加到下一条记录，旧记录一概不动 */
    uint32_t idx = found ? (last_idx + 1u) : 0u;

    if (idx >= META_RECORD_COUNT ||
        !boot_flash_is_blank(record_addr(idx), sizeof(ota_meta_t))) {

        /* 写满了才整体擦除。这一步掉电会丢历史，但默认值仍指向 A 区，
           设备不会变砖（见 boot_meta_load 的默认值）。 */
        if (boot_flash_erase(META_BASE, META_SECTOR_SIZE) != BOOT_FLASH_OK)
            return -1;
        idx = 0u;
    }

    return write_record(idx, &next);
}

int boot_meta_set_pending(uint32_t slot, uint32_t version, uint32_t crc, uint32_t size)
{
    ota_meta_t m;
    boot_meta_load(&m);
    m.pending_slot    = slot;
    m.attempts        = 0u;
    m.version[slot]   = version;
    m.crc[slot]       = crc;
    m.size[slot]      = size;
    return boot_meta_save(&m);
}

int boot_meta_confirm(uint32_t slot)
{
    ota_meta_t m;
    boot_meta_load(&m);
    m.active_slot  = slot;
    m.pending_slot = SLOT_NONE;
    m.attempts     = 0u;
    return boot_meta_save(&m);
}

int boot_meta_bump_attempts(void)
{
    ota_meta_t m;
    boot_meta_load(&m);
    m.attempts++;
    return boot_meta_save(&m);
}
