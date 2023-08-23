#include "main.h"

#include <stdio.h>
#include <string.h>

#include "audio_element.h"
#include "audio_idf_version.h"
#include "audio_mem.h"
#include "audio_pipeline.h"
#include "audio_recorder.h"
#include "audio_thread.h"
#include "audio_tone_uri.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "amrnb_encoder.h"
#include "amrwb_encoder.h"
#include "filter_resample.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "raw_stream.h"
#include "recorder_encoder.h"
#include "recorder_sr.h"
#include "tone_stream.h"
#include "es7210.h"
#include "sdkconfig.h"

#include "model_path.h"

#include "wav_encoder.h"

static char *TAG = "wwe_work";

#define NO_ENCODER  (0)
#define ENC_2_AMRNB (1)
#define ENC_2_AMRWB (2)
#define ENC_2_WAV	(3)

#define UPLOAD_HTTP_STREAM  (1)
#if UPLOAD_HTTP_STREAM == (true)
#include "pipline_work.h"
#endif /* UPLOAD_HTTP_STREAM == (true) */

#define RECORDER_ENC_ENABLE (ENC_2_WAV)
#define VOICE2FILE          (true)
#define WAKENET_ENABLE      (true)
#define MULTINET_ENABLE     (false)
#define SPEECH_CMDS_RESET   (false)

#define SPEECH_COMMANDS     ("da kai dian deng,kai dian deng;guan bi dian deng,guan dian deng;guan deng;")

#ifndef CODEC_ADC_SAMPLE_RATE
#warning "Please define CODEC_ADC_SAMPLE_RATE first, default value is 48kHz may not correctly"
#define CODEC_ADC_SAMPLE_RATE    48000
#endif

#ifndef CODEC_ADC_BITS_PER_SAMPLE
#warning "Please define CODEC_ADC_BITS_PER_SAMPLE first, default value 16 bits may not correctly"
#define CODEC_ADC_BITS_PER_SAMPLE  I2S_BITS_PER_SAMPLE_16BIT
#endif

#ifndef RECORD_HARDWARE_AEC
#warning "The hardware AEC is disabled!"
#define RECORD_HARDWARE_AEC  (false)
#endif

#ifndef CODEC_ADC_I2S_PORT
#define CODEC_ADC_I2S_PORT  (0)
#endif

enum _rec_msg_id {
    REC_START = 1,
    REC_STOP,
    REC_CANCEL,
};

static esp_audio_handle_t     	player 		= NULL;
static audio_rec_handle_t     	recorder 	= NULL;
static audio_element_handle_t 	raw_read 	= NULL;
static audio_element_handle_t 	i2s_stream_reader 	= NULL;
static QueueHandle_t          	rec_q      	= NULL;
static audio_pipeline_handle_t pipeline 	= NULL;
static bool                   	voice_reading = false;


static void add_wav_file_header(FILE *fp,
								const uint32_t duration,
								const uint16_t num_channels,
								const uint32_t sampling_rate,
								const uint16_t bits_per_sample){
	  uint32_t data_size = sampling_rate * num_channels * bits_per_sample * duration / 8;

	  /* *************** ADD ".WAV" HEADER *************** */
	  uint8_t CHUNK_ID[4] = {'R', 'I', 'F', 'F'};
      fwrite(CHUNK_ID, 4, 1, fp);

	  uint32_t chunk_size = data_size + 36;
	  uint8_t CHUNK_SIZE[4] = {chunk_size, chunk_size >> 8, chunk_size >> 16, chunk_size >> 24};
      fwrite(CHUNK_SIZE, 4, 1, fp);

	  uint8_t FORMAT[4] = {'W', 'A', 'V', 'E'};
      fwrite(FORMAT, 4, 1, fp);

	  uint8_t SUBCHUNK_1_ID[4] = {'f', 'm', 't', ' '};
      fwrite(SUBCHUNK_1_ID, 4, 1, fp);

	  uint8_t SUBCHUNK_1_SIZE[4] = {0x10, 0x00, 0x00, 0x00};
      fwrite(SUBCHUNK_1_SIZE, 4, 1, fp);

	  uint8_t AUDIO_FORMAT[2] = {0x01, 0x00};
      fwrite(AUDIO_FORMAT, 2, 1, fp);

	  uint8_t NUM_CHANNELS[2] = {num_channels, num_channels >> 8};
      fwrite(NUM_CHANNELS, 2, 1, fp);

	  uint8_t SAMPLING_RATE[4] = {sampling_rate, sampling_rate >> 8, sampling_rate >> 16, sampling_rate >> 24};
      fwrite(SAMPLING_RATE, 4, 1, fp);

	  uint32_t byte_rate = num_channels * sampling_rate * bits_per_sample / 8;
	  uint8_t BYTE_RATE[4] = {byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24};
      fwrite(BYTE_RATE, 4, 1, fp);

	  uint16_t block_align = num_channels * bits_per_sample / 8;
	  uint8_t BLOCK_ALIGN[2] = {block_align, block_align >> 8};
      fwrite(BLOCK_ALIGN, 2, 1, fp);

	  uint8_t BITS_PER_SAMPLE[2] = {bits_per_sample, bits_per_sample >> 8};
      fwrite(BITS_PER_SAMPLE, 2, 1, fp);

	  uint8_t SUBCHUNK_2_ID[4] = {'d', 'a', 't', 'a'};
      fwrite(SUBCHUNK_2_ID, 4, 1, fp);

	  uint8_t SUBCHUNK_2_SIZE[4] = {data_size, data_size >> 8, data_size >> 16, data_size >> 24};
      fwrite(SUBCHUNK_2_SIZE, 4, 1, fp);
}


static void mod_wav_file_header(FILE *fp,
								const uint32_t duration,
								const uint16_t num_channels,
								const uint32_t sampling_rate,
								const uint16_t bits_per_sample){
	fseek(fp, 0, SEEK_SET);
	add_wav_file_header(fp, duration, num_channels, sampling_rate, bits_per_sample);
	fseek(fp, 0, SEEK_END);
}


static esp_audio_handle_t setup_player()
{
    esp_audio_cfg_t cfg = DEFAULT_ESP_AUDIO_CONFIG();
    audio_board_handle_t board_handle = audio_board_init();

    cfg.vol_handle = board_handle->audio_hal;
    cfg.vol_set = (audio_volume_set)audio_hal_set_volume;
    cfg.vol_get = (audio_volume_get)audio_hal_get_volume;
    cfg.resample_rate = 48000;
    cfg.prefer_type = ESP_AUDIO_PREFER_MEM;

    player = esp_audio_create(&cfg);
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    // Create readers and add to esp_audio
    tone_stream_cfg_t tone_cfg = TONE_STREAM_CFG_DEFAULT();
    tone_cfg.type = AUDIO_STREAM_READER;
    esp_audio_input_stream_add(player, tone_stream_init(&tone_cfg));

    // Add decoders and encoders to esp_audio
    mp3_decoder_cfg_t mp3_dec_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_dec_cfg.task_core = 1;
    esp_audio_codec_lib_add(player, AUDIO_CODEC_TYPE_DECODER, mp3_decoder_init(&mp3_dec_cfg));

    // Create writers and add to esp_audio
    i2s_stream_cfg_t i2s_writer = I2S_STREAM_CFG_DEFAULT();
    i2s_writer.i2s_config.sample_rate = 48000;
#if (CONFIG_ESP32_S3_KORVO2_V3_BOARD == 1) && (CONFIG_AFE_MIC_NUM == 1)
    i2s_writer.i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
#else
    i2s_writer.i2s_config.bits_per_sample = CODEC_ADC_BITS_PER_SAMPLE;
    i2s_writer.need_expand = (CODEC_ADC_BITS_PER_SAMPLE != I2S_BITS_PER_SAMPLE_16BIT);
#endif
    i2s_writer.type = AUDIO_STREAM_WRITER;

    esp_audio_output_stream_add(player, i2s_stream_init(&i2s_writer));

    // Set default volume
    esp_audio_vol_set(player, 80);
    AUDIO_MEM_SHOW(TAG);

    ESP_LOGI(TAG, "esp_audio instance is:%p\r\n", player);
    return player;
}

#if VOICE2FILE == (true)
static void voice_2_file(uint8_t *buffer, int len)
{
#define MAX_FNAME_LEN (50)
    static FILE *fp = NULL;
    static int fcnt = 0;
    static int data_size = 0;
    static char fname[MAX_FNAME_LEN] = { 0 };

    if (voice_reading) {
        if (!fp) {
            if (fp == NULL) {
                if (RECORDER_ENC_ENABLE == ENC_2_AMRNB) {
                    snprintf(fname, MAX_FNAME_LEN - 1, "/sdcard/amr_%d.amr", fcnt++);
                } else if (RECORDER_ENC_ENABLE == ENC_2_AMRWB){
                    snprintf(fname, MAX_FNAME_LEN - 1, "/sdcard/wamr_%d.amr", fcnt++);
                } else if (RECORDER_ENC_ENABLE == ENC_2_WAV){
                    snprintf(fname, MAX_FNAME_LEN - 1, "/sdcard/wav_%d.wav", fcnt++);
                } else {
                    snprintf(fname, MAX_FNAME_LEN - 1, "/sdcard/pcm_%d.pcm", fcnt++);
                }
                fp = fopen(fname, "wb");
                ESP_LOGI(TAG, "File opened: %s ", fname);
                data_size = 0;
                if(RECORDER_ENC_ENABLE == ENC_2_WAV){		//set wav header
                	size_t duration = 0; 					// lazy write
                	//16kHz mono 16000
    				mod_wav_file_header(fp, duration, CONFIG_AUDIO_CHANNELS, CONFIG_AUDIO_SAMPLE_RATE, CONFIG_AUDIO_BITS);
                }
                if (!fp) {
                    ESP_LOGE(TAG, "File open failed");
                }
            }
        }
        if (len) {
        	data_size += len;
            fwrite(buffer, len, 1, fp);
        }
    } else {
        if (fp) {
        	if(RECORDER_ENC_ENABLE == ENC_2_WAV){	//reset wav header
				size_t duration = data_size * 8 / CONFIG_AUDIO_BITS / CONFIG_AUDIO_CHANNELS / CONFIG_AUDIO_SAMPLE_RATE; // lazy write
                ESP_LOGI(TAG, "duration: %d", duration);
				mod_wav_file_header(fp, duration, CONFIG_AUDIO_CHANNELS, CONFIG_AUDIO_SAMPLE_RATE, CONFIG_AUDIO_BITS);
			}
        	data_size = 0;
            ESP_LOGI(TAG, "File closed: %s ", fname);
            fclose(fp);
            fp = NULL;
#if UPLOAD_HTTP_STREAM == (true)
            char dst_url[64];
            sprintf(dst_url, "http://%s:%d", CONFIG_TARGET_URL, CONFIG_TARGET_PORT);
            main_msg_t msg = {
            		.msg_id = FILE2HTTP,
            		.src = fname,
            		.dst = dst_url,
            };
            if (xQueueSend(main_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "main queue send failed");
            }
#endif /* UPLOAD_HTTP_STREAM == (true) */
        }
    }
}
#endif /* VOICE2FILE == (true) */

static void voice_read_task(void *args)
{
    const int buf_len = 2 * 1024;
    uint8_t *voiceData = audio_calloc(1, buf_len);
    int msg = 0;
    TickType_t delay = portMAX_DELAY;

    while (true) {
        if (xQueueReceive(rec_q, &msg, delay) == pdTRUE) {
            switch (msg) {
                case REC_START: {
                    ESP_LOGW(TAG, "voice read begin");
                    delay = 0;
                    voice_reading = true;
                    break;
                }
                case REC_STOP: {
                    ESP_LOGW(TAG, "voice read stopped");
                    delay = portMAX_DELAY;
                    voice_reading = false;
                    break;
                }
                case REC_CANCEL: {
                    ESP_LOGW(TAG, "voice read cancel");
                    delay = portMAX_DELAY;
                    voice_reading = false;
                    break;
                }
                default:
                    break;
            }
        }
        int ret = 0;
        if (voice_reading) {
            ret = audio_recorder_data_read(recorder, voiceData, buf_len, portMAX_DELAY);

            if (ret <= 0) {
                ESP_LOGW(TAG, "audio recorder read finished %d", ret);
                delay = portMAX_DELAY;
                voice_reading = false;
            }
        }
#if VOICE2FILE == (true)
        voice_2_file(voiceData, ret);
#endif /* VOICE2FILE == (true) */
    }

    free(voiceData);
    vTaskDelete(NULL);
}

static esp_err_t rec_engine_cb(audio_rec_evt_t type, void *user_data)
{
    if (AUDIO_REC_WAKEUP_START == type) {
        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_WAKEUP_START");
        esp_audio_sync_play(player, tone_uri[TONE_TYPE_DINGDONG], 0);
        if (voice_reading) {
            int msg = REC_CANCEL;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec cancel send failed");
            }
        }
    } else if (AUDIO_REC_VAD_START == type) {
        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_VAD_START");
        if (!voice_reading) {
            int msg = REC_START;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec start send failed");
            }
        }
    } else if (AUDIO_REC_VAD_END == type) {
        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_VAD_STOP");
        if (voice_reading) {
            int msg = REC_STOP;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec stop send failed");
            }
        }

    } else if (AUDIO_REC_WAKEUP_END == type) {
        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_WAKEUP_END");
    } else if (AUDIO_REC_COMMAND_DECT <= type) {
        ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_COMMAND_DECT");
        ESP_LOGW(TAG, "command %d", type);
//        esp_audio_sync_play(player, tone_uri[TONE_TYPE_HAODE], 0);
    } else {
        ESP_LOGE(TAG, "Unkown event");
    }
    return ESP_OK;
}

static int input_cb_for_afe(int16_t *buffer, int buf_sz, void *user_ctx, TickType_t ticks)
{
    return raw_stream_read(raw_read, (char *)buffer, buf_sz);
}

static void start_recorder()
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    if (NULL == pipeline) {
        return;
    }

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.i2s_port = CODEC_ADC_I2S_PORT;
    i2s_cfg.i2s_config.use_apll = 0;
    i2s_cfg.i2s_config.sample_rate = CODEC_ADC_SAMPLE_RATE;
#if (CONFIG_ESP32_S3_KORVO2_V3_BOARD == 1) && (CONFIG_AFE_MIC_NUM == 1)
    i2s_cfg.i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
#else
    i2s_cfg.i2s_config.bits_per_sample = CODEC_ADC_BITS_PER_SAMPLE;
#endif
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    audio_element_handle_t filter = NULL;
#if CODEC_ADC_SAMPLE_RATE != (16000)
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = CODEC_ADC_SAMPLE_RATE;
    rsp_cfg.dest_rate = 16000;
#if (CONFIG_ESP32_S3_KORVO2_V3_BOARD == 1) && (CONFIG_AFE_MIC_NUM == 2)
    rsp_cfg.mode = RESAMPLE_UNCROSS_MODE;
    rsp_cfg.src_ch = 4;
    rsp_cfg.dest_ch = 4;
    rsp_cfg.max_indata_bytes = 1024;
#endif
    filter = rsp_filter_init(&rsp_cfg);
#endif

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_read = raw_stream_init(&raw_cfg);

    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, raw_read, "raw");

    if (filter) {
        audio_pipeline_register(pipeline, filter, "filter");
        const char *link_tag[3] = {"i2s", "filter", "raw"};
        audio_pipeline_link(pipeline, &link_tag[0], 3);
    } else {
        const char *link_tag[2] = {"i2s", "raw"};
        audio_pipeline_link(pipeline, &link_tag[0], 2);
    }

    audio_pipeline_run(pipeline);
    ESP_LOGI(TAG, "Recorder has been created");

    recorder_sr_cfg_t recorder_sr_cfg = DEFAULT_RECORDER_SR_CFG();
    recorder_sr_cfg.afe_cfg.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    recorder_sr_cfg.afe_cfg.wakenet_init = WAKENET_ENABLE;
    recorder_sr_cfg.multinet_init = MULTINET_ENABLE;
    recorder_sr_cfg.afe_cfg.aec_init = RECORD_HARDWARE_AEC;
    recorder_sr_cfg.afe_cfg.agc_mode = AFE_MN_PEAK_NO_AGC;
#if (CONFIG_ESP32_S3_KORVO2_V3_BOARD == 1) && (CONFIG_AFE_MIC_NUM == 1)
    recorder_sr_cfg.afe_cfg.pcm_config.mic_num = 1;
    recorder_sr_cfg.afe_cfg.pcm_config.ref_num = 1;
    recorder_sr_cfg.afe_cfg.pcm_config.total_ch_num = 2;
    recorder_sr_cfg.input_order[0] = DAT_CH_0;
    recorder_sr_cfg.input_order[1] = DAT_CH_1;

    es7210_mic_select(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC3);
#endif

#if RECORDER_ENC_ENABLE
    recorder_encoder_cfg_t recorder_encoder_cfg = { 0 };
#if RECORDER_ENC_ENABLE == ENC_2_AMRNB
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 16000;    ESP_LOGI(TAG, "Recorder has been created");

    filter_cfg.dest_ch = 1;
    filter_cfg.dest_rate = 8000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.max_indata_bytes = 1024;

    amrnb_encoder_cfg_t amrnb_cfg = DEFAULT_AMRNB_ENCODER_CONFIG();
    amrnb_cfg.contain_amrnb_header = true;
    amrnb_cfg.stack_in_ext = true;
    I2S_CHANNEL_FMT_RIGHT_LEFT
    recorder_encoder_cfg.resample = rsp_filter_init(&filter_cfg);
    recorder_encoder_cfg.encoder = amrnb_encoder_init(&amrnb_cfg);
#elif RECORDER_ENC_ENABLE == ENC_2_AMRWB
    amrwb_encoder_cfg_t amrwb_cfg = DEFAULT_AMRWB_ENCODER_CONFIG();
    amrwb_cfg.contain_amrwb_header = true;
    amrwb_cfg.stack_in_ext = true;
    amrwb_cfg.out_rb_size = 4 * 1024;

    recorder_encoder_cfg.encoder = amrwb_encoder_init(&amrwb_cfg);
#elif RECORDER_ENC_ENABLE == ENC_2_WAV
    wav_encoder_cfg_t wav_cfg = DEFAULT_WAV_ENCODER_CONFIG();
    wav_cfg.stack_in_ext = true;

    recorder_encoder_cfg.encoder = wav_encoder_init(&wav_cfg);
#endif
#endif

    audio_rec_cfg_t cfg = AUDIO_RECORDER_DEFAULT_CFG();
    cfg.read = (recorder_data_read_t)&input_cb_for_afe;
    cfg.sr_handle = recorder_sr_create(&recorder_sr_cfg, &cfg.sr_iface);
#if SPEECH_CMDS_RESET
    char err[200];
    recorder_sr_reset_speech_cmd(cfg.sr_handle, SPEECH_COMMANDS, err);
#endif
#if RECORDER_ENC_ENABLE
    cfg.encoder_handle = recorder_encoder_create(&recorder_encoder_cfg, &cfg.encoder_iface);
#endif
    cfg.event_cb = rec_engine_cb;
    cfg.vad_off = 1000;
    recorder = audio_recorder_create(&cfg);
}

void enable_wwe_pipeline(bool enable){
	if(enable){
	    ESP_LOGI(TAG, "Enable wwe pipeline.");
	    /// remember set i2s clk again !
        i2s_stream_set_clk(i2s_stream_reader, CODEC_ADC_SAMPLE_RATE, CODEC_ADC_BITS_PER_SAMPLE, 2);
		audio_pipeline_reset_ringbuffer(pipeline);
		audio_pipeline_reset_elements(pipeline);
	    audio_pipeline_resume(pipeline);
	    audio_recorder_wakenet_enable(recorder, true);
	    ESP_LOGI(TAG, "enable_wwe_pipeline: %d.", __LINE__);
	} else {
	    ESP_LOGI(TAG, "Disable wwe pipeline.");
	    audio_recorder_wakenet_enable(recorder, false);
	    audio_pipeline_pause(pipeline);
	}
}

void enable_wwe_trigger(bool enable){
	if(enable){
		audio_recorder_trigger_start(recorder);
	} else {
		audio_recorder_trigger_stop(recorder);
	}
}

void init_wwe_work(){
//    audio_board_init();
    setup_player();
    start_recorder();

    rec_q = xQueueCreate(8, sizeof(int));
    audio_thread_create(NULL, "read_task", voice_read_task, NULL, 4 * 1024, 5, true, 0);


    ESP_LOGI(TAG, "init_wwe_work done");
}
