#include "circular_strip.h"
#include "application.h"
#include <esp_log.h>
#include <algorithm>

#define TAG "CircularStrip"

#define BLINK_INFINITE -1

CircularStrip::CircularStrip(gpio_num_t gpio, uint16_t max_leds) : max_leds_(max_leds) {
    // If the gpio is not connected, you should use NoLed class
    assert(gpio != GPIO_NUM_NC);

    colors_.resize(max_leds_);

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = max_leds_;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    esp_timer_create_args_t strip_timer_args = {
        .callback = [](void *arg) {
            auto strip = static_cast<CircularStrip*>(arg);
            std::lock_guard<std::mutex> lock(strip->mutex_);
            if (strip->strip_callback_ != nullptr) {
                strip->strip_callback_();
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "strip_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&strip_timer_args, &strip_timer_));
}

CircularStrip::~CircularStrip() {
    esp_timer_stop(strip_timer_);
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}


void CircularStrip::SetAllColor(StripColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = color;
        led_strip_set_pixel(led_strip_, i, color.red, color.green, color.blue);
    }
    led_strip_refresh(led_strip_);
}

void CircularStrip::SetSingleColor(uint8_t index, StripColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    colors_[index] = color;
    led_strip_set_pixel(led_strip_, index, color.red, color.green, color.blue);
    led_strip_refresh(led_strip_);
}

void CircularStrip::SetMultiColors(const std::vector<StripColor>& colors) {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    int count = std::min(max_leds_, static_cast<int>(colors.size()));
    for (int i = 0; i < count; i++) {
        colors_[i] = colors[i];
        led_strip_set_pixel(led_strip_, i, colors[i].red, colors[i].green, colors[i].blue);
    }
    led_strip_refresh(led_strip_);
}

void CircularStrip::Blink(StripColor color, int interval_ms) {
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = color;
    }
    StartStripTask(interval_ms, [this]() {
        static bool on = true;
        if (on) {
            for (int i = 0; i < max_leds_; i++) {
                led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
            }
            led_strip_refresh(led_strip_);
        } else {
            led_strip_clear(led_strip_);
        }
        on = !on;
    });
}

void CircularStrip::FadeOut(int interval_ms) {
    StartStripTask(interval_ms, [this]() {
        bool all_off = true;
        for (int i = 0; i < max_leds_; i++) {
            colors_[i].red /= 2;
            colors_[i].green /= 2;
            colors_[i].blue /= 2;
            if (colors_[i].red != 0 || colors_[i].green != 0 || colors_[i].blue != 0) {
                all_off = false;
            }
            led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
        }
        if (all_off) {
            led_strip_clear(led_strip_);
            esp_timer_stop(strip_timer_);
        } else {
            led_strip_refresh(led_strip_);
        }
    });
}

void CircularStrip::Breathe(StripColor low, StripColor high, int interval_ms) {
    StartStripTask(interval_ms, [this, low, high]() {
        static bool increase = true;
        static StripColor color = low;
        if (increase) {
            if (color.red < high.red) {
                color.red++;
            }
            if (color.green < high.green) {
                color.green++;
            }
            if (color.blue < high.blue) {
                color.blue++;
            }
            if (color.red == high.red && color.green == high.green && color.blue == high.blue) {
                increase = false;
            }
        } else {
            if (color.red > low.red) {
                color.red--;
            }
            if (color.green > low.green) {
                color.green--;
            }
            if (color.blue > low.blue) {
                color.blue--;
            }
            if (color.red == low.red && color.green == low.green && color.blue == low.blue) {
                increase = true;
            }
        }
        for (int i = 0; i < max_leds_; i++) {
            led_strip_set_pixel(led_strip_, i, color.red, color.green, color.blue);
        }
        led_strip_refresh(led_strip_);
    });
}

void CircularStrip::Scroll(StripColor low, StripColor high, int length, int interval_ms) {
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = low;
    }
    StartStripTask(interval_ms, [this, low, high, length]() {
        static int offset = 0;
        for (int i = 0; i < max_leds_; i++) {
            colors_[i] = low;
        }
        for (int j = 0; j < length; j++) {
            int i = (offset + j) % max_leds_;
            colors_[i] = high;
        }
        for (int i = 0; i < max_leds_; i++) {
            led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
        }
        led_strip_refresh(led_strip_);
        offset = (offset + 1) % max_leds_;
    });
}

void CircularStrip::StartStripTask(int interval_ms, std::function<void()> cb) {
    if (led_strip_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    
    strip_callback_ = cb;
    esp_timer_start_periodic(strip_timer_, interval_ms * 1000);
}

void CircularStrip::SetBrightness(uint8_t default_brightness, uint8_t low_brightness) {
    default_brightness_ = default_brightness;
    low_brightness_ = low_brightness;
    OnStateChanged();
}

void CircularStrip::OnStateChanged() {
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();
    switch (device_state) {
        case kDeviceStateStarting: {
            StripColor low = { 0, 0, 0 };
            StripColor high = { low_brightness_, low_brightness_, default_brightness_ };
            Scroll(low, high, 3, 100);
            break;
        }
        case kDeviceStateWifiConfiguring: {
            StripColor color = { low_brightness_, low_brightness_, default_brightness_ };
            Blink(color, 500);
            break;
        }
        case kDeviceStateIdle:
            FadeOut(50);
            break;
        case kDeviceStateConnecting: {
            StripColor color = { low_brightness_, low_brightness_, default_brightness_ };
            SetAllColor(color);
            break;
        }
        case kDeviceStateListening:
        case kDeviceStateAudioTesting: {
            StripColor color = { default_brightness_, low_brightness_, low_brightness_ };
            SetAllColor(color);
            break;
        }
        case kDeviceStateSpeaking: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            SetAllColor(color);
            break;
        }
        case kDeviceStateUpgrading: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            Blink(color, 100);
            break;
        }
        case kDeviceStateActivating: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            Blink(color, 500);
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown led strip event: %d", device_state);
            return;
    }
}

// ============================================================
// 自定义歌曲灯光特效
// ============================================================

void CircularStrip::StopEffect() {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    led_strip_clear(led_strip_);
    led_strip_refresh(led_strip_);
}

void CircularStrip::PlayCustomVideoEffect(int duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);

    // 清空当前颜色缓存
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = {0, 0, 0};
    }

    int64_t start_time = esp_timer_get_time();  // 微秒

    StartStripTask(40, [this, start_time, duration_ms]() {
        int64_t now = esp_timer_get_time();
        int elapsed = (now - start_time) / 1000;  // 毫秒

        if (elapsed >= duration_ms) {
            led_strip_clear(led_strip_);
            led_strip_refresh(led_strip_);
            esp_timer_stop(strip_timer_);
            return;
        }

        // 先全部清空
        for (int i = 0; i < max_leds_; i++) {
            colors_[i] = {0, 0, 0};
        }

        // ========== 按时间轴还原视频效果 ==========
        if (elapsed < 1500) {
            // 0~1.5s：中间青色亮段向两边扩展
            int center = max_leds_ / 2;
            int progress = elapsed;  // 0 ~ 1500
            int half_len = 2 + (progress * (max_leds_ / 2 - 2)) / 1500;
            for (int i = center - half_len; i <= center + half_len; i++) {
                if (i >= 0 && i < max_leds_) {
                    colors_[i] = {0, 180, 255};  // 青色
                }
            }
        }
        else if (elapsed < 2500) {
            // 1.5~2.5s：全红
            for (int i = 0; i < max_leds_; i++) {
                colors_[i] = {180, 0, 0};
            }
        }
        else if (elapsed < 3500) {
            // 2.5~3.5s：全灭
            // 已清空，不做任何事
        }
        else if (elapsed < 5500) {
            // 3.5~5.5s：间隔红色点
            for (int i = 0; i < max_leds_; i += 4) {
                colors_[i] = {200, 0, 0};
                if (i + 1 < max_leds_) colors_[i + 1] = {120, 0, 0};
            }
        }
        else if (elapsed < 7000) {
            // 5.5~7s：间隔粉色点
            for (int i = 1; i < max_leds_; i += 5) {
                colors_[i] = {180, 0, 120};
                if (i + 1 < max_leds_) colors_[i + 1] = {140, 0, 90};
            }
        }
        else if (elapsed < 9000) {
            // 7~9s：红 + 白/蓝混合点
            for (int i = 0; i < max_leds_; i += 6) {
                colors_[i] = {200, 0, 0};
                if (i + 2 < max_leds_) colors_[i + 2] = {80, 80, 200};
                if (i + 4 < max_leds_) colors_[i + 4] = {160, 0, 80};
            }
        }
        else if (elapsed < 10000) {
            // 9~10s：全灭
            // 已清空
        }
        else if (elapsed < 14500) {
            // 10~14.5s：紫色/白色光段从左向右移动
            int progress = elapsed - 10000;  // 0 ~ 4500
            int segment_len = 4500 + 40;
            int pos = (progress * segment_len) / 4500 - 20;

            // 主光段（偏白紫）
            int main_len = max_leds_ / 3;
            for (int j = 0; j < main_len; j++) {
                int idx = pos + j;
                if (idx >= 0 && idx < max_leds_) {
                    int center_j = main_len / 2;
                    uint8_t bright = 180 - std::abs(j - center_j) * 6;
                    if (bright > 180) bright = 180;
                    colors_[idx] = {static_cast<uint8_t>(bright / 2), static_cast<uint8_t>(bright / 3), bright};
                }
            }
            // 跟随的短光段
            int follow_len = max_leds_ / 5;
            for (int j = 0; j < follow_len; j++) {
                int idx = pos - main_len - 5 + j;
                if (idx >= 0 && idx < max_leds_) {
                    colors_[idx] = {60, 40, 140};
                }
            }
        }
        else if (elapsed < 16000) {
            // 14.5~16s：蓝色段向右移动并逐渐变短
            int progress = elapsed - 14500;  // 0 ~ 1500
            int move_range = max_leds_ - max_leds_ / 3;
            int pos = max_leds_ / 5 + (progress * move_range) / 1500;
            int len = max_leds_ / 3 - progress / 20;
            if (len < 3) len = 3;

            for (int j = 0; j < len; j++) {
                int idx = pos + j;
                if (idx >= 0 && idx < max_leds_) {
                    colors_[idx] = {20, 40, 220};
                }
            }
        }
        else if (elapsed < 18000) {
            // 16~18s：中间红色短段 + 青蓝点
            int center = max_leds_ / 2;
            int red_len = max_leds_ / 6;
            for (int i = center - red_len; i < center + red_len; i++) {
                if (i >= 0 && i < max_leds_) {
                    colors_[i] = {200, 0, 30};
                }
            }
            // 青色点缀
            int offset = red_len + max_leds_ / 8;
            if (center - offset >= 0) colors_[center - offset] = {0, 180, 220};
            if (center - offset + 2 >= 0) colors_[center - offset + 2] = {0, 140, 200};
            if (center + offset - 2 < max_leds_) colors_[center + offset - 2] = {0, 140, 200};
            if (center + offset < max_leds_) colors_[center + offset] = {0, 180, 220};
        }
        else {
            // 18~20s：少量青色点后熄灭
            int center = max_leds_ / 2;
            int fade = 20000 - elapsed;  // 0 ~ 2000
            uint8_t b = (fade > 150) ? 180 : static_cast<uint8_t>(fade * 180 / 150);
            int offset = max_leds_ / 12;
            if (center - offset >= 0)
                colors_[center - offset] = {0, static_cast<uint8_t>(b / 2), b};
            colors_[center] = {0, static_cast<uint8_t>(b / 2), b};
            if (center + offset < max_leds_)
                colors_[center + offset] = {0, static_cast<uint8_t>(b / 2), b};
        }

        // 统一刷新到灯带
        for (int i = 0; i < max_leds_; i++) {
            led_strip_set_pixel(led_strip_, i,
                                colors_[i].red,
                                colors_[i].green,
                                colors_[i].blue);
        }
        led_strip_refresh(led_strip_);
    });
}
