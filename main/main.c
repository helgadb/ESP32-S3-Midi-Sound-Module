#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "usb/usb_host.h"

// Drivers do Sistema
#include "midi_class_driver_txrx.h"
#include "midi_uart.h"
#include "led_rgb.h"
#include "audio_sd_system.h"
#include "esp_heap_caps.h"
#include "sfz_parser.h"

static const char *TAG = "MIDI_MAIN";

//=============================================================================
// CONFIGURAÇÕES DE TASKS (Prioridades e Tamanhos de Pilha)
//=============================================================================
#define DAEMON_TASK_PRIORITY    21
#define CLASS_TASK_PRIORITY     3

#define UART_TASK_PRIORITY      (CLASS_TASK_PRIORITY + 1)
#define USB2UART_TASK_PRIORITY  (CLASS_TASK_PRIORITY + 4)

#define UART_STACK_SIZE         4096
#define USB_STACK_SIZE          4096
#define DAEMON_STACK_SIZE       4096
#define CLASS_STACK_SIZE        8192

#define UART_NUM_CFG            UART_NUM_1

//=============================================================================
// HOOKS E CALLBACKS
//=============================================================================

// Callback disparado quando dados MIDI chegam do USB e precisam ir para o conector físico UART
void process_usb_rx_for_uart(const uint8_t *data, size_t length)
{
    midi_uart_send_to_uart(data, length);
}

//=============================================================================
// TASKS SECUNDÁRIAS (Rodando em Background)
//=============================================================================

// Task: USB Host Daemon (Gerencia conexão/desconexão física de dispositivos USB no Core 0)
static void usb_daemon_task(void *arg)
{
    SemaphoreHandle_t sem = (SemaphoreHandle_t)arg;

    ESP_LOGI(TAG, "Installing USB Host Library...");
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    xSemaphoreGive(sem);
    vTaskDelay(20);

    while (1) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "USB: no clients");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGW(TAG, "USB: no devices connected");
        }
    }
}

// Task: UART -> USB (Lê o MIDI que vem das conexões físicas e repassa para a USB)
static void uart_to_usb_task(void *arg)
{
    uint8_t data[1024];
    ESP_LOGI(TAG, "UART->USB task started");

    while (1)
    {
        int len = uart_read_bytes(
            UART_NUM_CFG,
            data,
            sizeof(data),
            pdMS_TO_TICKS(10)
        );

        if (len > 0) {
            midi_uart_parse_and_send_to_usb(data, len);
        }

        vTaskDelay(1);
    }
}

// Task: Sequência Visual de Inicialização do LED RGB
static void led_indicator_task(void *arg)
{
    ESP_LOGI(TAG, "LED task started");
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "LED: RED");
    set_led_red(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "LED: GREEN");
    set_led_green(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "LED: BLUE");
    set_led_blue(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    set_led_blue(true);
    ESP_LOGI(TAG, "LED boot sequence complete - steady blue");
    
    vTaskDelete(NULL); // Deleta a si mesma quando conclui a animação
}

//=============================================================================
// APLICAÇÃO PRINCIPAL (Ponto de Entrada)
//=============================================================================
void app_main(void)
{
    SemaphoreHandle_t ready_sem = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "     MIDI USB <-> UART Pass-Through     ");
    ESP_LOGI(TAG, "=================================");

    //---------------------------------------------------------
    // 1. INICIALIZAÇÃO VISUAL (Feedback imediato para o usuário)
    //---------------------------------------------------------
    init_led_rgb();
    set_led_rgb(0, 0, 0); // Desliga o LED antes de iniciar a sequência
    vTaskDelay(pdMS_TO_TICKS(50));

    // Executa a sequência de inicialização em uma task separada para não travar o boot
    xTaskCreate(
        led_indicator_task,
        "led_indicator",
        2048,
        NULL,
        10,
        NULL
    );

    // Aguarda o LED terminar sua sequência de boot antes de estressar o processador
    vTaskDelay(pdMS_TO_TICKS(2000));

    //---------------------------------------------------------
    // 2. INICIALIZAÇÃO DO HARDWARE DE ÁUDIO E ARMAZENAMENTO
    //---------------------------------------------------------
    init_audio_pcm5102a(); // Inicializa o DAC I2S
    init_sd_card();        // Inicializa o barramento SPI do SD Card

    // Aguarda o cartão SD se estabilizar eletricamente
    vTaskDelay(pdMS_TO_TICKS(2000));

    //---------------------------------------------------------
    // 3. CARREGAMENTO DOS SAMPLES DO SD PARA A PSRAM
    //---------------------------------------------------------
    // Carrega o kit completo do SD para a PSRAM
    sfz_parser_load_kit("/sdcard/TR808Kit.sfz");

    //---------------------------------------------------------
    // 4. ATIVAÇÃO DO MOTOR DE ÁUDIO POLIFÔNICO (Core 1)
    //---------------------------------------------------------
    // Dispara a esteira de áudio e o mixer dedicados no Core 1
    audio_system_start();

 
    //---------------------------------------------------------
    // 5. INICIALIZAÇÃO DO ECOSSISTEMA MIDI
    //---------------------------------------------------------
    midi_uart_init(); // Prepara o hardware da UART física para MIDI

    // Task: Processa a entrada USB MIDI e envia para a UART (Trava no Core 1 para priorizar áudio/MIDI)
    midi_uart_start_usb_to_uart_task(
        USB2UART_TASK_PRIORITY,
        USB_STACK_SIZE,
        1
    );

    // Task: Daemon do Host USB (Roda no Core 0)
    xTaskCreatePinnedToCore(
        usb_daemon_task,
        "usb_daemon",
        DAEMON_STACK_SIZE,
        ready_sem,
        DAEMON_TASK_PRIORITY,
        NULL,
        0
    );

    // Task: Driver de Classe USB MIDI (Roda no Core 0)
    xTaskCreatePinnedToCore(
        class_driver_task,
        "usb_midi_class",
        CLASS_STACK_SIZE,
        ready_sem,
        CLASS_TASK_PRIORITY,
        NULL,
        0
    );

    // Task: Envia o MIDI vindo da UART para a USB (Roda no Core 1)
    xTaskCreatePinnedToCore(
        uart_to_usb_task,
        "uart_to_usb",
        UART_STACK_SIZE,
        NULL,
        UART_TASK_PRIORITY,
        NULL,
        1
    );

    //---------------------------------------------------------
    // 6. DIAGNÓSTICO DO SISTEMA E VERIFICAÇÃO DE MEMÓRIA
    //---------------------------------------------------------
    ESP_LOGI(TAG, "System Ready. MIDI pass-through active.");
    ESP_LOGI(TAG, "LED is BLUE = System running");

    size_t psram_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI("PSRAM_TEST", "PSRAM total livre: %d bytes (%.2f MB)", 
             psram_size, (float)psram_size / (1024.0f * 1024.0f));
             
    if (psram_size == 0) {
        ESP_LOGE("PSRAM_TEST", "ERRO: PSRAM nao foi detectada! Verifique as configuracoes no menuconfig.");
    } else {
        ESP_LOGI("PSRAM_TEST", "PSRAM detectada com sucesso!");
    }
}