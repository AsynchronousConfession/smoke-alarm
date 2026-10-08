#include "board.h"

#include "driver/ledc.h"
#include "esp_log.h"

#include "board_config.h"

static const char *TAG = "board";

#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_TIMER_SEL   LEDC_TIMER_0
#define LEDC_DUTY_RES    LEDC_TIMER_10_BIT
#define LEDC_FREQ_HZ     5000
#define LEDC_DUTY_MAX    1023

#define CH_BACKLIGHT     LEDC_CHANNEL_0
#define CH_LED_R         LEDC_CHANNEL_1
#define CH_LED_G         LEDC_CHANNEL_2
#define CH_LED_B         LEDC_CHANNEL_3

static void ledc_add_channel(ledc_channel_t ch, int gpio, uint32_t duty)
{
    const ledc_channel_config_t cfg = {
        .gpio_num   = gpio,
        .speed_mode = LEDC_MODE,
        .channel    = ch,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_SEL,
        .duty       = duty,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cfg));
}

static void ledc_set(ledc_channel_t ch, uint32_t duty)
{
    ledc_set_duty(LEDC_MODE, ch, duty);
    ledc_update_duty(LEDC_MODE, ch);
}

void board_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num       = LEDC_TIMER_SEL,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    /* 背光是高电平点亮：满占空比 = 最亮 */
    ledc_add_channel(CH_BACKLIGHT, LCD_PIN_BL, LEDC_DUTY_MAX);
    /* RGB 是低电平点亮：满占空比 = 灭 */
    ledc_add_channel(CH_LED_R, BOARD_PIN_LED_R, LEDC_DUTY_MAX);
    ledc_add_channel(CH_LED_G, BOARD_PIN_LED_G, LEDC_DUTY_MAX);
    ledc_add_channel(CH_LED_B, BOARD_PIN_LED_B, LEDC_DUTY_MAX);

    ESP_LOGI(TAG, "背光 PWM=IO%d, RGB=IO%d/%d/%d (LEDC %d Hz)",
             LCD_PIN_BL, BOARD_PIN_LED_R, BOARD_PIN_LED_G, BOARD_PIN_LED_B, LEDC_FREQ_HZ);
}

void board_backlight_set(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    ledc_set(CH_BACKLIGHT, (uint32_t)(LEDC_DUTY_MAX * percent / 100));
}

void board_rgb_set(int r, int g, int b)
{
    const ledc_channel_t chs[3] = {CH_LED_R, CH_LED_G, CH_LED_B};
    const int values[3] = {r, g, b};

    for (int i = 0; i < 3; i++) {
        int v = values[i];
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        /* 低电平点亮：亮度越高，占空比越低 */
        ledc_set(chs[i], (uint32_t)(LEDC_DUTY_MAX - LEDC_DUTY_MAX * v / 100));
    }
}
