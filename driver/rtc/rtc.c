#include <string.h>
#include "stm32f4xx.h"
#include "rtc.h"

//rtc时钟初始化函数
void rtc_init(void)
{
    RTC_InitTypeDef RTC_InitStruct;
    RTC_StructInit(&RTC_InitStruct);//先设置初始值
    RTC_Init(&RTC_InitStruct);
    
    RCC_RTCCLKCmd(ENABLE);
    RTC_WaitForSynchro();
}

//设置一次时间
static void _rtc_set_time_once(const rtc_date_time_t *date_time)
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;
    
    RTC_DateStructInit(&date);
    RTC_TimeStructInit(&time);
    
    date.RTC_Year = date_time->year - 2000;
    date.RTC_Month = date_time->month;
    date.RTC_Date = date_time->day;
    date.RTC_WeekDay = date_time->weekday;
	
    time.RTC_Hours = date_time->hour;
    time.RTC_Minutes = date_time->minute;
    time.RTC_Seconds = date_time->second;
    
    RTC_SetDate(RTC_Format_BIN, &date);
    RTC_SetTime(RTC_Format_BIN, &time);
    
}
//获取一次时间
static void _rtc_get_time_once(rtc_date_time_t *date_time)
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;
    
    RTC_DateStructInit(&date);
    RTC_TimeStructInit(&time);
    
    RTC_GetDate(RTC_Format_BIN, &date);
    RTC_GetTime(RTC_Format_BIN, &time);
    
    date_time->year = 2000 + date.RTC_Year;
    date_time->month = date.RTC_Month;
    date_time->day = date.RTC_Date;
    date_time->weekday = date.RTC_WeekDay;
    date_time->hour = time.RTC_Hours;
    date_time->minute = time.RTC_Minutes;
    date_time->second = time.RTC_Seconds;
}
//设置时间
void rtc_set_time(const rtc_date_time_t *date_time)
{
    rtc_date_time_t rtime;
    do {
        _rtc_set_time_once(date_time);  
        _rtc_get_time_once(&rtime);		
    } while (date_time->second != rtime.second);
}
//获取时间
void rtc_get_time(rtc_date_time_t *date_time)
{
    rtc_date_time_t time1, time2;// 两个临时变量，分别存放两次读取的结果
    do {
        _rtc_get_time_once(&time1);// 第一次读取 RTC 硬件时间
        _rtc_get_time_once(&time2);// 紧接着第二次读取 RTC 硬件时间
    } while (memcmp(&time1, &time2, sizeof(rtc_date_time_t)) != 0);// 两次结果不一致 → 说明读取过程中时间发生了跳变，循环重读
    
    memcpy(date_time, &time1, sizeof(rtc_date_time_t));
	// 两次结果完全一致 → 把正确时间拷贝给输出参数
}
