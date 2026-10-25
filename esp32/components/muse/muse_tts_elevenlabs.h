/*
 * Copyright (c) 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ElevenLabs voice synthesis for the original Muse gadget UI.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_TTS_PACKET_BYTES 1024

typedef enum {
    MUSE_TTS_DATA = 1,
    MUSE_TTS_DONE = 2,
    MUSE_TTS_FAILED = 3,
} muse_tts_packet_kind_t;

typedef struct {
    uint32_t id;
    uint16_t len;
    uint8_t kind;
    uint8_t bytes[MUSE_TTS_PACKET_BYTES];
} muse_tts_packet_t;

/* Keys live in NVS; never in the firmware sdkconfig or checked-in files. */
bool muse_tts_elevenlabs_init(void);
bool muse_tts_elevenlabs_ready(void);
esp_err_t muse_tts_elevenlabs_set_key(const char *key);
esp_err_t muse_tts_elevenlabs_set_voice(const char *voice);
void muse_tts_elevenlabs_clear(void);
bool muse_tts_elevenlabs_submit(uint32_t request_id, const char *text);
bool muse_tts_elevenlabs_poll(muse_tts_packet_t *packet);
void muse_tts_elevenlabs_cancel(void);

#ifdef __cplusplus
}
#endif
