#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "workqueue.h"

//定义工作信息结构体 
typedef struct
{
    work_t work;
    void *param;
} work_message_t;

static QueueHandle_t work_msg_queue;//定义队列句柄 work_msg_queue

//工作任务函数 一直在运行不停读队列 运行work(param)
static void work_func(void *param)
{
    work_message_t msg;
    
    while (1)
    {
        xQueueReceive(work_msg_queue, &msg, portMAX_DELAY);//工作任务函数一直在读队列
        msg.work(msg.param);//执行函数指针
    }
}

//工作队列初始化函数
void workqueue_init(void)
{
    work_msg_queue = xQueueCreate(16, sizeof(work_message_t));//创建工作队列
    configASSERT(work_msg_queue);
    xTaskCreate(work_func, "workqueue", 1024, NULL, 5, NULL);//创建工作任务
}

//工作队列运行函数
void workqueue_run(work_t work, void *param)
{
    configASSERT(work_msg_queue);
    work_message_t msg = { work, param };//将传入的两个参数打包进结构体变量
    xQueueSend(work_msg_queue, &msg, portMAX_DELAY);//将两个参数发送到队列 满了阻塞
}
