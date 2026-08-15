#include "led_strip_control.h"
#include "settings.h"
#include "mcp_server.h"
#include <esp_log.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#define TAG "LedStripControl"

// ============================================================
// 基础工具函数
// ============================================================

int LedStripControl::LevelToBrightness(int level) const {
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    return (1 << level) - 1;
}

StripColor LedStripControl::RGBToColor(int red, int green, int blue) {
    red = std::max(0, std::min(255, red));
    green = std::max(0, std::min(255, green));
    blue = std::max(0, std::min(255, blue));
    return {static_cast<uint8_t>(red), static_cast<uint8_t>(green), static_cast<uint8_t>(blue)};
}

uint8_t LedStripControl::LerpUint8(uint8_t from, uint8_t to, float t) {
    return static_cast<uint8_t>(from + (to - from) * t);
}

StripColor LedStripControl::LerpColor(StripColor from, StripColor to, float t) {
    return {
        LerpUint8(from.red, to.red, t),
        LerpUint8(from.green, to.green, t),
        LerpUint8(from.blue, to.blue, t)
    };
}

void LedStripControl::StopCurrentEffect() {
    is_playing_ = false;
    if (effect_task_) {
        vTaskDelay(pdMS_TO_TICKS(150));
        effect_task_ = nullptr;
    }
}

LedEffectType LedStripControl::ParseEffectType(const std::string& type_str) {
    if (type_str == "color_gradient") return LedEffectType::COLOR_GRADIENT;
    if (type_str == "breathe") return LedEffectType::BREATHE;
    if (type_str == "random_flicker") return LedEffectType::RANDOM_FLICKER;
    if (type_str == "partial_light") return LedEffectType::PARTIAL_LIGHT;
    if (type_str == "light_spot_move") return LedEffectType::LIGHT_SPOT_MOVE;
    if (type_str == "quick_flash") return LedEffectType::QUICK_FLASH;
    return LedEffectType::COLOR_GRADIENT;  // 默认
}

// ============================================================
// 特效任务入口
// ============================================================

void LedStripControl::RunEffect() {
    switch (current_effect_.type) {
        case LedEffectType::COLOR_GRADIENT:  RunColorGradient(); break;
        case LedEffectType::BREATHE:         RunBreathe(); break;
        case LedEffectType::RANDOM_FLICKER:  RunRandomFlicker(); break;
        case LedEffectType::PARTIAL_LIGHT:   RunPartialLight(); break;
        case LedEffectType::LIGHT_SPOT_MOVE: RunLightSpotMove(); break;
        case LedEffectType::QUICK_FLASH:     RunQuickFlash(); break;
    }
}

// ============================================================
// 6 种特效实现
// ============================================================

// 1. 颜色渐变：从颜色1平滑过渡到颜色2
void LedStripControl::RunColorGradient() {
    const auto& p = current_effect_;
    ESP_LOGI(TAG, "ColorGradient: (%d,%d,%d)->(%d,%d,%d) %dms",
             p.r1, p.g1, p.b1, p.r2, p.g2, p.b2, p.duration_ms);

    StripColor from = {p.r1, p.g1, p.b1};
    StripColor to = {p.r2, p.g2, p.b2};
    int steps = std::max(1, p.duration_ms / p.interval_ms);

    for (int i = 0; i <= steps && is_playing_; i++) {
        float t = static_cast<float>(i) / steps;
        led_strip_->SetAllColor(LerpColor(from, to, t));
        vTaskDelay(pdMS_TO_TICKS(p.interval_ms));
    }
}

// 2. 呼吸灯：亮度周期性变化
void LedStripControl::RunBreathe() {
    const auto& p = current_effect_;
    ESP_LOGI(TAG, "Breathe: (%d,%d,%d) min=%d max=%d %dms",
             p.r1, p.g1, p.b1, p.breathe_min, p.breathe_max, p.duration_ms);

    StripColor base = {p.r1, p.g1, p.b1};
    int steps = std::max(1, p.duration_ms / p.interval_ms);
    int half = steps / 2;

    while (is_playing_) {
        // 渐亮
        for (int i = 0; i <= half && is_playing_; i++) {
            float t = static_cast<float>(i) / half;
            uint8_t bri = p.breathe_min + (p.breathe_max - p.breathe_min) * t;
            StripColor c = {
                static_cast<uint8_t>(base.red * bri / 255),
                static_cast<uint8_t>(base.green * bri / 255),
                static_cast<uint8_t>(base.blue * bri / 255)
            };
            led_strip_->SetAllColor(c);
            vTaskDelay(pdMS_TO_TICKS(p.interval_ms));
        }
        // 渐暗
        for (int i = half; i >= 0 && is_playing_; i--) {
            float t = static_cast<float>(i) / half;
            uint8_t bri = p.breathe_min + (p.breathe_max - p.breathe_min) * t;
            StripColor c = {
                static_cast<uint8_t>(base.red * bri / 255),
                static_cast<uint8_t>(base.green * bri / 255),
                static_cast<uint8_t>(base.blue * bri / 255)
            };
            led_strip_->SetAllColor(c);
            vTaskDelay(pdMS_TO_TICKS(p.interval_ms));
        }
    }
}

// 3. 随机闪烁：随机LED以随机亮度闪烁
void LedStripControl::RunRandomFlicker() {
    const auto& p = current_effect_;
    ESP_LOGI(TAG, "RandomFlicker: (%d,%d,%d) count=%d %dms",
             p.r1, p.g1, p.b1, p.flicker_count, p.duration_ms);

    int elapsed = 0;
    while (elapsed < p.duration_ms && is_playing_) {
        led_strip_->SetAllColor({0, 0, 0});

        for (int i = 0; i < p.flicker_count; i++) {
            int idx = esp_random() % max_leds_;
            uint8_t bri = 50 + (esp_random() % 206);
            StripColor c = {
                static_cast<uint8_t>(p.r1 * bri / 255),
                static_cast<uint8_t>(p.g1 * bri / 255),
                static_cast<uint8_t>(p.b1 * bri / 255)
            };
            led_strip_->SetSingleColor(idx, c);
        }

        vTaskDelay(pdMS_TO_TICKS(p.interval_ms));
        elapsed += p.interval_ms;
    }
}

// 4. 局部亮起：指定范围的LED亮起
void LedStripControl::RunPartialLight() {
    const auto& p = current_effect_;
    ESP_LOGI(TAG, "PartialLight: (%d,%d,%d) led %d-%d %dms",
             p.r1, p.g1, p.b1, p.start_led, p.end_led, p.duration_ms);

    StripColor color = {p.r1, p.g1, p.b1};
    led_strip_->SetAllColor({0, 0, 0});

    int start = std::max(0, std::min(p.start_led, max_leds_ - 1));
    int end = std::max(start, std::min(p.end_led, max_leds_ - 1));
    for (int i = start; i <= end; i++) {
        led_strip_->SetSingleColor(i, color);
    }

    vTaskDelay(pdMS_TO_TICKS(p.duration_ms));
}

// 5. 光斑移动：一个亮斑在灯带上移动
void LedStripControl::RunLightSpotMove() {
    const auto& p = current_effect_;
    ESP_LOGI(TAG, "LightSpotMove: (%d,%d,%d) size=%d speed=%dms",
             p.r1, p.g1, p.b1, p.spot_size, p.spot_speed);

    StripColor bright = {p.r1, p.g1, p.b1};
    StripColor dim = {
        static_cast<uint8_t>(p.r1 / 10),
        static_cast<uint8_t>(p.g1 / 10),
        static_cast<uint8_t>(p.b1 / 10)
    };

    int pos = 0;
    int dir = p.spot_reverse ? -1 : 1;

    while (is_playing_) {
        // 暗色背景
        for (int i = 0; i < max_leds_; i++) {
            led_strip_->SetSingleColor(i, dim);
        }
        // 光斑
        for (int i = 0; i < p.spot_size; i++) {
            int idx = (pos + i) % max_leds_;
            if (idx < 0) idx += max_leds_;
            led_strip_->SetSingleColor(idx, bright);
        }

        vTaskDelay(pdMS_TO_TICKS(p.spot_speed));

        pos += dir;
        if (pos >= max_leds_) pos = 0;
        if (pos < 0) pos = max_leds_ - 1;
    }
}

// 6. 快速闪断：快速亮灭
void LedStripControl::RunQuickFlash() {
    const auto& p = current_effect_;
    ESP_LOGI(TAG, "QuickFlash: (%d,%d,%d) on=%dms off=%dms x%d",
             p.r1, p.g1, p.b1, p.flash_on_ms, p.flash_off_ms, p.flash_count);

    StripColor color = {p.r1, p.g1, p.b1};
    StripColor off = {0, 0, 0};

    for (int i = 0; i < p.flash_count && is_playing_; i++) {
        led_strip_->SetAllColor(color);
        vTaskDelay(pdMS_TO_TICKS(p.flash_on_ms));
        led_strip_->SetAllColor(off);
        vTaskDelay(pdMS_TO_TICKS(p.flash_off_ms));
    }
}

// ============================================================
// 特效任务函数
// ============================================================

static void EffectTaskFunc(void* arg) {
    auto* ctrl = static_cast<LedStripControl*>(arg);
    ctrl->RunEffect();
    // 任务结束，清理
    vTaskDelete(nullptr);
}

// ============================================================
// 构造函数
// ============================================================

LedStripControl::LedStripControl(CircularStrip* led_strip)
    : led_strip_(led_strip) {
    max_leds_ = 8;  // 默认8颗LED

    Settings settings("led_strip");
    brightness_level_ = settings.GetInt("brightness", 4);
    led_strip_->SetBrightness(LevelToBrightness(brightness_level_), 4);

    auto& mcp_server = McpServer::GetInstance();

    // ---- 基础工具 ----

    mcp_server.AddTool("self.led_strip.get_brightness",
        "Get the brightness of the led strip (0-8)",
        PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            return brightness_level_;
        });

    mcp_server.AddTool("self.led_strip.set_brightness",
        "Set the brightness of the led strip (0-8)",
        PropertyList({
            Property("level", kPropertyTypeInteger, 0, 8)
        }), [this](const PropertyList& properties) -> ReturnValue {
            int level = properties["level"].value<int>();
            brightness_level_ = level;
            led_strip_->SetBrightness(LevelToBrightness(brightness_level_), 4);
            Settings settings("led_strip", true);
            settings.SetInt("brightness", brightness_level_);
            return true;
        });

    mcp_server.AddTool("self.led_strip.set_single_color",
        "Set the color of a single led.",
        PropertyList({
            Property("index", kPropertyTypeInteger, 0, max_leds_ - 1),
            Property("red", kPropertyTypeInteger, 0, 255),
            Property("green", kPropertyTypeInteger, 0, 255),
            Property("blue", kPropertyTypeInteger, 0, 255)
        }), [this](const PropertyList& properties) -> ReturnValue {
            int index = properties["index"].value<int>();
            int red = properties["red"].value<int>();
            int green = properties["green"].value<int>();
            int blue = properties["blue"].value<int>();
            led_strip_->SetSingleColor(index, RGBToColor(green, red, blue));
            return true;
        });

    mcp_server.AddTool("self.led_strip.set_all_color",
        "Set the color of all leds.",
        PropertyList({
            Property("red", kPropertyTypeInteger, 0, 255),
            Property("green", kPropertyTypeInteger, 0, 255),
            Property("blue", kPropertyTypeInteger, 0, 255)
        }), [this](const PropertyList& properties) -> ReturnValue {
            int red = properties["red"].value<int>();
            int green = properties["green"].value<int>();
            int blue = properties["blue"].value<int>();
            led_strip_->SetAllColor(RGBToColor(green, red, blue));
            return true;
        });

    mcp_server.AddTool("self.led_strip.blink",
        "Blink the led strip.",
        PropertyList({
            Property("red", kPropertyTypeInteger, 0, 255),
            Property("green", kPropertyTypeInteger, 0, 255),
            Property("blue", kPropertyTypeInteger, 0, 255),
            Property("interval", kPropertyTypeInteger, 0, 1000)
        }), [this](const PropertyList& properties) -> ReturnValue {
            int red = properties["red"].value<int>();
            int green = properties["green"].value<int>();
            int blue = properties["blue"].value<int>();
            int interval = properties["interval"].value<int>();
            led_strip_->Blink(RGBToColor(green, red, blue), interval);
            return true;
        });

    mcp_server.AddTool("self.led_strip.scroll",
        "Scroll the led strip.",
        PropertyList({
            Property("red", kPropertyTypeInteger, 0, 255),
            Property("green", kPropertyTypeInteger, 0, 255),
            Property("blue", kPropertyTypeInteger, 0, 255),
            Property("length", kPropertyTypeInteger, 1, 7),
            Property("interval", kPropertyTypeInteger, 0, 1000)
        }), [this](const PropertyList& properties) -> ReturnValue {
            int red = properties["red"].value<int>();
            int green = properties["green"].value<int>();
            int blue = properties["blue"].value<int>();
            int interval = properties["interval"].value<int>();
            int length = properties["length"].value<int>();
            StripColor low = RGBToColor(4, 4, 4);
            StripColor high = RGBToColor(green, red, blue);
            led_strip_->Scroll(low, high, length, interval);
            return true;
        });

    // ---- 自定义特效工具 ----

    mcp_server.AddTool("self.led_strip.play_effect",
        "Play a custom LED effect. type: color_gradient, breathe, random_flicker, partial_light, light_spot_move, quick_flash",
        PropertyList({
            Property("type", kPropertyTypeString),
            Property("r1", kPropertyTypeInteger, 0, 255),
            Property("g1", kPropertyTypeInteger, 0, 255),
            Property("b1", kPropertyTypeInteger, 0, 255),
            Property("r2", kPropertyTypeInteger, 0, 255),
            Property("g2", kPropertyTypeInteger, 0, 255),
            Property("b2", kPropertyTypeInteger, 0, 255),
            Property("duration_ms", kPropertyTypeInteger, 100, 60000),
            Property("interval_ms", kPropertyTypeInteger, 10, 1000),
            Property("breathe_min", kPropertyTypeInteger, 0, 255),
            Property("breathe_max", kPropertyTypeInteger, 0, 255),
            Property("flicker_count", kPropertyTypeInteger, 1, 8),
            Property("start_led", kPropertyTypeInteger, 0, 7),
            Property("end_led", kPropertyTypeInteger, 0, 7),
            Property("spot_size", kPropertyTypeInteger, 1, 4),
            Property("spot_speed", kPropertyTypeInteger, 20, 1000),
            Property("spot_reverse", kPropertyTypeBoolean),
            Property("flash_on_ms", kPropertyTypeInteger, 10, 500),
            Property("flash_off_ms", kPropertyTypeInteger, 10, 1000),
            Property("flash_count", kPropertyTypeInteger, 1, 50)
        }), [this](const PropertyList& properties) -> ReturnValue {
            StopCurrentEffect();

            std::string type_str = properties["type"].value<std::string>();
            current_effect_.type = ParseEffectType(type_str);
            current_effect_.r1 = properties["r1"].value<int>();
            current_effect_.g1 = properties["g1"].value<int>();
            current_effect_.b1 = properties["b1"].value<int>();
            current_effect_.r2 = properties["r2"].value<int>();
            current_effect_.g2 = properties["g2"].value<int>();
            current_effect_.b2 = properties["b2"].value<int>();
            current_effect_.duration_ms = properties["duration_ms"].value<int>();
            current_effect_.interval_ms = properties["interval_ms"].value<int>();
            current_effect_.breathe_min = properties["breathe_min"].value<int>();
            current_effect_.breathe_max = properties["breathe_max"].value<int>();
            current_effect_.flicker_count = properties["flicker_count"].value<int>();
            current_effect_.start_led = properties["start_led"].value<int>();
            current_effect_.end_led = properties["end_led"].value<int>();
            current_effect_.spot_size = properties["spot_size"].value<int>();
            current_effect_.spot_speed = properties["spot_speed"].value<int>();
            current_effect_.spot_reverse = properties["spot_reverse"].value<bool>();
            current_effect_.flash_on_ms = properties["flash_on_ms"].value<int>();
            current_effect_.flash_off_ms = properties["flash_off_ms"].value<int>();
            current_effect_.flash_count = properties["flash_count"].value<int>();

            // 在新任务中播放
            xTaskCreate(EffectTaskFunc, "led_effect", 4096, this, 5, &effect_task_);
            return true;
        });

    mcp_server.AddTool("self.led_strip.stop_effect",
        "Stop the currently playing LED effect.",
        PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            StopCurrentEffect();
            led_strip_->SetAllColor({0, 0, 0});
            return true;
        });

    // ---- 自定义歌曲灯光特效 ----

    mcp_server.AddTool("self.led_strip.play_song_effect",
        "Play the custom song light show effect (20 seconds). Features: cyan expand, red flash, dots, purple sweep, blue move, cyan fade.",
        PropertyList({
            Property("duration_ms", kPropertyTypeInteger, 5000, 30000)
        }), [this](const PropertyList& properties) -> ReturnValue {
            StopCurrentEffect();
            int duration = properties["duration_ms"].value<int>();
            led_strip_->PlayCustomVideoEffect(duration);
            return true;
        });
}

LedStripControl::~LedStripControl() {
    StopCurrentEffect();
}
