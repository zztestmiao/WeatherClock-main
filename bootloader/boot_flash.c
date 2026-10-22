/*
 * boot_flash.c —— 直接操作 FLASH 寄存器，不依赖 StdPeriph，bootloader 与 app 都能用。
 *
 * 关键点（面试常问）：
 *  1. F4 的扇区大小不等（16/64/128KB），擦除单位是"扇区"，不能按字节擦。
 *  2. 擦除/编程期间 CPU 取指会被总线 stall。若代码本身跑在 Flash 上，
 *     擦除 128KB 扇区期间 CPU 会停住 1~2 秒 —— 必须提前把要做的事做完，
 *     并且不要在擦除期间指望 UART 中断还能响应。
 *  3. 编程最小单位是 4 字节（PSIZE=x32），只能把 1 写成 0，不能写回 1。
 *  4. 写完立刻回读可能拿到 ART 数据缓存里的旧值，必须先失效 D-Cache。
 */
#include "boot_flash.h"
#include "stm32f4xx.h"

#define SECTOR_COUNT   8u

static const uint32_t s_sector_addr[SECTOR_COUNT] = {
    0x08000000u, 0x08004000u, 0x08008000u, 0x0800C000u,
    0x08010000u, 0x08020000u, 0x08040000u, 0x08060000u
};

static const uint32_t s_sector_size[SECTOR_COUNT] = {
    16u * 1024u, 16u * 1024u, 16u * 1024u, 16u * 1024u,
    64u * 1024u, 128u * 1024u, 128u * 1024u, 128u * 1024u
};

static int sector_of(uint32_t addr)
{
    for (uint32_t i = 0u; i < SECTOR_COUNT; i++) {
        if (addr >= s_sector_addr[i] && addr < s_sector_addr[i] + s_sector_size[i])
            return (int)i;
    }
    return -1;
}

static void clear_flash_flags(void)
{
    uint32_t flags = FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_WRPERR |
                     FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR;
#ifdef FLASH_SR_RDERR
    flags |= FLASH_SR_RDERR;
#endif
    FLASH->SR = flags;              /* 这些位写 1 清零 */
}

static int flash_has_error(void)
{
    uint32_t err = FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR |
                   FLASH_SR_PGPERR | FLASH_SR_PGSERR;
#ifdef FLASH_SR_RDERR
    err |= FLASH_SR_RDERR;
#endif
    return (FLASH->SR & err) != 0u;
}

void boot_flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

void boot_flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

void boot_flash_dcache_reset(void)
{
    /* 官方序列：关数据缓存 -> 复位 -> 重新打开 */
    FLASH->ACR &= ~FLASH_ACR_DCEN;
    FLASH->ACR |= FLASH_ACR_DCRST;
    FLASH->ACR &= ~FLASH_ACR_DCRST;
    FLASH->ACR |= FLASH_ACR_DCEN;
}

static int erase_one_sector(uint32_t idx)
{
    clear_flash_flags();

    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |= FLASH_CR_PSIZE_1;              /* x32 并行度 */

    FLASH->CR |= FLASH_CR_SER;
    FLASH->CR &= ~FLASH_CR_SNB;
    FLASH->CR |= (idx << FLASH_CR_SNB_Pos);

    FLASH->CR |= FLASH_CR_STRT;
    while (FLASH->SR & FLASH_SR_BSY) { }

    FLASH->CR &= ~FLASH_CR_SER;
    FLASH->CR &= ~FLASH_CR_SNB;

    if (flash_has_error()) {
        clear_flash_flags();
        return BOOT_FLASH_ERR;
    }
    return BOOT_FLASH_OK;
}

int boot_flash_erase(uint32_t addr, uint32_t len)
{
    if (len == 0u)
        return BOOT_FLASH_OK;

    int first = sector_of(addr);
    int last  = sector_of(addr + len - 1u);
    if (first < 0 || last < 0)
        return BOOT_FLASH_ERR;

    boot_flash_unlock();
    for (int i = first; i <= last; i++) {
        if (erase_one_sector((uint32_t)i) != BOOT_FLASH_OK) {
            boot_flash_lock();
            return BOOT_FLASH_ERR;
        }
    }
    boot_flash_lock();
    boot_flash_dcache_reset();
    return BOOT_FLASH_OK;
}

int boot_flash_erase_range(uint32_t addr, uint32_t len)
{
    return boot_flash_erase(addr, len);
}

int boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if ((addr & 0x3u) != 0u || (len & 0x3u) != 0u)
        return BOOT_FLASH_ERR;
    if (sector_of(addr) < 0 || sector_of(addr + len - 1u) < 0)
        return BOOT_FLASH_ERR;

    boot_flash_unlock();
    clear_flash_flags();

    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |= FLASH_CR_PSIZE_1;
    FLASH->CR |= FLASH_CR_PG;

    int ret = BOOT_FLASH_OK;
    for (uint32_t i = 0u; i < len; i += 4u) {
        uint32_t word = (uint32_t)data[i]
                      | ((uint32_t)data[i + 1u] << 8)
                      | ((uint32_t)data[i + 2u] << 16)
                      | ((uint32_t)data[i + 3u] << 24);

        *(volatile uint32_t *)(addr + i) = word;
        while (FLASH->SR & FLASH_SR_BSY) { }

        if (flash_has_error()) {
            ret = BOOT_FLASH_ERR;
            break;
        }
    }

    FLASH->CR &= ~FLASH_CR_PG;
    clear_flash_flags();
    boot_flash_lock();
    boot_flash_dcache_reset();

    if (ret != BOOT_FLASH_OK)
        return ret;

    /* 回读校验 */
    for (uint32_t i = 0u; i < len; i++) {
        if (*(volatile uint8_t *)(addr + i) != data[i])
            return BOOT_FLASH_ERR;
    }
    return BOOT_FLASH_OK;
}

void boot_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    boot_flash_dcache_reset();
    for (uint32_t i = 0u; i < len; i++)
        buf[i] = *(volatile uint8_t *)(addr + i);
}

int boot_flash_is_blank(uint32_t addr, uint32_t len)
{
    boot_flash_dcache_reset();
    for (uint32_t i = 0u; i < len; i++) {
        if (*(volatile uint8_t *)(addr + i) != 0xFFu)
            return 0;
    }
    return 1;
}
