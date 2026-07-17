#include "audio_sd_system.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"

static const char *TAG = "SOUND_SYSTEM";

// Tabela de ganho recalculada para curva exponencial (Gain = (velocity / 127) ^ 1.5)
// Normalizada para um teto seguro e potente de 0.85f (máximo volume antes de saturar o mixer)
static const float VELOCITY_GAIN_LUT[128] = {
    0.0000f, 0.0002f, 0.0005f, 0.0009f, 0.0014f, 0.0020f, 0.0027f, 0.0035f,
    0.0044f, 0.0054f, 0.0065f, 0.0076f, 0.0088f, 0.0100f, 0.0113f, 0.0128f,
    0.0143f, 0.0158f, 0.0174f, 0.0191f, 0.0209f, 0.0228f, 0.0247f, 0.0267f,
    0.0287f, 0.0308f, 0.0330f, 0.0353f, 0.0377f, 0.0401f, 0.0426f, 0.0452f,
    0.0478f, 0.0506f, 0.0534f, 0.0562f, 0.0591f, 0.0621f, 0.0652f, 0.0683f,
    0.0715f, 0.0748f, 0.0781f, 0.0815f, 0.0850f, 0.0886f, 0.0921f, 0.0958f,
    0.0995f, 0.1033f, 0.1072f, 0.1112f, 0.1152f, 0.1193f, 0.1234f, 0.1276f,
    0.1319f, 0.1362f, 0.1406f, 0.1451f, 0.1497f, 0.1543f, 0.1590f, 0.1637f,
    0.1685f, 0.1733f, 0.1782f, 0.1832f, 0.1883f, 0.1934f, 0.1986f, 0.2039f,
    0.2092f, 0.2146f, 0.2200f, 0.2255f, 0.2311f, 0.2368f, 0.2425f, 0.2483f,
    0.2541f, 0.2601f, 0.2660f, 0.2721f, 0.2782f, 0.2844f, 0.2906f, 0.2970f,
    0.3033f, 0.3098f, 0.3163f, 0.3229f, 0.3295f, 0.3362f, 0.3430f, 0.3498f,
    0.3567f, 0.3636f, 0.3707f, 0.3778f, 0.3849f, 0.3922f, 0.3995f, 0.4068f,
    0.4143f, 0.4217f, 0.4293f, 0.4369f, 0.4446f, 0.4523f, 0.4601f, 0.4680f,
    0.4759f, 0.4839f, 0.4920f, 0.5001f, 0.5083f, 0.5165f, 0.5248f, 0.5332f,
    0.5416f, 0.5501f, 0.5587f, 0.5673f, 0.5760f, 0.5847f, 0.5935f, 0.8500f
};

#define I2S_BCK_IO           (GPIO_NUM_4)
#define I2S_DIN_IO           (GPIO_NUM_7)
#define I2S_WS_IO            (GPIO_NUM_6)

#define SDMMC_CMD_IO         (GPIO_NUM_11)
#define SDMMC_CLK_IO         (GPIO_NUM_12)
#define SDMMC_D0_IO          (GPIO_NUM_13)

static i2s_chan_handle_t tx_chan = NULL;

//=============================================================================
// GERENCIADOR DE POLIFONIA (12 VOZES SIMULTÂNEAS)
//=============================================================================
#define MAX_VOICES 12

typedef struct {
    int16_t *buffer;
    size_t sample_qty;
    volatile int playback_index; // -1 indica canal livre
    volatile float volume;
    int midi_note;
    int group; 
} audio_voice_t;

static audio_voice_t voices[MAX_VOICES];

void init_audio_pcm5102a(void) {
    ESP_LOGI(TAG, "Configurando PCM5102A para latencia extrema...");
    
    // Inicializa a lista de vozes como inativas
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].playback_index = -1;
        voices[i].buffer = NULL;
        voices[i].midi_note = -1;
        voices[i].group = -1;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 2; 
    chan_cfg.dma_frame_num = 32; 

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DIN_IO,
            .din  = I2S_GPIO_UNUSED,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
}

void init_sd_card(void) {
    ESP_LOGI(TAG, "Montando Cartao SD via SDMMC...");
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10, // Aumentado para suportar leitura dinâmica
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SDMMC_CLK_IO;
    slot_config.cmd = SDMMC_CMD_IO;
    slot_config.d0  = SDMMC_D0_IO;
    slot_config.width = 1;

    ESP_ERROR_CHECK(esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card));
}

void audio_play_raw(const int16_t *buffer, size_t bytes_to_write) {
    if (tx_chan == NULL) return;
    size_t bytes_written = 0;
    i2s_channel_write(tx_chan, buffer, bytes_to_write, &bytes_written, portMAX_DELAY);
}

// Encontra uma voz livre (ou rouba a mais antiga/avançada se necessário) e dispara
void trigger_audio_buffer(int16_t *buffer, size_t sample_qty, uint8_t velocity, 
                          int midi_note, int group, int off_by, int polyphony, bool is_silence) {
    
    // --- LÓGICA 1: OFF_BY (Corte cruzado, ex: Hi-Hat Aberto cortado por Hi-Hat Fechado) ---
    if (off_by != -1) {
        for (int i = 0; i < MAX_VOICES; i++) {
            // Se a voz está tocando e pertence ao grupo que esta nova nota deve cortar
            if (voices[i].playback_index >= 0 && voices[i].group == off_by) {
                voices[i].playback_index = -1; // Interrompe o áudio imediatamente
            }
        }
    }

    // --- LÓGICA 2: POLYPHONY (Limite de vozes simultâneas por grupo) ---
    if (group != -1 && polyphony > 0) {
        int active_voices_in_group = 0;
        int oldest_voice_idx = -1;
        int max_progress = -1;

        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].playback_index >= 0 && voices[i].group == group) {
                active_voices_in_group++;
                // A voz com maior índice de reprodução está mais perto do fim (mais antiga)
                if (voices[i].playback_index > max_progress) {
                    max_progress = voices[i].playback_index;
                    oldest_voice_idx = i;
                }
            }
        }

        // Se estourar o limite de polifonia do grupo, desativa a voz mais antiga dele
        if (active_voices_in_group >= polyphony && oldest_voice_idx != -1) {
            voices[oldest_voice_idx].playback_index = -1;
        }
    }

    // --- LÓGICA 3: SAMPLE=*SILENCE ---
    if (is_silence) {
        // Regiões de silêncio servem apenas para disparar o "off_by" (mutes).
        // Como o corte já foi processado acima, encerramos sem ocupar canais de áudio reais.
        return; 
    }

    if (buffer == NULL || sample_qty == 0) return;

    int target_voice = -1;

    // Procura canal livre
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].playback_index == -1) {
            target_voice = i;
            break;
        }
    }

    // Voice Stealing global padrão se faltar canais gerais
    if (target_voice == -1) {
        int max_index = -1;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].playback_index > max_index) {
                max_index = voices[i].playback_index;
                target_voice = i;
            }
        }
    }

    if (target_voice != -1) {
        voices[target_voice].buffer = buffer;
        voices[target_voice].sample_qty = sample_qty;
        voices[target_voice].midi_note = midi_note; // Salva metadado
        voices[target_voice].group = group;         // Salva metadado
        
        if (velocity > 127) velocity = 127;
        voices[target_voice].volume = VELOCITY_GAIN_LUT[velocity]; 
        voices[target_voice].playback_index = 0; 
    }
}

// Task de mistura em tempo real (Soma polifônica)
static void audio_player_task(void *pvParameters) {
    #define CHUNK_SIZE_SAMPLES 64 
    int16_t temp_buffer[CHUNK_SIZE_SAMPLES];

    while (1) {
        // Zera o buffer temporário de mistura
        int32_t mixed_samples[CHUNK_SIZE_SAMPLES] = {0};
        bool active_sound = false;

        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].playback_index >= 0 && voices[v].buffer != NULL) {
                active_sound = true;
                int start_idx = voices[v].playback_index;
                size_t qty = voices[v].sample_qty;
                float vol = voices[v].volume;

                for (size_t i = 0; i < CHUNK_SIZE_SAMPLES; i++) {
                    int current_sample_pos = start_idx + i;
                    if (current_sample_pos < qty) {
                        mixed_samples[i] += (int32_t)(voices[v].buffer[current_sample_pos] * vol);
                    }
                }
                
                voices[v].playback_index += CHUNK_SIZE_SAMPLES;
                if (voices[v].playback_index >= qty) {
                    voices[v].playback_index = -1; // Desativa canal finalizado
                }
            }
        }

        if (active_sound) {
            // Conversão de 32-bit para 16-bit com algoritmo Soft Clipper funcional
            for (size_t i = 0; i < CHUNK_SIZE_SAMPLES; i++) {
                int32_t s = mixed_samples[i];
                
                // Limite a partir do qual a compressão suave começa (~75% da amplitude máxima)
                int32_t threshold = 24576; 
                
                if (s > threshold) {
                    s = threshold + ((s - threshold) / 3);
                } else if (s < -threshold) {
                    s = -threshold + ((s + threshold) / 3);
                }
                
                // Hard limiting final de segurança contra estouros absolutos
                if (s > 32767) s = 32767;
                else if (s < -32768) s = -32768;
                
                temp_buffer[i] = (int16_t)s;
            }
        } else {
            memset(temp_buffer, 0, sizeof(temp_buffer));
        }

        audio_play_raw(temp_buffer, CHUNK_SIZE_SAMPLES * sizeof(int16_t));
    }
}

void audio_system_start(void) {
    xTaskCreatePinnedToCore(audio_player_task, "audio_player", 4096, NULL, 22, NULL, 1);
    ESP_LOGI(TAG, "Mixer Polifonico de 12 Vozes Ativado.");
}