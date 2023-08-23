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

static const char *TAG = "file2player";

static audio_pipeline_handle_t file2player_pipeline;
static audio_element_handle_t i2s_stream_writer;
static audio_element_handle_t wav_decoder;
static audio_element_handle_t fatfs_stream_reader;
static audio_element_handle_t rsp_handle;
static audio_event_iface_handle_t file2player_evt;
static playlist_operator_handle_t sdcard_list_handle = NULL;

static void _sdcard_url_save_cb(void *user_data, char *url) {
    playlist_operator_handle_t sdcard_handle = (playlist_operator_handle_t)user_data;
    esp_err_t ret = sdcard_list_save(sdcard_handle, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to save sdcard url to sdcard playlist");
    }
//    ESP_LOGW(TAG, "sdcard url saved: %s ", url);
}

void init_file2player(){
    ESP_LOGI(TAG, "[1.0] Initialize peripherals management");
//    ESP_LOGI(TAG, "[1.1] Initialize and start peripherals");

    ESP_LOGI(TAG, "[1.2] Set up a sdcard playlist and scan sdcard music save to it");
    sdcard_list_create(&sdcard_list_handle);
    sdcard_scan(_sdcard_url_save_cb, "/sdcard", 0, (const char *[]) {"wav"}, 1, sdcard_list_handle);
    sdcard_list_show(sdcard_list_handle);

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
//    audio_board_handle_t board_handle = audio_board_init();
//    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);


    ESP_LOGI(TAG, "[4.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    file2player_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(file2player_pipeline);


    ESP_LOGI(TAG, "[4.1] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.i2s_config.sample_rate = 48000;
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[4.2] Create wav decoder to decode wav file");
    wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
    wav_decoder = wav_decoder_init(&wav_cfg);

    ESP_LOGI(TAG, "[4.3] Create resample filter");
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = CONFIG_AUDIO_SAMPLE_RATE;
    rsp_cfg.src_bits = CONFIG_AUDIO_BITS;
    rsp_cfg.src_ch = CONFIG_AUDIO_CHANNELS;
    rsp_handle = rsp_filter_init(&rsp_cfg);

    ESP_LOGI(TAG, "[4.4] Create fatfs stream to read data from sdcard");
    char *url = NULL;
    sdcard_list_current(sdcard_list_handle, &url);
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);
//    audio_element_set_uri(fatfs_stream_reader, url);

    ESP_LOGI(TAG, "[4.5] Register all elements to audio pipeline");
    audio_pipeline_register(file2player_pipeline, fatfs_stream_reader, "fatfs");
    audio_pipeline_register(file2player_pipeline, wav_decoder, "wav");
    audio_pipeline_register(file2player_pipeline, rsp_handle, "filter");
    audio_pipeline_register(file2player_pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[4.6] Link it together [sdcard]-->fatfs_stream-->wav_decoder-->resample-->i2s_stream-->[codec_chip]");
    const char *link_tag1[4] = {"fatfs", "wav", "filter", "i2s"};
    // link later when file_i2s_pipeline will use
    audio_pipeline_link(file2player_pipeline, &link_tag1[0], 4);


    ESP_LOGI(TAG, "[5.0] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    file2player_evt = audio_event_iface_init(&evt_cfg);

}


void deinit_file2player(){

    ESP_LOGI(TAG, "[ 7.1 ] Stop file2player_pipeline");
    audio_pipeline_stop(file2player_pipeline);
    audio_pipeline_wait_for_stop(file2player_pipeline);
    audio_pipeline_terminate(file2player_pipeline);

    ESP_LOGI(TAG, "[ 7.3 ] Unregister file2http_pipeline");
    audio_pipeline_unregister(file2player_pipeline, wav_decoder);
    audio_pipeline_unregister(file2player_pipeline, i2s_stream_writer);
    audio_pipeline_unregister(file2player_pipeline, rsp_handle);
    ESP_LOGI(TAG, "[ 7.4 ] Unregister file2http_pipeline");
    audio_pipeline_unregister(file2player_pipeline, fatfs_stream_reader);


    /* Terminate the pipeline before removing the listener */
    ESP_LOGI(TAG, "[ 7.5 ] Removing pipeline listener");
    audio_pipeline_remove_listener(file2player_pipeline);

    /* Stop all peripherals before removing the listener */

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(file2player_evt);

    /* Release all resources */
    sdcard_list_destroy(sdcard_list_handle);
    audio_pipeline_deinit(file2player_pipeline);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(wav_decoder);
    audio_element_deinit(rsp_handle);
    audio_element_deinit(fatfs_stream_reader);
}

void run_file2player(const char *src_url, const char *dst_url){
    ESP_LOGI(TAG, "[6.1] Listen for all file2player_pipeline events (set it after pipeline_link)");
    audio_pipeline_set_listener(file2player_pipeline, file2player_evt);

	ESP_LOGW(TAG, "URL: %s", src_url);
    audio_pipeline_stop(file2player_pipeline);
    audio_pipeline_wait_for_stop(file2player_pipeline);
	audio_element_set_uri(fatfs_stream_reader, src_url);
	audio_pipeline_reset_ringbuffer(file2player_pipeline);
	audio_pipeline_reset_elements(file2player_pipeline);
    ESP_LOGI(TAG, "[6.0] Running file2player_pipeline...");
    audio_pipeline_change_state(file2player_pipeline, AEL_STATE_INIT);
	audio_pipeline_run(file2player_pipeline);

	while(1){
		audio_event_iface_msg_t msg;
		esp_err_t ret = audio_event_iface_listen(file2player_evt, &msg, portMAX_DELAY);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
			break;
		}
		if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
			// Set music info for a new song to be played
			if (msg.source == (void *) wav_decoder
				&& msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
				audio_element_info_t music_info = {0};
				audio_element_getinfo(wav_decoder, &music_info);
				ESP_LOGI(TAG, "[ * ] Received music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
						 music_info.sample_rates, music_info.bits, music_info.channels);
				audio_element_setinfo(i2s_stream_writer, &music_info);
				rsp_filter_set_src_info(rsp_handle, music_info.sample_rates, music_info.channels);
			}
			// Advance to the next song when previous finishes
			if (msg.source == (void *) i2s_stream_writer
				&& msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
				audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
				if (el_state == AEL_STATE_FINISHED) {
					ESP_LOGI(TAG, "[ * ] Finished,");
					break;
				}
			}
		}
	}
}

void enable_file2player(bool enable){
	if(enable){
	    ESP_LOGI(TAG, "Enable file2player_pipeline.");
		audio_pipeline_resume(file2player_pipeline);

	} else {
	    ESP_LOGI(TAG, "Disable file2player_pipeline.");
	    audio_pipeline_pause(file2player_pipeline);
	}
}

