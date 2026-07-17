#ifndef SFZ_PARSER_H
#define SFZ_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_MIDI_NOTES 128
#define MAX_REGIONS_PER_NOTE 12 // Limite de variações de velocidade/RoundRobin por nota

typedef struct {
    int16_t *buffer;          // Alocado na PSRAM
    size_t sample_qty;        // Tamanho do áudio
    uint8_t lovel;            // Velocidade MIDI Mínima
    uint8_t hivel;            // Velocidade MIDI Máxima
    uint8_t seq_length;       // Qtd de Round Robins
    uint8_t seq_position;     // Posição neste Round Robin
    int group;                // <-- ID do grupo (Padrão: -1)
    int off_by;               // <-- Corta o grupo ID (Padrão: -1)[cite: 10]
    int polyphony;            // <-- Limite de polifonia do grupo (Padrão: 0 = ilimitado)[cite: 10]
    bool is_silence;          // <-- Identifica se é sample=*silence[cite: 10]
} sfz_region_t;

typedef struct {
    sfz_region_t regions[MAX_REGIONS_PER_NOTE];
    uint8_t region_count;
    uint8_t rr_counter;       // Alternador sequencial de Round Robin
} drum_note_map_t;

/**
 * @brief Varre o arquivo SFZ, resolve dependências e carrega as amostras para a PSRAM.
 */
void sfz_parser_load_kit(const char *sfz_path);

/**
 * @brief Seleciona a amostra correta baseado na nota e força (velocity), e envia ao mixer.
 */
void sfz_trigger_note(uint8_t note, uint8_t velocity);

#endif // SFZ_PARSER_H