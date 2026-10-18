#ifndef QEMU_CONSOLE_H
#define QEMU_CONSOLE_H

#include <esp_err.h>

// Initialize UART-based test console for QEMU mode.
// Provides interactive commands to simulate hardware events.
esp_err_t qemu_console_init(void);

#endif // QEMU_CONSOLE_H
