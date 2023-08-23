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

static const char *TAG = "http2file";

static audio_pipeline_handle_t http2file_pipeline;
static audio_element_handle_t http_stream_reader;
static audio_element_handle_t fatfs_stream_writer;
static audio_event_iface_handle_t http2file_evt;

void init_http2file(){
    ESP_LOGI(TAG, "[1.0] Initialize peripherals management");
//    ESP_LOGI(TAG, "[1.1] Initialize and start peripherals");


    ESP_LOGI(TAG, "[3.0] Create http2file pipeline for download");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    http2file_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(http2file_pipeline);

    ESP_LOGI(TAG, "[3.1] Create http stream to read data");
	http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
	http_stream_reader = http_stream_init(&http_cfg);

    ESP_LOGI(TAG, "[3.2] Create fatfs stream to write data to sdcard");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_WRITER;
    fatfs_stream_writer = fatfs_stream_init(&fatfs_cfg);

	ESP_LOGI(TAG, "[3.3] Register all elements to http2file pipeline");
	audio_pipeline_register(http2file_pipeline, http_stream_reader, "http");
	audio_pipeline_register(http2file_pipeline, fatfs_stream_writer, "fatfs");

	ESP_LOGI(TAG, "[3.4] Link it together [http_server]-->http_stream-->fatfs_stream-->[sdcard]");
	const char *link_tag[2] = {"http", "fatfs"};
	audio_pipeline_link(http2file_pipeline, &link_tag[0], 2);

//	ESP_LOGI(TAG, "[3.6] Set up  uri (http as http_stream, amr as amr decoder, and default output is i2s)");
//	audio_element_set_uri(http_stream_reader, "https://dl.espressif.com/dl/audio/ff-16b-1c-8000hz.amr");
//	audio_element_set_uri(http_stream_reader, "http://192.168.1.104:9000/test.mp3");


    ESP_LOGI(TAG, "[4.0] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    http2file_evt = audio_event_iface_init(&evt_cfg);

//    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
//    audio_pipeline_set_listener(http2player_pipeline, http2player_evt);

}

void deinit_http2file(){

    ESP_LOGI(TAG, "[ 7.1 ] Stop http2file_pipeline");
    audio_pipeline_stop(http2file_pipeline);
    audio_pipeline_wait_for_stop(http2file_pipeline);
    audio_pipeline_terminate(http2file_pipeline);

    ESP_LOGI(TAG, "[ 7.3 ] Unregister http2file_pipeline");
    audio_pipeline_unregister(http2file_pipeline, fatfs_stream_writer);
    audio_pipeline_unregister(http2file_pipeline, http_stream_reader);


    /* Terminate the pipeline before removing the listener */
    ESP_LOGI(TAG, "[ 7.4 ] Removing pipeline listener");
    audio_pipeline_remove_listener(http2file_pipeline);

    /* Stop all peripherals before removing the listener */

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(http2file_evt);

    /* Release all resources */
    audio_pipeline_deinit(http2file_pipeline);
    audio_element_deinit(fatfs_stream_writer);
    audio_element_deinit(http_stream_reader);
}

void run_http2file(const char *src_url, const char *dst_url){

	audio_element_state_t el_state = audio_element_get_state(fatfs_stream_writer);
	if(AEL_STATE_RUNNING == el_state){
	    audio_pipeline_stop(http2file_pipeline);
	    ESP_LOGI(TAG, "[7.x] audio_pipeline_stop done: %d", __LINE__);
	    audio_pipeline_wait_for_stop(http2file_pipeline);
	    ESP_LOGI(TAG, "[7.x] audio_pipeline_wait_for_stop done: %d", __LINE__);
	}
    audio_pipeline_reset_ringbuffer(http2file_pipeline);
    ESP_LOGI(TAG, "[7.x] audio_pipeline_reset_ringbuffer done: %d", __LINE__);
    audio_pipeline_reset_elements(http2file_pipeline);
    ESP_LOGI(TAG, "[7.x] audio_pipeline_reset_elements done: %d", __LINE__);
    audio_pipeline_terminate(http2file_pipeline);
    ESP_LOGI(TAG, "[7.x] audio_pipeline_terminate done: %d", __LINE__);
    ESP_LOGI(TAG, "[7.2] Set http_stream_reader URL: %s", src_url);
	audio_element_set_uri(http_stream_reader, src_url);
    ESP_LOGI(TAG, "[7.2] Set fatfs_stream_writer URL: %s", dst_url);
    audio_element_set_uri(fatfs_stream_writer, dst_url);

    ESP_LOGI(TAG, "[7.3] Listen for all file2http_pipeline events (set it after pipeline_link)");
    audio_pipeline_set_listener(http2file_pipeline, http2file_evt);
    audio_pipeline_change_state(http2file_pipeline, AEL_STATE_INIT);
    audio_pipeline_run(http2file_pipeline);

//    char* uri = audio_element_get_uri(http_stream_reader);
//    ESP_LOGE(TAG, "[7.4] Get http_stream_reader URL: %s", uri);
    ESP_LOGI(TAG, "[7.x] audio_pipeline_run done: %d", __LINE__);

	while(1){
		audio_event_iface_msg_t msg;
		esp_err_t ret = audio_event_iface_listen(http2file_evt, &msg, portMAX_DELAY);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
			break;
		}
		if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
			// Advance to the next song when previous finishes
			if (msg.source == (void *) fatfs_stream_writer
				&& msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
				audio_element_state_t el_state = audio_element_get_state(fatfs_stream_writer);
				if (el_state == AEL_STATE_FINISHED) {
					ESP_LOGI(TAG, "[ * ] Finished,");
					break;
				}
			}
		} else {
			ESP_LOGI(TAG, "[ * ] CMD:%d Type: %d,", msg.cmd, msg.source_type);
		}
	}
}

void enable_http2file(bool enable){
	if(enable){
	    ESP_LOGI(TAG, "Enable http2player_pipeline.");
	    audio_pipeline_change_state(http2file_pipeline, AEL_STATE_INIT);
		audio_pipeline_reset_ringbuffer(http2file_pipeline);
		audio_pipeline_reset_elements(http2file_pipeline);
		audio_pipeline_resume(http2file_pipeline);

	} else {
	    ESP_LOGI(TAG, "Disable http2player_pipeline.");
	    audio_pipeline_pause(http2file_pipeline);
	}
}

