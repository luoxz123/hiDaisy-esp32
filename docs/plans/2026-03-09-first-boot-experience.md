# First Boot Experience Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix WiFi auto-provisioning flow on first boot (no hardcoded fallback) and add a water-ripple boot animation.

**Architecture:** Two independent changes: (1) Modify `Application::Start()` WiFi init to remove hardcoded SSID fallback — when no NVS credentials exist, call `WifiManager::Init()` which already falls through to `StartProvisioning()`. (2) Add `ShowBootAnimation()` / `HideBootAnimation()` to `DisplayManager` using LVGL arc objects + lv_anim for expanding ripple circles, shown between LVGL init and WiFi connected/provisioning.

**Tech Stack:** ESP-IDF v5.x, LVGL v8, FreeRTOS, C++

**Issue:** #177

---

### Task 1: Create feature branch

**Step 1: Create branch from main**

```bash
cd /Users/maxazure/projects/hilily-esp32
git checkout main
git pull origin main
git checkout -b feature/177-first-boot-experience
```

**Step 2: Verify branch**

```bash
git branch --show-current
```

Expected: `feature/177-first-boot-experience`

---

### Task 2: Fix WiFi provisioning entry — remove hardcoded fallback

The core bug: when `HasStoredCredentials()` returns false, `Application::Start()` calls `Init(WIFI_SSID, WIFI_PASS)` which saves the hardcoded test WiFi and connects to it, instead of entering provisioning mode.

**Files:**
- Modify: `main/application.cc:142-148` (WiFi init logic)

**Step 1: Simplify WiFi init to always use `Init()` (no-arg version)**

Replace lines 142-148 in `application.cc`:

```cpp
// OLD:
if (WifiManager::GetInstance().HasStoredCredentials()) {
    WifiManager::GetInstance().Init();
} else {
    WifiManager::GetInstance().Init(WIFI_SSID, WIFI_PASS);
}

// NEW:
WifiManager::GetInstance().Init();
```

The no-arg `Init()` already handles both cases:
- Has NVS credentials → `LoadAllNetworks()` → `InitSta()`
- No NVS credentials → `StartProvisioning()`

**Step 2: Remove unused hardcoded WiFi constants**

Delete lines 43-44 from `application.cc`:

```cpp
// DELETE these lines:
static const char *WIFI_SSID = "Lucia-home";
static const char *WIFI_PASS = "1234567890";
```

**Step 3: Update provisioning mode handling in Start()**

Replace lines 355-374. Currently the provisioning UI is shown after WiFi init, but when `Init()` enters provisioning mode, we need to also add the `kDeviceStateWifiConfiguring` state (or reuse existing flow). Since `WifiManager::Init()` sets `provisioning_ = true` internally, the existing check `IsProvisioning()` already works. But we should add the new boot animation transition:

```cpp
// After WiFi init, decide flow
if (WifiManager::GetInstance().IsProvisioning()) {
    // No stored credentials → show provisioning UI
    DisplayManager::GetInstance().HideBootAnimation();
    auto &wm = WifiManager::GetInstance();

    // Get AP SSID for display
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "HiLily-%02X%02X", mac[4], mac[5]);

    char msg[200];
    snprintf(msg, sizeof(msg),
             "WiFi Setup\n\n"
             "1. Connect WiFi:\n"
             "   %s\n"
             "2. Open browser:\n"
             "   http://192.168.4.1\n\n"
             "PIN: %s",
             ap_ssid, wm.GetConfigPin().c_str());
    DisplayManager::GetInstance().ShowOverlay(msg, 120000);
    ESP_LOGI(TAG, "Provisioning mode: AP='%s', PIN=%s", ap_ssid, wm.GetConfigPin().c_str());
} else {
    state_machine_.TransitionTo(kDeviceStateWifiConnecting);
    WifiManager::GetInstance().Connect();
}
```

**Step 4: Build and verify compilation**

```bash
# Build from VS Code ESP-IDF extension, or:
cd /Users/maxazure/projects/hilily-esp32
idf.py build
```

Expected: Build succeeds (boot animation methods will be stubbed in next task).

**Step 5: Commit**

```bash
git add main/application.cc
git commit -m "fix: remove hardcoded WiFi fallback, auto-enter provisioning when no credentials (#177)"
```

---

### Task 3: Add scan endpoint to provisioning mode HTTP server

Currently `StartProvisioning()` does NOT register the `/scan` endpoint (only `StartConfigMode()` does). Fix this so first-time setup users can scan networks.

**Files:**
- Modify: `main/wifi_manager.cc:341-360` (StartProvisioning HTTP setup)

**Step 1: Add scan endpoint registration in StartProvisioning()**

After line 359 (the delete handler registration), add:

```cpp
httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET,
    .handler = HttpScanHandler, .user_ctx = this };
httpd_register_uri_handler(http_server_, &scan_uri);
```

**Step 2: Build and verify**

```bash
idf.py build
```

**Step 3: Commit**

```bash
git add main/wifi_manager.cc
git commit -m "fix: register /scan endpoint in provisioning mode for first-time WiFi setup (#177)"
```

---

### Task 4: Add boot animation to DisplayManager (header)

**Files:**
- Modify: `main/display_manager.h`

**Step 1: Add boot animation declarations**

Add after the `HideOverlay()` declaration (line 37):

```cpp
// Boot animation (water-ripple effect with "initializing" text)
void ShowBootAnimation();
void HideBootAnimation();
```

Add private members after `overlay_timer_` (line 72):

```cpp
// Boot animation
lv_obj_t* boot_anim_container_ = nullptr;
lv_obj_t* boot_center_dot_ = nullptr;
lv_obj_t* boot_text_label_ = nullptr;
lv_obj_t* boot_ripples_[3] = {};       // 3 concentric expanding circles
lv_anim_t boot_ripple_anims_[3] = {};
esp_timer_handle_t boot_anim_timer_ = nullptr;
bool boot_anim_active_ = false;
static void BootAnimTimerCallback(void* arg);
void UpdateBootAnimation();
```

**Step 2: Commit**

```bash
git add main/display_manager.h
git commit -m "feat: add boot animation method declarations to DisplayManager (#177)"
```

---

### Task 5: Implement boot animation (water-ripple effect)

The animation: a pulsing center dot + 3 concentric circles that expand outward from center, fading as they grow. Text "正在初始化..." below.

**Files:**
- Modify: `main/display_manager.cc`

**Step 1: Implement ShowBootAnimation()**

Add after `HideOverlay()` implementation (after line 353):

```cpp
void DisplayManager::ShowBootAnimation()
{
    xSemaphoreTake(ui_mutex_, portMAX_DELAY);

    if (boot_anim_active_) {
        xSemaphoreGive(ui_mutex_);
        return;
    }
    boot_anim_active_ = true;

    // Hide face during boot animation
    face_renderer_.SetVisible(false);

    // Hide status label and other UI during boot
    if (status_label_) lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (wifi_icon_) lv_obj_add_flag(wifi_icon_, LV_OBJ_FLAG_HIDDEN);
    if (time_label_) lv_obj_add_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
    if (battery_bar_) lv_obj_add_flag(battery_bar_, LV_OBJ_FLAG_HIDDEN);

    // Container for boot animation (full screen)
    boot_anim_container_ = lv_obj_create(screen_);
    lv_obj_set_size(boot_anim_container_, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(boot_anim_container_, 0, 0);
    lv_obj_set_style_bg_color(boot_anim_container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(boot_anim_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_anim_container_, 0, 0);
    lv_obj_clear_flag(boot_anim_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(boot_anim_container_);

    // Center pulsing dot (8px radius circle)
    boot_center_dot_ = lv_obj_create(boot_anim_container_);
    lv_obj_set_size(boot_center_dot_, 16, 16);
    lv_obj_align(boot_center_dot_, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_radius(boot_center_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(boot_center_dot_, lv_color_make(100, 200, 255), 0);
    lv_obj_set_style_bg_opa(boot_center_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_center_dot_, 0, 0);
    lv_obj_clear_flag(boot_center_dot_, LV_OBJ_FLAG_SCROLLABLE);

    // 3 ripple rings (arc objects used as circles)
    for (int i = 0; i < 3; i++) {
        boot_ripples_[i] = lv_obj_create(boot_anim_container_);
        int initial_size = 20;
        lv_obj_set_size(boot_ripples_[i], initial_size, initial_size);
        lv_obj_align(boot_ripples_[i], LV_ALIGN_CENTER, 0, -20);
        lv_obj_set_style_radius(boot_ripples_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(boot_ripples_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(boot_ripples_[i], lv_color_make(100, 200, 255), 0);
        lv_obj_set_style_border_width(boot_ripples_[i], 2, 0);
        lv_obj_set_style_border_opa(boot_ripples_[i], LV_OPA_70, 0);
        lv_obj_clear_flag(boot_ripples_[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    // Text label
    boot_text_label_ = lv_label_create(boot_anim_container_);
    lv_obj_set_style_text_color(boot_text_label_, lv_color_make(150, 150, 150), 0);
    lv_obj_align(boot_text_label_, LV_ALIGN_CENTER, 0, 50);
    lv_label_set_text(boot_text_label_, "Initializing...");

    xSemaphoreGive(ui_mutex_);

    // Start animation timer (updates every 30ms for smooth animation)
    if (!boot_anim_timer_) {
        esp_timer_create_args_t args = {};
        args.callback = BootAnimTimerCallback;
        args.arg = this;
        args.name = "boot_anim";
        esp_timer_create(&args, &boot_anim_timer_);
    }
    esp_timer_start_periodic(boot_anim_timer_, 30000);  // 30ms = ~33fps

    ESP_LOGI(TAG, "Boot animation started");
}

void DisplayManager::BootAnimTimerCallback(void* arg)
{
    auto* self = static_cast<DisplayManager*>(arg);
    self->UpdateBootAnimation();
}

void DisplayManager::UpdateBootAnimation()
{
    if (!boot_anim_active_) return;
    if (xSemaphoreTake(ui_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) return;

    static uint32_t frame = 0;
    frame++;

    // Center dot pulse: gentle size oscillation
    if (boot_center_dot_) {
        float pulse = 1.0f + 0.3f * sinf((float)frame * 0.1f);
        int dot_size = (int)(16.0f * pulse);
        lv_obj_set_size(boot_center_dot_, dot_size, dot_size);
        lv_obj_align(boot_center_dot_, LV_ALIGN_CENTER, 0, -20);
    }

    // Ripple animation: each ring expands from center outward, then resets
    // Staggered by 1/3 of the cycle period
    int cycle_frames = 60;  // ~1.8s per full cycle at 33fps
    for (int i = 0; i < 3; i++) {
        if (!boot_ripples_[i]) continue;

        int offset = i * (cycle_frames / 3);
        int phase = ((int)frame + offset) % cycle_frames;
        float t = (float)phase / (float)cycle_frames;  // 0.0 → 1.0

        // Size: 20px → 180px
        int size = 20 + (int)(t * 160.0f);
        lv_obj_set_size(boot_ripples_[i], size, size);
        lv_obj_align(boot_ripples_[i], LV_ALIGN_CENTER, 0, -20);

        // Opacity: starts at 70%, fades to 0%
        uint8_t opa = (uint8_t)((1.0f - t) * 178);  // 178 = LV_OPA_70
        lv_obj_set_style_border_opa(boot_ripples_[i], opa, 0);

        // Border width: thins as it expands
        int bw = 2 - (int)(t * 1.5f);
        if (bw < 1) bw = 1;
        lv_obj_set_style_border_width(boot_ripples_[i], bw, 0);
    }

    xSemaphoreGive(ui_mutex_);
}

void DisplayManager::HideBootAnimation()
{
    if (!boot_anim_active_) return;

    // Stop timer
    if (boot_anim_timer_) {
        esp_timer_stop(boot_anim_timer_);
    }

    xSemaphoreTake(ui_mutex_, portMAX_DELAY);

    boot_anim_active_ = false;

    // Clean up LVGL objects
    if (boot_anim_container_) {
        lv_obj_del(boot_anim_container_);
        boot_anim_container_ = nullptr;
        boot_center_dot_ = nullptr;
        boot_text_label_ = nullptr;
        for (int i = 0; i < 3; i++) boot_ripples_[i] = nullptr;
    }

    // Restore normal UI elements
    if (status_label_) lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (wifi_icon_) lv_obj_clear_flag(wifi_icon_, LV_OBJ_FLAG_HIDDEN);
    if (time_label_) lv_obj_clear_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
    face_renderer_.SetVisible(true);

    xSemaphoreGive(ui_mutex_);

    ESP_LOGI(TAG, "Boot animation hidden");
}
```

**Step 2: Build and verify**

```bash
idf.py build
```

**Step 3: Commit**

```bash
git add main/display_manager.cc
git commit -m "feat: implement water-ripple boot animation with expanding circles (#177)"
```

---

### Task 6: Integrate boot animation into Application startup flow

**Files:**
- Modify: `main/application.cc` (Start method)

**Step 1: Show boot animation right after display init**

After line 104 (`DisplayManager::GetInstance().Start();`), add:

```cpp
// Show boot animation while initializing subsystems
DisplayManager::GetInstance().ShowBootAnimation();
```

**Step 2: Hide boot animation before WiFi connected/provisioning transition**

The boot animation should be hidden at two points:
1. When entering provisioning mode (already handled in Task 2 — `HideBootAnimation()` before `ShowOverlay`)
2. When WiFi connects successfully — in `HandleWifiConnected()`

In `HandleWifiConnected()`, add at the very beginning (after the log line):

```cpp
void Application::HandleWifiConnected() {
    ESP_LOGI(TAG, "Wi-Fi connected, authenticating...");
    DisplayManager::GetInstance().HideBootAnimation();  // ← ADD THIS
    // ... rest of method unchanged
```

Also, if WiFi is already configured and we transition to `kDeviceStateWifiConnecting`, the animation continues during WiFi connection — this is the intended behavior. The animation disappears when WiFi connects.

**Step 3: Handle edge case — provisioning → WiFi connected after restart**

After provisioning saves credentials and restarts, the next boot will:
1. Show boot animation
2. `Init()` → `LoadAllNetworks()` → `InitSta()` (has credentials now)
3. `Connect()` → WiFi connects → `HandleWifiConnected()` → `HideBootAnimation()`

This flow is already correct.

**Step 4: Build and verify**

```bash
idf.py build
```

**Step 5: Commit**

```bash
git add main/application.cc
git commit -m "feat: integrate boot animation into startup flow, hide on WiFi connect (#177)"
```

---

### Task 7: Flash, test, and verify

**Step 1: Flash firmware to device**

```bash
./flash.sh
```

Remember: Press BOOT + RST to enter download mode manually.

**Step 2: Monitor serial output**

```bash
./monitor.sh
```

**Step 3: Verify acceptance criteria**

Test cases:
1. **Fresh boot (no NVS credentials):**
   - Erase NVS first: `idf.py erase-flash` then re-flash
   - Boot animation should appear immediately
   - After ~2s, should auto-enter provisioning mode
   - Screen shows AP name (`HiLily-XXYY`) and PIN
   - Serial log: "No stored WiFi credentials, starting provisioning"

2. **Provisioning flow:**
   - Connect phone to `HiLily-XXYY` AP
   - Open `http://192.168.4.1` in browser
   - Enter PIN → see config page
   - Click "Scan WiFi Networks" → should list nearby networks
   - Select network, enter password, save
   - Device restarts and connects

3. **Normal boot (with stored credentials):**
   - Boot animation appears during init
   - WiFi connects → animation disappears → face appears
   - No "Lucia-home" in serial logs

4. **Regression:**
   - Long-press still opens config mode
   - Button press starts listening
   - Wake word detection works

**Step 4: Commit any fixes needed**

---

### Task 8: Create Pull Request

**Step 1: Push branch**

```bash
git push -u origin feature/177-first-boot-experience
```

**Step 2: Create PR**

```bash
gh pr create \
  --title "feat: first boot experience — auto WiFi provisioning + boot animation (#177)" \
  --body "$(cat <<'EOF'
## Summary
- Remove hardcoded WiFi fallback (`Lucia-home`), auto-enter AP provisioning when no NVS credentials
- Register `/scan` endpoint in provisioning mode (was missing, only in config mode)
- Add water-ripple boot animation (LVGL programmatic circles + center pulse)
- Animation shown from LVGL init until WiFi connects or provisioning starts

## Changes
- `application.cc`: Simplify WiFi init to single `Init()` call, integrate boot animation
- `wifi_manager.cc`: Add `/scan` to `StartProvisioning()` HTTP server
- `display_manager.h/cc`: New `ShowBootAnimation()` / `HideBootAnimation()` with ripple effect

Closes #177

## Test plan
- [ ] Erase NVS, flash, verify auto-provisioning (no hardcoded SSID)
- [ ] Connect to HiLily AP, verify scan + save works
- [ ] Normal boot with saved WiFi: animation → face transition
- [ ] Long-press config mode still works
- [ ] Button/wake word regression passes
EOF
)"
```
