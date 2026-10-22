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

#define MILLISECONDS(x) (x)
#define SECONDS(x)      MILLISECONDS((x) * 1000)
#define MINUTES(x)      SECONDS((x) * 60)
#define HOURS(x)        MINUTES((x) * 60)
#define DAYS(x)          HOURS((x) * 24)

//时间�?
#define TIME_SYNC_INTERVAL          HOURS(1)
#define WIFI_UPDATE_INTERVAL        SECONDS(5)
#define TIME_UPDATE_INTERVAL        SECONDS(1)
#define INNER_UPDATE_INTERVAL       SECONDS(3)
#define OUTDOOR_UPDATE_INTERVAL     MINUTES(1)

#define MLOOP_EVT_TIME_SYNC         (1 << 0)
#define MLOOP_EVT_WIFI_UPDATE       (1 << 1)
#define MLOOP_EVT_INNER_UPDATE      (1 << 2)
#define MLOOP_EVT_OUTDOOR_UPDATE    (1 << 3)
#define MLOOP_EVT_ALL               (MLOOP_EVT_TIME_SYNC | \
                                     MLOOP_EVT_WIFI_UPDATE | \
                                     MLOOP_EVT_INNER_UPDATE | \
                                     MLOOP_EVT_OUTDOOR_UPDATE)
//定义�?件定时器的句�?
static TimerHandle_t time_sync_timer;
static TimerHandle_t wifi_update_timer;
static TimerHandle_t time_update_timer;
static TimerHandle_t inner_update_timer;
static TimerHandle_t outdoor_update_timer;

//网络校准时间函数 1小时更新一�?
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
    
    rtc_date.year = esp_date.year;
    rtc_date.month = esp_date.month;
    rtc_date.day = esp_date.day;
    rtc_date.hour = esp_date.hour;
    rtc_date.minute = esp_date.minute;
    rtc_date.second = esp_date.second;
    rtc_date.weekday = esp_date.weekday;
    rtc_set_time(&rtc_date);
    
err:
    xTimerChangePeriod(time_sync_timer, pdMS_TO_TICKS(restart_sync_delay), 0);
}

//wifi更新函数
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


//系统时间显示函数 读取rtc�?件里面的时钟，完成显�?  1秒刷新一�?
static void time_update(void)
{
    static rtc_date_time_t last_date = { 0 };//上一�?时间
    
    rtc_date_time_t date;//现在的时�?
    rtc_get_time(&date);//�?1】�?�取�?件RTC当前时间，放到date结构�?
    
	//�?2】合法性校验：年份小于2020，�?�为RTC时间�?初�?�化/异常，直接退出，不刷新UI
    if (date.year < 2020)
    {
        return;
    }
    
	//�?3】�?�比：当前时间date �? 上一次保存的last_da
	// memcmp：内存比较，两块内存完全相等返回0te
    if (memcmp(&date, &last_date, sizeof(rtc_date_time_t)) == 0)
    {
		// 时间没有变化，不需要刷新屏幕，直接return
        return;
    }
	
    //�?4】走到这里代�?：时间发生了变化
    // 把当前最新时间，保存到last_date，作为下一次�?�比的“旧时间�?
    memcpy(&last_date, &date, sizeof(rtc_date_time_t));
	
	//�?5】调用UI绘制函数，刷新屏幕时间、日�?
    main_page_redraw_time(&date);
    main_page_redraw_date(&date);
}

//室内温度更新函数
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

//室�?�温度更新函�?
static void outdoor_update(void)
{
    static weather_info_t last_weather = { 0 };
    
    weather_info_t weather = { 0 };
    const char *weather_url = WEATHER_URL;//保存天气api
    //我的�?"https://api.seniverse.com/v3/weather/now.json?key=SJmqOCVX6Nb3dnjsm&location=wuhan&language=en&unit=c"
	//梅花�?"https://api.seniverse.com/v3/weather/now.json?key=SfRic8Wmp-Qh3OeFk&location=WTEMH46Z5N09&language=en&unit=c"
	const char *weather_http_response = esp_at_http_get(weather_url);//获取api返回的信�?
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

typedef void (*app_job_t)(void);//定义函数指针类型 app_job_t

//#1.1 较长时间的回调函数最终调用的函数
static void app_work(void *param)
{
    app_job_t job = (app_job_t)param;//定义函数指针job 赋值为定时器的ID，也就是传入的参�?
    job();//调用job函数
}

//#1. work_timer回调函数 较长时间
static void work_timer_cb(TimerHandle_t timer)
{
    app_job_t job = (app_job_t)pvTimerGetTimerID(timer);
    workqueue_run(app_work, job);//这里的app_work实际上就�?我们最后�?�运行的函数 传入的参数是job
}

//#2. app_timer回调函数 轻量时间
static void app_timer_cb(TimerHandle_t timer)
{
    app_job_t job = (app_job_t)pvTimerGetTimerID(timer);//获取定时器的ID 实际上是获取了time_update函数的地址
    job();//调用job函数 �?质上�?在执行定时器ID地址的函�? 也就是time_updata函数
}

//qpp初�?�化函数 创建定时�?，投入工作队列，�?动定时器
void app_init(void)
{
	//动态创�?5�?�?件定时器  xTimerCreate("名称", 周期, �?否自动重�?, 定时器ID, 回调函数)
    time_update_timer = xTimerCreate("time update", pdMS_TO_TICKS(TIME_UPDATE_INTERVAL), pdTRUE, time_update, app_timer_cb);
    time_sync_timer = xTimerCreate("time sync", pdMS_TO_TICKS(200), pdFALSE, time_sync, work_timer_cb);
    wifi_update_timer = xTimerCreate("wifi update", pdMS_TO_TICKS(WIFI_UPDATE_INTERVAL), pdTRUE, wifi_update, work_timer_cb);
    inner_update_timer = xTimerCreate("inner upadte", pdMS_TO_TICKS(INNER_UPDATE_INTERVAL), pdTRUE, inner_update, work_timer_cb);
    outdoor_update_timer = xTimerCreate("outdoor update", pdMS_TO_TICKS(OUTDOOR_UPDATE_INTERVAL), pdTRUE, outdoor_update, work_timer_cb);
	
	//耗时长的丢入工作队列里面 先�?�这几个任务跑�??一�? 不然有的定时器时间太长了
    workqueue_run(app_work, time_sync);//实际上就�?写队�? 将这两个参数放入结构体写入队�?
    workqueue_run(app_work, wifi_update);//这个函数的意思就�?运�?�app_work() 参数就是wifi_update
    workqueue_run(app_work, inner_update);
    workqueue_run(app_work, outdoor_update);
    
	//�?�?5�?定时�?
    xTimerStart(time_update_timer, 0);
    xTimerStart(time_sync_timer, 0);
    xTimerStart(wifi_update_timer, 0);
    xTimerStart(inner_update_timer, 0);
    xTimerStart(outdoor_update_timer, 0);
}
