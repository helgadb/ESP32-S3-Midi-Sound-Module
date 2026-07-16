#ifndef AUDIO_SD_SYSTEM_H
#define AUDIO_SD_SYSTEM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o DAC PCM5102A via I2S configurado para latência extrema.
 * 
 * Configura o canal I2S_NUM_0 como Master, com taxa de amostragem de 44.1kHz,
 * 16-bit estéreo e buffers DMA reduzidos ao limite estável para latência de ~1.45ms.
 */
void init_audio_pcm5102a(void);

/**
 * @brief Inicializa e monta o Cartão SD usando o barramento SDMMC em modo de 1-bit.
 * 
 * Monta o sistema de arquivos FAT no diretório "/sdcard".
 */
void init_sd_card(void);

/**
 * @brief Carrega o arquivo de áudio "38.wav" do cartão SD diretamente para a PSRAM.
 * 
 * Esta função varre o arquivo ignorando metadados (como chunks LIST, JUNK, etc.)
 * e localiza o chunk de áudio bruto ("data"), alocando apenas os dados úteis.
 */
void load_snare_to_ram(void);

/**
 * @brief Ativa a reprodução instantânea do som da caixa (snare).
 * 
 * @param velocity Força do toque (padrão MIDI: 0 a 127). Controla o ganho do áudio.
 */
void trigger_snare(uint8_t velocity);

/**
 * @brief Escreve dados de áudio brutos diretamente no canal DMA do I2S.
 * 
 * @param buffer Ponteiro para o array de amostras de áudio (16-bit).
 * @param bytes_to_write Quantidade de bytes a serem transmitidos.
 */
void audio_play_raw(const int16_t *buffer, size_t bytes_to_write);

/**
 * @brief Inicializa e dispara o motor (task) de áudio de tempo real no Core 1.
 * 
 * Substitui a antiga função 'disparar_teste_de_audio'. Mantém o barramento
 * I2S quente enviando silêncio quando não há áudio tocando para evitar cliques.
 */
void audio_system_start(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_SD_SYSTEM_H