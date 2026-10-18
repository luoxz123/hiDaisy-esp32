#ifndef SETTINGS_H
#define SETTINGS_H

#include <esp_err.h>
#include <cstdint>

// NVS-backed settings with defaults
class Settings {
public:
    static Settings& GetInstance();

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    esp_err_t Init();

    // Volume: 0-100, default 70
    int GetVolume() const;
    void SetVolume(int volume);

    // Brightness: 0-100, default 75
    int GetBrightness() const;
    void SetBrightness(int brightness);

    // Timezone offset in minutes from UTC, default 480 (UTC+8)
    int GetTimezoneOffset() const;
    void SetTimezoneOffset(int offset_minutes);

private:
    Settings() = default;
    ~Settings() = default;

    int GetInt(const char* ns, const char* key, int default_val) const;
    void SetInt(const char* ns, const char* key, int value);

    static constexpr int kDefaultVolume = 70;
    static constexpr int kDefaultBrightness = 75;
    static constexpr int kDefaultTimezoneOffset = 480; // UTC+8
};

#endif // SETTINGS_H
