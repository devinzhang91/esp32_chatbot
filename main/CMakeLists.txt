# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES )
set(COMPONENT_PRIV_REQUIRES )

set(COMPONENT_SRCS "main.c wwe_work.c wifi_work.c  file2http.c http2file.c file2player.c http2player.c")
set(COMPONENT_ADD_INCLUDEDIRS "")
#set(COMPONENT_SRCDIRS  .)
#set(COMPONENT_ADD_INCLUDEDIRS . )
set(COMPONENT_REQUIRES 
	esp_actions 
	audio_sal 
	audio_stream 
	audio_hal 
	audio_board 
	audio_recorder 
	esp_peripherals 
	audio_flash_tone
	ssd1306
	playlist)

register_component()

