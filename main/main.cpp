#include "emulator.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char* TAG = "main";

extern "C" void app_main(void);

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Starting GB EMU ESP32 ===");
    // CPU frequency is configured in sdkconfig (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)
    static emulator::Emulator emulator;

    // Initialiser l'émulateur
    esp_err_t res = emulator.init();
    if (res != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize emulator! Error: %s", esp_err_to_name(res));
        return;
    }

    ESP_LOGI(TAG, "Emulator initialized, starting main loop...");

    // Enable CPU debug logs for troubleshooting (uncomment if needed)
    // emulator.enableCPUDebugLogs(true);
    // ESP_LOGI(TAG, "CPU debug logs enabled");

    // Lancer la boucle principale (menu -> jeu -> menu -> ...)
    emulator.run();

    ESP_LOGI(TAG, "Emulator task started, deleting main task to free resources...");

    // Delete the main task to free up resources
    // The emulator task will continue running
    vTaskDelete(nullptr);
}
