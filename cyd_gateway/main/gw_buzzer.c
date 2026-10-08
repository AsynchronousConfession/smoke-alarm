#include "gw_buzzer.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"

static const char *TAG = "BUZ";

/* board.c 用了 LEDC 低速模式 TIMER_0 + 通道 0~3（背光/RGB），这里用 TIMER_1 + 通道 4，
 * 频率独立（背光 5kHz、报警声 2.7kHz），互不影响。 */
#define BUZ_MODE        LEDC_LOW_SPEED_MODE
#define BUZ_TIMER       LEDC_TIMER_1
#define BUZ_CHANNEL     LEDC_CHANNEL_4
#define BUZ_DUTY_RES    LEDC_TIMER_10_BIT
#define BUZ_DUTY_MAX    1023
#define BUZ_FREQ_HZ     2700

/* 节奏：嘀 150ms / 停 150ms × 3，然后长停 1.5s（家用报警器的常见节奏） */
#define BEEP_TICK_MS    50
#define BEEP_ON_MS      150
#define BEEP_GAP_MS     150
#define BEEP_COUNT      3
#define BEEP_UNIT_MS    (BEEP_ON_MS + BEEP_GAP_MS)
#define BEEP_CYCLE_MS   (BEEP_COUNT * BEEP_UNIT_MS + 1500)

static esp_timer_handle_t s_timer;
static bool s_on;          /* 当前是否处于"要响"的状态 */
static bool s_tone;        /* 喇叭此刻是否在发声 */
static int  s_ms;          /* 报警开始后累计的毫秒数 */
static int  s_selftest_ms; /* 开机自检的"嘀"一声剩余毫秒数 */

static void buzzer_write(bool on)
{
    if (on == s_tone) {
        return;
    }
    s_tone = on;
    ledc_set_duty(BUZ_MODE, BUZ_CHANNEL, on ? (BUZ_DUTY_MAX / 2) : 0);
    ledc_update_duty(BUZ_MODE, BUZ_CHANNEL);
}

static void buzzer_tick(void *arg)
{
    (void)arg;

    /* 开机自检：先"嘀"一声（150~200ms），证明喇叭是通的 */
    if (s_selftest_ms > 0) {
        s_selftest_ms -= BEEP_TICK_MS;
        buzzer_write(true);
        return;
    }

    if (!s_on) {
        buzzer_write(false);
        s_ms = 0;
        return;
    }

    const int t     = s_ms % BEEP_CYCLE_MS;
    const int unit  = t / BEEP_UNIT_MS;          /* 第几声 */
    const int phase = t % BEEP_UNIT_MS;          /* 这一声里的位置 */

    buzzer_write(unit < BEEP_COUNT && phase < BEEP_ON_MS);
    s_ms += BEEP_TICK_MS;
}

void gw_buzzer_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = BUZ_MODE,
        .duty_resolution = BUZ_DUTY_RES,
        .timer_num       = BUZ_TIMER,
        .freq_hz         = BUZ_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t ch = {
        .gpio_num   = BOARD_PIN_SPEAKER_DAC,
        .speed_mode = BUZ_MODE,
        .channel    = BUZ_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BUZ_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    const esp_timer_create_args_t args = {
        .callback = buzzer_tick,
        .name     = "gw_buzzer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, BEEP_TICK_MS * 1000));

    s_selftest_ms = 200;        /* 开机"嘀"一声，用来确认喇叭接好了 */
    ESP_LOGI(TAG, "报警声就绪: IO%d 方波 %d Hz（喇叭要插在扬声器座上，开机应听到一声嘀）",
             BOARD_PIN_SPEAKER_DAC, BUZ_FREQ_HZ);
}

void gw_buzzer_test(void)
{
    s_selftest_ms = 600;
    ESP_LOGI(TAG, "自检：嘀 600ms");
}

void gw_buzzer_set(bool on)
{
    if (on == s_on) {
        return;
    }
    s_on = on;
    s_ms = 0;
    if (!on) {
        buzzer_write(false);
    }
    ESP_LOGI(TAG, "报警声 %s", on ? "开始（嘀嘀嘀-停 循环）" : "停止");
}
