/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2022 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "main.h"

#include "periph_adc_button.h"
#include "audio_mem.h"

#include "ssd1306.h"

static char *TAG = "esp32_speech_bot";

QueueHandle_t          	main_q      	= NULL;

static void log_setup(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("HTTP_STREAM", ESP_LOG_VERBOSE);


    esp_log_level_set("AUDIO_THREAD", ESP_LOG_ERROR);
    esp_log_level_set("I2C_BUS", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_HAL", ESP_LOG_ERROR);
    esp_log_level_set("ESP_AUDIO_TASK", ESP_LOG_ERROR);
    esp_log_level_set("ESP_DECODER", ESP_LOG_ERROR);
    esp_log_level_set("I2S", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_FORGE", ESP_LOG_ERROR);
    esp_log_level_set("ESP_AUDIO_CTRL", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_ERROR);
    esp_log_level_set("TONE_PARTITION", ESP_LOG_ERROR);
    esp_log_level_set("TONE_STREAM", ESP_LOG_ERROR);
    esp_log_level_set("MP3_DECODER", ESP_LOG_ERROR);
    esp_log_level_set("I2S_STREAM", ESP_LOG_ERROR);
    esp_log_level_set("RSP_FILTER", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_EVT", ESP_LOG_ERROR);
}

esp_err_t periph_callback(audio_event_iface_msg_t *event, void *context)
{
    ESP_LOGD(TAG, "Periph Event received: src_type:%x, source:%p cmd:%d, data:%p, data_len:%d",
        event->source_type, event->source, event->cmd, event->data, event->data_len);
    switch (event->source_type) {
        case PERIPH_ID_ADC_BTN:
            if (((int)event->data == get_input_rec_id()) && (event->cmd == PERIPH_ADC_BUTTON_PRESSED)) {
            	enable_wwe_trigger(true);
                ESP_LOGI(TAG, "REC KEY PRESSED");
            } else if (((int)event->data == get_input_rec_id()) &&
                        (event->cmd == PERIPH_ADC_BUTTON_RELEASE || event->cmd == PERIPH_ADC_BUTTON_LONG_RELEASE)) {
            	enable_wwe_trigger(false);
                ESP_LOGI(TAG, "REC KEY RELEASE");
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void app_main(void)
{
    log_setup();

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
//    periph_cfg.extern_stack = true;
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    if (set != NULL) {
        esp_periph_set_register_callback(set, periph_callback, NULL);
    }
    audio_board_sdcard_init(set, SD_MODE_1_LINE);
    audio_board_key_init(set);

	SSD1306_t dev;
	ssd1306_init(&dev, 128, 64);
	ssd1306_clear_screen(&dev, false);
	ssd1306_contrast(&dev, 0xff);
	ssd1306_display_text(&dev, 0, "Hello", 5, false);

    init_wifi_work(set);
    init_wwe_work();
    init_file2http();
    init_http2file();
    init_http2player();

    main_q = xQueueCreate(8, sizeof(main_msg_t));
    main_msg_t msg;

    while (1) {

        if (xQueueReceive(main_q, &msg, portMAX_DELAY) == pdTRUE) {
            switch (msg.msg_id) {
                case FILE2HTTP:
                    ESP_LOGI(TAG, "Upload file: %s to %s.", msg.src, msg.dst);
                    // MUST be disable wakenet !
					enable_wwe_pipeline(false);
					enable_file2http(true);
                    run_file2http(msg.src, msg.dst);
                    enable_file2http(false);
					enable_wwe_pipeline(true);
                    break;
                case HTTP2FILE:
                    ESP_LOGI(TAG, "Download file: %s", msg.msg);
                    break;
                case FILE2PLAYER:
                    ESP_LOGI(TAG, "Play local file: %s", msg.src);
					enable_wwe_pipeline(false);
					enable_file2player(true);
					run_file2player(msg.src, msg.dst);
					enable_file2player(false);
					enable_wwe_pipeline(true);
                    break;
                case HTTP2PLAYER:
                    ESP_LOGI(TAG, "Play online file: %s", msg.src);
					enable_wwe_pipeline(false);
					enable_http2player(true);
					run_http2player(msg.src, msg.dst);
					enable_http2player(false);
					enable_wwe_pipeline(true);
                    break;
                default:
                    break;
            }
        }
    }

    deinit_file2http();
}
