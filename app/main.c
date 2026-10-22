#include "FreeRTOS.h"
#include "task.h"
#include "workqueue.h"
#include "app.h"
#include "ui.h"
#include "wifi.h"
#include "page.h"

extern void board_lowlevel_init(void);
extern void board_init(void);

static void main_init(void *param)
{
    board_init();
	
    ui_init();//ui初始化 创建了第三个ui任务

    welcome_page_display();//#1.显示欢迎界面

    wifi_init();
    wifi_page_display();//#2.显示wifi配置界面
    wifi_wait_connect();

    main_page_display();//#3.主页面显示 第一次初始化
	
    app_init();//app初始化，开启后马上刷新一次数据，之后按照规定的周期刷新

    vTaskDelete(NULL);
}

int main(void)
{
    board_lowlevel_init();//板级初始化
	
    workqueue_init();//工作队列初始化 创建了第一个workqueue任务

    xTaskCreate(main_init, "init", 1024, NULL, 9, NULL);//创建第二个任务main_init

    vTaskStartScheduler();//开启调度器

    while (1)
    {
        ; // code should not run here
    }
}
