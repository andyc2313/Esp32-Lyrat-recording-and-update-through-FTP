#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_spiffs.h"
#include "sys/time.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "wav_encoder.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "wav_decoder.h"
#include "fatfs_stream.h"
#include "periph_wifi.h"
#include "periph_sdcard.h"
#include "board.h"
#include "FtpClient.h"
#include "FtpClient.c"

#include "audio_idf_version.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

#define WAKEUP_TIME_SECONDS 5

static const char *TAG = "PIPELINE_REC_WAV_SDCARD";

#define RECORD_TIME_SECONDS (47)  

void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }

    if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR,  IP2STR(&event->ip_info.ip));
    }
}

void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "tw.pool.ntp.org");
    sntp_init();
}

void obtain_time(void) {
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (retry < retry_count) {
        ESP_LOGI(TAG, "System time is set successfully");
        ESP_LOGI(TAG, "Current time: %d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        ESP_LOGE(TAG, "Failed to set system time, rebooting...");
        esp_restart();  // 重新啟動 ESP32
    }
}

void app_main(void)
{
    init_nvs();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_sta_config_t cfg_sta = {
        .ssid = "",
        .password = "",
    };
    
    esp_wifi_set_config(WIFI_IF_STA, (wifi_config_t *) &cfg_sta);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
    esp_wifi_start();
    initialize_sntp();
    obtain_time();
    setenv("TZ", "UTC-8", 1); 
    tzset();

    // Audio recording code
    audio_pipeline_handle_t pipeline_wav;
    audio_element_handle_t wav_fatfs_stream_writer, i2s_stream_reader, wav_encoder;

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "[1.0] Mount sdcard");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    ESP_LOGI(TAG, "[2.0] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline_wav for recording");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_wav = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline_wav);

    ESP_LOGI(TAG, "[3.1] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_cfg.multi_out_num = 1;
    i2s_cfg.task_core = 1;
    i2s_cfg.i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
    // Add your specific configuration based on your board and audio format
#if defined CONFIG_ESP_LYRAT_MINI_V1_1_BOARD
    i2s_cfg.i2s_port = 1;
#endif
#ifdef CONFIG_CHOICE_AMR_WB
    i2s_cfg.i2s_config.sample_rate = 44100;
#elif defined CONFIG_CHOICE_AMR_NB
    i2s_cfg.i2s_config.sample_rate = 44100;
#endif
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[3.2] Create wav encoder to encode wav format");
    wav_encoder_cfg_t wav_cfg = DEFAULT_WAV_ENCODER_CONFIG();
    wav_encoder = wav_encoder_init(&wav_cfg);

    ESP_LOGI(TAG, "[3.3] Create fatfs stream to write data to sdcard");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_WRITER;
    wav_fatfs_stream_writer = fatfs_stream_init(&fatfs_cfg);

    time_t t;
    struct tm *local_time;
    time(&t);
    local_time = localtime(&t);
    // int next_5am_seconds = (5 - local_time->tm_hour) * 3600 - local_time->tm_min * 60 - local_time->tm_sec;
    // if (next_5am_seconds <= 0) {
    //     next_5am_seconds += 24 * 3600;
    // }

    // if (local_time->tm_hour >= 5 && local_time->tm_hour < 19)
    if (1)
    {
        char filename[64];
        strftime(filename, sizeof(filename), "/sdcard/%Y.%m.%d.%H.%M.%S.wav", local_time);

        ESP_LOGI(TAG, "[3.4] File name: %s", filename);
        audio_element_info_t info = AUDIO_ELEMENT_INFO_DEFAULT();
        audio_element_getinfo(i2s_stream_reader, &info);
        audio_element_setinfo(wav_fatfs_stream_writer, &info);

        ESP_LOGI(TAG, "[3.5] Register all elements to audio pipeline");
        audio_pipeline_register(pipeline_wav, i2s_stream_reader, "i2s");
        audio_pipeline_register(pipeline_wav, wav_encoder, "wav");
        audio_pipeline_register(pipeline_wav, wav_fatfs_stream_writer, "wav_file");

        ESP_LOGI(TAG, "[3.6] Link it together [codec_chip]-->i2s_stream-->wav_encoder-->fatfs_stream-->[sdcard]");
        const char *link_wav[3] = {"i2s", "wav", "wav_file"};
        audio_pipeline_link(pipeline_wav, &link_wav[0], 3);

        ESP_LOGI(TAG, "[3.7] Set up uri (file as fatfs_stream, wav as wav encoder)");
        audio_element_set_uri(wav_fatfs_stream_writer, filename);

        ESP_LOGI(TAG, "[4.0] Set up event listener");
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
        audio_pipeline_set_listener(pipeline_wav, evt);

        ESP_LOGI(TAG, "[4.1] Listening event from peripherals");
        audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

        ESP_LOGI(TAG, "[5.0] Start audio_pipeline");
        audio_pipeline_run(pipeline_wav);

        ESP_LOGI(TAG, "[6.0] Listen for all pipeline events, record for %d seconds", RECORD_TIME_SECONDS);
        int second_recorded = 0;

        while (1){
            audio_event_iface_msg_t msg;
            if (audio_event_iface_listen(evt, &msg, 1000 / portTICK_RATE_MS) != ESP_OK){
                second_recorded++;
                ESP_LOGI(TAG, "[ * ] Recording ... %d", second_recorded);
                if (second_recorded >= RECORD_TIME_SECONDS){
                    ESP_LOGI(TAG, "Finishing recording");
                    audio_element_set_ringbuf_done(i2s_stream_reader);
                }
                continue;
            }

            /* Stop when the last pipeline element (i2s_stream_reader in this case) receives stop event */
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)i2s_stream_reader 
                &&msg.cmd == AEL_MSG_CMD_REPORT_STATUS 
                &&(((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))){
                ESP_LOGW(TAG, "[ * ] Stop event received");
                break;
            }
        }

        vTaskDelay(5 * 1000 / portTICK_PERIOD_MS);

        audio_pipeline_stop(pipeline_wav);
        audio_pipeline_wait_for_stop(pipeline_wav);
        audio_pipeline_terminate(pipeline_wav);
        audio_pipeline_unregister_more(pipeline_wav, i2s_stream_reader,
                                        wav_encoder, wav_fatfs_stream_writer, NULL);

        ESP_LOGI(TAG, "開始上傳"); 
        ESP_LOGI(TAG, "ftp server:%s", CONFIG_FTP_SERVER);
        ESP_LOGI(TAG, "ftp user  :%s", CONFIG_FTP_USER);
        static NetBuf_t* ftpClientNetBuf = NULL;
        FtpClient* ftpClient = getFtpClient();
        int connect = ftpClient->ftpClientConnect(CONFIG_FTP_SERVER, CONFIG_FTP_PORT, &ftpClientNetBuf);
        ESP_LOGI(TAG, "connect=%d", connect);
        if (connect == 0) {
            ESP_LOGE(TAG, "FTP server connect fail");
            esp_restart();
        }

        // 登入 FTP 伺服器
        int login = ftpClient->ftpClientLogin(CONFIG_FTP_USER, CONFIG_FTP_PASSWORD, ftpClientNetBuf);
        ESP_LOGI(TAG, "login=%d", login);
        if (login == 0) {
            ESP_LOGE(TAG, "FTP server login fail");
            esp_restart();
        }

        char file_path[128];  // 根据实际需要调整数组大小
        snprintf(file_path, sizeof(file_path), "%s", filename);

        // 開啟文件
        FILE* file = fopen(file_path, "rb");
        if (!file) {
            ESP_LOGE(TAG, "無法打開文件: %s, 錯誤碼: %d", file_path, errno);
            esp_restart();
        }

        // 獲取文件大小
        fseek(file, 0, SEEK_END);
        size_t file_size = ftell(file);
        rewind(file);

        // 分配緩衝區
        char* buffer = (char*)malloc(file_size);
        if (!buffer) {
            ESP_LOGE(TAG, "內存不足");
            fclose(file);
            return;
        }

        // 讀取文件內容到緩衝區
        fread(buffer, 1, file_size, file);
        ESP_LOGI(TAG, "文件路徑：%s", file_path);
        ESP_LOGI(TAG, "文件大小：%d", file_size);
        ESP_LOGI(TAG, "FTP 開始上傳");

        char new_path[128]; 
        // sprintf(new_path, "/Lab303/esp32/2024_Taipei-Q3/%04d.%02d.%02d.%02d.%02d.%02d.wav",
        //     local_time->tm_year + 1900, local_time->tm_mon + 1, local_time->tm_mday,
        //     local_time->tm_hour, local_time->tm_min, local_time->tm_sec);

        sprintf(new_path, "/Lab303/esp32/Yunlin/steal1/%04d.%02d.%02d.%02d.%02d.%02d.wav",
            local_time->tm_year + 1900, local_time->tm_mon + 1, local_time->tm_mday,
            local_time->tm_hour, local_time->tm_min, local_time->tm_sec);

        // sprintf(new_path, "/Lab303/esp32/test/%04d.%02d.%02d.%02d.%02d.%02d.wav",
        //     local_time->tm_year + 1900, local_time->tm_mon + 1, local_time->tm_mday,
        //     local_time->tm_hour, local_time->tm_min, local_time->tm_sec);

        ftpClient->ftpClientPut(file_path, new_path, FTP_CLIENT_BINARY, ftpClientNetBuf);

        char* lastResponse = getLastResponseFtpClient(ftpClientNetBuf);
        if (lastResponse != NULL) {
            printf("FTP 上傳響應: %s\n", lastResponse);

            // 提取響應碼
            int responseCode;
            if (sscanf(lastResponse, "%d", &responseCode) == 1) {
                if (responseCode >= 200 && responseCode < 300) {
                    printf("FTP 上傳成功\n");
                } else {
                    printf("FTP 上傳失敗\n");
                    esp_restart();
                }
            } else {
                printf("無法提取響應碼\n");
                esp_restart();  
            }
        } else {
            printf("無法獲取 FTP 響應\n");
            esp_restart();
        }

        vTaskDelay(10 * 1000 / portTICK_PERIOD_MS);

        free(buffer);
        fclose(file);

        if (unlink(file_path) == 0) {
            ESP_LOGI(TAG, "成功删除文件: %s", file_path);
        } else {
            ESP_LOGE(TAG, "删除文件失败: %s, 错误码: %d", file_path, errno);
        }

        // 關閉 FTP 連接
        ftpClient->ftpClientQuit(ftpClientNetBuf);

        // 停止 Wi-Fi
        esp_periph_set_stop_all(set);
        audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

        // 釋放資源
        audio_pipeline_deinit(pipeline_wav);
        audio_element_deinit(i2s_stream_reader);
        audio_element_deinit(wav_encoder);
        audio_element_deinit(wav_fatfs_stream_writer);
        esp_periph_set_destroy(set);

        /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
        audio_event_iface_destroy(evt);

        ESP_LOGI(TAG, "[7.0] Entering deep sleep after recording for %d seconds", RECORD_TIME_SECONDS);
        vTaskDelay(5 * 1000 / portTICK_PERIOD_MS);
        esp_sleep_enable_timer_wakeup(WAKEUP_TIME_SECONDS * 1000000);
        ESP_LOGI(TAG, "Entering deep sleep");
        esp_deep_sleep_start();
        // esp_restart();
    }
    // else {
    //     char time_str[64];
    //     strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", local_time);
    //     ESP_LOGI(TAG, "local_time: %s", time_str);
    //     ESP_LOGI(TAG, "[4.0] Entering deep sleep after recording for %d seconds", RECORD_TIME_SECONDS);
    //     char next_5am_seconds_str[10]; 
    //     snprintf(next_5am_seconds_str, sizeof(next_5am_seconds_str), "%d", next_5am_seconds);
    //     esp_sleep_enable_timer_wakeup((uint64_t)next_5am_seconds * 1000000);
    //     ESP_LOGI(TAG, "Entering deep sleep %s seconds", next_5am_seconds_str);
    //     esp_deep_sleep_start();
    // }
}
