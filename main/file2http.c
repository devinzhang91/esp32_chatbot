#include "main.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_mem.h"
#include "audio_thread.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "wav_decoder.h"
#include "filter_resample.h"
#include "http_stream.h"
#include "sdkconfig.h"

#include "esp_peripherals.h"
#include "esp_http_client.h"
#include "periph_sdcard.h"
#include "board.h"

#include "sdcard_list.h"
#include "sdcard_scan.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "file2http";

static audio_pipeline_handle_t file2http_pipeline;
static audio_element_handle_t fatfs_stream_reader;
static audio_element_handle_t http_stream_writer;
static audio_event_iface_handle_t file2http_evt;

static playlist_operator_handle_t sdcard_list_handle = NULL;

static void _sdcard_url_save_cb(void *user_data, char *url) {
    playlist_operator_handle_t sdcard_handle = (playlist_operator_handle_t)user_data;
    esp_err_t ret = sdcard_list_save(sdcard_handle, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to save sdcard url to sdcard playlist");
    }
//    ESP_LOGW(TAG, "sdcard url saved: %s ", url);
}

esp_err_t _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    char len_buf[16];
    static int total_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d", msg->buffer_len);
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        char dat[10] = {0};
        snprintf(dat, sizeof(dat), "%d", CONFIG_AUDIO_SAMPLE_RATE);
        esp_http_client_set_header(http, "x-audio-sample-rates", dat);
        memset(dat, 0, sizeof(dat));
        snprintf(dat, sizeof(dat), "%d", CONFIG_AUDIO_BITS);
        esp_http_client_set_header(http, "x-audio-bits", dat);
        memset(dat, 0, sizeof(dat));
        snprintf(dat, sizeof(dat), "%d", CONFIG_AUDIO_CHANNELS);
        esp_http_client_set_header(http, "x-audio-channel", dat);
        total_write = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST) {
        // write data
        int wlen = sprintf(len_buf, "%x\r\n", msg->buffer_len);
        if (esp_http_client_write(http, len_buf, wlen) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, msg->buffer, msg->buffer_len) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, "\r\n", 2) <= 0) {
            return ESP_FAIL;
        }
        total_write += msg->buffer_len;
        printf("\033[A\33[2K\rTotal bytes written: %d\n", total_write);
        return msg->buffer_len;
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST");
        char *buf = audio_calloc(1, 128);
        assert(buf);
        int read_len = esp_http_client_read(http, buf, 128);
        if (read_len <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        buf[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char *)buf);
        ///test
//        static char file_name[64];
//        sprintf(file_name, "http://192.168.1.110:9001/%s", buf);
//        ESP_LOGI(TAG, "HTTP2PLAYER file name: %s", file_name);
//        main_msg_t msg = {
//        		.msg_id = HTTP2PLAYER,
//        		.src = file_name,
//        		.dst = "",
//        };
//        if (xQueueSend(main_q, &msg, 0) != pdPASS) {
//            ESP_LOGE(TAG, "main queue send failed");
//        }
        ///end test
        audio_free(buf);
        return ESP_OK;
    }
    return ESP_OK;
}

void init_file2http(){
    ESP_LOGI(TAG, "[1.0] Initialize peripherals management");
//    ESP_LOGI(TAG, "[1.1] Initialize and start peripherals");

    ESP_LOGI(TAG, "[1.2] Set up a sdcard playlist and scan sdcard music save to it");
    sdcard_list_create(&sdcard_list_handle);
    sdcard_scan(_sdcard_url_save_cb, "/sdcard", 0, (const char *[]) {"wav"}, 1, sdcard_list_handle);
    sdcard_list_show(sdcard_list_handle);

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
//    audio_board_handle_t board_handle = audio_board_init();
//    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[4.0] Create file2http pipeline for upload");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    file2http_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(file2http_pipeline);


    ESP_LOGI(TAG, "[4.4] Create fatfs stream to read data from sdcard");
    char *url = NULL;
    sdcard_list_current(sdcard_list_handle, &url);
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

    ESP_LOGI(TAG, "[4.7] Create http stream to post data to server");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_WRITER;
    http_cfg.event_handle = _http_stream_event_handle;
    http_stream_writer = http_stream_init(&http_cfg);

    ESP_LOGI(TAG, "[4.8] Register all elements to audio pipeline");
    audio_pipeline_register(file2http_pipeline, fatfs_stream_reader, "fatfs");
    audio_pipeline_register(file2http_pipeline, http_stream_writer, "http");

    ESP_LOGI(TAG, "[4.9] Link it together [sdcard]-->fatfs_stream-->http_stream->[http_server]");
    const char *link_tag2[2] = {"fatfs", "http"};
    //link later when file2http_pipeline will use
    audio_pipeline_link(file2http_pipeline, &link_tag2[0], 2);

    ESP_LOGI(TAG, "[5.0] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    file2http_evt = audio_event_iface_init(&evt_cfg);

}

void deinit_file2http(){

    ESP_LOGI(TAG, "[ 7.2 ] Stop file2http_pipeline");
    audio_pipeline_stop(file2http_pipeline);
    audio_pipeline_wait_for_stop(file2http_pipeline);
    audio_pipeline_terminate(file2http_pipeline);

    ESP_LOGI(TAG, "[ 7.4 ] Unregister file2http_pipeline");
    audio_pipeline_unregister(file2http_pipeline, http_stream_writer);
    audio_pipeline_unregister(file2http_pipeline, fatfs_stream_reader);

    /* Terminate the pipeline before removing the listener */
    ESP_LOGI(TAG, "[ 7.5 ] Removing pipeline listener");
    audio_pipeline_remove_listener(file2http_pipeline);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(file2http_evt);

    /* Release all resources */
    sdcard_list_destroy(sdcard_list_handle);
    audio_pipeline_deinit(file2http_pipeline);
    audio_element_deinit(http_stream_writer);
    audio_element_deinit(fatfs_stream_reader);
}

void run_file2http(const char *src_url, const char *dst_url){
    ESP_LOGI(TAG, "[7.0] Link it together [sdcard]-->fatfs_stream-->http_stream->[http_server]");

    /*
     * There is no effect when follow APIs output warning message on the first time record
     */
	audio_element_state_t el_state = audio_element_get_state(http_stream_writer);
	if(AEL_STATE_RUNNING == el_state){
	    audio_pipeline_stop(file2http_pipeline);
	    ESP_LOGI(TAG, "[7.x] audio_pipeline_stop done: %d", __LINE__);
	    audio_pipeline_wait_for_stop(file2http_pipeline);
	    ESP_LOGI(TAG, "[7.x] audio_pipeline_wait_for_stop done: %d", __LINE__);
	}
    audio_pipeline_reset_ringbuffer(file2http_pipeline);
    ESP_LOGI(TAG, "[7.x] audio_pipeline_reset_ringbuffer done: %d", __LINE__);
    audio_pipeline_reset_elements(file2http_pipeline);
    ESP_LOGI(TAG, "[7.x] audio_pipeline_reset_elements done: %d", __LINE__);
    audio_pipeline_terminate(file2http_pipeline);
    ESP_LOGI(TAG, "[7.x] audio_pipeline_terminate done: %d", __LINE__);
    ESP_LOGI(TAG, "[7.2] Set fatfs_stream_reader URL: %s", src_url);
	audio_element_set_uri(fatfs_stream_reader, src_url);
    audio_element_set_uri(http_stream_writer, dst_url);

    ESP_LOGI(TAG, "[7.3] Listen for all file2http_pipeline events (set it after pipeline_link)");
    audio_pipeline_set_listener(file2http_pipeline, file2http_evt);
    audio_pipeline_change_state(file2http_pipeline, AEL_STATE_INIT);
    audio_pipeline_run(file2http_pipeline);
    ESP_LOGI(TAG, "[7.x] audio_pipeline_run done: %d", __LINE__);

	while(1){
		audio_event_iface_msg_t msg;
		esp_err_t ret = audio_event_iface_listen(file2http_evt, &msg, portMAX_DELAY);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
			break;
		}
		if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
			if (msg.source == (void *) http_stream_writer
				&& msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
				audio_element_state_t el_state = audio_element_get_state(http_stream_writer);
				if (el_state == AEL_STATE_FINISHED) {
					ESP_LOGI(TAG, "[ * ] Finished,");
	                audio_element_set_ringbuf_done(fatfs_stream_reader);
					break;
				}
			}
		} else {
			ESP_LOGI(TAG, "[ * ] CMD:%d Type: %d,", msg.cmd, msg.source_type);
		}
	}

	ESP_LOGI(TAG, "[7.5] Exit file2http_wav...");
}

void enable_file2http(bool enable){
	if(enable){
	    ESP_LOGI(TAG, "Enable file2http_pipeline.");
		audio_pipeline_resume(file2http_pipeline);

	} else {
	    ESP_LOGI(TAG, "Disable file2http_pipeline.");
	    audio_pipeline_pause(file2http_pipeline);
	}
}
