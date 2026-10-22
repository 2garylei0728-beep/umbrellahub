# 通訊協定說明（v4.2 韌體實際行為，2026-10-21 確記）

> 本文件是三方通訊的權威描述。任何修改伺服器或韌體前，請先讀本文件。

## 三段通訊

```
手機瀏覽器 ═══ JSON（帶 id）═══ 中繼伺服器 ═══ 純文字 ═══ ESP32 韌體
                                     ▲
                              JSON（不帶 id）上行
```

## 1. 下行：伺服器 → ESP32（純文字命令）

固件 `handleServerCmd(const String& raw)` 只認以下**純文字**命令（其餘內容靜默忽略）：

| 命令 | 行為 |
|---|---|
| `status` | 立即推送一次狀態 |
| `store` | 找空格→產生 4 位碼→**立即回 result(ok,slot,code)**→進入忙碌週期（轉盤+開門） |
| `take 1234` | 碼錯誤→立即回 result(ok:false,「取傘碼錯誤」)；正確→進入忙碌週期，**不回 result** |
| `open` / `close` | 立即回 result |
| `clear` | 清空所有格位，回 result + 推送狀態 |
| `home N` | 設定對正格（1-8），回 result + 推送狀態 |
| `go N` | 轉到第 N 格（阻塞式旋轉），回 result + 推送狀態 |

## 2. 上行：ESP32 → 伺服器（JSON，不帶 id）

- `{"t":"status","free":N,"total":8,"busy":bool,"cur":N,"codes":[...]}`——每 2 秒推送一次
- `{"t":"result","ok":bool,"msg":"...","slot":N,"code":"...."}`——見上表「立即回」的場合

**關鍵細節：take 成功不發 result。** 韌體只推送 busy 狀態週期
（busy=false → busy=true → …→ busy=false）。伺服器因此以「單一命令飛行中
（commandInFlight）」配對無 id 的回覆，並在忙碌週期結束時推斷 take 成功，
用提交時計算的 `expectedSlot`（取傘碼在 codes 中的位置）回給瀏覽器。

## 3. 瀏覽器 ↔ 伺服器（JSON，帶 id）

- 前端送出：`{t:'store'|'take'|'admin', id, code?, key?, act?, n?}`
- 伺服器回：`{t:'result', id, ok, msg, slot, code}`（id 原樣配回）、
  `{t:'status', ...}`、`{t:'presence', online}`

## 歷史教訓（2026-10-21）

早期本地版伺服器誤將下行實作為 JSON（`{"t":"store","id":N}`）——韌體
`JSON.parse` 雖不執行，但 `m == "store"` 比對的是**原始字串**，JSON 格式
永遠匹配不到，指令被**靜默丟棄**。而本地 mock_esp32.js 當時也講 JSON，
所以測試全綠，卻從未真正驅動過櫃子。Manus 部署時閱讀 .ino 發現並修正為
純文字協定；本倉庫的 `server.js`（main 分支）即為協議正確版，
`test/mock_esp32.js` 亦已改為真實協議（文字下行 + 無 id JSON 上行）。

**教訓：mock 必須逐字複製真實設備的行為，否則測試只是自我應驗。**
