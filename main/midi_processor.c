#include "midi_processor.h"
#include "audio_sd_system.h"
#include "esp_log.h"

static const char *TAG = "MIDI_PROCESSOR";

void midi_processor_handle_message(uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t message_type = status & 0xF0; // Isola o tipo de comando (Note On, Note Off, CC, etc.)
    uint8_t channel      = status & 0x0F; // Isola o canal MIDI (0 a 15, representando canais 1 a 16)

    switch (message_type) {
        
        case 0x90: { // --- EVENTO: NOTE ON (Nota Pressionada / Batida) ---
            uint8_t note = data1;
            uint8_t velocity = data2;

            if (velocity > 0) {
                // Central de Mapeamento das Notas da Bateria (General MIDI standard)
                switch (note) {
                    case 38: // Caixa (Snare)
                        trigger_snare(velocity);
                        break;

                    /* 
                    Aqui fica ridiculamente fácil expandir seu projeto futuramente:
                    
                    case 36: // Bumbo (Bass Drum)
                        trigger_kick(velocity); 
                        break;
                        
                    case 42: // Chimbal Fechado (Closed Hi-Hat)
                        trigger_hihat_closed(velocity);
                        break;
                    */

                    default:
                        ESP_LOGD(TAG, "[Canal %d] Nota %d (Vel: %d) nao mapeada para som.", channel + 1, note, velocity);
                        break;
                }
            } else {
                // Pelo padrão MIDI, um Note On com velocidade 0 é equivalente a um Note Off
                // Útil se você precisar interromper sons (como abafar um prato - Cymbal Choke)
            }
            break;
        }

        case 0x80: { // --- EVENTO: NOTE OFF (Nota Solta) ---
            uint8_t note = data1;
            // Tratar interrupção de notas de sustentação se necessário
            break;
        }

        case 0xB0: { // --- EVENTO: CONTROL CHANGE (CC) ---
            uint8_t controller = data1;
            uint8_t value = data2;

            // Exemplo: Pedal de expressão do Chimbal (geralmente CC 4)
            if (controller == 4) {
                // open_hihat_amount = value; // Guarda o quanto o pedal está aberto/fechado
            }
            break;
        }

        default:
            // Outros tipos de mensagem (Pitch Bend, Program Change, etc.) podem ser ignorados por enquanto
            break;
    }
}