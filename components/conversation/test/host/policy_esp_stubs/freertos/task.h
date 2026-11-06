#pragma once
#include "freertos/FreeRTOS.h"
#define taskENTER_CRITICAL(mux) ::policy_esp_stub::enter(mux)
#define taskEXIT_CRITICAL(mux) ::policy_esp_stub::leave(mux)
