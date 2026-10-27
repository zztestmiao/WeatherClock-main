#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "weather.h"

bool parse_seniverse_response(const char *response, weather_info_t *info)
{
	// 先定位响应中的 results 区域；后续查找都从这里开始。
	response = strstr(response, "\"results\":");
	if (response == NULL)
		return false;
	
	// location 对象包含地点信息；找不到它就无法继续解析预期的响应结构。
	const char *location_response = strstr(response, "\"location\":");
	if (location_response == NULL)
		return false;
	
	// 读取地点名称，保存到 city 字段。
	const char *loaction_name_response = strstr(location_response, "\"name\":");
	if (loaction_name_response)
	{
		// %31 限制读入字符数，避免超过 city 数组的长度。
		sscanf(loaction_name_response, "\"name\": \"%31[^\"]\"", info->city);
	}
	
	// 读取地点路径，保存到 loaction 字段。
	const char *loaction_path_response = strstr(location_response, "\"path\":");
	if (loaction_path_response)
	{
		sscanf(loaction_path_response, "\"path\": \"%128[^\"]\"", info->loaction);
	}
	
	// now 对象包含当前天气；找不到它时，响应不符合预期结构。
	const char *now_response = strstr(response, "\"now\":");
	if (now_response == NULL)
		return false;
	
	// 读取天气文字描述，例如晴、多云等。
	const char *now_text_response = strstr(now_response, "\"text\":");
	if (now_text_response)
	{
		sscanf(now_text_response, "\"text\": \"%15[^\"]\"", info->weather);
	}
	
	// 读取天气代码，并将字符串形式的数字写入整数变量。
	const char *now_code_response = strstr(now_response, "\"code\":");
	if (now_code_response)
	{
		sscanf(now_code_response, "\"code\": \"%d\"", &info->weather_code);
	}
	
	// 先用字符数组接收温度文本，再转换为浮点数。
	char temperature_str[16] = { 0 };
	const char *now_temperature_response = strstr(now_response, "\"temperature\":");
	if (now_temperature_response)
	{
		// 只有成功读到温度文本后才更新 temperature。
		if (sscanf(now_temperature_response, "\"temperature\": \"%15[^\"]\"", temperature_str) == 1)
			info->temperature = atof(temperature_str);
	}
	
	// 找到了 results、location 和 now；其他字段即使缺失也不会影响此返回值。
	return true;
}
