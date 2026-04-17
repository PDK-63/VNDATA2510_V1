#pragma once

#include "esp_err.h"
#define TCA_PIN(port, bit)   ((port) * 8 + (bit))

#define APP_DO1_TCA_PIN      TCA_PIN(0, 0)
#define APP_DO2_TCA_PIN      TCA_PIN(0, 1)

#define APP_DI1_TCA_PIN      TCA_PIN(1, 7)
#define APP_DI2_TCA_PIN      TCA_PIN(1, 6)
#define APP_DI3_TCA_PIN      TCA_PIN(1, 5)

void app_logic_load_runtime_config(void);

esp_err_t app_logic_init(void);
