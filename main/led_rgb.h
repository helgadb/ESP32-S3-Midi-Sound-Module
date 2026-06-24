#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void init_led_rgb(void);
void set_led_rgb(uint8_t red, uint8_t green, uint8_t blue);
void set_led_blue(bool on);
void set_led_red(bool on);
void set_led_green(bool on);

#ifdef __cplusplus
}
#endif