#include "midi_processor.h"
#include "sfz_parser.h" // Modificado para apontar para o nosso novo motor
#include "esp_log.h"

//static const char *TAG = "MIDI_PROCESSOR";

void midi_processor_handle_message(uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t message_type = status & 0xF0;
    //uint8_t channel      = status & 0x0F;

    switch (message_type) {
        case 0x90: { // --- EVENTO: NOTE ON ---
            uint8_t note = data1;
            uint8_t velocity = data2;

            if (velocity > 0) {
                // Roda o algoritmo de gatilho do SFZ carregado na memória
                sfz_trigger_note(note, velocity);
            }
            break;
        }

        case 0x80: // --- EVENTO: NOTE OFF ---
            break;

        case 0xB0: // --- EVENTO: CONTROL CHANGE (Expressão do pedal de HiHat futuramente) ---
            break;

        default:
            break;
    }
}