#ifndef BATTERY_H
#define BATTERY_H

#include <esp_err.h>
#include <cstdint>

struct BatterySnapshot {
    uint32_t voltage_mv = 0;
    uint8_t percentage = 0;
    bool valid = false;
    bool low_confirmed = false;
    uint32_t sequence = 0;
};

class Battery {
public:
    static Battery& GetInstance();

    Battery(const Battery&) = delete;
    Battery& operator=(const Battery&) = delete;

    esp_err_t Init(int adc_pin);
    void StartMonitoring();

    uint32_t GetVoltage() const { return voltage_mv_; }
    uint8_t GetPercentage() const;
    BatterySnapshot GetSnapshot() const;

private:
    Battery() = default;
    ~Battery() = default;

    static void MonitorTask(void* arg);
    uint32_t ReadRaw();

    int adc_pin_ = -1;
    uint32_t voltage_mv_ = 0;

    // Median filter. Initial ADC readings are discarded before publishing.
    static const int FILTER_SIZE = 5;
    static const int WARMUP_SAMPLES = 5;
    uint32_t filter_buf_[FILTER_SIZE] = {};
    int filter_idx_ = 0;
    bool filter_filled_ = false;
    int warmup_remaining_ = WARMUP_SAMPLES;
    uint8_t low_sample_count_ = 0;
    volatile bool snapshot_valid_ = false;
    volatile bool low_confirmed_ = false;
    volatile uint8_t percentage_ = 0;
    volatile uint32_t snapshot_sequence_ = 0;
};

#endif // BATTERY_H
