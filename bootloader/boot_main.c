/*
 * boot_main.c —— 双区 OTA Bootloader 主流程
 *
 * 职责（只做三件事，越简单越不容易变砖）：
 *   1. 读元数据，决定该启动哪个槽
 *   2. 校验镜像（魔术字/加载地址/长度/CRC32）
 *   3. 跳转；失败则回滚，全挂则进入恢复模式
 *
 * 下载和写 Flash 全部由 App 完成，Bootloader 不碰网络 —— 这样它只有几 KB，
 * 也能保证"升级逻辑出错"永远不会破坏引导能力。
 */
#include <string.h>
#include "stm32f4xx.h"
#include "boot_layout.h"
#include "boot_flash.h"
#include "boot_crc32.h"
#include "boot_meta.h"

/* ---------------------------------------------------------------------------
 * 极简串口（USART1: PA9=TX, PA10=PA10），只用于调试输出
 * 如果你板子上 USART1 被占用，改这里的 GPIO/外设即可
 * -------------------------------------------------------------------------*/
static void boot_uart_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* PA9 / PA10 复用 AF7 */
    GPIOA->MODER  = (GPIOA->MODER & ~(3u << 18) & ~(3u << 20))
                  | (2u << 18) | (2u << 20);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFu << 4) & ~(0xFu << 8))
                  | (7u << 4) | (7u << 8);
    GPIOA->OSPEEDR |= (3u << 18) | (3u << 20);

    /* 84MHz / (16 * 115200) ≈ 45.57 -> BRR = 45.57*16 = 729 */
    USART1->BRR = 0x2D9u;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void boot_putc(char c)
{
    while (!(USART1->SR & USART_SR_TXE)) { }
    USART1->DR = (uint8_t)c;
}

static void boot_puts(const char *s)
{
    while (*s)
        boot_putc(*s++);
}

static void boot_put_hex(uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    boot_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        boot_putc(h[(v >> i) & 0xFu]);
}

static void boot_put_dec(uint32_t v)
{
    char buf[11];
    int i = 0;
    if (v == 0u) { boot_putc('0'); return; }
    while (v && i < 10) { buf[i++] = (char)('0' + v % 10u); v /= 10u; }
    while (i) boot_putc(buf[--i]);
}

static void boot_delay_ms(uint32_t ms)
{
    /* 不依赖 SysTick 中断，直接轮询计数，避免影响跳转后的现场 */
    while (ms--) {
        for (volatile uint32_t i = 0u; i < 14000u; i++) { __NOP(); }
    }
}

/* ---------------------------------------------------------------------------
 * 镜像校验
 * -------------------------------------------------------------------------*/
static int image_header_ok(uint32_t slot, img_header_t *out)
{
    img_header_t h;
    boot_flash_read(SLOT_BASE_OF(slot), (uint8_t *)&h, sizeof(h));

    if (h.magic != IMG_MAGIC)
        return 0;
    if (h.header_version != IMG_HEADER_VERSION)
        return 0;
    if (boot_crc32((const uint8_t *)&h, sizeof(h) - 4u) != h.header_crc32)
        return 0;
    if (h.load_addr != SLOT_BASE_OF(slot) + IMG_HEADER_SIZE)
        return 0;
    if (h.image_size == 0u || h.image_size > SLOT_SIZE_OF(slot) - IMG_HEADER_SIZE)
        return 0;

    uint32_t sp = *(volatile uint32_t *)h.load_addr;
    if ((sp & 0xFFF00000u) != 0x20000000u)        /* 栈必须在 SRAM1 */
        return 0;

    if (out)
        *out = h;
    return 1;
}

/* 全镜像 CRC：约 200KB，168MHz 下几十毫秒，启动代价可以接受 */
static int image_crc_ok(uint32_t slot, const img_header_t *h)
{
    uint8_t buf[1024];
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t left = h->image_size;
    uint32_t addr = h->load_addr;

    while (left) {
        uint32_t n = (left > sizeof(buf)) ? sizeof(buf) : left;
        boot_flash_read(addr, buf, n);
        crc = boot_crc32_update(crc, buf, n);
        addr += n;
        left -= n;
    }
    return boot_crc32_final(crc) == h->image_crc32;
}

/* ---------------------------------------------------------------------------
 * 跳转：关中断、复位 SysTick、清 NVIC、设 VTOR 和 MSP
 * -------------------------------------------------------------------------*/
typedef void (*app_entry_t)(void);

static void jump_to_app(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t *)addr;
    uint32_t pc = *(volatile uint32_t *)(addr + 4u);

    __disable_irq();

    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;

    for (uint32_t i = 0u; i < 8u; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    /* 关掉 bootloader 用过的串口，避免残留中断 */
    USART1->CR1 = 0u;

    SCB->VTOR = addr;
    __DSB();
    __ISB();

    __set_MSP(sp);
    __enable_irq();

    ((app_entry_t)pc)();
    /* 正常不会返回到这里 */
    while (1) { }
}

static void recovery_mode(void)
{
    boot_puts("[BOOT] no valid image, recovery mode\r\n");
    while (1) {
        boot_puts(".");
        boot_delay_ms(500);
    }
}

/* ---------------------------------------------------------------------------
 * 主流程
 * -------------------------------------------------------------------------*/
int main(void)
{
    boot_uart_init();
    SCB->VTOR = BOOT_BASE;              /* bootloader 自己的向量表 */

    ota_meta_t meta;
    int meta_ok = (boot_meta_load(&meta) == 0);

    boot_puts("\r\n[BOOT] WeatherClock bootloader\r\n");
    boot_puts("[BOOT] meta: ");
    boot_puts(meta_ok ? "ok" : "default");
    boot_puts(" active=");
    boot_put_dec(meta.active_slot);
    boot_puts(" pending=");
    if (meta.pending_slot == SLOT_NONE) boot_puts("none");
    else boot_put_dec(meta.pending_slot);
    boot_puts(" attempts=");
    boot_put_dec(meta.attempts);
    boot_puts("\r\n");

    uint32_t cand = meta.active_slot;

    if (meta.pending_slot != SLOT_NONE) {
        if (meta.attempts >= meta.max_attempts) {
            /* 新固件连续启动失败 -> 回滚 */
            boot_puts("[BOOT] pending image failed too many times, rollback\r\n");
            meta.pending_slot = SLOT_NONE;
            meta.attempts     = 0u;
            boot_meta_save(&meta);
            cand = meta.active_slot;
        } else {
            cand = meta.pending_slot;
            boot_meta_bump_attempts();
            boot_puts("[BOOT] try pending slot ");
            boot_put_dec(cand);
            boot_puts("\r\n");
        }
    }

    img_header_t hdr;
    if (!image_header_ok(cand, &hdr)) {
        uint32_t other = (cand == 0u) ? 1u : 0u;
        boot_puts("[BOOT] slot ");
        boot_put_dec(cand);
        boot_puts(" header invalid, try other\r\n");

        if (!image_header_ok(other, &hdr))
            recovery_mode();

        cand = other;
    }

    if (!image_crc_ok(cand, &hdr)) {
        boot_puts("[BOOT] slot ");
        boot_put_dec(cand);
        boot_puts(" crc mismatch\r\n");

        uint32_t other = (cand == 0u) ? 1u : 0u;
        if (!image_header_ok(other, &hdr) || !image_crc_ok(other, &hdr))
            recovery_mode();
        cand = other;
    }

    boot_puts("[BOOT] jump to slot ");
    boot_put_dec(cand);
    boot_puts(" v");
    boot_put_dec((hdr.version >> 16) & 0xFFu);
    boot_putc('.');
    boot_put_dec((hdr.version >> 8) & 0xFFu);
    boot_putc('.');
    boot_put_dec(hdr.version & 0xFFu);
    boot_puts(" @");
    boot_put_hex(hdr.load_addr);
    boot_puts("\r\n");

    boot_delay_ms(50);
    jump_to_app(hdr.load_addr);
}
