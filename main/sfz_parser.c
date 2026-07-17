#include "sfz_parser.h"
#include "audio_sd_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SFZ_PARSER";

// Tabela Hash/Mapeamento direto para as 128 notas MIDI
static drum_note_map_t drum_kit[MAX_MIDI_NOTES];
static size_t total_allocated_psram = 0;

//=============================================================================
// SISTEMA DE CACHE PARA EVITAR DUPLICIDADE NA PSRAM
//=============================================================================
#define MAX_CACHED_SAMPLES 128 // Quantidade máxima de arquivos únicos no kit

typedef struct {
    char filepath[256];
    int16_t *buffer;
    size_t sample_qty;
} cached_sample_t;

static cached_sample_t sample_cache[MAX_CACHED_SAMPLES];
static int cached_sample_count = 0;

//=============================================================================
// UTILITÁRIO: CARREGADOR DE WAV COM EVITAÇÃO DE DUPLICADOS
//=============================================================================
static int16_t* load_wav_file(const char *filepath, size_t *out_samples_qty) {
    // 1. Verifica se o arquivo já está na PSRAM (Cache)
    for (int i = 0; i < cached_sample_count; i++) {
        if (strcmp(sample_cache[i].filepath, filepath) == 0) {
            *out_samples_qty = sample_cache[i].sample_qty;
            ESP_LOGI(TAG, "-> Reutilizando sample do cache (PSRAM): %s", filepath);
            return sample_cache[i].buffer;
        }
    }

    // 2. Se não estiver no cache, carrega do SD normalmente
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Arquivo nao encontrado: %s", filepath);
        return NULL;
    }

    char header[12];
    if (fread(header, 1, 12, f) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(f);
        return NULL;
    }

    char chunk_id[4];
    uint32_t chunk_size;
    uint32_t data_size = 0;
    
    // Variáveis para capturar as propriedades reais do WAV
    uint16_t num_channels = 2;     // Padrão estéreo caso não encontre o chunk fmt
    uint16_t bits_per_sample = 16; // Padrão 16 bits
    uint32_t sample_rate = 44100;

    // Varre os subchunks do arquivo WAV
    while (fread(chunk_id, 1, 4, f) == 4) {
        if (fread(&chunk_size, 4, 1, f) != 1) break;
        
        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t audio_format;
            fread(&audio_format, 2, 1, f);
            fread(&num_channels, 2, 1, f);
            fread(&sample_rate, 4, 1, f);
            
            fseek(f, 6, SEEK_CUR); // Pula ByteRate (4 bytes) e BlockAlign (2 bytes)
            fread(&bits_per_sample, 2, 1, f);
            
            // Se o chunk 'fmt ' for estendido (maior que 16 bytes padrão), pula o resto dele
            if (chunk_size > 16) {
                fseek(f, chunk_size - 16, SEEK_CUR);
            }
            // Alinhamento RIFF: se o tamanho do chunk for ímpar, há um byte de padding vazio
            if (chunk_size & 1) fseek(f, 1, SEEK_CUR);
        }
        else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            break;
        }
        else {
            // Pula chunks irrelevantes (LIST, metadata, etc.) respeitando o alinhamento par
            fseek(f, (chunk_size + 1) & ~1, SEEK_CUR);
        }
    }

    if (data_size == 0) {
        fclose(f);
        return NULL;
    }

    // --- VALIDAÇÕES DE SEGURANÇA ---
    if (bits_per_sample != 16) {
        ESP_LOGE(TAG, "Erro: O arquivo %s possui %d bits. Apenas 16-bits e suportado!", filepath, bits_per_sample);
        fclose(f);
        return NULL;
    }
    if (sample_rate != 44100) {
        ESP_LOGW(TAG, "Aviso: O arquivo %s esta em %dHz. Esperado 44100Hz. O pitch sofrera alteracao!", filepath, sample_rate);
    }

    int16_t *buffer = NULL;
    size_t samples_qty = 0;
    size_t allocated_size = 0;

    if (num_channels == 1) {
        // --- ARQUIVO MONO DETECTADO ---
        size_t mono_samples_qty = data_size / sizeof(int16_t);
        samples_qty = mono_samples_qty * 2; // O número de amostras dobra no estéreo
        allocated_size = data_size * 2;     // O tamanho em bytes dobra

        // Aloca o tamanho total (estéreo) diretamente na PSRAM
        buffer = (int16_t *)heap_caps_malloc(allocated_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) {
            ESP_LOGE(TAG, "Sem memoria PSRAM para conversao Mono->Stereo: %s", filepath);
            fclose(f);
            return NULL;
        }

        // Lê os dados Mono do SD diretamente na PRIMEIRA METADE do buffer alocado
        fread(buffer, 1, data_size, f);

        // Expansão In-Place (Varre de trás para frente para não sobrescrever dados antes da hora)
        // Nota: O índice 'i' DEVE ser do tipo int (com sinal) para o laço terminar corretamente em 0
        for (int i = (int)mono_samples_qty - 1; i >= 0; i--) {
            int16_t sample = buffer[i];
            buffer[2 * i]     = sample; // Canal Esquerdo
            buffer[2 * i + 1] = sample; // Canal Direito
        }
        
        ESP_LOGI(TAG, "-> Arquivo Mono convertido para Estereo com sucesso: %s", filepath);
    } 
    else {
        // --- ARQUIVO ESTÉREO PADRÃO ---
        samples_qty = data_size / sizeof(int16_t);
        allocated_size = data_size;

        buffer = (int16_t *)heap_caps_malloc(allocated_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) {
            ESP_LOGE(TAG, "Sem memoria PSRAM para: %s", filepath);
            fclose(f);
            return NULL;
        }

        fread(buffer, 1, allocated_size, f);
    }

    fclose(f);
    *out_samples_qty = samples_qty;
    total_allocated_psram += allocated_size;

    // 3. Adiciona ao cache para futuros mapeamentos
    if (cached_sample_count < MAX_CACHED_SAMPLES) {
        strncpy(sample_cache[cached_sample_count].filepath, filepath, sizeof(sample_cache[cached_sample_count].filepath) - 1);
        sample_cache[cached_sample_count].buffer = buffer;
        sample_cache[cached_sample_count].sample_qty = samples_qty;
        cached_sample_count++;
        ESP_LOGI(TAG, "-> Carregado de %s e cacheado com sucesso (Tamanho: %d bytes)", filepath, allocated_size);
    } else {
        ESP_LOGW(TAG, "Aviso: Cache de samples cheio (%d). Carregado sem cachear.", MAX_CACHED_SAMPLES);
    }

    return buffer;
}

// Limpeza de espaços em branco
static void trim(char *str) {
    int len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\r' || str[len - 1] == '\n' || str[len - 1] == '\t')) {
        str[--len] = '\0';
    }
    int start = 0;
    while (str[start] == ' ' || str[start] == '\t') start++;
    if (start > 0) memmove(str, str + start, len - start + 1);
}

// Auxiliar para juntar o default_path com o nome do sample de forma segura
static void build_full_path(char *dest, size_t dest_size, const char *default_path, const char *sample_name) {
    if (strlen(default_path) > 0) {
        int len = strlen(default_path);
        // Garante que não haverá barras duplicadas ou faltando
        if (default_path[len - 1] == '/' || sample_name[0] == '/') {
            snprintf(dest, dest_size, "/sdcard/%s%s", default_path, sample_name);
        } else {
            snprintf(dest, dest_size, "/sdcard/%s/%s", default_path, sample_name);
        }
    } else {
        snprintf(dest, dest_size, "/sdcard/%s", sample_name);
    }
}

//=============================================================================
// PARSER DO ARQUIVO SFZ (CORRIGIDO PARA ACEITAR NOTA MIDI 0)
//=============================================================================
void sfz_parser_load_kit(const char *sfz_path) {
    FILE *f = fopen(sfz_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Nao foi possivel abrir o arquivo SFZ: %s", sfz_path);
        return;
    }

    ESP_LOGI(TAG, "Iniciando processamento do arquivo SFZ...");
    
    // Limpa o mapa, reseta o cache de duplicados e o contador de alocação
    memset(drum_kit, 0, sizeof(drum_kit));
    memset(sample_cache, 0, sizeof(sample_cache));
    cached_sample_count = 0;
    total_allocated_psram = 0;

    char line[512];
    
    // Variáveis de estado do leitor (0xFF / 255 agora significa "sem nota configurada")
    uint8_t current_group_key = 0xFF; 
    uint8_t current_group_lovel = 0;
    uint8_t current_group_hivel = 127;
    uint8_t current_group_seq_length = 1;
    int current_group_id = -1;         // <-- ADICIONADO
    int current_group_off_by = -1;     // <-- ADICIONADO
    int current_group_polyphony = 0;   // <-- ADICIONADO
    char default_path[128] = ""; 

    // Estrutura temporária para a Região atual sendo lida
    sfz_region_t temp_region;
    char temp_sample_name[128] = "";
    bool inside_region = false;

    // Macro/Função lambda inline utilitária para evitar duplicação ao salvar regiões
    #define SAVE_CURRENT_REGION_IF_VALID() \
    if (inside_region && current_group_key != 0xFF) { \
        uint8_t key = current_group_key; \
        if (drum_kit[key].region_count < MAX_REGIONS_PER_NOTE) { \
            int idx = drum_kit[key].region_count; \
            drum_kit[key].regions[idx] = temp_region; \
            if (temp_region.is_silence) { \
                drum_kit[key].regions[idx].buffer = NULL; \
                drum_kit[key].regions[idx].sample_qty = 0; \
                drum_kit[key].region_count++; \
            } else if (strlen(temp_sample_name) > 0) { \
                char full_path[256]; \
                build_full_path(full_path, sizeof(full_path), default_path, temp_sample_name); \
                drum_kit[key].regions[idx].buffer = load_wav_file(full_path, &drum_kit[key].regions[idx].sample_qty); \
                if (drum_kit[key].regions[idx].buffer) { \
                    drum_kit[key].region_count++; \
                } \
            } \
        } \
        inside_region = false; \
    }

while (fgets(line, sizeof(line), f)) {
        char *comment = strstr(line, "//");
        if (comment) *comment = '\0';
        trim(line);
        if (line[0] == '\0') continue;

        // Detectou cabeçalho de Grupo
        if (strstr(line, "<group>")) {
            SAVE_CURRENT_REGION_IF_VALID();
            
            // Reseta variáveis do grupo para padrões
            current_group_key = 0xFF;
            current_group_lovel = 0;
            current_group_hivel = 127;
            current_group_seq_length = 1;
            current_group_id = -1;       // <-- ADICIONADO
            current_group_off_by = -1;   // <-- ADICIONADO
            current_group_polyphony = 0; // <-- ADICIONADO

            char *p = strstr(line, "<group>");
            memset(p, ' ', 7);
        }

        // Detectou cabeçalho de Região
        if (strstr(line, "<region>")) {
            SAVE_CURRENT_REGION_IF_VALID();
            
            inside_region = true;
            memset(&temp_region, 0, sizeof(temp_region));
            temp_region.lovel = current_group_lovel;
            temp_region.hivel = current_group_hivel;
            temp_region.seq_length = current_group_seq_length;
            temp_region.seq_position = 1;
            temp_region.group = current_group_id;       // <-- Herda do Escopo do Grupo
            temp_region.off_by = current_group_off_by;   // <-- Herda do Escopo do Grupo
            temp_region.polyphony = current_group_polyphony; // <-- Herda do Escopo do Grupo
            temp_region.is_silence = false;             // <-- Padrão Falso
            strcpy(temp_sample_name, "");

            char *p = strstr(line, "<region>");
            memset(p, ' ', 8);
        }

        char *token = strtok(line, " \t\r\n");
        while (token) {
            char *eq = strchr(token, '=');
            if (eq) {
                *eq = '\0';
                char *opcode = token;
                char *value = eq + 1;
                trim(opcode);
                trim(value);

                if (strcmp(opcode, "default_path") == 0) {
                    strncpy(default_path, value, sizeof(default_path) - 1);
                    for (int i = 0; default_path[i]; i++) {
                        if (default_path[i] == '\\') default_path[i] = '/';
                    }
                }
                // --- PARSER DE ESCOPO: BLOCO <GROUP> ---
                else if (!inside_region) {
                    if (strcmp(opcode, "key") == 0) current_group_key = atoi(value);
                    else if (strcmp(opcode, "lovel") == 0) current_group_lovel = atoi(value);
                    else if (strcmp(opcode, "hivel") == 0) current_group_hivel = atoi(value);
                    else if (strcmp(opcode, "seq_length") == 0) current_group_seq_length = atoi(value);
                    else if (strcmp(opcode, "group") == 0) current_group_id = atoi(value);        // <-- ADICIONADO[cite: 10]
                    else if (strcmp(opcode, "off_by") == 0) current_group_off_by = atoi(value);   // <-- ADICIONADO[cite: 10]
                    else if (strcmp(opcode, "polyphony") == 0) current_group_polyphony = atoi(value); // <-- ADICIONADO[cite: 10]
                } 
                // --- PARSER DE ESCOPO: BLOCO <REGION> ---
                else {
                    if (strcmp(opcode, "key") == 0) current_group_key = atoi(value);
                    else if (strcmp(opcode, "lovel") == 0) temp_region.lovel = atoi(value);
                    else if (strcmp(opcode, "hivel") == 0) temp_region.hivel = atoi(value);
                    else if (strcmp(opcode, "seq_length") == 0) temp_region.seq_length = atoi(value);
                    else if (strcmp(opcode, "seq_position") == 0) temp_region.seq_position = atoi(value);
                    else if (strcmp(opcode, "group") == 0) temp_region.group = atoi(value);        // <-- ADICIONADO[cite: 10]
                    else if (strcmp(opcode, "off_by") == 0) temp_region.off_by = atoi(value);   // <-- ADICIONADO[cite: 10]
                    else if (strcmp(opcode, "polyphony") == 0) temp_region.polyphony = atoi(value); // <-- ADICIONADO[cite: 10]
                    else if (strcmp(opcode, "sample") == 0) {
                        strncpy(temp_sample_name, value, sizeof(temp_sample_name) - 1);
                        if (temp_sample_name[0] == '"') {
                            memmove(temp_sample_name, temp_sample_name + 1, strlen(temp_sample_name));
                            int len = strlen(temp_sample_name);
                            if (len > 0 && temp_sample_name[len - 1] == '"') temp_sample_name[len - 1] = '\0';
                        }
                        // Verifica se é instrução nativa de silêncio
                        if (strcmp(temp_sample_name, "*silence") == 0) {
                            temp_region.is_silence = true; // <-- ADICIONADO[cite: 10]
                        }
                    }
                }
            }
            token = strtok(NULL, " \t\r\n");
        }
    }

    // Processa a última região pendente do arquivo
    SAVE_CURRENT_REGION_IF_VALID();
    #undef SAVE_CURRENT_REGION_IF_VALID

    fclose(f);
    ESP_LOGI(TAG, "Kit de Bateria carregado com sucesso! Total REAL na PSRAM: %.2f MB", 
             (float)total_allocated_psram / (1024.0f * 1024.0f));
}

//=============================================================================
// ALGORITMO DE SELEÇÃO DINÂMICA EM TEMPO REAL (Midi Note -> Som PSRAM)
//=============================================================================
void sfz_trigger_note(uint8_t note, uint8_t velocity) {
    if (note >= MAX_MIDI_NOTES || velocity == 0) return;
    ESP_LOGI(TAG, "==> MIDI IN: Nota = %d | Velocity = %d", note, velocity);

    drum_note_map_t *map = &drum_kit[note];
    if (map->region_count == 0) return;

    sfz_region_t *selected = NULL;

    // 1. Filtra as regiões que batem com a força (velocity) do toque recebido
    sfz_region_t *eligible_rrs[MAX_REGIONS_PER_NOTE];
    uint8_t eligible_count = 0;

    for (int i = 0; i < map->region_count; i++) {
        sfz_region_t *reg = &map->regions[i];
        if (velocity >= reg->lovel && velocity <= reg->hivel) {
            eligible_rrs[eligible_count++] = reg;
        }
    }

    if (eligible_count == 0) return;

    // 2. Se houver variação Round Robin (seq_length > 1), alterna sequencialmente
    if (eligible_count > 1) {
        uint8_t target_pos = (map->rr_counter % eligible_count) + 1;
        for (int i = 0; i < eligible_count; i++) {
            if (eligible_rrs[i]->seq_position == target_pos) {
                selected = eligible_rrs[i];
                break;
            }
        }
        if (!selected) selected = eligible_rrs[0];
        map->rr_counter++;
    } else {
        selected = eligible_rrs[0];
    }

    // 3. Envia o buffer correspondente diretamente ao mixer de 12 canais
    if (selected) {
        trigger_audio_buffer(selected->buffer, selected->sample_qty, velocity, 
                             note, selected->group, selected->off_by, 
                             selected->polyphony, selected->is_silence);
    }
}