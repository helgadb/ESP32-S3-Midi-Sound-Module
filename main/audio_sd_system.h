#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Inicialização de hardware
void init_audio_pcm5102a(void);
void init_sd_card(void);

// Funções de baixo nível de áudio
void audio_play_raw(const int16_t *buffer, size_t bytes_to_write);

// Novas funções para o Sampler (Carregamento e Disparo rápido)
void load_snare_to_ram(void);
void trigger_snare(uint8_t velocity);

// Controle da Task de reprodução
void disparar_teste_de_audio(void);
