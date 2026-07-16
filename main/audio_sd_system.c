#include "audio_sd_system.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h" // Essencial para gerenciar a PSRAM
#include "freertos/task.h"

static const char *TAG = "SOUND_SYSTEM";

//=============================================================================
// CONFIGURAÇÕES DE HARDWARE (PINOS)
//=============================================================================
// Definição dos pinos do Áudio (PCM5102A)
#define I2S_BCK_IO           (GPIO_NUM_4)
#define I2S_DIN_IO           (GPIO_NUM_7)
#define I2S_WS_IO            (GPIO_NUM_6)

// Definição dos pinos do Cartão SD (Modo SDMMC 1-bit)
#define SDMMC_CMD_IO         (GPIO_NUM_11)
#define SDMMC_CLK_IO         (GPIO_NUM_12)
#define SDMMC_D0_IO          (GPIO_NUM_13)

//=============================================================================
// VARIÁVEIS GLOBAIS DO SISTEMA DE ÁUDIO
//=============================================================================
static i2s_chan_handle_t tx_chan = NULL;

// Ponteiro para o buffer alocado obrigatoriamente na PSRAM
static int16_t *snare_buffer = NULL;
static size_t snare_samples_qty = 0;             // Quantidade de amostras (16-bit)

// Variáveis de controle de reprodução (voláteis para comunicação segura entre cores)
static volatile int playback_sample_index = -1;  // -1 significa que está em silêncio
static volatile float playback_volume = 1.0f;

//=============================================================================
// CONTROLADORES DE HARDWARE (I2S & SD CARD)
//=============================================================================

// 1. Inicialização do DAC PCM5102A com foco em baixíssima latência
void init_audio_pcm5102a(void) {
    ESP_LOGI(TAG, "Configurando PCM5102A para latencia extrema...");
    
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    
    // Reduzido ao limite operacional estável: 2 buffers de apenas 32 amostras
    // Latência pura de hardware: (2 * 32) / 44100 = ~1.45 milissegundos!
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

// 2. Inicialização do Barramento SDMMC do Cartão SD
void init_sd_card(void) {
    ESP_LOGI(TAG, "Montando Cartao SD via SDMMC...");
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
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

// 3. Envio bruto de áudio diretamente para o barramento I2S DMA
void audio_play_raw(const int16_t *buffer, size_t bytes_to_write) {
    if (tx_chan == NULL) return;
    size_t bytes_written = 0;
    i2s_channel_write(tx_chan, buffer, bytes_to_write, &bytes_written, portMAX_DELAY);
}

//=============================================================================
// MANIPULAÇÃO DE ÁUDIO E ARQUIVOS (WAV -> PSRAM)
//=============================================================================

// Carrega o arquivo de áudio WAV do cartão SD diretamente para a PSRAM, pulando metadados
void load_snare_to_ram(void) {
    const char *file_path = "/sdcard/38.wav";
    FILE *f = fopen(file_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Erro fatal: Nao foi possivel abrir %s", file_path);
        return;
    }

    // Validação básica do cabeçalho RIFF/WAVE
    char header[12];
    if (fread(header, 1, 12, f) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Erro: Arquivo %s nao e um WAV valido.", file_path);
        fclose(f);
        return;
    }

    uint32_t data_size = 0;
    uint32_t data_offset = 0;
    char chunk_id[4];
    uint32_t chunk_size;

    // Varre os subchunks do arquivo procurando pelo bloco de áudio ("data")
    while (fread(chunk_id, 1, 4, f) == 4) {
        if (fread(&chunk_size, 4, 1, f) != 1) {
            break;
        }

        if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            data_offset = ftell(f); // Guarda a posição exata onde o áudio bruto começa
            break;
        }

        // Se não for o chunk "data" (como LIST ou JUNK), pula mantendo o alinhamento de Word do RIFF
        uint32_t skip_size = (chunk_size + 1) & ~1;
        fseek(f, skip_size, SEEK_CUR);
    }

    if (data_size == 0 || data_offset == 0) {
        ESP_LOGE(TAG, "Erro: Bloco de audio 'data' nao encontrado no WAV.");
        fclose(f);
        return;
    }

    // Define o tamanho em amostras (cada amostra de 16 bits tem 2 bytes)
    snare_samples_qty = data_size / sizeof(int16_t);

    // Aloca memória exclusivamente na PSRAM para caber o arquivo inteiro
    snare_buffer = (int16_t *)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (snare_buffer == NULL) {
        ESP_LOGE(TAG, "Erro: Memoria PSRAM insuficiente!");
        fclose(f);
        return;
    }

    // Lê os dados brutos de áudio do SD, jogando-os na PSRAM
    fseek(f, data_offset, SEEK_SET);
    size_t bytes_read = fread(snare_buffer, 1, data_size, f);
    
    ESP_LOGI(TAG, "Sucesso: %d bytes (%d amostras) carregados na PSRAM. Metadados descartados!", 
             bytes_read, snare_samples_qty);
    fclose(f);
}

// Ativa a reprodução setando o volume baseado no Velocity do MIDI e reinicia o ponteiro
void trigger_snare(uint8_t velocity) {
    if (snare_buffer == NULL || velocity == 0) return;

    playback_volume = (float)velocity / 127.0f;
    playback_sample_index = 0; // Inicia na amostra zero de forma atômica
}

//=============================================================================
// MOTORES DE EXECUÇÃO (TASKS FREE_RTOS)
//=============================================================================

// Task contínua que mantém a esteira do DMA do I2S aquecida (evita estalos e cliques)
static void audio_player_task(void *pvParameters) {
    // 64 amostras de 16 bits = 128 bytes por bloco
    #define CHUNK_SIZE_SAMPLES 64 
    int16_t temp_buffer[CHUNK_SIZE_SAMPLES];

    while (1) {
        if (playback_sample_index >= 0 && snare_buffer != NULL) {
            size_t samples_to_play = CHUNK_SIZE_SAMPLES;
            bool finished = false;

            // Evita leitura além do limite real do arquivo alocado na PSRAM
            if (playback_sample_index + samples_to_play > snare_samples_qty) {
                samples_to_play = snare_samples_qty - playback_sample_index;
                finished = true;
            }

            if (samples_to_play > 0) {
                // Copia as amostras aplicando o ganho do Velocity do MIDI
                for (size_t i = 0; i < samples_to_play; i++) {
                    int32_t sample = (int32_t)(snare_buffer[playback_sample_index + i] * playback_volume);
                    
                    // Limitador digital (Anti-clipping) para proteger os amplificadores
                    if (sample > 32767) sample = 32767;
                    else if (sample < -32768) sample = -32768;
                    
                    temp_buffer[i] = (int16_t)sample;
                }

                // Aplica fadeout rápido de atenuação no bloco final para evitar cliques
                if (finished) {
                    for (size_t i = 0; i < samples_to_play; i++) {
                        uint32_t fade_factor = 256 - ((i * 256) / samples_to_play);
                        temp_buffer[i] = (int16_t)((temp_buffer[i] * fade_factor) >> 8);
                    }
                }

                // Completa o buffer restante com silêncio caso o arquivo acabe no meio do chunk
                if (samples_to_play < CHUNK_SIZE_SAMPLES) {
                    memset(&temp_buffer[samples_to_play], 0, (CHUNK_SIZE_SAMPLES - samples_to_play) * sizeof(int16_t));
                }

                playback_sample_index += samples_to_play;
            }

            if (finished) {
                playback_sample_index = -1; // Desliga a reprodução do player
            }
        } else {
            // Silêncio digital absoluto para manter o barramento I2S operante sem ruídos
            memset(temp_buffer, 0, CHUNK_SIZE_SAMPLES * sizeof(int16_t));
        }

        // Escreve os dados tratados no DMA do I2S
        audio_play_raw(temp_buffer, CHUNK_SIZE_SAMPLES * sizeof(int16_t));
    }
}

// Inicializa a task de áudio de tempo real travada no Core 1 (substituta da antiga disparar_teste_de_audio)
void audio_system_start(void) {
    xTaskCreatePinnedToCore(
        audio_player_task,
        "audio_player",
        4096,
        NULL,
        22, // Prioridade de tempo real máxima (acima de tarefas de sistema)
        NULL,
        1   // Travado estritamente no Core 1
    );
    ESP_LOGI(TAG, "Motor de Audio de Baixissima Latencia Ativado no Core 1.");
}