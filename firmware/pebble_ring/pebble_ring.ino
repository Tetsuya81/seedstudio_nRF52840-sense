// =============================================================
//  pebble_ring - Phase 1: USB接続 + 模擬ボタン
//  Board: Seeed XIAO nRF52840 Sense
//  FQBN : Seeeduino:nrf52:xiaonRF52840Sense
//
//  実ボタンをまだ配線していないので、USBシリアルのコマンドで
//  ボタンの押下波形を注入し、録音デバイスの状態機械とLED表示を
//  そのまま動かして確認する。
//
//  ボタン入力は ButtonSource で抽象化してあるため、実機化のときは
//  g_button の指す先を GpioButtonSource に変えるだけでよい。
// =============================================================

#include <Adafruit_TinyUSB.h>   // このコアでは Serial(USB CDC) にこれが必要

#include "config.h"
#include "ButtonEvent.h"
#include "ButtonSource.h"
#include "VirtualButtonSource.h"
#include "GpioButtonSource.h"
#include "PressDetector.h"
#include "StatusLed.h"
#include "RecorderApp.h"
#include "MicCheck.h"
#include "FlashCheck.h"

static VirtualButtonSource g_virtual;
static GpioButtonSource    g_gpio(PIN_USER_BUTTON);   // Phase 6 用 (未配線)
static ButtonSource*       g_button = &g_virtual;

static PressDetector g_detector;
static StatusLed     g_led;
static RecorderApp   g_app;

// -------------------------------------------------------------
static void printHelp() {
  Serial.println();
  Serial.println(F("==== pebble_ring / phase1 : simulated button ===="));
  Serial.println(F("  p    短押し      (press for 80ms)"));
  Serial.println(F("  l    長押し      (press for 1200ms)"));
  Serial.println(F("  d    ダブル押し  (80ms x2, gap 150ms)"));
  Serial.println(F("  [    押しっぱなし開始"));
  Serial.println(F("  ]    離す"));
  Serial.println(F("  s    状態表示"));
  Serial.println(F("  v    詳細ログ切替 (Down/Up も表示)"));
  Serial.println(F("  i    LED極性を反転 (点灯が逆に見えるとき)"));
  Serial.println(F("  x    LEDセルフテスト (R->G->B->White)"));
  Serial.println(F("  g    ボタン入力を virtual <-> gpio で切替"));
  Serial.println(F("  m    マイク実測 (0.5秒録って RMS/ピーク/dBFS を表示)"));
  Serial.println(F("  f    フラッシュ実測 (容量と録音可能時間)"));
  Serial.println(F("  h    このヘルプ"));
  Serial.println(F("------------------------------------------------"));
  Serial.println(F("  SLEEP --long--> IDLE --short--> RECORDING"));
  Serial.println(F("  RECORDING: short=stop / double=marker / long=stop+sleep"));
  Serial.println(F("  LED: off=SLEEP  green blink=IDLE  red=REC  blue=marker"));
  Serial.println(F("================================================"));
  Serial.println();
}

static void handleCommand(char c, uint32_t now) {
  switch (c) {
    case 'p': g_virtual.pressFor(kSimShortMs); break;
    case 'l': g_virtual.pressFor(kSimLongMs);  break;
    case 'd': g_virtual.pressTwice(kSimShortMs, kSimDoubleGapMs); break;
    case '[': g_virtual.hold();    Serial.println(F("(hold)")); break;
    case ']': g_virtual.release(); Serial.println(F("(release)")); break;

    case 's': g_app.printStatus(now, g_button->name()); break;
    case 'v':
      g_app.setVerbose(!g_app.verbose());
      Serial.print(F("verbose = "));
      Serial.println(g_app.verbose() ? F("on") : F("off"));
      break;
    case 'i':
      g_led.toggleActiveLow();
      Serial.print(F("LED active level = "));
      Serial.println(g_led.activeLow() ? F("LOW") : F("HIGH"));
      break;
    case 'x':
      Serial.println(F("LED self test..."));
      g_led.selfTest();
      break;
    case 'g':
      if (g_button == &g_virtual) {
        g_button = &g_gpio;
        g_button->begin();
        Serial.println(F("button source = gpio  (D1 is not wired yet)"));
      } else {
        g_button = &g_virtual;
        g_button->begin();
        Serial.println(F("button source = virtual"));
      }
      g_detector.begin(g_button->isDown(), now);
      break;
    case 'm': MicCheck::run(Serial); break;
    case 'f': FlashCheck::run(Serial); break;
    case 'h': case '?': printHelp(); break;
    default: break;
  }
}

// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // USB CDC が上がるまで少し待つ (最大3秒。未接続でも先へ進む)
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) { delay(10); }

  g_led.begin();
  g_virtual.begin();
  g_app.begin(&g_led, &Serial);
  g_detector.begin(g_button->isDown(), millis());

  printHelp();
  Serial.println(F("ready. type 'p' to simulate a short press."));
}

void loop() {
  uint32_t now = millis();

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n' || c == ' ') continue;
    handleCommand(c, now);
  }

  g_button->poll(now);
  g_detector.update(g_button->isDown(), now);

  ButtonEvent e;
  while ((e = g_detector.take()) != ButtonEvent::None) {
    g_app.handle(e, now);
  }

  g_app.update(now);
  g_led.update(now);
}
