/*
 * [CYD port] 屏幕 + 触摸 + LVGL 初始化
 *
 * 这些参数是在 outputs/esp32_cyd_lvgl 工程里实机调通的（镜像/反相/字节序/触摸镜像），
 * 直接搬过来复用；引脚定义在 board_config.h。
 */

#include "display.h"

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "board.h"
#include "board_config.h"

static const char *TAG = "DISP";

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t    s_lcd    = NULL;
static esp_lcd_panel_io_handle_t s_tp_io  = NULL;
static esp_lcd_touch_handle_t    s_tp     = NULL;

static void lcd_init(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_PIN_SCLK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = LCD_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num       = LCD_PIN_CS,
        .dc_gpio_num       = LCD_PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &s_lcd_io));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .color_space    = ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_lcd_io, &panel_config, &s_lcd));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_lcd, LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_lcd, LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_lcd, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_lcd, true));

    ESP_LOGI(TAG, "LCD ILI9341 就绪 %dx%d (SPI2, CS=IO%d DC=IO%d)", LCD_H_RES, LCD_V_RES, LCD_PIN_CS, LCD_PIN_DC);
}

static void touch_init(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num     = TOUCH_PIN_SCLK,
        .mosi_io_num     = TOUCH_PIN_MOSI,
        .miso_io_num     = TOUCH_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 0,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t tp_io_config = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(TOUCH_PIN_CS);
    tp_io_config.spi_mode = 0;
    tp_io_config.pclk_hz  = 2 * 1000 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TOUCH_SPI_HOST, &tp_io_config, &s_tp_io));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max        = TOUCH_X_MAX,
        .y_max        = TOUCH_Y_MAX,
        .rst_gpio_num = -1,
        .int_gpio_num = TOUCH_PIN_IRQ,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = TOUCH_SWAP_XY,
            .mirror_x = TOUCH_MIRROR_X,
            .mirror_y = TOUCH_MIRROR_Y,
        },
    };
    /* 第一个参数是 panel io 句柄本身，不是它的地址 */
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(s_tp_io, &tp_cfg, &s_tp));
    ESP_LOGI(TAG, "XPT2046 触摸就绪 (CS=IO%d IRQ=IO%d)", TOUCH_PIN_CS, TOUCH_PIN_IRQ);
}

static void lvgl_init(void)
{
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_lcd_io,
        .panel_handle  = s_lcd,
        /* 经典 ESP32 无 PSRAM，且要同时跑 BLE + WiFi，缓冲先取小一点：
         * 双缓冲 240×20 行 ≈ 19 KB */
        .buffer_size   = LCD_H_RES * LVGL_BUF_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = LCD_SWAP_XY,
            .mirror_x = LCD_MIRROR_X,
            .mirror_y = LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp 失败");
        return;
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = disp,
        .handle = s_tp,
    };
    if (lvgl_port_add_touch(&touch_cfg) == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_touch 失败");
        return;
    }

    ESP_LOGI(TAG, "LVGL 就绪（双缓冲 %d 行）", LVGL_BUF_LINES);
}

void display_init(void)
{
    board_init();          /* 背光 + RGB 指示灯（LEDC PWM） */
    lcd_init();
    touch_init();
    lvgl_init();
}
