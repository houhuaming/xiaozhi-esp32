#ifndef LED_STRIP_CONTROL_H
#define LED_STRIP_CONTROL_H

#include "led/circular_strip.h"
#include <vector>
#include <atomic>
#include <string>

// 特效类型枚举
enum class LedEffectType {
    COLOR_GRADIENT,    // 颜色渐变
    BREATHE,           // 呼吸
    RANDOM_FLICKER,    // 随机闪烁
    PARTIAL_LIGHT,     // 局部亮起
    LIGHT_SPOT_MOVE,   // 光斑移动
    QUICK_FLASH        // 快速闪断
};

// 单个特效参数
struct LedEffectParams {
    LedEffectType type;

    // 通用参数
    int duration_ms = 1000;      // 持续时间
    int interval_ms = 50;        // 刷新间隔

    // 颜色参数 (起始颜色)
    uint8_t r1 = 255, g1 = 0, b1 = 0;
    // 颜色参数 (结束颜色，用于渐变)
    uint8_t r2 = 0, g2 = 0, b2 = 255;

    // 呼吸参数
    uint8_t breathe_min = 10;
    uint8_t breathe_max = 255;

    // 随机闪烁参数
    int flicker_count = 3;

    // 局部亮起参数
    int start_led = 0;
    int end_led = 3;

    // 光斑移动参数
    int spot_size = 2;
    int spot_speed = 100;
    bool spot_reverse = false;

    // 快速闪断参数
    int flash_on_ms = 30;
    int flash_off_ms = 50;
    int flash_count = 5;
};

class LedStripControl {
private:
    CircularStrip* led_strip_;
    int brightness_level_;
    int max_leds_;
    std::atomic<bool> is_playing_{false};
    TaskHandle_t effect_task_ = nullptr;

    // 当前播放的序列
    LedEffectParams current_effect_;

    int LevelToBrightness(int level) const;
    StripColor RGBToColor(int red, int green, int blue);
    StripColor LerpColor(StripColor from, StripColor to, float t);
    uint8_t LerpUint8(uint8_t from, uint8_t to, float t);

    // 特效执行函数
    void RunColorGradient();
    void RunBreathe();
    void RunRandomFlicker();
    void RunPartialLight();
    void RunLightSpotMove();
    void RunQuickFlash();

    void StopCurrentEffect();

    // 解析特效类型字符串
    LedEffectType ParseEffectType(const std::string& type_str);

public:
    // 供任务调用
    void RunEffect();
    explicit LedStripControl(CircularStrip* led_strip);
    ~LedStripControl();
};

#endif // LED_STRIP_CONTROL_H
