#include "led_rgb.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "LED_RGB";

#define LED_RGB_GPIO    48  // 21 para ESP32-S3-ZERO ou 48 para YD-ESP32-S3
#define LED_NUM_LEDS    1

static led_strip_handle_t led_strip;

void init_led_rgb(void)
{
    ESP_LOGI(TAG, "Inicializando LED RGB WS2812 no pino GPIO %d", LED_RGB_GPIO);

    // Configuração do LED Strip para versão 2.5.0
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_RGB_GPIO,
        .max_leds = LED_NUM_LEDS,
        // A versão 2.5.0 NÃO tem o campo color_component_format
    };

    // Configuração do backend RMT
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };

    // Criar o handle do LED strip
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    // Inicializar com o LED apagado
    led_strip_clear(led_strip);

    // Acender o LED na cor AZUL para indicar inicialização
    set_led_blue(true);
    ESP_LOGI(TAG, "LED RGB inicializado. Cor: AZUL (sistema ligado)");
}

void set_led_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED strip não inicializado");
        return;
    }
    // Define a cor para o primeiro LED (índice 0)
    // O WS2812 espera ordem GRB (Green, Red, Blue)
    led_strip_set_pixel(led_strip, 0, green, red, blue);
    // Atualiza o LED para mostrar a nova cor
    led_strip_refresh(led_strip);
}

void set_led_blue(bool on)
{
    if (on) {
        set_led_rgb(0, 0, 255);
    } else {
        set_led_rgb(0, 0, 0);
    }
}

void set_led_red(bool on)
{
    if (on) {
        set_led_rgb(255, 0, 0);
    } else {
        set_led_rgb(0, 0, 0);
    }
}

void set_led_green(bool on)
{
    if (on) {
        set_led_rgb(0, 255, 0);
    } else {
        set_led_rgb(0, 0, 0);
    }
}