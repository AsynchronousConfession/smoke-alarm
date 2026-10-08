#include "gw_cal.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "CAL";

#define CAL_NAMESPACE   "cal"

/* MAC（12 个十六进制字符）正好可以当 NVS 的 key（上限 15 字符）。
 * 统一转成小写，避开 NVS key 对大小写的限制。 */
static bool mac_key_ok(const char *mac12)
{
    return mac12 != NULL && strlen(mac12) == 12;
}

static void mac_to_key(const char *mac12, char *out /* >= 13 */)
{
    int i = 0;
    for (; i < 12 && mac12[i] != '\0'; i++) {
        const char c = mac12[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[i] = '\0';
}

/* 开机自检：写-读-删一个"和 MAC 同形状"的保留 key，确认 NVS 通路是好的。
 * 只动保留 key，不会碰真实标定数据。 */
static void cal_nvs_selftest(void)
{
    static const char *KEY = "0000deadbeef";
    nvs_handle_t h;
    if (nvs_open(CAL_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 读写自检: 打不开命名空间");
        return;
    }
    const uint32_t want = 123456;
    bool ok = (nvs_set_u32(h, KEY, want) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    uint32_t got = 0;
    ok = ok && (nvs_get_u32(h, KEY, &got) == ESP_OK) && (got == want);
    nvs_erase_key(h, KEY);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS 读写自检: %s", ok ? "通过" : "失败");
}

void gw_cal_init(void)
{
    nvs_handle_t h;
    if (nvs_open(CAL_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_close(h);
        ESP_LOGI(TAG, "标定数据就绪（NVS 命名空间 \"%s\"）", CAL_NAMESPACE);
    } else {
        ESP_LOGI(TAG, "还没有任何标定数据（第一次上电属于正常）");
    }
    /* 自检用读写方式打开，会顺便把命名空间建出来；所以这一步必须无条件执行 */
    cal_nvs_selftest();
}

bool gw_cal_get(const char *mac12, uint32_t *r0_ohm)
{
    if (!mac_key_ok(mac12) || r0_ohm == NULL) {
        return false;
    }
    char key[16];
    mac_to_key(mac12, key);
    nvs_handle_t h;
    if (nvs_open(CAL_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint32_t v = 0;
    const esp_err_t err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    if (err != ESP_OK || v == 0) {
        return false;
    }
    *r0_ohm = v;
    return true;
}

bool gw_cal_set(const char *mac12, uint32_t r0_ohm)
{
    if (!mac_key_ok(mac12) || r0_ohm == 0) {
        return false;
    }
    char key[16];
    mac_to_key(mac12, key);
    nvs_handle_t h;
    if (nvs_open(CAL_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败");
        return false;
    }
    esp_err_t err = nvs_set_u32(h, key, r0_ohm);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 R0 失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "已保存 %s 的 R0=%luΩ", mac12, (unsigned long)r0_ohm);
    return true;
}

bool gw_cal_clear(const char *mac12)
{
    if (!mac_key_ok(mac12)) {
        return false;
    }
    char key[16];
    mac_to_key(mac12, key);
    nvs_handle_t h;
    if (nvs_open(CAL_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}
