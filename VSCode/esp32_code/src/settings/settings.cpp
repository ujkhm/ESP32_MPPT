#include "settings.h"

// 四個共享結構各自的自旋鎖實體(宣告見 settings.h「記憶體保護 / 執行緒安全存取介面」)。
// 分開成 4 顆而非共用 1 顆全域鎖，是為了讓互不相關的模組(例如 ina232 只是在更新
// 電壓電流，不該因此卡住 speed_sensor 的 ISR 或 motor_PID 的控制迴圈)不會互相阻塞。
portMUX_TYPE g_speed_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_pid_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_ina_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_ui_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_meas_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_bt_mux = portMUX_INITIALIZER_UNLOCKED;
