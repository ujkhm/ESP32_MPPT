#pragma once
#include <Arduino.h>
#include <Preferences.h> // 用於將變數存入FLASH
// 常數設定

// speed_sensor.h
#define buf_idx_bit 4                    // 環形緩衝深度位元數 → 2^4 = 16 筆時間戳
#define space_number 2                   // 用於測速之光柵盤總格數(遮光/透光格對數，依實際光柵修改)
#define EDGES_PER_REV (space_number * 2) // 雙沿觸發時，每一整圈的邊緣數

// 定速就緒門檻：控制迴路實際使用的轉速一律採用「同位置整圈」量測(rpm_from_same_slot)，
// 本身就用同一物理光柵位置抵銷每格加工誤差，不需要額外的格距誤差學習/補償；
// 這裡只保留一個簡單的樣本數門檻，確保開環固定 PWM 後至少觀察幾圈、EMA 濾波值
// 已收斂，才允許離開 LEARNING 進入 READY / 定速
#define READY_SETTLE_MIN_SAMPLES 24 // 開環固定 PWM 後，至少觀察幾圈才允許進入 READY / 定速
#define SPEED_STALL_TIMEOUT_MS 300  // 超過此時間無新邊緣 → 視為停轉/無法可靠量測

// 診斷用(不影響控制邏輯)：PID_RUN 定速期間偵測異常事件並印一次 [EVT]，方便日後排查
// 轉速忽然跳變/收斂變慢等現象時，快速判斷是感測端訊號問題還是馬達真實物理擾動
#define SPEED_JUMP_WARN_RPM 120   // 單一測速週期(read_space)轉速變化超過此值視為異常，印警告
#define SPEED_EDGE_GAP_WARN_MS 100 // 定速中連續無新邊緣超過此時間(仍未達 stall 判定)視為異常，印警告

// 邊緣去彈跳(debounce)：光柵訊號偶爾會有機械彈跳/雜訊造成極短間隔的假觸發，
// 對「整圈平均」影響很小(分母大)，但會讓「單格補償」算出離譜的瞬時轉速(分母只有 1/4 圈)。
// 兩次觸發間隔對應轉速若超過 RPM_RUNAWAY_MAX×此倍數，視為雜訊直接在 ISR 丟棄、不計入。
#define MIN_EDGE_TICKS_SAFETY_RPM_MULT 2.5f

// 「同位置整圈」量測法可正常讀取的最低轉速門檻(對應舊版約 1000RPM 失靈現象) + 餘量
#define RPM_INIT_READABLE 1000.0f
#define RPM_INIT_MARGIN 150.0f

// 階梯開環起動：升一次 PWM → 等待穩定 → 擷取第一筆可用轉速
#define MOTOR_PWM_STEP_RATIO 0.05f        // 每次升高的 duty 佔最大輸出比例
#define MOTOR_STEP_SETTLE_MS 250          // 每次升 PWM 後等待穩定的時間(ms)
#define MOTOR_PROBE_TIMEOUT_MS 400        // settle 後等待第一筆可用轉速的逾時(ms)，逾時視為 0 並再升階
#define MOTOR_STARTUP_PWM_MAX_RATIO 0.70f // 開環起動允許的最大 duty 比例

// 失控保護
#define RPM_RUNAWAY_MAX 17000.0f    // 轉速超過此值視為飛車，立即切斷輸出
#define FAULT_STALL_WITH_PWM_MS 500 // 已 hold/定速 後：有 PWM 卻持續無脈衝的容許時間(ms)

// PID 自動調參(sTune)：速度感測器調教完成(const_speed_ready)後執行
// 注意：本機台 PWM→RPM 增益極大(小小的佔空比變化就能造成數千 RPM 變化)，
// 快速的『反曲點法(IP)』很容易被步階響應中的震盪/超調騙到過大的製程增益，
// 進而算出過度激進、會在 0↔滿載間來回硬切換的 PID 參數，故改用：
//   1) 完整 5T 測試(direct5T)取代反曲點法(directIP)，較不怕響應有震盪
//   2) NoOvershoot_PI 規則(只用 P+I，不含 D)取代傳統 Ziegler-Nichols(較不激進)：
//      QuickPID 內部把 Kd 換算成每輪增益是「Kd / 取樣秒數」，而定速閉環取樣週期
//      (read_space)僅 15ms，若含 D 項會被放大達數百倍，量測轉速的正常雜訊就足以
//      讓輸出在 0↔滿載間反覆硬切換、永遠無法收斂；改用純 PI 規則從根源避開此問題
//      (此製程 Tau/DeadTime 比值高，sTune 自評「easy to control」，PI 已足夠)
//   3) 較小的測試步階(貼近實際工作點附近的線性區)
//   4) 依目前轉速動態估計 inputSpan，而非直接套用飛車保護的絕對上限
//   5) 額外乘上安全係數(PID_TUNE_GAIN_SCALE)做保守化
//   6) ★把 sTune 的無因次增益換算回真實工程單位★
//      sTune 內部的製程增益是 (ΔPV/inputSpan)/(ΔCO/outputSpan)，是無因次的，
//      它推導出的 Kp 也是「正規化增益」；但 QuickPID 吃的是真實單位(RPM→PWM count)，
//      兩者差 inputSpan/outputSpan 倍。本機台 inputSpan≈5000RPM、outputSpan=1023count，
//      比值高達約 4.9，直接套用會讓 Kp 放大近 5 倍而逼近臨界增益，必定劇烈震盪。
//      (sTune 官方範例的 inputSpan/outputSpan 多半數量級相近、比值≈1，所以不會踩到)
//      實際換算與重算規則見 motor_PID.cpp 的 sTune::tunings 分支
#define PID_AUTOTUNE_ENABLE 1                // 1=啟用自動調參
#define PID_AUTOTUNE_EVERY_BOOT 1            // 1=每次上電都調；0=僅 NVS 尚未 tuned 時調一次
#define PID_TUNE_SETTLE_SEC 3                // 調參前在 outputStart 穩定秒數
#define PID_TUNE_TEST_SEC 25                 // 開環步階測試時間上限(秒)；5T 法需約 5×Tau，寧可抓寬鬆
#define PID_TUNE_SAMPLES 250                 // 測試取樣點數(建議 200~500)
#define PID_TUNE_STEP_RATIO 0.08f            // 相對目前開環 duty 再升的比例(佔 max PWM)；愈小愈接近目標點附近的線性區
#define PID_TUNE_INPUT_SPAN_MIN 2000.0f      // sTune 輸入(RPM)全幅估計下限，實際值於執行期依目前轉速動態估算
#define PID_TUNE_ESTOP_RATIO 0.75f           // 調參期間緊急停止門檻 = RPM_RUNAWAY_MAX × 此比例(較保守)
#define PID_TUNE_PRE_SETTLE_MS 500           // 測速就緒後，先等待此時間讓濾波轉速真正收斂，再鎖定 keep_rpm / 開始測試
#define PID_TUNE_GAIN_SCALE 0.8f             // 對算出的 Kp/Ki/Kd 額外乘上的安全係數(<1 更保守)
// NoOvershoot_PI 的閉環迴路增益 = Kp×製程增益 = 0.35×(Tau/死時間)，只由可控性比值決定。
// 萬一某次量到異常小的死時間(雜訊或取樣抖動)會讓迴路增益暴衝而震盪，故對此比值設上限；
// 20 對應迴路增益 7，對 Tau/td 已屬「很好控制」的系統而言仍相當保守
#define PID_TUNE_TAU_DEAD_RATIO_MAX 20.0f
#define PID_TUNE_TOTAL_TIMEOUT_MARGIN_SEC 10 // 整段調參(settle+test)之外再加的總時長保險絲，逾時視為失敗並斷電
#define PID_TUNE_SERIAL_VERBOSE 1            // 1=調參時印進度/sTune 摘要；0=僅關鍵事件
#define PID_TUNE_SERIAL_MS 500               // 調參進度列印間隔(ms)

// 轉速量測濾波與輸出斜率限制：
// 「同位置整圈」量測本身雜訊已經很低，仍加一層輕量濾波避免殘餘尖峰污染
// keep_rpm 鎖定與 PID 輸入；定速輸出也需限制斜率，避免每個控制週期都在
// 0↔滿載間硬切換造成機械衝擊
#define SPEED_FILTER_ALPHA 0.3f // 轉速量測的 EMA 濾波係數(愈小愈平滑、愈慢反應)
#define PID_OUTPUT_SLEW_MAX 60  // 定速閉環每次控制週期(read_space)PWM 最大變化量(count，滿載為 1023)

// 轉速穩調旗標(speed_stable)判定：僅在整機正常閉環且貼近目標時才可能為 true
#define SPEED_STABLE_ABS_EPS 40.0f             // |實際轉速-目標| 低於此值(RPM)視為貼近
#define SPEED_STABLE_REL_EPS 0.025f            // 或相對誤差低於此比例視為貼近
#define SPEED_STABLE_NEED_HITS 20              // 連續滿足幾次(約 read_space×N ms)才置 true
#define SPEED_STABLE_MIN_RPM RPM_INIT_READABLE // 低於此轉速不宣告穩調(尚非可穩定讀取區)

// 測速 / 起動階段(供除錯與模組協調)
enum speed_init_phase : uint8_t
{
    SPEED_PHASE_STEP_UP = 0,  // 階梯升 PWM，等待 settle
    SPEED_PHASE_PROBE = 1,    // settle 結束，等待第一筆可用同位置轉速
    SPEED_PHASE_LEARNING = 2, // 已達可讀門檻，固定當前 PWM 做就緒觀察(穩定確認)
    SPEED_PHASE_READY = 3,    // 測速初始化完成
    SPEED_PHASE_FAULT = 4,    // 失控保護觸發，輸出切斷
    SPEED_PHASE_PID_TUNE = 5, // 測速完成後，正在自動調 PID
    SPEED_PHASE_PID_RUN = 6,  // PID 定速運行中
};

// 故障碼(settings.fault_code)
enum speed_fault_code : uint8_t
{
    FAULT_NONE = 0,
    FAULT_RUNAWAY_RPM = 1,      // 轉速超過 RPM_RUNAWAY_MAX
    FAULT_STALL_WITH_PWM = 2,   // 有 PWM 輸出卻測不到轉速
    FAULT_PWM_MAX_NO_SPEED = 3, // 開環已達上限仍無法達到可讀轉速
    FAULT_AUTOTUNE_FAIL = 4,    // PID 自動調參失敗/中止
};

// 變數設定
// speed sensor settings and shared variables
struct speed_sensor
{
    uint16_t read_space = 15;                     // 多久讀取一次速度感測器的值,單位為毫秒(此值亦是PID之計算間隔)
    float now_speed;                              // 現在主動力馬達的轉速(RPM)
    uint8_t buf_idx : buf_idx_bit = 0;            // 測速環形緩衝寫入指標(自動在 0~15 循環)
    uint32_t cap_buffer[(1 << buf_idx_bit)];      // 用於儲存最後的幾個數據(取決於上面的buf_idx)
    uint8_t count_number : buf_idx_bit;           // 指標快照
    uint16_t Timestamp_state[(1 << buf_idx_bit)]; // 用於儲存cap_buffer陣列中數值的狀態(0:可用 1:不可用)
    uint32_t edge_count = 0;                      // 絕對邊緣計數(ISR遞增，用於對應物理格序)
    uint16_t settle_samples = 0;                  // 開環固定 PWM 後，已觀察到的同位置整圈樣本數
    bool speed_valid = false;                     // 目前 now_speed 是否可信(未逾時)
    bool const_speed_ready = false;               // 就緒觀察完成 → 允許定速/調參
    float min_measurable_rpm = RPM_INIT_READABLE; // 可正常讀取的最低轉速門檻
    uint8_t init_phase = SPEED_PHASE_STEP_UP;     // 目前初始化階段
    bool rpm_stable = false;                      // 內部：開環已 hold 在可讀門檻以上(非對外穩調旗標)

    // ---- 對外：轉速穩調旗標(其他模組請讀這個) ----
    // true  = PID+測速皆就緒、無異常、轉速可穩定讀取且已貼近 keep_rpm
    // false = 初始化中 / 調參中 / 故障 / 換目標後尚未到位 / 轉速不可信
    bool speed_stable = false;

    // ---- 與 motor_PID 協調的開環階梯介面 ----
    uint16_t ol_pwm_cmd = 0;       // 目前開環 PWM duty 指令(由 motor_PID 寫入)
    bool ol_hold = false;          // true：停在當前 PWM，進行就緒觀察(穩定確認)
    bool ol_probe_request = false; // motor：settle 結束，請求擷取第一筆可用轉速
    bool ol_probe_ready = false;   // sensor：已擷取到 probe 轉速
    float ol_probe_rpm = 0.0f;     // 該次階梯的 probe 轉速

    // ---- 失控保護(兩模組共用) ----
    bool fault = false;
    uint8_t fault_code = FAULT_NONE;
};
extern volatile speed_sensor settings; // 實體在speed_sensor.cpp中

struct motor_PID
{
    float keep_rpm = 0;        // 目標轉速(RPM)；0 時調參完成後會鎖存當前轉速
    uint32_t pwm_freq = 20000; // PWM頻率
    uint8_t pwm_res = 10;      // PWM解析度(位元)
    float Kp;                  // 以下參數如要修改預設值請到void load()內修改
    float Ki;
    float Kd;
    bool tuned = false;           // NVS：是否已完成過自動調參
    bool autotune_active = false; // 執行期：目前是否正在 sTune
    bool autotune_done = false;   // 執行期：本次上電是否已完成調參(或已跳過)

    // 【結構體自我載入】
    void load() volatile
    {
        Preferences prefs;
        prefs.begin("pid_cfg", true); // 只讀模式
        Kp = prefs.getFloat("Kp", 0.0f);
        Ki = prefs.getFloat("Ki", 0.0f);
        Kd = prefs.getFloat("Kd", 0.0f);
        tuned = prefs.getBool("tuned", false);
        keep_rpm = prefs.getFloat("keep_rpm", keep_rpm);
        prefs.end();
    }

    // 【結構體自我儲存】
    void save() volatile
    {
        Preferences prefs;
        prefs.begin("pid_cfg", false); // 讀寫模式
        prefs.putFloat("Kp", Kp);
        prefs.putFloat("Ki", Ki);
        prefs.putFloat("Kd", Kd);
        prefs.putBool("tuned", tuned);
        prefs.putFloat("keep_rpm", keep_rpm);
        prefs.end();
    }
};
extern volatile motor_PID PID_settings; // 實體在motor_PID.cpp中
