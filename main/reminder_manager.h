#ifndef REMINDER_MANAGER_H
#define REMINDER_MANAGER_H

#include <esp_err.h>
#include <functional>
#include <string>
#include <vector>
#include <ctime>

struct ReminderEntry {
    char reminder_id[32];
    char content[64];
    int64_t timestamp;
    bool triggered;
};

class ReminderManager {
public:
    using TriggerCallback = std::function<void(const std::string& reminder_id)>;

    static ReminderManager& GetInstance();

    ReminderManager(const ReminderManager&) = delete;
    ReminderManager& operator=(const ReminderManager&) = delete;

    esp_err_t Init();
    void Start();

    // Called when server sends reminder_created / cancel_reminder
    esp_err_t AddReminder(const char* reminder_id, int64_t timestamp, const char* content = "");
    esp_err_t CancelReminder(const char* reminder_id);

    // Get next reminder timestamp (for sleep wakeup calculation)
    int64_t GetNextReminderTimestamp() const;

    void OnTrigger(TriggerCallback callback) { trigger_callback_ = std::move(callback); }

    int GetReminderCount() const { return (int)reminders_.size(); }

private:
    ReminderManager() = default;
    ~ReminderManager() = default;

    static void ScanTask(void* arg);

    esp_err_t LoadFromNvs();
    esp_err_t SaveToNvs();

    std::vector<ReminderEntry> reminders_;
    TriggerCallback trigger_callback_;
    static const int MAX_REMINDERS = 10;
};

#endif // REMINDER_MANAGER_H
