#pragma once

#include <stdint.h>

/**
 * @brief Processa uma mensagem MIDI genérica de 3 bytes.
 * 
 * Esta função centraliza as mensagens vindas tanto do USB MIDI quanto do DIN5 (UART).
 * 
 * @param status  Byte de Status (ex: 0x90 para Note On no Canal 1)
 * @param data1   Primeiro dado (geralmente o número da Nota MIDI)
 * @param data2   Segundo dado (geralmente a Velocidade/Volume ou valor de CC)
 */
void midi_processor_handle_message(uint8_t status, uint8_t data1, uint8_t data2);