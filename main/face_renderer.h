#ifndef FACE_RENDERER_H
#define FACE_RENDERER_H

#include "device_state.h"
#include <lvgl.h>

// Parametric face — rounded-rectangle robot style
struct FaceParams {
    float eye_h;     // 0.0 = closed line, 1.0 = max open
    float mouth_w;   // 0.0 = narrow, 1.0 = full width
    float mouth_h;   // 0.0 = thin line, 1.0 = tall opening
    float mouth_y;   // -1.0 = up (smile hint), 1.0 = down (sad hint)
    float smile_arc; // 0.0 = rectangle, 1.0 = crescent moon smile

    static FaceParams Lerp(const FaceParams& a, const FaceParams& b, float t) {
        return {
            a.eye_h     + (b.eye_h     - a.eye_h)     * t,
            a.mouth_w   + (b.mouth_w   - a.mouth_w)   * t,
            a.mouth_h   + (b.mouth_h   - a.mouth_h)   * t,
            a.mouth_y   + (b.mouth_y   - a.mouth_y)   * t,
            a.smile_arc + (b.smile_arc - a.smile_arc)  * t,
        };
    }
};

class FaceRenderer {
public:
    ~FaceRenderer();
    void Init(lv_obj_t* parent);
    void SetTargetEmotion(EmotionType emotion, float intensity = 1.0f);
    void Tick();
    void SetVisible(bool visible);

    static constexpr int kCanvasW = 220;
    static constexpr int kCanvasH = 180;

private:
    void Render();
    void DrawRoundRect(int x, int y, int w, int h, int radius, lv_color_t color);
    void DrawCrescentSmile(int cx, int cy, int diameter, int thickness, lv_color_t color);
    void DrawEyelashes(int eye_x, int eye_w, int eye_top, bool is_left, lv_color_t color);
    void DrawBlush(int eye_x, int eye_w, int eye_bottom, bool is_left);
    void UpdateBlink();
    void UpdateIdle();

    static FaceParams GetEmotionPreset(EmotionType emotion);
    static lv_color_t GetEmotionColor(EmotionType emotion);
    static lv_color_t GetEmotionBgColor(EmotionType emotion);

    lv_obj_t* canvas_ = nullptr;
    lv_color_t* canvas_buf_ = nullptr;
    lv_timer_t* anim_timer_ = nullptr;

    FaceParams target_ = {};
    FaceParams display_ = {};

    lv_color_t face_color_;
    lv_color_t target_face_color_;
    lv_color_t bg_color_;
    lv_color_t target_bg_color_;

    // Blink
    uint32_t next_blink_tick_ = 0;
    bool blinking_ = false;
    int blink_frame_ = 0;
    static constexpr int kBlinkFrames = 6;

    // Idle micro-animation
    float idle_eye_y_ = 0.0f;    // breathing offset
    float idle_eye_x_ = 0.0f;    // horizontal drift
    float idle_target_x_ = 0.0f;
    uint32_t next_drift_tick_ = 0;

    uint32_t tick_ = 0;

    // Layout — larger, centered, feminine proportions
    static constexpr int kEyeW     = 54;
    static constexpr int kEyeHMax  = 42;
    static constexpr int kEyeHMin  = 3;
    static constexpr int kEyeRad   = 12;
    static constexpr int kLeftEyeX = 28;   // left edge of left eye
    static constexpr int kRightEyeX = 138;  // left edge of right eye
    static constexpr int kEyeCy    = 68;   // center Y of eyes

    static constexpr int kMouthWMax = 65;
    static constexpr int kMouthWMin = 14;
    static constexpr int kMouthHMax = 30;
    static constexpr int kMouthHMin = 5;
    static constexpr int kMouthRad  = 8;
    static constexpr int kMouthCx   = 110;  // center X of mouth
    static constexpr int kMouthCy   = 140;  // base center Y of mouth
    static constexpr int kMouthYRange = 10;  // max offset pixels

    // Blush — circular
    static constexpr int kBlushW   = 16;
    static constexpr int kBlushH   = 16;
    static constexpr int kBlushRad = 8;
};

#endif // FACE_RENDERER_H
