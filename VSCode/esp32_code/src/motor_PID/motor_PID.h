#pragma once

#include <Arduino.h>
#include "settings/settings.h"
#include <QuickPID.h>
#include "pins/pins.h"
#include "RTOS/RTOS.h"
#include "esp32-hal.h"

void motor_PID_start();