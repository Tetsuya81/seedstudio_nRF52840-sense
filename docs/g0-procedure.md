# G0: 限定実機再開手順（承認待ち）

## 位置づけ

G0はHARDWARE_HOLDの全面解除ではない。固定済みのG1A smoke imageを1回だけuploadし、
受信専用serial sessionを1回だけ開いて、PDMとUSB CDCの経路が生きているかを確認する。
G0だけではPhase 2を完了せず、G1A本測定、production firmware、QSPI、MSC、storage試験は許可しない。

実施には、この文書と下記image hashを対象にしたユーザーの明示承認が必要である。
承認後も`docs/collab/HARDWARE_HOLD`は削除せず、1回限りのscope tokenで例外を強制する。

## 既知の開始状態

- `[実測]` 基板は現在Macへ接続中で、旧applicationは既に起動済みである（MSG-058）
- `[実測]` 旧application由来のQSPI初期化とMSC公開は承認前に既に起きている（MSG-058）
- したがって「旧applicationを一度も起動しない」は条件から取り下げる
- G0手順としてQSPI/MSC/mountを能動的に操作せず、double reset以後は旧applicationを再起動しない

## 固定imageとhost gate

| 項目 | 値 |
|---|---|
| source | `firmware/g0_microphone_smoke/g0_microphone_smoke.ino` |
| FQBN | `Seeeduino:nrf52:xiaonRF52840Sense` |
| DFU package | `build/g0_microphone_smoke/g0_microphone_smoke.ino.zip` |
| package SHA-256 | `61bd270181460e84491535d8cbd8fecb31c36313d15a2d1e58d8e0cc4252c183` |
| HEX SHA-256 | `f5a0af8fc591d88b3bfb4446de20e7b2b1f80b6c902216c28f39468bae0cffab` |
| compile size | Flash 46,076 / 811,008 B、RAM 7,232 / 237,568 B |
| linked gate | QSPI / flash / filesystem / MSC / attach-detach path 0、PDM pathあり |

imageは16kHz / 16bit / mono、gain 20でPDMを自動開始し、1秒ごとにsample数、callback数、
DC、RMS、peak、clip、drop、overflow、no-sampleを出力する。host commandは読まず、
QSPI、flash、filesystem、MSC、battery ADC、LED、reset pathを持たない。
TinyUSBはG0専用configで`CFG_TUD_MSC=0`としてbuildする。

再buildでpackage hashが変わった場合、この承認対象ではなくなる。再レビュー・再承認する。

## 許可する構成

```text
MacBook（内蔵battery 50%以上、AC chargerを外す）
  └─ 既知の正常なUSB data cable 1本
       └─ XIAO nRF52840 Sense単体（battery・外部配線なし）
```

ACを外す理由は安全保証ではなく接続要素を減らすためである。hub、外部電源、LiPo、breadboard、
外部sensor、タクトスイッチ、追加配線は使わない。基板は乾いた非導電面へ置く。

## 許可しない操作

- G0手順としてのQSPI/recording flash read・write・erase・format・JEDEC取得
- MSC登録、LUN操作、mount、`diskutil`、`hdiutil`、旧firmware command
- 双方向monitor、hostからserialへのwrite、1200bps touch
- upload/serial失敗後のretry、再列挙、再reset、別toolによる復旧
- battery接続、外部配線、電源断・再現・storage試験

## 実施手順（7段）

各段で継続条件を満たした場合だけ次へ進む。失敗または結果不明なら、その場で終了する。

### Step 0 — host-only image gate（完了）

- `bash scripts/build_g0.sh`: compileとlinked gate PASS
- `python3 tools/storage_safety/test_guards.py`: 9 tests PASS
- package hashを上表へ固定

### Step 1 — preflight

ユーザーが次を確認する。

1. Mac上の作業を保存し、重要データの通常backupが完了
2. Mac battery 50%以上。chargerと不要なUSB/Thunderbolt機器を外した
3. 基板・connector・cableに発熱跡、変色、焦げ臭、破損、異物、緩みがない
4. 基板にbattery・外部電源・外部配線がなく、乾いた非導電面にある
5. 現物firmwareの完全backupとrecording flashのraw backupをG0では行わないことを了承

1件でも未確認または異常があれば開始しない。

### Step 2 — A1: 現接続のままbootloaderへ移行

明示承認後、ユーザーがRESETを素早く2回操作する。LEDのbootloader表示を目視確認する。
Mac、基板、cableを観察し、異常、reset loop、接続音反復があれば停止する。

この段階ではhost commandを実行しない。bootloader表示を確認できなければ、再resetせずG0を終了する。

### Step 4 — PID検査＋upload 1回

scopeを**device列挙前に消費**し、専用wrapper内で`2886:0045`がexactly 1件であることを検査する。
一致した場合だけ固定hashのDFU packageをuploadする。1200bps touchと追加port待機は無効にする。

0件、複数、PID不一致、timeout、nonzero、結果不明なら停止する。scopeは消費済みなのでretryしない。
upload logは`docs/logs/g0-upload.log`へ即時追記する。

### Step 5 — upload後の停止確認

commandを発行せず10秒観察する。Macが正常、発熱・臭い・reset/re-enumeration loopがなく、
upload exit 0の場合だけserialへ進む。

### Step 7 — application PID検査＋受信専用serial 1 session

serial scopeをdevice列挙前に消費する。wrapper内で`2886:8045`がexactly 1件の場合だけ、
115200bps固定、host writeなし、最大60秒で開く。logは`docs/logs/g0-serial.log`へ即時追記する。

最初の10秒以内に`G0_BANNER`、`G0_BEGIN result=ok`、`G0_REPORT`を確認する。
ユーザーは静かな状態10秒、30〜40cmから通常音量の発声10秒、手を静かに動かす10秒を行い、
残り時間は観察する。

無出力、PDM begin失敗、no-sample、drop、overflow、継続clip、serial切断、reset、異常があれば停止する。

### Step 8 — 終了

serial closeを確認し、commandなしで10秒観察後、ユーザーがMac側のUSB plugを抜く。
hash、両log、各step結果、実施しなかった禁止操作を記録する。

## 緊急停止

Macのfreeze/restart/著しい遅延、unexpected reconnect反復、基板・cable・connectorの発熱、
変色、焦げ臭、煙、異音、command timeout、識別不能、想定外resetのいずれかで即時停止する。

新しいcommand、unmount、reset、retry、別toolによる復旧を行わない。安全に可能ならMac側plugを抜き、
熱い基板には触れない。時刻、最後に完了したstep、Mac・基板の状態を記録し、HARDWARE_HOLD全面状態へ戻す。

## G0合格条件

- Step 0/1/2/4/5/7/8が各1回で完了
- upload 1回、serial session 1回、retry 0、禁止操作0
- Mac、基板、cableの異常0
- PIDがupload時`0045`、serial時`8045`で各exactly 1件
- `G0_BEGIN result=ok`、no-sample/drop/overflow 0、継続clipなし

合格後もG1A本測定には別scopeと別承認が必要である。

## 承認文

実施を承認する場合は、次を明示する。

> `docs/g0-procedure.md`の修正版G0を、package SHA-256
> `61bd270181460e84491535d8cbd8fecb31c36313d15a2d1e58d8e0cc4252c183`、
> 記載された構成・禁止操作・停止条件・各1回の範囲で実施することを承認します。

この承認を`chat.md`へ記録した後にだけ、短時間有効なscope tokenを発行する。
