# G1A: Phase 2 マイク実測手順（ドラフト・未承認）

## 位置づけ

この文書は`docs/g1a-microphone-plan.md`を実行可能なscopeへ落とすドラフトである。
`HARDWARE_HOLD`は継続し、固定image、SHA-256、wrapper、host gate、ユーザー承認が
揃うまで実機操作を許可しない。G0の承認やtokenはG1Aへ流用しない。

G1Aはマイク測定だけを行う。QSPI、録音flash、FatFs、MSC、storage G1、
production firmware、battery、外部配線はscope外である。

## 設計原則

1. LEDを状態判定に使わない。bootloaderはUSB PID `2886:0045` exactly 1件、
   applicationは`2886:8045` exactly 1件で判定する。
2. Stage別の固定imageを使う。hostからserial commandを書かず、firmwareが時間区間と
   markerを自動生成する。
3. tokenはStageごとに1つだけ発行する。前Stageのlogと数値判定が終わるまで次を発行しない。
4. operationはdevice/port access前に消費する。失敗、timeout、結果不明でもretryしない。
5. serialは受信専用で開き、host writeは0 byteとする。
6. 静的link検査に加え、application起動後に新しいvolume名が現れないことを確認する。
   volumeの中身は読まず、mount/unmount commandも発行しない。

## Stage別の上限

| Stage | 固定image | 物理double reset | upload | serial-read | session上限 |
|---|---|---:|---:|---:|---|
| 1 baseline | `g1a_s1_stats` | 1 | 1 | 1 | 150秒 |
| 2 finger | `g1a_s2_finger` | 1 | 1 | 1 | 60秒 |
| 3 handling | `g1a_s3_handling` | 1 | 1 | 1 | 180秒 |
| 4 duct | `g1a_s4_duct` | 1 | 1 | 2 | 各45秒 |
| 5 raw AB | `g1a_s5_raw` | 1 | 1 | 5 | 各45秒 |
| **最大合計** | 5 image | **5** | **5** | **10** | — |

この表は全Stageへ進めた場合の絶対上限である。Stage 2に不合格が1項目でもあれば
Stage 3〜5 tokenを発行しない。未発行分を別用途へ振り替えない。

Stage別imageとする理由は、受信専用を維持したまま測定modeを明確に固定し、
電源断や再接続後もsession番号に依存せず同じprotocolを再現するためである。

## 共通host gate

各imageについて、実機接続前に次を行う。

- source、ELF、HEX、DFU packageのSHA-256を固定
- QSPI / flash / filesystem / MSC / mount / reset / serial-read commandへの
  source参照と最終link symbolが0であることを検査
- PDM begin/read callback、区間marker、drop/overflow/no_sample counterが
  最終imageに存在することを検査
- wrapperをpseudo terminalと偽`arduino-cli`で試験し、scope未発行、hash不一致、
  PID 0件/複数/不一致、timeout、重複operation、順序違反でdevice access前または
  次操作前に停止することを確認
- Stage 5はframe header、sample count、CRC32、trailerをhost fixtureで検証し、
  10秒 = 160,000 samples = 320,000 bytes以外を不合格にする

再buildでhashが変われば、そのStageの承認対象から外す。

## Stage token

Stage Nごとの`docs/collab/G1A_STAGE_N_SCOPE.json`は次を固定する。

- `scope: G1A-stage-N`
- `approval_ref: MSG-NNN`
- UTC expiry
- Stage番号と固定image SHA-256
- 順序付きoperation ID、種類、最大時間、固定log path
- upload時のbootloader PID、serial時のapplication PID

消費記録はtoken hash、operation ID、消費時刻、終了結果をappend-onlyで残す。
token変更後の継続、同一operationの再利用、前operationがpassしていない状態での
次operationを拒否する。次Stageのtokenは別pathへ新規発行し、前Stageの未使用枠を
引き継がない。

## 共通実施順序

各Stageで次を1操作ずつ行い、継続条件を確認する。

1. Mac上の作業保存、battery 50%以上、chargerと不要な機器を外す。
2. 基板、connector、cable、固定具に破損、異物、発熱跡、臭いがないことを確認する。
3. USBを外した状態で、Stageの装着物・固定具を取り付ける。導電物と外部配線は禁止。
4. USB接続後、物理RESETを素早く2回だけ操作する。LEDは記録しても判定に使わない。
5. upload operationを消費し、PID `0045` exactly 1件の場合だけ固定packageを1回uploadする。
6. application PID `8045` exactly 1件、bootloader volume消失、新規volume 0を
   volume名だけで確認する。異常時はserialへ進まない。
7. serial operationを消費し、固定上限内で受信する。console表示とlog追記を同時に行い、
   hostからbyteを書かない。
8. session close後にlogを解析し、Stage固有gateを判定する。
9. 異常がなければMac側USB plugを抜く。次Stageは新しいtokenの発行まで開始しない。

## Stage 1 — baseline

firmware markerに従い、起動過渡を除外して次を測る。

1. 静音30秒
2. gain `0x14 / 0x28 / 0x3C`を各10秒
3. 30cm通常音量、30cm大声、40cm通常音量、40cm大声を各10秒

準備区間を含めsession上限は150秒。gain値はmarkerごとにlogへ残す。
drop、overflow、no_sampleのいずれかが1以上なら測定無効として停止する。
Stage 2以降の固定gainはStage 1結果をClaude、ChatGPT、Grokがレビューして決め、
その値をStage 2 imageとhashへ固定する。未レビューのdefault値で先へ進まない。

## Stage 2 — finger gate

基板ごと人差し指へ非導電固定し、口元30〜40cmで次を各10秒測る。

1. `S2_SILENCE`: 無発話、手を固定
2. `S2_SPEECH_STILL`: 通常音量の発話、手を固定
3. `S2_MOVE_ONLY`: 無発話、手を通常使用範囲で動かす
4. `S2_SPEECH_MOVE`: 通常音量の発話、同時に手を動かす

区間markerとsample数が揃わないsessionは無効とする。`docs/g1a-microphone-plan.md`の
S1〜S4を計算し、不合格が1つでもあればStage 3以降を行わない。
条件付きは記録してStage 3へ進めるが、Stage 5の文字起こし結果と再評価する。

## Stage 3 — handling

固定gainで、握る/開く、指曲げ、衣服擦れ、keyboard、歩行を、
無発話10秒と発話10秒の2条件で測る。準備markerを含め180秒を上限とする。
drop、overflow、no_sample、区間欠落があれば停止する。

## Stage 4 — duct

同じ固定phraseと姿勢で、ductなし、ductありを別serial sessionとして各30秒測る。
1 session目の終了後はMac側USBを抜き、無通電状態で厚紙と非導電tapeのmockを付ける。
基板、USB connector、antenna、RESET、mic holeへ粘着部や紙片を接触させない。
再接続後はapplication PID `8045` exactly 1件と新規volume 0を確認し、2 session目へ進む。

drop、overflow、no_sample、区間欠落があれば停止する。帯域別energyは同じphraseの
対応区間で比較し、絶対音圧の校正値とは扱わない。

## Stage 5 — raw AB

raw専用imageを1回uploadし、静かな部屋、屋外または店内、歩行、衣服擦れ、小声の
5条件を別sessionで各10秒取得する。各条件間はserialを閉じ、Mac側USBを抜いてから
移動・準備する。再接続後はapplication PID `8045` exactly 1件と新規volume 0を確認する。

各sessionはframe header、condition ID、gain、sample rate、sample count、CRC32、
trailerを含む。hostはexactly 320,000 byteをbinary fileへ保存し、SHA-256をlogへ残す。
frame不正、sample不足/超過、CRC不一致、timeout、serial切断はそのconditionを不合格として
停止し、retryしない。

## 即時停止条件

- PID 0件、複数、不一致、device消失、予期しない再列挙
- 新しいvolume名、QSPI/MSC pathの兆候
- drop、overflow、no_sample、clip条件違反、区間marker欠落
- Macのfreeze/restart/著しい遅延
- 基板、cable、connectorの発熱、変色、焦げ臭、煙、異音
- command timeout、nonzero、結果不明

停止後は新しいcommand、追加reset、retry、mount/unmount、別toolによる復旧を行わない。
安全に可能ならMac側plugを抜き、時刻、最後のpass operation、観測結果を記録し、
`HARDWARE_HOLD`全面状態へ戻す。

## 承認までに残る作業

1. Stage 1〜5 firmwareとcompile-only buildを実装する。
2. G1A image checker、Stage token guard、stats/raw wrapper、host fixtureを実装する。
3. host gate結果と各package SHA-256を本書へ固定する。
4. ClaudeとGrokがscope、数値gate、回数、物理手順をレビューする。
5. ユーザーが固定hash、各回数、禁止操作、停止条件を明示承認する。

この5項目が完了するまでG1A tokenを発行せず、実機へアクセスしない。
