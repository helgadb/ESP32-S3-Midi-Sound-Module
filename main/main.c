#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "usb/usb_host.h"

#include "midi_class_driver_txrx.h"
#include "midi_uart.h"
#include "led_rgb.h"

static const char *TAG = "MIDI_MAIN";

//==========================
// CONFIGURATIONS
//==========================
#define DAEMON_TASK_PRIORITY    2
#define CLASS_TASK_PRIORITY     3

#define UART_TASK_PRIORITY      (CLASS_TASK_PRIORITY + 1)
#define USB2UART_TASK_PRIORITY  (CLASS_TASK_PRIORITY + 4)

#define UART_STACK_SIZE         4096
#define USB_STACK_SIZE          4096
#define DAEMON_STACK_SIZE       4096
#define CLASS_STACK_SIZE        8192

#define UART_NUM_CFG            UART_NUM_1

//==========================
// TASK: USB Host Daemon
//==========================
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

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            ESP_LOGW(TAG, "USB: no clients");
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
            ESP_LOGW(TAG, "USB: no devices connected");
    }
}

//==========================
// TASK: UART → USB
//==========================
static void uart_to_usb_task(void *arg)
{
    uint8_t data[1024];

    ESP_LOGI(TAG, "UART→USB task started");

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

//==========================
// Hook (USB → UART)
//==========================
void process_usb_rx_for_uart(const uint8_t *data, size_t length)
{
    midi_uart_send_to_uart(data, length);
}

//==========================
// TASK: LED Indicator
//==========================
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
    
    vTaskDelete(NULL);
}

//==========================
// APP MAIN
//==========================
void app_main(void)
{
    SemaphoreHandle_t ready_sem = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "     MIDI USB ↔ UART Pass-Through     ");
    ESP_LOGI(TAG, "=================================");

    init_led_rgb();
    
    set_led_rgb(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    // LED task - criada primeira para executar a sequência completa
    xTaskCreate(
        led_indicator_task,
        "led_indicator",
        2048,
        NULL,
        10,
        NULL
    );

    // Aguarda LED terminar a sequência
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Inicializar UART MIDI
    midi_uart_init();

    // Start USB→UART task
    midi_uart_start_usb_to_uart_task(
        USB2UART_TASK_PRIORITY,
        USB_STACK_SIZE,
        1
    );

    // USB Host Daemon task
    xTaskCreatePinnedToCore(
        usb_daemon_task,
        "usb_daemon",
        DAEMON_STACK_SIZE,
        ready_sem,
        DAEMON_TASK_PRIORITY,
        NULL,
        0
    );

    // USB MIDI Class Driver task
    xTaskCreatePinnedToCore(
        class_driver_task,
        "usb_midi_class",
        CLASS_STACK_SIZE,
        ready_sem,
        CLASS_TASK_PRIORITY,
        NULL,
        0
    );

    // UART → USB task
    xTaskCreatePinnedToCore(
        uart_to_usb_task,
        "uart_to_usb",
        UART_STACK_SIZE,
        NULL,
        UART_TASK_PRIORITY,
        NULL,
        1
    );

    ESP_LOGI(TAG, "System Ready. MIDI pass-through active.");
    ESP_LOGI(TAG, "LED is BLUE = System running");
}