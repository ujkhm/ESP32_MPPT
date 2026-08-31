# 微型發電機測試機 / Micro Generator Test Machine

以 **ESP32** 為核心的小型有刷直流發電機**特性量測機台**，不是 MPPT 控制器。

主動力 775 馬達把待測發電機帶到指定轉速後，依序量測連續安全電流、內阻與開路常數，再算出固定測試電阻上的功率曲線與可用轉速上限，上位機可存成 PDF 報表。

## 倉庫結構

```
micro-generator-test-machine
├── 3D_print_Component/   變速箱與機構 3D 模型（外殼 STL 另補）
├── BOM/                  元件清單與互動式 BOM
├── KiCad/                原理圖、PCB、Gerber
├── VSCode/esp32_code/    ESP32 韌體（PlatformIO）
└── Host computer/        免安裝上位機執行檔與說明
```

各資料夾內另有說明。

## 上位機

`Host computer/MicroGeneratorHost.exe` 免安裝。複製該資料夾到 Windows 電腦，雙擊執行，依畫面以藍牙連接下位機即可測試。詳見 [Host computer/README.md](./Host%20computer/README.md)。

## 目前狀態

- 電路與 PCB：完成（見 `KiCad/`、`BOM/`）
- 韌體量測流程：完成（安全電流、內阻、曲線）
- 上位機：完成（藍牙監控與 PDF 報表）
- 外殼：待補 STL
