/*
 * Copyright (c) 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Direct streaming TTS: ESP32 -> https://api.elevenlabs.io.
 * Only ElevenLabs account/voice credentials stored in local NVS; no proxy,
 * no hardcoded keys and no changes to the original Muse/Jollybot UI.
 *
 * WARNING: Ordinary ESP-IDF NVS is not encrypted. Treat access to the
 * device/flash and any complete flash dump as access to the ElevenLabs key.
 */
#include "muse_tts_elevenlabs.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_MUSE_TTS_ELEVENLABS
namespace {

constexpr char kNamespace[] = "eleven_tts";
constexpr char kApiHost[] = "https://api.elevenlabs.io/v1/text-to-speech/";
constexpr char kApiPath[] = "/stream?output_format=mp3_22050_32";
constexpr char kModel[] = "eleven_flash_v2_5";
constexpr size_t kMaxKey = 255;
constexpr size_t kMaxVoice = 63;
constexpr size_t kMaxText = 1023;       // Existing Muse caption storage.
constexpr size_t kMaxAudio = 512 * 1024;
constexpr int kHttpTimeoutMs = 10000;
constexpr int kWorkerStackBytes = 24 * 1024;

const char *const TAG = "muse_tts_elevenlabs";

struct Job {
    uint32_t id;
    char text[kMaxText + 1];
};

struct Response {
    uint32_t id;
    size_t bytes;
    bool mp3;
    bool failed;
};

QueueHandle_t s_jobs;
QueueHandle_t s_audio;
std::atomic<uint32_t> s_active{0};
bool s_initialized;

void wipe(void *ptr, size_t len)
{
    volatile unsigned char *p = static_cast<volatile unsigned char *>(ptr);
    while (len--) *p++ = 0;
}

bool valid_printable(const char *value, size_t max)
{
    if (!value) return false;
    size_t len = strnlen(value, max + 1);
    if (len < 1 || len > max) return false;
    for (size_t i = 0; i < len; i++) {
        if (static_cast<unsigned char>(value[i]) <= 32 ||
            static_cast<unsigned char>(value[i]) >= 127) return false;
    }
    return true;
}

bool valid_voice(const char *voice)
{
    if (!valid_printable(voice, kMaxVoice)) return false;
    for (const char *p = voice; *p; p++) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

esp_err_t read_config(char *key, size_t key_size, char *voice, size_t voice_size)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t a = key_size, b = voice_size;
    err = nvs_get_str(nvs, "api_key", key, &a);
    if (err == ESP_OK) err = nvs_get_str(nvs, "voice_id", voice, &b);
    nvs_close(nvs);
    if (err == ESP_OK && (!valid_printable(key, kMaxKey) || !valid_voice(voice))) {
        err = ESP_ERR_INVALID_ARG;
    }
    return err;
}

esp_err_t store(const char *field, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (value && *value) err = nvs_set_str(nvs, field, value);
    else {
        err = nvs_erase_key(nvs, field);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

bool current(uint32_t id) { return s_active.load() == id; }

bool enqueue(muse_tts_packet_t &packet)
{
    while (current(packet.id)) {
        if (xQueueSend(s_audio, &packet, pdMS_TO_TICKS(50)) == pdTRUE) return true;
    }
    return false;
}

/* Invoked only in the HTTPS worker task, never on the Muse/Noise task. */
esp_err_t on_http_event(esp_http_client_event_t *event)
{
    auto *state = static_cast<Response *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        strcasecmp(event->header_key, "Content-Type") == 0) {
        state->mp3 = strncasecmp(event->header_value, "audio/mpeg", 10) == 0 ||
                     strncasecmp(event->header_value, "audio/mp3", 9) == 0;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (!current(state->id)) return ESP_FAIL;
    if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
    if (!state->mp3 || !event->data || event->data_len < 0) {
        state->failed = true;
        return ESP_FAIL;
    }
    const auto *data = static_cast<const uint8_t *>(event->data);
    size_t remaining = static_cast<size_t>(event->data_len);
    if (remaining > kMaxAudio - state->bytes) {
        state->failed = true;
        ESP_LOGW(TAG, "TTS audio exceeds 512 KiB limit");
        return ESP_FAIL;
    }
    while (remaining) {
        muse_tts_packet_t packet{};
        packet.id = state->id;
        packet.kind = MUSE_TTS_DATA;
        packet.len = remaining > MUSE_TTS_PACKET_BYTES ? MUSE_TTS_PACKET_BYTES : remaining;
        memcpy(packet.bytes, data, packet.len);
        if (!enqueue(packet)) return ESP_FAIL;
        state->bytes += packet.len;
        remaining -= packet.len;
        data += packet.len;
    }
    return ESP_OK;
}

void synthesize(const Job &job)
{
    if (!current(job.id)) return;
    char key[kMaxKey + 1] = {};
    char voice[kMaxVoice + 1] = {};
    esp_err_t credentials = read_config(key, sizeof(key), voice, sizeof(voice));
    if (credentials != ESP_OK) {
        ESP_LOGW(TAG, "ElevenLabs credentials missing or invalid");
        wipe(key, sizeof(key));
        goto finish_error;
    }

    {
        char url[sizeof(kApiHost) + kMaxVoice + sizeof(kApiPath)];
        snprintf(url, sizeof(url), "%s%s%s", kApiHost, voice, kApiPath);
        Response response{job.id, 0, false, false};
        bool ok = false;
        cJSON *json = cJSON_CreateObject();
        if (json) {
            cJSON_AddStringToObject(json, "text", job.text);
            cJSON_AddStringToObject(json, "model_id", kModel);
            char *body = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (body) {
                esp_http_client_config_t cfg{};
                cfg.url = url;
                cfg.crt_bundle_attach = esp_crt_bundle_attach;
                cfg.event_handler = on_http_event;
                cfg.user_data = &response;
                cfg.timeout_ms = kHttpTimeoutMs;
                cfg.buffer_size = 2048;
                cfg.buffer_size_tx = 2048;
                cfg.disable_auto_redirect = true;
                esp_http_client_handle_t client = esp_http_client_init(&cfg);
                if (client) {
                    esp_http_client_set_method(client, HTTP_METHOD_POST);
                    esp_http_client_set_header(client, "xi-api-key", key);
                    esp_http_client_set_header(client, "Content-Type", "application/json");
                    esp_http_client_set_header(client, "Accept", "audio/mpeg");
                    esp_http_client_set_post_field(client, body, strlen(body));
                    esp_err_t result = esp_http_client_perform(client);
                    int status = esp_http_client_get_status_code(client);
                    ok = result == ESP_OK && status == 200 && response.mp3 &&
                         !response.failed && response.bytes > 0 && current(job.id);
                    if (!ok && current(job.id)) {
                        ESP_LOGW(TAG, "ElevenLabs TTS failed (HTTP %d, ESP %s)",
                                 status, esp_err_to_name(result));
                    }
                    esp_http_client_cleanup(client);
                }
                wipe(body, strlen(body));
                cJSON_free(body);
            }
        }
        wipe(key, sizeof(key));
        wipe(voice, sizeof(voice));
        muse_tts_packet_t end{};
        end.id = job.id;
        end.kind = ok ? MUSE_TTS_DONE : MUSE_TTS_FAILED;
        if (current(job.id)) enqueue(end);
        return;
    }

finish_error:
    muse_tts_packet_t end{};
    end.id = job.id;
    end.kind = MUSE_TTS_FAILED;
    if (current(job.id)) enqueue(end);
}

void worker(void *)
{
    Job job{};
    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE && current(job.id)) {
            synthesize(job);
        }
    }
}

} // namespace

extern "C" bool muse_tts_elevenlabs_init(void)
{
    if (s_initialized) return true;
    s_jobs = xQueueCreateWithCaps(1, sizeof(Job), MALLOC_CAP_SPIRAM);
    s_audio = xQueueCreateWithCaps(16, sizeof(muse_tts_packet_t), MALLOC_CAP_SPIRAM);
    if (!s_jobs || !s_audio ||
        xTaskCreatePinnedToCoreWithCaps(worker, "eleven_tts", kWorkerStackBytes, nullptr, 4, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "ElevenLabs worker initialization failed");
        return false;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Direct ElevenLabs TTS available (configure tts.key/tts.voice)");
    return true;
}

extern "C" bool muse_tts_elevenlabs_ready(void)
{
    if (!s_initialized) return false;
    char key[kMaxKey + 1] = {};
    char voice[kMaxVoice + 1] = {};
    bool ready = read_config(key, sizeof(key), voice, sizeof(voice)) == ESP_OK;
    wipe(key, sizeof(key));
    return ready;
}

extern "C" esp_err_t muse_tts_elevenlabs_set_key(const char *key)
{
    if (key && *key && !valid_printable(key, kMaxKey)) return ESP_ERR_INVALID_ARG;
    return store("api_key", key);
}

extern "C" esp_err_t muse_tts_elevenlabs_set_voice(const char *voice)
{
    if (voice && *voice && !valid_voice(voice)) return ESP_ERR_INVALID_ARG;
    return store("voice_id", voice);
}

extern "C" void muse_tts_elevenlabs_clear(void)
{
    muse_tts_elevenlabs_cancel();
    store("api_key", "");
    store("voice_id", "");
}

extern "C" bool muse_tts_elevenlabs_submit(uint32_t id, const char *text)
{
    if (!s_initialized || !id || !text || !*text) return false;
    Job job{};
    job.id = id;
    strlcpy(job.text, text, sizeof(job.text));
    s_active.store(id);
    return xQueueOverwrite(s_jobs, &job) == pdTRUE;
}

extern "C" bool muse_tts_elevenlabs_poll(muse_tts_packet_t *packet)
{
    return s_initialized && packet && xQueueReceive(s_audio, packet, 0) == pdTRUE;
}

extern "C" void muse_tts_elevenlabs_cancel(void)
{
    s_active.store(0);
}

#endif // CONFIG_MUSE_TTS_ELEVENLABS
