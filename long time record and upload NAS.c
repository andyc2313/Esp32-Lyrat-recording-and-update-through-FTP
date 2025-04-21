#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "wav_encoder.h"
#include "wav_fatfs_stream.h"
#include "board.h"
#include "audio_event_iface.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "esp_sleep.h"
#include "ftp_client.h"

#define MAX_FILES_TO_UPLOAD 10
#define RECORD_TIME_SECONDS 10
#define WAKEUP_TIME_SECONDS 10
#define UPLOAD_HOUR 23
#define UPLOAD_MINUTE 59
#define UPLOAD_SECOND 0

static const char *TAG = "audio_pipeline";
char upload_files[MAX_FILES_TO_UPLOAD][128];  // 存儲上傳的檔案路徑
int num_files_to_upload = 0;  // 待上傳的檔案數量

void upload_files_to_ftp() {
    ESP_LOGI(TAG, "開始定時上傳");

    static NetBuf_t* ftpClientNetBuf = NULL;
    FtpClient* ftpClient = getFtpClient();
    int connect = ftpClient->ftpClientConnect(CONFIG_FTP_SERVER, CONFIG_FTP_PORT, &ftpClientNetBuf);

    if (connect == 0) {
        ESP_LOGE(TAG, "FTP 伺服器連接失敗");
        esp_restart();
    }

    int login = ftpClient->ftpClientLogin(CONFIG_FTP_USER, CONFIG_FTP_PASSWORD, ftpClientNetBuf);
    if (login == 0) {
        ESP_LOGE(TAG, "FTP 伺服器登錄失敗");
        esp_restart();
    }

    for (int i = 0; i < num_files_to_upload; i++) {
        char* file_path = upload_files[i];
        char new_path[128]; 
        snprintf(new_path, sizeof(new_path), "/Lab303/esp32/test/%s", basename(file_path));
        ftpClient->ftpClientPut(file_path, new_path, FTP_CLIENT_BINARY, ftpClientNetBuf);
        char* lastResponse = getLastResponseFtpClient(ftpClientNetBuf);
        if (lastResponse != NULL) {
            int responseCode;
            if (sscanf(lastResponse, "%d", &responseCode) == 1) {
                if (responseCode >= 200 && responseCode < 300) {
                    ESP_LOGI(TAG, "檔案上傳成功: %s", file_path);
                } else {
                    ESP_LOGE(TAG, "檔案上傳失敗: %s", file_path);
                }
            }
        }
    }

    ftpClient->ftpClientQuit(ftpClientNetBuf);
    num_files_to_upload = 0;  // 清空待上傳的檔案陣列
}

void app_main(void) {
    init_nvs();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_sta_config_t cfg_sta = {
        .ssid = "TP-Link_207C",
        .password = "303303303",
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
    i2s_cfg.i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[3.2] Create wav encoder to encode wav format");
    wav_encoder_cfg_t wav_cfg = DEFAULT_WAV_ENCODER_CONFIG();
    wav_encoder = wav_encoder_init(&wav_cfg);

    ESP_LOGI(TAG, "[3.3] Create fatfs stream to write wav file to sdcard");
    fatfs_stream_cfg_t fs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fs_cfg.type = AUDIO_STREAM_WRITER;
    wav_fatfs_stream_writer = fatfs_stream_init(&fs_cfg);

    if (1) {
        char filename[64];
        time_t now;
        time(&now);
        struct tm *local_time = localtime(&now);
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

            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)i2s_stream_reader 
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS 
                && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))){
                ESP_LOGW(TAG, "[ * ] Stop event received");
                break;
            }
        }

        vTaskDelay(5 * 1000 / portTICK_PERIOD_MS);

        audio_pipeline_stop(pipeline_wav);
        audio_pipeline_wait_for_stop(pipeline_wav);
        audio_pipeline_terminate(pipeline_wav);
                audio_pipeline_unregister_more(pipeline_wav, i2s_stream_reader, wav_encoder, wav_fatfs_stream_writer, NULL);

        // 將檔案路徑添加到陣列中
        if (num_files_to_upload < MAX_FILES_TO_UPLOAD) {
            snprintf(upload_files[num_files_to_upload], sizeof(upload_files[num_files_to_upload]), "%s", filename);
            num_files_to_upload++;
        }

        audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);
        audio_event_iface_destroy(evt);
        audio_pipeline_deinit(pipeline_wav);
        audio_element_deinit(i2s_stream_reader);
        audio_element_deinit(wav_encoder);
        audio_element_deinit(wav_fatfs_stream_writer);
        esp_periph_set_destroy(set);

        ESP_LOGI(TAG, "[7.0] Entering deep sleep after recording for %d seconds", RECORD_TIME_SECONDS);
        vTaskDelay(5 * 1000 / portTICK_PERIOD_MS);
        esp_sleep_enable_timer_wakeup(WAKEUP_TIME_SECONDS * 1000000);
        ESP_LOGI(TAG, "Entering deep sleep");
        esp_deep_sleep_start();
    }
}

void obtain_time(void) {
    // 等待系統時間同步
    ESP_LOGI(TAG, "Waiting for system time to be set...");
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;
    while (timeinfo.tm_year < (2020 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}

void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
    }
}

void set_upload_timer(void) {
    struct tm timeinfo;
    time_t now;
    time(&now);
    localtime_r(&now, &timeinfo);
    timeinfo.tm_hour = UPLOAD_HOUR;
    timeinfo.tm_min = UPLOAD_MINUTE;
    timeinfo.tm_sec = UPLOAD_SECOND;

    time_t future = mktime(&timeinfo);
    time_t current = time(NULL);
    if (current > future) {
        future += 24 * 60 * 60;
    }
    int64_t delay = (future - current) * 1000000;
    esp_sleep_enable_timer_wakeup(delay);
    ESP_LOGI(TAG, "Entering deep sleep for next upload");
    esp_deep_sleep_start();
}
