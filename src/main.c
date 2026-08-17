#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/rmt_tx.h"
#include "device_config.h"
#include "wifi.h"
#include "mqtt_transport.h"
#include "gdb_server.h"
#include "demo_task.h"
#include "ramfs.h"
#include "file_manager.h"
#ifdef CONFIG_UART_RELAY_ENABLE
#include "uart_relay.h"
#endif

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "GDB over MQTT - starting");

    /* Turn off WS2812 on GPIO8 by sending 24 zero bits via RMT */
    rmt_channel_handle_t tx_chan = NULL;
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = 8,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    if (rmt_new_tx_channel(&tx_cfg, &tx_chan) == ESP_OK) {
        rmt_encoder_handle_t encoder = NULL;
        rmt_bytes_encoder_config_t enc_cfg = {
            .bit0 = { .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0 },
            .bit1 = { .duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0 },
            .flags.msb_first = true,
        };
        if (rmt_new_bytes_encoder(&enc_cfg, &encoder) == ESP_OK) {
            rmt_enable(tx_chan);
            uint8_t grb[3] = {0, 0, 0};
            rmt_transmit_config_t tx_config = { .loop_count = 0 };
            rmt_transmit(tx_chan, encoder, grb, sizeof(grb), &tx_config);
            rmt_tx_wait_all_done(tx_chan, 100);
            rmt_disable(tx_chan);
            rmt_del_encoder(encoder);
        }
        rmt_del_channel(tx_chan);
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(device_config_init());
    const device_config_t *cfg = device_config_get();

    ESP_ERROR_CHECK(wifi_init_sta(cfg->wifi_ssid, cfg->wifi_password));
    ESP_LOGI(TAG, "WiFi connected");

    ESP_ERROR_CHECK(mqtt_transport_init(cfg->mqtt_uri, cfg->device_id));
    ESP_LOGI(TAG, "MQTT transport ready");

    ramfs_init();
    ESP_ERROR_CHECK(file_manager_init());
    ESP_LOGI(TAG, "File manager ready");

    ESP_ERROR_CHECK(gdb_server_init());
    ESP_LOGI(TAG, "GDB server running — attach with: target remote localhost:3333");

#ifdef CONFIG_UART_RELAY_ENABLE
    ESP_ERROR_CHECK(uart_relay_init());
    ESP_LOGI(TAG, "UART console relay active");
#endif

    demo_task_start();
}
