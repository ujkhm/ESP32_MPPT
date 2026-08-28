# Host computer — micro generator 上位機

本資料夾為 **免安裝可攜式** 上位機：執行檔、`data/` 設定與紀錄檔皆放在同一目錄，可直接複製整個資料夾到其他電腦使用。

上位機透過**藍牙(BluetoothSerial 虛擬序列埠)**連接下位機，全程用精靈式(wizard)流程引導：配對裝置 → 選擇儲存位置 → 放置發電機並按 START → 即時狀態，測試完成後可存成 PDF 報表。

## 目錄結構

```
Host computer/
├── ESP32_MPPT_Host.exe   ← 打包後的執行檔（build.bat 產生）
├── data/                 ← 執行期資料（設定）
│   └── config.json
├── src/                  ← Python 原始碼（開發用）
│   ├── app/              ← Tkinter 精靈介面
│   └── core/             ← 藍牙配對、序列埠、遙測解析、PDF 報表
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
3. 依畫面指示配對 micro generator 下位機、選擇儲存位置
4. 設定會寫入 `data/config.json`，無需安裝 Python

## 與下位機通訊

| 項目 | 說明 |
|------|------|
| 連線方式 | 藍牙 SPP(配對後 Windows 會建立一個虛擬 COM 埠，程式用一般序列埠方式讀取) |
| 鮑率 | 115200 |
| 資料格式 | 每行一個 JSON 物件：`{"t":"live",...}` 約每 200ms 一次、`{"t":"curve",...}` 曲線算完後每約 3 秒重送一次整張表 |

## 依賴

- Python 3.10+（開發用）
- `pyserial`、`matplotlib`
- `winsdk`（藍牙掃描/配對用；Windows Runtime 的 Python 投影）
- PyInstaller（僅打包用）

### 關於 `winsdk` 安裝

`winsdk` 在較新的 Python 版本(例如 3.13)上可能沒有現成的二進位輪子，`pip install` 時會嘗試**從原始碼建置**，需要：

1. 安裝 **Visual Studio Build Tools**，並勾選「**使用 C++ 的傳統桌面開發**」工作負載
   （或執行：`winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`）
2. 重新執行 `pip install -r requirements.txt`

**如果 `winsdk`裝不起來也沒關係**：程式會自動偵測，「配對裝置」畫面會退化成手動選擇 COM 埠(使用者需自行先在 Windows 藍牙設定完成配對一次)，其餘功能(即時狀態、PDF 報表等)完全不受影響。

## 常見問題

**找不到藍牙裝置：** 確認下位機已開機、藍牙功能正常，且電腦藍牙已開啟。掃描約需幾秒才會列出附近裝置。

**配對成功但連不上：** Windows 建立虛擬 COM 埠有時需要幾秒，程式會自動等待；若逾時，會提示改用手動選擇 COM 埠。

**打包後 exe 無法啟動：** 重新執行 `setup_env.bat` 後再 `build.bat`；若防毒軟體攔截，加入白名單。

**修改程式後 exe 沒更新：** 必須重新執行 `build.bat`，開發測試請用 `run_dev.bat`。
