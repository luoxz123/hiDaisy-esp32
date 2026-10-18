#include "application.h"
#include "ota_manager.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#ifdef CONFIG_HILILY_QEMU_MODE
#include "qemu_console.h"
#endif

static const char *TAG = "main";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "HiLily ESP32 starting...");

#ifdef CONFIG_HILILY_QEMU_MODE
    ESP_LOGI(TAG, "*** QEMU EMULATION MODE ***");
    qemu_console_init();
#endif

    Application& app = Application::GetInstance();
    app.Start();
    app.Run();  // Never returns
}
