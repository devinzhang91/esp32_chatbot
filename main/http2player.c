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
#include "i2s_stream.h"
#include "wav_decoder.h"
#include "filter_resample.h"
#include "http_stream.h"
#include "sdkconfig.h"

#include "esp_peripherals.h"
#include "esp_http_client.h"
#include "periph_sdcard.h"
#include "board.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "http2player";

static audio_pipeline_handle_t http2player_pipeline;
static audio_element_handle_t i2s_stream_writer;
static audio_element_handle_t audio_decoder;
static audio_element_handle_t http_stream_reader;
static audio_event_iface_handle_t http2player_evt;

int player_volume;

void init_http2player(){
    ESP_LOGI(TAG, "[1.0] Initialize peripherals management");
//    ESP_LOGI(TAG, "[1.1] Initialize and start peripherals");

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
//    audio_board_handle_t board_handle = audio_board_init();
//    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);


    ESP_LOGI(TAG, "[3.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    http2player_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(http2player_pipeline);

    ESP_LOGI(TAG, "[3.1] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
//    i2s_cfg.i2s_config.sample_rate = 48000;
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[3.2] Create http stream to read data");
	http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
	http_stream_reader = http_stream_init(&http_cfg);

	ESP_LOGI(TAG, "[3.3] Create audio decoder to decode audio file");
	wav_decoder_cfg_t decoder_cfg = DEFAULT_WAV_DECODER_CONFIG();
	audio_decoder = wav_decoder_init(&decoder_cfg);

	ESP_LOGI(TAG, "[3.4] Register all elements to audio pipeline");
	audio_pipeline_register(http2player_pipeline, http_stream_reader, "http");
	audio_pipeline_register(http2player_pipeline, audio_decoder,      "decoder");
	audio_pipeline_register(http2player_pipeline, i2s_stream_writer,  "i2s");

	ESP_LOGI(TAG, "[3.5] Link it together [http_server]-->http_stream-->audio_decoder-->i2s_stream-->[codec_chip]");
	const char *link_tag[3] = {"http", "decoder", "i2s"};
	audio_pipeline_link(http2player_pipeline, &link_tag[0], 3);

//	ESP_LOGI(TAG, "[3.6] Set up  uri (http as http_stream, amr as amr decoder, and default output is i2s)");
//	audio_element_set_uri(http_stream_reader, "https://dl.espressif.com/dl/audio/ff-16b-1c-8000hz.amr");
//	audio_element_set_uri(http_stream_reader, "http://192.168.1.104:9000/test.mp3");


    ESP_LOGI(TAG, "[4.0] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    http2player_evt = audio_event_iface_init(&evt_cfg);

//    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
//    audio_pipeline_set_listener(http2player_pipeline, http2player_evt);

}

void deinit_http2player(){

    ESP_LOGI(TAG, "[ 7.1 ] Stop http2player_pipeline");
    audio_pipeline_stop(http2player_pipeline);
    audio_pipeline_wait_for_stop(http2player_pipeline);
    audio_pipeline_terminate(http2player_pipeline);

    ESP_LOGI(TAG, "[ 7.3 ] Unregister http2player_pipeline");
    audio_pipeline_unregister(http2player_pipeline, i2s_stream_writer);
    audio_pipeline_unregister(http2player_pipeline, audio_decoder);
    audio_pipeline_unregister(http2player_pipeline, http_stream_reader);


    /* Terminate the pipeline before removing the listener */
    ESP_LOGI(TAG, "[ 7.4 ] Removing pipeline listener");
    audio_pipeline_remove_listener(http2player_pipeline);

    /* Stop all peripherals before removing the listener */

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(http2player_evt);

    /* Release all resources */
    audio_pipeline_deinit(http2player_pipeline);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(audio_decoder);
    audio_element_deinit(http_stream_reader);
}

void run_http2player(const char *src_url, const char *dst_url){
    ESP_LOGI(TAG, "[6.0] Listen for all http2player_pipeline events (set it after pipeline_link)");
    audio_pipeline_set_listener(http2player_pipeline, http2player_evt);

	ESP_LOGW(TAG, "URL: %s", src_url);
    audio_pipeline_stop(http2player_pipeline);
    audio_pipeline_wait_for_stop(http2player_pipeline);
	audio_element_set_uri(http_stream_reader, src_url);
	audio_pipeline_reset_ringbuffer(http2player_pipeline);
	audio_pipeline_reset_elements(http2player_pipeline);
    ESP_LOGI(TAG, "[6.1] Running http2player_pipeline...");
    audio_pipeline_change_state(http2player_pipeline, AEL_STATE_INIT);
	audio_pipeline_run(http2player_pipeline);

	while(1){
		audio_event_iface_msg_t msg;
		esp_err_t ret = audio_event_iface_listen(http2player_evt, &msg, portMAX_DELAY);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
			break;
		}
		if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
			if (msg.source == (void *) audio_decoder
				&& msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
				audio_element_info_t music_info = {0};
				audio_element_getinfo(audio_decoder, &music_info);
				ESP_LOGI(TAG, "[ * ] Received music info from wav decoder, sample_rates=%d, bits=%d, ch=%d",
						 music_info.sample_rates, music_info.bits, music_info.channels);
				audio_element_setinfo(i2s_stream_writer, &music_info);
	            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
				continue;
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

void enable_http2player(bool enable){
	if(enable){
	    ESP_LOGI(TAG, "Enable http2player_pipeline.");
	    audio_pipeline_change_state(http2player_pipeline, AEL_STATE_INIT);
		audio_pipeline_reset_ringbuffer(http2player_pipeline);
		audio_pipeline_reset_elements(http2player_pipeline);
		audio_pipeline_resume(http2player_pipeline);

	} else {
	    ESP_LOGI(TAG, "Disable http2player_pipeline.");
	    audio_pipeline_pause(http2player_pipeline);
	}
}

