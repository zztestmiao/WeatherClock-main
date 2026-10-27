/*
 * 天气时钟应用调度层
 * 时间显示在 FreeRTOS 定时器服务任务中执行；网络与传感器操作提交到 workqueue。
 * 页面函数只提交 UI 消息，屏幕驱动由 UI 任务调用。文件编码：UTF-8。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "workqueue.h"
#include "rtc.h"
#include "aht20.h"
#include "esp_at.h"
#include "weather.h"
#include "page.h"
#include "app.h"

// 时间换算宏：这里的计算结果以毫秒为单位，创建定时器时再转换为 tick。
#define MILLISECONDS(x) (x)
#define SECONDS(x)      MILLISECONDS((x) * 1000)
#define MINUTES(x)      SECONDS((x) * 60)
#define HOURS(x)        MINUTES((x) * 60)
#define DAYS(x)          HOURS((x) * 24)

// 各业务的刷新周期：校时 1 小时、WiFi 5 秒、时间显示 1 秒、室内采样 3 秒、天气 1 分钟。
#define TIME_SYNC_INTERVAL          HOURS(1)
#define WIFI_UPDATE_INTERVAL        SECONDS(5)
#define TIME_UPDATE_INTERVAL        SECONDS(1)
#define INNER_UPDATE_INTERVAL       SECONDS(3)
#define OUTDOOR_UPDATE_INTERVAL     MINUTES(1)

// 事件位定义：当前文件的调度使用软件定时器与工作队列，暂未使用这些事件位。
#define MLOOP_EVT_TIME_SYNC         (1 << 0)
#define MLOOP_EVT_WIFI_UPDATE       (1 << 1)
#define MLOOP_EVT_INNER_UPDATE      (1 << 2)
#define MLOOP_EVT_OUTDOOR_UPDATE    (1 << 3)
#define MLOOP_EVT_ALL               (MLOOP_EVT_TIME_SYNC | \
                                     MLOOP_EVT_WIFI_UPDATE | \
                                     MLOOP_EVT_INNER_UPDATE | \
                                     MLOOP_EVT_OUTDOOR_UPDATE)

// 保存 5 个软件定时器的句柄，用于启动定时器或修改周期。
static TimerHandle_t time_sync_timer;
static TimerHandle_t wifi_update_timer;
static TimerHandle_t time_update_timer;
static TimerHandle_t inner_update_timer;
static TimerHandle_t outdoor_update_timer;

// 网络校时，由 workqueue 任务执行。读取 ESP 的 SNTP 时间并写入 RTC；失败后 1 秒重试，成功后 1 小时再校时。
static void time_sync(void)
{
    uint32_t restart_sync_delay = TIME_SYNC_INTERVAL;
    rtc_date_time_t rtc_date = { 0 };

    esp_date_time_t esp_date = { 0 };
    if (!esp_at_sntp_get_time(&esp_date))
    {
        printf("[SNTP] get time failed\n");
        restart_sync_delay = SECONDS(1);
        goto err;
    }

    if (esp_date.year < 2000)
    {
        printf("[SNTP] invalid date formate\n");
        restart_sync_delay = SECONDS(1);
        goto err;
    }

    printf("[SNTP] sync time: %04u-%02u-%02u %02u:%02u:%02u (%d)\n",
        esp_date.year, esp_date.month, esp_date.day,
        esp_date.hour, esp_date.minute, esp_date.second, esp_date.weekday);

    // 将网络时间结构体的各个字段复制到 RTC 时间结构体。
    rtc_date.year = esp_date.year;
    rtc_date.month = esp_date.month;
    rtc_date.day = esp_date.day;
    rtc_date.hour = esp_date.hour;
    rtc_date.minute = esp_date.minute;
    rtc_date.second = esp_date.second;
    rtc_date.weekday = esp_date.weekday;
    rtc_set_time(&rtc_date);

err:
    // 调整并重新启动单次校时定时器；发送定时器命令时不等待队列空间。
    xTimerChangePeriod(time_sync_timer, pdMS_TO_TICKS(restart_sync_delay), 0);
}

// 查询 WiFi 信息，由 workqueue 任务执行。当前实现仅在连接状态改变时更新 SSID；只改变 RSSI 或 SSID 不会刷新。
static void wifi_update(void)
{
    static esp_wifi_info_t last_info = { 0 };

    esp_wifi_info_t info = { 0 };
    if (!esp_at_get_wifi_info(&info))
    {
        printf("[AT] wifi info get failed\n");
        return;
    }

    if (memcmp(&info, &last_info, sizeof(esp_wifi_info_t)) == 0)
    {
        return;
    }

    if (last_info.connected == info.connected)
    {
        return;
    }

    if (info.connected)
    {
        printf("[WIFI] connected to %s\n", info.ssid);
        printf("[WIFI] SSID: %s, BSSID: %s, Channel: %d, RSSI: %d\n",
                info.ssid, info.bssid, info.channel, info.rssi);
        main_page_redraw_wifi_ssid(info.ssid);
    }
    else
    {
        printf("[WIFI] disconnected from %s\n", last_info.ssid);
        main_page_redraw_wifi_ssid("wifi lost");
    }

    memcpy(&last_info, &info, sizeof(esp_wifi_info_t));
}

// 读取 RTC 并更新时间与日期显示，由定时器服务任务直接执行。此路径应避免阻塞；当前 UI 入队仍可能等待队列空间。
static void time_update(void)
{
    // 静态变量跨调用保留上一份时间，用于判断是否需要刷新。
    static rtc_date_time_t last_date = { 0 };

    rtc_date_time_t date;
    // 步骤 1：读取 RTC，将当前时间写入 date。
    rtc_get_time(&date);

    // 步骤 2：年份早于 2020 时跳过显示。这只是当前代码采用的有效性判断，并非完整日期校验。
    if (date.year < 2020)
    {
        return;
    }

    // 步骤 3：按结构体内存比较本次与上次结果；相同时直接返回。
    if (memcmp(&date, &last_date, sizeof(rtc_date_time_t)) == 0)
    {

        return;
    }

    // 步骤 4：保存本次结果，供下次调用比较。
    memcpy(&last_date, &date, sizeof(rtc_date_time_t));

    // 步骤 5：调用页面函数提交时间和日期绘制消息，实际屏幕操作由 UI 任务完成。
    main_page_redraw_time(&date);
    main_page_redraw_date(&date);
}

// 更新室内温湿度，由 workqueue 任务执行：启动 AHT20 测量、等待完成、读取数据，变化后提交显示更新。
static void inner_update(void)
{
    static float last_temperature, last_humidity;

    if (!aht20_start_measurement())
    {
        printf("[AHT20] start measurement failed\n");
        return;
    }

    if (!aht20_wait_for_measurement())
    {
        printf("[AHT20] wait for measurement failed\n");
        return;
    }

    float temperature = 0.0f, humidity = 0.0f;

    if (!aht20_read_measurement(&temperature, &humidity))
    {
        printf("[AHT20] read measurement failed\n");
        return;
    }

    if (temperature == last_temperature && humidity == last_humidity)
    {
        return;
    }

    last_temperature = temperature;
    last_humidity = humidity;

    printf("[AHT20] Temperature: %.1f, Humidity: %.1f\n", temperature, humidity);
    main_page_redraw_inner_temperature(temperature);
    main_page_redraw_inner_humidity(humidity);
}

// 更新室外天气，由 workqueue 任务执行：请求天气接口、解析响应，数据变化后刷新温度和图标。
static void outdoor_update(void)
{
    static weather_info_t last_weather = { 0 };

    weather_info_t weather = { 0 };
    // 天气接口地址由 app.h 中的 WEATHER_URL 配置，注释中不重复保存真实密钥。
    const char *weather_url = WEATHER_URL;

	// 返回指针指向 AT 驱动内部接收缓冲，应在下一次 AT 操作覆盖它之前完成解析。
	const char *weather_http_response = esp_at_http_get(weather_url);
    if (weather_http_response == NULL)
    {
        printf("[WEATHER] http error\n");
        return;
    }

    if (!parse_seniverse_response(weather_http_response, &weather))
    {
        printf("[WEATHER] parse failed\n");
        return;
    }

    if (memcmp(&last_weather, &weather, sizeof(weather_info_t)) == 0)
    {
        return;
    }

    memcpy(&last_weather, &weather, sizeof(weather_info_t));
    printf("[WEATHER] %s, %s, %.1f\n", weather.city, weather.weather, weather.temperature);

    main_page_redraw_outdoor_temperature(weather.temperature);
    main_page_redraw_outdoor_weather_icon(weather.weather_code);
}

// 业务函数指针类型：指向无参数、无返回值的函数，例如 time_update 或 inner_update。
typedef void (*app_job_t)(void);

// 工作队列的统一入口：从参数恢复业务函数指针，再在 workqueue 任务中调用它。
static void app_work(void *param)
{
    app_job_t job = (app_job_t)param;
    job();
}

// 耗时业务的定时器回调：在定时器服务任务中取出业务函数并投递给 workqueue。注意 workqueue_run 当前可能阻塞入队。
static void work_timer_cb(TimerHandle_t timer)
{
    app_job_t job = (app_job_t)pvTimerGetTimerID(timer);
    // 只提交 app_work 和 job，不在这里执行实际业务；workqueue 任务取出消息后调用 app_work(job)。
    workqueue_run(app_work, job);
}

// 轻量业务的定时器回调：取出并直接调用定时器 ID 中保存的函数，当前用于时间显示。
static void app_timer_cb(TimerHandle_t timer)
{
    app_job_t job = (app_job_t)pvTimerGetTimerID(timer);
    job();
}

// 应用初始化：创建定时器、提交首次业务刷新，再启动定时器。由 main_init 任务在主页面建立后调用。
void app_init(void)
{

    // xTimerCreate 的参数依次为名称、tick 周期、是否自动重载、用户 ID、回调。这里使用用户 ID 保存业务函数地址。
    time_update_timer = xTimerCreate("time update", pdMS_TO_TICKS(TIME_UPDATE_INTERVAL), pdTRUE, time_update, app_timer_cb);
    // 校时使用单次定时器，初始延时 200 ms；time_sync 完成后重新设置下一次等待时间。
    time_sync_timer = xTimerCreate("time sync", pdMS_TO_TICKS(200), pdFALSE, time_sync, work_timer_cb);
    wifi_update_timer = xTimerCreate("wifi update", pdMS_TO_TICKS(WIFI_UPDATE_INTERVAL), pdTRUE, wifi_update, work_timer_cb);
    inner_update_timer = xTimerCreate("inner upadte", pdMS_TO_TICKS(INNER_UPDATE_INTERVAL), pdTRUE, inner_update, work_timer_cb);
    outdoor_update_timer = xTimerCreate("outdoor update", pdMS_TO_TICKS(OUTDOOR_UPDATE_INTERVAL), pdTRUE, outdoor_update, work_timer_cb);

    // 主动提交首次刷新，避免室内和天气数据等待整个周期；这些调用仅入队，业务由工作任务随后执行。
    workqueue_run(app_work, time_sync);
    workqueue_run(app_work, wifi_update);
    workqueue_run(app_work, inner_update);
    workqueue_run(app_work, outdoor_update);

    // 向定时器服务任务发送启动命令；等待时间为 0。其余自动重载定时器按各自周期持续触发。
    xTimerStart(time_update_timer, 0);
    xTimerStart(time_sync_timer, 0);
    xTimerStart(wifi_update_timer, 0);
    xTimerStart(inner_update_timer, 0);
    xTimerStart(outdoor_update_timer, 0);
}
