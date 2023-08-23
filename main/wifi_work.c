#include "main.h"

#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "audio_idf_version.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "wifi_work";

void init_wifi_work(esp_periph_set_handle_t set){
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
		// NVS partition was truncated and needs to be erased
		// Retry nvs_flash_init
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
	ESP_ERROR_CHECK(esp_netif_init());
#else
	tcpip_adapter_init();
#endif


	ESP_LOGI(TAG, "Start and wait for Wi-Fi network");
	periph_wifi_cfg_t wifi_cfg = {
		.ssid = CONFIG_WIFI_SSID,
		.password = CONFIG_WIFI_PASSWORD,
	};
    ESP_LOGI(TAG, "ssid:%s password:%s", CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
	esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
	esp_periph_start(set, wifi_handle);
	periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
}
