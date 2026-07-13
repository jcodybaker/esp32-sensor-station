extern "C" {
#include "m5stick_display.h"
#include "sensors.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
}

#include <M5Unified.h>

static const char *TAG = "m5stick_display";

namespace {

constexpr TickType_t kPollIntervalMs = 300;
constexpr TickType_t kPageIntervalMs = 3000;
// Number of consecutive polls a new orientation must win by before we act on
// it, so a brief shake or a reading near a 45 degree boundary doesn't cause
// the screen to flicker between rotations.
constexpr int kRotationHysteresisPolls = 4;

// M5StickC Plus rotation 1 (M5GFX default) is landscape with the power
// button on the right edge and the USB-C port at the bottom-left. We pick
// whichever of the four rotations puts the "down" edge closest to gravity.
int rotation_from_accel(float ax, float ay) {
    if (fabsf(ax) > fabsf(ay)) {
        return ax > 0 ? 3 : 1;
    }
    return ay > 0 ? 0 : 2;
}

void format_sensor_value(const sensor_data_t *sensor, char *buf, size_t buf_len) {
    if (sensor->available) {
        snprintf(buf, buf_len, "%.2f %s", sensor->value, sensor->unit);
    } else {
        snprintf(buf, buf_len, "-- %s", sensor->unit);
    }
}

void draw_footer(const settings_t *settings) {
    auto &d = M5.Display;
    d.setTextDatum(textdatum_t::bottom_left);
    d.setTextPadding(d.width());
    d.setTextSize(1);
    const char *hostname = (settings->hostname && settings->hostname[0]) ? settings->hostname : "unknown";
    d.drawString(hostname, 2, d.height() - 1);
}

// Single sensor, large font, filling most of the screen.
void draw_big_sensor(const sensor_data_t *sensor) {
    auto &d = M5.Display;
    d.setTextDatum(textdatum_t::top_left);
    d.setTextPadding(d.width());

    d.setTextSize(2);
    d.drawString(sensor->display_name, 2, 4);

    char value_buf[48];
    format_sensor_value(sensor, value_buf, sizeof(value_buf));
    d.setTextSize(3);
    d.drawString(value_buf, 2, 26);
}

// Two sensors stacked, smaller font so both fit at once.
void draw_small_sensor(const sensor_data_t *sensor, int top) {
    auto &d = M5.Display;
    d.setTextDatum(textdatum_t::top_left);
    d.setTextPadding(d.width());

    d.setTextSize(1);
    d.drawString(sensor->display_name, 2, top);

    char value_buf[48];
    format_sensor_value(sensor, value_buf, sizeof(value_buf));
    d.setTextSize(2);
    d.drawString(value_buf, 2, top + 10);
}

void render(int page) {
    auto &d = M5.Display;
    int count = sensors_get_count();

    if (count == 0) {
        d.setTextDatum(textdatum_t::top_left);
        d.setTextPadding(d.width());
        d.setTextSize(2);
        d.drawString("No sensors", 2, 4);
        return;
    }

    if (count == 2) {
        int row_h = (d.height() - 10) / 2;
        draw_small_sensor(sensors_get_by_index(0), 0);
        draw_small_sensor(sensors_get_by_index(1), row_h);
        return;
    }

    draw_big_sensor(sensors_get_by_index(page % count));
}

void m5stick_display_task(void *arg) {
    settings_t *settings = static_cast<settings_t *>(arg);

    auto cfg = M5.config();
    cfg.clear_display = true;
    cfg.output_power = true;
    cfg.internal_imu = true;
    cfg.internal_rtc = false;
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    M5.begin(cfg);

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

    bool have_imu = (M5.Imu.getType() != m5::imu_none);
    int current_rotation = 1;
    int candidate_rotation = current_rotation;
    int candidate_streak = 0;
    M5.Display.setRotation(current_rotation);

    uint8_t last_brightness = settings->lcd_brightness;
    M5.Display.setBrightness(last_brightness);

    int page = 0;
    TickType_t last_page_switch = xTaskGetTickCount();

    ESP_LOGI(TAG, "M5StickC Plus display task started (imu=%d)", have_imu);

    while (true) {
        M5.update();

        if (have_imu && M5.Imu.update()) {
            auto data = M5.Imu.getImuData();
            int rotation = rotation_from_accel(data.accel.x, data.accel.y);
            if (rotation == candidate_rotation) {
                if (candidate_streak < kRotationHysteresisPolls) {
                    candidate_streak++;
                }
            } else {
                candidate_rotation = rotation;
                candidate_streak = 1;
            }
            if (candidate_rotation != current_rotation && candidate_streak >= kRotationHysteresisPolls) {
                current_rotation = candidate_rotation;
                M5.Display.setRotation(current_rotation);
                M5.Display.fillScreen(TFT_BLACK);
            }
        }

        if (settings->lcd_brightness != last_brightness) {
            last_brightness = settings->lcd_brightness;
            M5.Display.setBrightness(last_brightness);
        }

        int count = sensors_get_count();
        if (count > 2) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_page_switch >= pdMS_TO_TICKS(kPageIntervalMs)) {
                page = (page + 1) % count;
                last_page_switch = now;
                M5.Display.fillScreen(TFT_BLACK);
            }
        } else {
            page = 0;
        }

        render(page);
        draw_footer(settings);

        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

}  // namespace

extern "C" void m5stick_display_init(settings_t *settings) {
    BaseType_t ok = xTaskCreatePinnedToCore(
        m5stick_display_task, "m5stick_disp", 8192, settings, 4, nullptr, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
    }
}
