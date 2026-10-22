/*
 * ota_client.c —— App 侧 OTA：通过 ESP-AT 以 TCP + HTTP Range 下载固件到备用槽
 *
 * 与 esp_at.c 的关系：
 *   esp_at.c 是"中断 + 信号量 + 行缓冲"的文本命令模型，处理不了二进制流。
 *   本文件在升级期间接管 USART2：关掉 RXNE 中断，改成阻塞式按字节读，
 *   并且用 AT+CIPRECVMODE=1（被动接收）把数据暂存在 ESP 侧，
 *   这样即使在写 Flash 时停顿几十毫秒，也不会丢数据。
 *
 * 关键：升级期间必须停掉 app.c 里那些会发 AT 指令的定时器，
 *       否则会和这里抢 UART2。由 app.c 提供下面两个函数。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "boot_layout.h"
#include "boot_flash.h"
#include "boot_crc32.h"
#include "boot_meta.h"
#include "ota_client.h"

/* 由 app.c 实现：暂停/恢复所有会访问 UART2 的软件定时器 */
extern void app_updates_suspend(void);
extern void app_updates_resume(void);

#define OTA_RX_CHUNK       1024u
#define OTA_WBUF_SIZE      1024u
#define OTA_HEADER_MAX     2048u
#define OTA_CMD_TIMEOUT    3000u
#define OTA_CONN_TIMEOUT   15000u
#define OTA_DATA_TIMEOUT   30000u

static ota_state_t s_state = OTA_IDLE;
static uint32_t    s_got;
static uint32_t    s_total;
static char        s_line[128];

/* ===========================================================================
 * 1. USART2 裸读写
 * =========================================================================*/
static void uart_enter_raw(void)
{
    USART2->CR1 &= ~USART_CR1_RXNEIE;
    NVIC_DisableIRQ(USART2_IRQn);

    /* 等 esp_at.c 的 DMA 发送结束 */
    while (DMA1_Stream6->CR & DMA_SxCR_EN) { }

    /* 丢掉残留字节 */
    while (USART2->SR & USART_SR_RXNE)
        (void)USART2->DR;
}

static void uart_leave_raw(void)
{
    while (USART2->SR & USART_SR_RXNE)
        (void)USART2->DR;

    NVIC_ClearPendingIRQ(USART2_IRQn);
    USART2->CR1 |= USART_CR1_RXNEIE;
    NVIC_EnableIRQ(USART2_IRQn);
}

static void raw_send(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0u; i < len; i++) {
        while (!(USART2->SR & USART_SR_TXE)) { }
        USART2->DR = data[i];
    }
    while (!(USART2->SR & USART_SR_TC)) { }
}

static int raw_read_byte(uint8_t *b, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();

    while (!(USART2->SR & USART_SR_RXNE)) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms))
            return -1;
    }
    *b = (uint8_t)USART2->DR;
    return 0;
}

static int raw_read(uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    for (uint32_t i = 0u; i < len; i++) {
        if (raw_read_byte(&buf[i], timeout_ms) != 0)
            return -1;
    }
    return 0;
}

/* 读一行（到 \n 为止，去掉 \r）；返回长度，超时返回 -1 */
static int raw_read_line(char *out, uint32_t max, uint32_t timeout_ms)
{
    uint32_t n = 0u;
    TickType_t start = xTaskGetTickCount();

    for (;;) {
        uint8_t c;
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms))
            return -1;
        if (raw_read_byte(&c, timeout_ms) != 0)
            return -1;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (n + 1u < max)
            out[n++] = (char)c;
    }
    out[n] = '\0';
    return (int)n;
}

/* ===========================================================================
 * 2. AT 命令
 * =========================================================================*/
/* 只等待 OK/ERROR，不发送任何内容 */
static int wait_ok(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        if (raw_read_line(s_line, sizeof(s_line), timeout_ms) < 0)
            return -2;
        if (strcmp(s_line, "OK") == 0)
            return 0;
        if (strcmp(s_line, "ERROR") == 0 || strcmp(s_line, "FAIL") == 0)
            return -1;
    }
    return -2;
}

static int at_cmd(const char *cmd, uint32_t timeout_ms)
{
    char buf[160];
    int len = snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    raw_send((const uint8_t *)buf, (uint32_t)len);
    return wait_ok(timeout_ms);
}

/* 原始 TCP 建链 */
static int tcp_open(const char *host, uint16_t port)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", host, (unsigned)port);

    /* 可能返回 OK 也可能返回 ALREADY CONNECTED */
    char buf[160];
    int len = snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    raw_send((const uint8_t *)buf, (uint32_t)len);

    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(OTA_CONN_TIMEOUT)) {
        if (raw_read_line(s_line, sizeof(s_line), OTA_CONN_TIMEOUT) < 0)
            return -2;
        if (strcmp(s_line, "OK") == 0 || strcmp(s_line, "ALREADY CONNECTED") == 0)
            return 0;
        if (strcmp(s_line, "ERROR") == 0 || strcmp(s_line, "FAIL") == 0)
            return -1;
    }
    return -2;
}

/* AT+CIPSEND 发送裸数据 */
static int tcp_send(const uint8_t *data, uint32_t len)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned)len);
    raw_send((const uint8_t *)cmd, (uint32_t)strlen(cmd));
    raw_send((const uint8_t *)"\r\n", 2u);

    /* 等 '>' 提示符 */
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(OTA_CMD_TIMEOUT))
            return -2;

        uint8_t c;
        if (raw_read_byte(&c, OTA_CMD_TIMEOUT) != 0)
            return -2;
        if (c == '>')
            break;
    }

    raw_send(data, len);
    return wait_ok(OTA_CONN_TIMEOUT);
}

/*
 * 被动接收：AT+CIPRECVMODE=1 之后用 AT+CIPRECVDATA 主动取
 * 返回实际字节数；0 表示暂时没数据；<0 出错
 */
static int esp_recv(uint8_t *buf, uint32_t max)
{
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+CIPRECVDATA=%u", (unsigned)max);
    raw_send((const uint8_t *)cmd, (uint32_t)strlen(cmd));
    raw_send((const uint8_t *)"\r\n", 2u);

    /* 等一行形如 +CIPRECVDATA:<len>, */
    for (;;) {
        if (raw_read_line(s_line, sizeof(s_line), OTA_DATA_TIMEOUT) < 0)
            return -2;

        if (strncmp(s_line, "+CIPRECVDATA:", 13) == 0) {
            uint32_t len = (uint32_t)atoi(s_line + 13);
            if (len == 0u)
                return 0;
            if (len > max)
                return -3;
            if (raw_read(buf, len, OTA_DATA_TIMEOUT) != 0)
                return -2;
            return (int)len;
        }
        if (strcmp(s_line, "OK") == 0)
            return 0;
        if (strcmp(s_line, "ERROR") == 0 || strcmp(s_line, "FAIL") == 0)
            return -1;
    }
}

/* ===========================================================================
 * 3. URL 解析
 * =========================================================================*/
typedef struct
{
    char     host[64];
    uint16_t port;
    char     path[96];
} ota_url_t;

static bool parse_url(const char *url, ota_url_t *u)
{
    const char *p = url;
    memset(u, 0, sizeof(*u));
    u->port = 80u;

    if (strncmp(p, "http://", 7) != 0)
        return false;               /* https 需要 AT+CIPSSLCCONF，另行处理 */
    p += 7;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (slash == NULL)
        return false;

    if (colon != NULL && colon < slash) {
        uint32_t n = (uint32_t)(colon - p);
        if (n >= sizeof(u->host))
            return false;
        memcpy(u->host, p, n);
        u->port = (uint16_t)atoi(colon + 1);
    } else {
        uint32_t n = (uint32_t)(slash - p);
        if (n >= sizeof(u->host))
            return false;
        memcpy(u->host, p, n);
    }

    if (strlen(slash) >= sizeof(u->path))
        return false;
    strcpy(u->path, slash);
    return true;
}

/* ===========================================================================
 * 4. 写槽缓冲（4 字节对齐）
 * =========================================================================*/
static uint8_t  s_wbuf[OTA_WBUF_SIZE];
static uint32_t s_wlen;
static uint32_t s_wofs;
static uint32_t s_wslot;

static bool flash_flush(bool final)
{
    uint32_t n = s_wlen;

    if (n == 0u)
        return true;

    if (final && (n & 0x3u)) {
        uint32_t pad = 4u - (n & 0x3u);
        for (uint32_t i = 0u; i < pad; i++)
            s_wbuf[n + i] = 0xFFu;
        n += pad;
    }

    if (boot_flash_write(SLOT_BASE_OF(s_wslot) + s_wofs, s_wbuf, n) != BOOT_FLASH_OK)
        return false;

    s_wofs += n;
    s_wlen = 0u;
    return true;
}

static bool slot_append(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0u; i < len; i++) {
        if (s_wofs + s_wlen >= SLOT_SIZE_OF(s_wslot))
            return false;
        s_wbuf[s_wlen++] = data[i];
        if (s_wlen == OTA_WBUF_SIZE) {
            if (!flash_flush(false))
                return false;
        }
    }
    return true;
}

/* ===========================================================================
 * 5. 主流程
 * =========================================================================*/
static bool find_header_end(const uint8_t *buf, uint32_t len, uint32_t *pos)
{
    for (uint32_t i = 0u; i + 4u <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *pos = i + 4u;
            return true;
        }
    }
    return false;
}

static uint32_t get_content_length(const char *hdr)
{
    const char *p = strstr(hdr, "Content-Length:");
    if (!p)
        p = strstr(hdr, "content-length:");
    if (!p)
        return 0u;
    return (uint32_t)atoi(p + 15);
}

bool ota_upgrade_from_url(const char *url)
{
    ota_url_t u;
    ota_meta_t meta;
    uint8_t chunk[OTA_RX_CHUNK];
    uint8_t hbuf[OTA_HEADER_MAX];
    uint32_t hlen = 0u;
    bool hdr_done = false;

    s_state = OTA_CONNECTING;
    s_got = 0u;
    s_total = 0u;

    if (!parse_url(url, &u))
        goto fail;

    boot_meta_load(&meta);

    /* 目标槽 = 当前不在跑的那个 */
    s_wslot = ota_running_slot() == 0u ? 1u : 0u;
    s_wofs = 0u;
    s_wlen = 0u;

    app_updates_suspend();
    uart_enter_raw();

    if (at_cmd("AT", OTA_CMD_TIMEOUT) != 0)
        goto fail_raw;

    /* 被动接收：数据先存 ESP，本机忙的时候不会丢 */
    if (at_cmd("AT+CIPRECVMODE=1", OTA_CMD_TIMEOUT) != 0)
        goto fail_raw;

    if (tcp_open(u.host, u.port) != 0)
        goto fail_raw;

    {
        char req[320];
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Connection: close\r\n"
                         "Range: bytes=0-\r\n"
                         "\r\n",
                         u.path, u.host);
        if (tcp_send((const uint8_t *)req, (uint32_t)n) != 0)
            goto fail_raw;
    }

    s_state = OTA_DOWNLOADING;

    for (;;) {
        int n = esp_recv(chunk, sizeof(chunk));
        if (n < 0)
            goto fail_raw;
        if (n == 0) {
            /* 没数据了：要么下完了，要么连接关了 */
            break;
        }

        if (!hdr_done) {
            if (hlen + (uint32_t)n > sizeof(hbuf))
                goto fail_raw;
            memcpy(hbuf + hlen, chunk, (uint32_t)n);
            hlen += (uint32_t)n;

            uint32_t pos;
            if (!find_header_end(hbuf, hlen, &pos))
                continue;               /* 头还没收完 */

            hbuf[hlen < sizeof(hbuf) ? hlen : sizeof(hbuf) - 1u] = '\0';
            if (strncmp((const char *)hbuf, "HTTP/1.1 200", 12) != 0 &&
                strncmp((const char *)hbuf, "HTTP/1.0 200", 12) != 0 &&
                strncmp((const char *)hbuf, "HTTP/1.1 206", 12) != 0) {
                goto fail_raw;          /* 404/500 等 */
            }

            s_total = get_content_length((const char *)hbuf);
            if (s_total == 0u || s_total > IMG_HEADER_SIZE + OTA_MAX_IMAGE_SIZE)
                goto fail_raw;

            /* 先把目标槽擦干净（此时下载还没开始大量涌进来） */
            if (boot_flash_erase(SLOT_BASE_OF(s_wslot), s_total) != BOOT_FLASH_OK)
                goto fail_raw;

            hdr_done = true;

            if (!slot_append(hbuf + pos, hlen - pos))
                goto fail_raw;
            s_got = hlen - pos;
        } else {
            if (!slot_append(chunk, (uint32_t)n))
                goto fail_raw;
            s_got += (uint32_t)n;
        }
    }

    if (!hdr_done || (s_got + 0u) == 0u)
        goto fail_raw;

    if (!flash_flush(true))
        goto fail_raw;

    /* ---- 校验：读回来算，确保不是"写进去了但内容不对" ---- */
    s_state = OTA_VERIFYING;

    {
        img_header_t h;
        boot_flash_read(SLOT_BASE_OF(s_wslot), (uint8_t *)&h, sizeof(h));

        if (h.magic != IMG_MAGIC)
            goto fail_raw;
        if (h.load_addr != SLOT_BASE_OF(s_wslot) + IMG_HEADER_SIZE)
            goto fail_raw;
        if (h.image_size == 0u || h.image_size > SLOT_SIZE_OF(s_wslot) - IMG_HEADER_SIZE)
            goto fail_raw;

        uint32_t crc = 0xFFFFFFFFu;
        uint32_t left = h.image_size;
        uint32_t addr = h.load_addr;
        while (left) {
            uint32_t m = (left > sizeof(chunk)) ? sizeof(chunk) : left;
            boot_flash_read(addr, chunk, m);
            crc = boot_crc32_update(crc, chunk, m);
            addr += m;
            left -= m;
        }

        if (boot_crc32_final(crc) != h.image_crc32)
            goto fail_raw;

        /* 落到元数据：下次上电 bootloader 会尝试启动这个槽 */
        boot_meta_set_pending(s_wslot, h.version, h.image_crc32, h.image_size);
    }

    at_cmd("AT+CIPCLOSE", OTA_CMD_TIMEOUT);
    uart_leave_raw();
    app_updates_resume();

    /* 立即重启，让 bootloader 接管 */
    NVIC_SystemReset();

fail_raw:
    at_cmd("AT+CIPCLOSE", OTA_CMD_TIMEOUT);
    uart_leave_raw();
    app_updates_resume();

fail:
    s_state = OTA_FAILED;
    return false;
}

/* ===========================================================================
 * 6. 确认与状态
 * =========================================================================*/
uint32_t ota_running_slot(void)
{
    return (SCB->VTOR >= SLOT_B_BASE) ? 1u : 0u;
}

void ota_confirm_self(void)
{
    ota_meta_t meta;
    uint32_t slot = ota_running_slot();

    if (boot_meta_load(&meta) != 0)
        return;

    if (meta.pending_slot == slot) {
        boot_meta_confirm(slot);
        printf("[OTA] slot %u confirmed, rollback cleared\n", (unsigned)slot);
    }
}

ota_state_t ota_get_state(void)    { return s_state; }
uint32_t    ota_get_progress(void) { return s_got;   }
uint32_t    ota_get_total(void)    { return s_total; }
