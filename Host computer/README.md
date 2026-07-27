# Host computer — ESP32 MPPT 上位機

本資料夾為 **免安裝可攜式** 上位機：執行檔、`data/` 設定與紀錄檔皆放在同一目錄，可直接複製整個資料夾到其他電腦使用。

## 目錄結構

```
Host computer/
├── ESP32_MPPT_Host.exe   ← 打包後的執行檔（build.bat 產生）
├── data/                 ← 執行期資料（設定、CSV 紀錄）
│   ├── config.json
│   └── logs/
├── src/                  ← Python 原始碼（開發用）
├── assets/               ← 圖示等靜態資源（選用）
├── .venv/                ← 本機虛擬環境（不提交 git）
├── setup_env.bat         ← 建立環境並安裝依賴
├── run_dev.bat           ← 開發模式執行（即時修改測試）
└── build.bat             ← 打包成 .exe 並複製到本目錄
```

## 快速開始（開發）

1. 雙擊或在終端執行 `setup_env.bat`（只需第一次）
2. 修改 `src/` 內程式碼
3. 執行 `run_dev.bat` 測試
4. 確認無誤後執行 `build.bat` 產生 `ESP32_MPPT_Host.exe`

## 免安裝使用（給使用者）

1. 複製整個 `Host computer` 資料夾
2. 雙擊 `ESP32_MPPT_Host.exe`
3. 選擇 ESP32 的 COM 埠 → 連線
4. 設定與紀錄會寫入 `data/`，無需安裝 Python

## 與 ESP32 通訊

| 項目 | 預設值 |
|------|--------|
| 鮑率 | 115200 |
| 資料格式 | 每秒一行 RPM 數值（對應韌體 `Serial.println(now_speed)`） |

可在 `data/config.json` 調整鮑率、自動存檔等選項。

## 依賴

- Python 3.10+（開發用）
- pyserial、matplotlib
- PyInstaller（僅打包用）

## 常見問題

**找不到 COM 埠：** 確認 USB 驅動已安裝，按「重新整理」。

**打包後 exe 無法啟動：** 重新執行 `setup_env.bat` 後再 `build.bat`；若防毒軟體攔截，加入白名單。

**修改程式後 exe 沒更新：** 必須重新執行 `build.bat`，開發測試請用 `run_dev.bat`。
