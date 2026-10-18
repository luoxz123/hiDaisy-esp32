#include "face_renderer.h"
#include <esp_log.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <cmath>

static const char *TAG = "face";

static void face_anim_cb(lv_timer_t *timer)
{
    static_cast<FaceRenderer*>(timer->user_data)->Tick();
}

// ---------------------------------------------------------------------------
// Destructor — release PSRAM buffer and LVGL timer
// ---------------------------------------------------------------------------
FaceRenderer::~FaceRenderer()
{
    if (anim_timer_) { lv_timer_del(anim_timer_); anim_timer_ = nullptr; }
    if (canvas_)     { lv_obj_del(canvas_);       canvas_ = nullptr; }
    if (canvas_buf_) { heap_caps_free(canvas_buf_); canvas_buf_ = nullptr; }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void FaceRenderer::Init(lv_obj_t* parent)
{
    size_t buf_bytes = LV_CANVAS_BUF_SIZE_TRUE_COLOR(kCanvasW, kCanvasH);
    canvas_buf_ = (lv_color_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!canvas_buf_) {
        ESP_LOGE(TAG, "Failed to alloc face canvas (%zu B)", buf_bytes);
        return;
    }

    canvas_ = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas_, canvas_buf_, kCanvasW, kCanvasH, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas_, LV_ALIGN_CENTER, 0, -5);

    target_ = GetEmotionPreset(kEmotionNeutral);
    display_ = target_;
    face_color_ = lv_color_make(255, 235, 240);  // soft pink-white
    target_face_color_ = face_color_;
    bg_color_ = lv_color_black();
    target_bg_color_ = bg_color_;

    Render();
    anim_timer_ = lv_timer_create(face_anim_cb, 33, this);  // ~30 fps

    ESP_LOGI(TAG, "Face renderer initialized (%dx%d canvas)", kCanvasW, kCanvasH);
}

void FaceRenderer::SetVisible(bool visible)
{
    if (!canvas_) return;
    if (visible) {
        lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// Emotion presets (at full intensity)
// ---------------------------------------------------------------------------
FaceParams FaceRenderer::GetEmotionPreset(EmotionType emotion)
{
    //                      eye_h  mouth_w  mouth_h  mouth_y  smile_arc
    switch (emotion) {
        case kEmotionHappy:    return {0.20f, 1.00f, 0.35f, -0.60f, 0.0f};
        case kEmotionSad:      return {0.80f, 0.30f, 0.12f,  0.70f, 0.0f};
        case kEmotionComfort:  return {0.28f, 0.80f, 0.28f, -0.40f, 0.0f};
        case kEmotionSurprise: return {1.00f, 0.40f, 0.90f,  0.00f, 0.0f};
        case kEmotionThinking: return {0.30f, 0.25f, 0.12f,  0.35f, 0.0f};
        case kEmotionListening: return {1.00f, 1.00f, 0.55f, -0.30f, 1.0f}; // wide eyes + crescent smile
        case kEmotionReminding: return {1.00f, 0.70f, 0.50f,  0.00f, 0.0f}; // wide-eyed alert
        default:               return {0.65f, 0.50f, 0.12f,  0.00f, 0.0f}; // neutral
    }
}

lv_color_t FaceRenderer::GetEmotionColor(EmotionType emotion)
{
    switch (emotion) {
        case kEmotionHappy:    return lv_color_make(255, 230, 200);  // warm peach
        case kEmotionSad:      return lv_color_make(190, 210, 255);  // soft blue
        case kEmotionComfort:  return lv_color_make(255, 210, 220);  // rose pink
        case kEmotionSurprise: return lv_color_make(255, 235, 200);  // light apricot
        case kEmotionThinking: return lv_color_make(220, 210, 255);  // soft lavender
        case kEmotionListening: return lv_color_make(150, 255, 180); // vivid green
        case kEmotionReminding: return lv_color_make(255, 180, 80);  // warm amber
        default:               return lv_color_make(255, 235, 240);  // pink-white
    }
}

lv_color_t FaceRenderer::GetEmotionBgColor(EmotionType emotion)
{
    // Always black background — emotion is conveyed through face element colors
    (void)emotion;
    return lv_color_black();
}

// ---------------------------------------------------------------------------
// SetTargetEmotion
// ---------------------------------------------------------------------------
void FaceRenderer::SetTargetEmotion(EmotionType emotion, float intensity)
{
    FaceParams neutral = GetEmotionPreset(kEmotionNeutral);
    FaceParams full    = GetEmotionPreset(emotion);
    target_ = FaceParams::Lerp(neutral, full, intensity);

    target_face_color_ = GetEmotionColor(emotion);
    target_bg_color_ = GetEmotionBgColor(emotion);
    if (intensity < 1.0f) {
        lv_color_t base = lv_color_make(255, 235, 240);
        target_face_color_ = lv_color_mix(target_face_color_, base,
                                           (uint8_t)(intensity * 255));
    }

    ESP_LOGI(TAG, "Target: eye_h=%.2f mouth_w=%.2f mouth_h=%.2f mouth_y=%.2f",
             target_.eye_h, target_.mouth_w, target_.mouth_h, target_.mouth_y);
}

// ---------------------------------------------------------------------------
// Tick — called by LVGL timer (~30 fps)
// ---------------------------------------------------------------------------
void FaceRenderer::Tick()
{
    if (!canvas_) return;
    tick_++;

    // Periodic debug log (~every 3s at 30fps)
    if (tick_ % 90 == 0) {
        ESP_LOGI(TAG, "tick=%lu disp:eh=%.2f mw=%.2f tgt:eh=%.2f mw=%.2f",
                 (unsigned long)tick_, display_.eye_h, display_.mouth_w,
                 target_.eye_h, target_.mouth_w);
    }

    UpdateBlink();
    UpdateIdle();

    // Smooth interpolation toward target (fast enough to be visible)
    constexpr float kS = 0.18f;
    display_ = FaceParams::Lerp(display_, target_, kS);

    // Smooth color
    face_color_ = lv_color_mix(target_face_color_, face_color_, (uint8_t)(kS * 255));
    bg_color_ = lv_color_mix(target_bg_color_, bg_color_, (uint8_t)(kS * 255));

    Render();
}

// ---------------------------------------------------------------------------
// Blink — very obvious, eyes close to a thin line
// ---------------------------------------------------------------------------
void FaceRenderer::UpdateBlink()
{
    if (blinking_) {
        blink_frame_++;
        if (blink_frame_ >= kBlinkFrames) {
            blinking_ = false;
            blink_frame_ = 0;
        }
    } else if (tick_ >= next_blink_tick_) {
        blinking_ = true;
        blink_frame_ = 0;
        next_blink_tick_ = tick_ + 80 + (esp_random() % 100);  // 2.6–6 s
    }
}

// ---------------------------------------------------------------------------
// Idle micro-animations: breathing + horizontal drift
// ---------------------------------------------------------------------------
void FaceRenderer::UpdateIdle()
{
    // Breathing: gentle sine wave on eye vertical position (period ~3.5s)
    idle_eye_y_ = sinf((float)tick_ * 0.06f) * 2.0f;

    // Horizontal drift
    constexpr float kDriftSmooth = 0.04f;
    idle_eye_x_ += (idle_target_x_ - idle_eye_x_) * kDriftSmooth;

    if (tick_ >= next_drift_tick_) {
        idle_target_x_ = ((float)(esp_random() % 100) / 100.0f - 0.5f) * 5.0f;
        next_drift_tick_ = tick_ + 100 + (esp_random() % 150);
    }
}

// ---------------------------------------------------------------------------
// Render — clears canvas and draws feminine robot face
// ---------------------------------------------------------------------------
void FaceRenderer::Render()
{
    lv_canvas_fill_bg(canvas_, bg_color_, LV_OPA_COVER);

    // --- Eyes ---
    float eye_h_f = display_.eye_h;

    // Apply blink: smooth triangle wave squishing eye height toward 0
    if (blinking_) {
        float half = kBlinkFrames / 2.0f;
        float blink_t;
        if ((float)blink_frame_ <= half) {
            blink_t = (float)blink_frame_ / half;        // 0→1 closing
        } else {
            blink_t = (float)(kBlinkFrames - blink_frame_) / half; // 1→0 opening
        }
        eye_h_f *= (1.0f - blink_t * 0.95f);  // keep 5% min during blink
    }

    int eye_h = kEyeHMin + (int)(eye_h_f * (kEyeHMax - kEyeHMin));
    if (eye_h < kEyeHMin) eye_h = kEyeHMin;
    if (eye_h > kEyeHMax) eye_h = kEyeHMax;

    int eye_top = kEyeCy - eye_h / 2 + (int)idle_eye_y_;

    int left_x  = kLeftEyeX  + (int)idle_eye_x_;
    int right_x = kRightEyeX + (int)idle_eye_x_;

    // Radius adapts: full round when eye_h is small, otherwise kEyeRad
    int eye_rad = (eye_h < kEyeRad * 2) ? eye_h / 2 : kEyeRad;

    // Draw eyes
    DrawRoundRect(left_x,  eye_top, kEyeW, eye_h, eye_rad, face_color_);
    DrawRoundRect(right_x, eye_top, kEyeW, eye_h, eye_rad, face_color_);

    // Eyelashes (only when eyes are open enough)
    if (eye_h > 10) {
        DrawEyelashes(left_x,  kEyeW, eye_top, true,  face_color_);
        DrawEyelashes(right_x, kEyeW, eye_top, false, face_color_);
    }

    // Cheek blush (below and outside eyes)
    int eye_bottom = eye_top + eye_h;
    DrawBlush(left_x,  kEyeW, eye_bottom, true);
    DrawBlush(right_x, kEyeW, eye_bottom, false);

    // --- Mouth ---
    int mouth_w = kMouthWMin + (int)(display_.mouth_w * (kMouthWMax - kMouthWMin));
    int mouth_h = kMouthHMin + (int)(display_.mouth_h * (kMouthHMax - kMouthHMin));
    if (mouth_w < kMouthWMin) mouth_w = kMouthWMin;
    if (mouth_h < kMouthHMin) mouth_h = kMouthHMin;

    int mouth_y_off = (int)(display_.mouth_y * kMouthYRange);

    if (display_.smile_arc > 0.3f) {
        // Arc smile: curved stroke line (⌣ shape)
        int arc_radius = 30 + (int)(display_.mouth_w * 15);  // 30-45px radius
        int arc_width = 4 + (int)(display_.smile_arc * 2);   // 4-6px stroke
        int arc_cy = kMouthCy + mouth_y_off - arc_radius + 8; // center above mouth
        DrawCrescentSmile(kMouthCx, arc_cy, arc_radius, arc_width, face_color_);
    } else {
        int mouth_x = kMouthCx - mouth_w / 2;
        int mouth_y = kMouthCy - mouth_h / 2 + mouth_y_off;
        int mouth_rad = (mouth_h < kMouthRad * 2) ? mouth_h / 2 : kMouthRad;
        DrawRoundRect(mouth_x, mouth_y, mouth_w, mouth_h, mouth_rad, face_color_);
    }

    lv_obj_invalidate(canvas_);
}

// ---------------------------------------------------------------------------
// Arc smile — a curved stroke line (segment of a circle's circumference)
// ---------------------------------------------------------------------------
void FaceRenderer::DrawCrescentSmile(int cx, int cy, int radius, int thickness,
                                      lv_color_t color)
{
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = thickness;
    dsc.opa = LV_OPA_COVER;
    dsc.rounded = 1;
    // Arc angles: 20° to 160° draws the bottom curve (smile shape ⌣)
    lv_canvas_draw_arc(canvas_, cx, cy, radius, 20, 160, &dsc);
}

// ---------------------------------------------------------------------------
// Eyelashes — 3 rects centered across the top of each eye
// ---------------------------------------------------------------------------
void FaceRenderer::DrawEyelashes(int eye_x, int eye_w, int eye_top,
                                  bool is_left, lv_color_t color)
{
    // 3 lashes evenly distributed at 25%, 50%, 75% of eye width
    int x1 = eye_x + eye_w / 4 - 1;
    int x2 = eye_x + eye_w / 2 - 1;
    int x3 = eye_x + eye_w * 3 / 4 - 1;

    DrawRoundRect(x1, eye_top - 8,  3, 9,  1, color);   // left lash
    DrawRoundRect(x2, eye_top - 10, 3, 11, 1, color);   // center lash (tallest)
    DrawRoundRect(x3, eye_top - 8,  3, 9,  1, color);   // right lash
}

// ---------------------------------------------------------------------------
// Blush — pink circles on the cheeks (outer sides, below eye level)
// ---------------------------------------------------------------------------
void FaceRenderer::DrawBlush(int eye_x, int eye_w, int eye_bottom, bool is_left)
{
    // Pink blush — colors are now correctly byte-swapped in flush_cb
    lv_color_t blush_color = lv_color_make(220, 100, 140);
    int by = kEyeCy + 20;  // below eye level, in the cheek area

    int bx;
    if (is_left) {
        bx = eye_x - kBlushW - 2;  // outer-left of left eye
    } else {
        bx = eye_x + eye_w + 2;    // outer-right of right eye
    }

    DrawRoundRect(bx, by, kBlushW, kBlushH, kBlushRad, blush_color);
}

// ---------------------------------------------------------------------------
// Helper: draw a filled rounded rectangle on the canvas
// ---------------------------------------------------------------------------
void FaceRenderer::DrawRoundRect(int x, int y, int w, int h, int radius, lv_color_t color)
{
    // Clamp to canvas bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > kCanvasW) w = kCanvasW - x;
    if (y + h > kCanvasH) h = kCanvasH - y;
    if (w <= 0 || h <= 0) return;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa   = LV_OPA_COVER;
    dsc.radius   = radius;
    dsc.border_width = 0;

    lv_canvas_draw_rect(canvas_, x, y, w, h, &dsc);
}
