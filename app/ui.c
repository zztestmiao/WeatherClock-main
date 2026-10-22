#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "st7789.h"
#include "ui.h"
#include "font.h"
#include "image.h"


//定义ui_action_t类型结构体，包含要做什么的信息
typedef enum
{
    UI_ACTION_FILL_COLOR,
    UI_ACTION_WRITE_STRING,
    UI_ACTION_DRAW_IMAGE,
} ui_action_t;

//ui_message_t结构体，用来操作UI函数
typedef struct
{
    ui_action_t action;	// ① 动作类型：这条消息要干嘛
	
    union				// ② 参数联合：不同动作的参数包，共用一块内存
    {
        struct
        {
            uint16_t x;
            uint16_t y;
            uint16_t width;
            uint16_t height;
            uint16_t color;
        } fill_color;
        struct
        {
            uint16_t x;
            uint16_t y;
            const char *str;
            uint16_t color;
            uint16_t bg_color;
            const font_t *font;
        } write_string;
        struct
        {
            uint16_t x;
            uint16_t y;
            const image_t *image;
        } draw_image;
    };
} ui_message_t;

//定义一个队列 ui_queue
static QueueHandle_t ui_queue;

//ui任务函数
/* 主要实现：画出背景，写字符串，画图片这三个功能 */
//始终循环运行读队列 读到msg后调用st7789驱动函数画出对应的图像
static void ui_func(void *param)
{
    ui_message_t msg;
    
    st7789_init();
    
    while (1)
    {
        xQueueReceive(ui_queue, &msg, portMAX_DELAY);//读ui队列，将读到的信息放入msg缓冲区
        
        switch (msg.action)
        {
        case UI_ACTION_FILL_COLOR:
            st7789_fill_color(msg.fill_color.x, msg.fill_color.y,
                              msg.fill_color.width, msg.fill_color.height,
                              msg.fill_color.color);
            break;
        case UI_ACTION_WRITE_STRING:
            st7789_write_string(msg.write_string.x, msg.write_string.y,
                                msg.write_string.str,
                                msg.write_string.color, msg.write_string.bg_color,
                                msg.write_string.font);
            vPortFree((void*)msg.write_string.str);//释放内存
            break;
        case UI_ACTION_DRAW_IMAGE:
            st7789_draw_image(msg.draw_image.x, msg.draw_image.y,
                              msg.draw_image.image);
            break;
        default:
            printf("Unknown UI action: %d\n", msg.action);
            break;
        }
    }
}


//UI初始化 创建ui任务
void ui_init(void)
{
    ui_queue = xQueueCreate(16, sizeof(ui_message_t));//创建UI队列
	
    configASSERT(ui_queue);//检查一下
	
    xTaskCreate(ui_func, "ui", 1024, NULL, 8, NULL);//创建UI任务	
}


//#1.UI填充颜色
void ui_fill_color(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color)
{
    ui_message_t msg;
	
    msg.action = UI_ACTION_FILL_COLOR;
    msg.fill_color.x = x;
    msg.fill_color.y = y;
    msg.fill_color.width = width;
    msg.fill_color.height = height;
    msg.fill_color.color = color;
    
    xQueueSend(ui_queue, &msg, portMAX_DELAY);//写入队列
}

//#2.UI写字符串
void ui_write_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color, const font_t *font)
{
    char *pstr = pvPortMalloc(strlen(str) + 1);//创建内存
    if (pstr == NULL)
    {
        printf("ui write string malloc failed: %s", str);
        return;
    }
    strcpy(pstr, str);//将str内容拷贝进pstr
	
    ui_message_t msg;
    msg.action = UI_ACTION_WRITE_STRING;
    msg.write_string.x = x;
    msg.write_string.y = y;
    msg.write_string.str = pstr;
    msg.write_string.color = color;
    msg.write_string.bg_color = bg_color;
    msg.write_string.font = font;
    
    xQueueSend(ui_queue, &msg, portMAX_DELAY);//写入队列
}

//#3.UI画图片
void ui_draw_image(uint16_t x, uint16_t y, const image_t *image)
{
    ui_message_t msg;
    msg.action = UI_ACTION_DRAW_IMAGE;
    msg.draw_image.x = x;
    msg.draw_image.y = y;
    msg.draw_image.image = image;
    
    xQueueSend(ui_queue, &msg, portMAX_DELAY);//写入队列
}
