#include <Arduino.h>
#include <PDM.h>

// G0-only image: PDM + receive-only USB CDC reporting.  Deliberately no QSPI,
// flash, filesystem, MSC, command parser, reset path, battery ADC, or LED.
namespace {

constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kGain = 20;
constexpr int kPdmBufferBytes = 512;
constexpr int kClipThreshold = 32760;
constexpr uint32_t kReportMs = 1000;
constexpr uint32_t kStallMs = 250;

int16_t g_buffer[kPdmBufferBytes / sizeof(int16_t)];
volatile uint32_t g_callbacks = 0;
volatile uint32_t g_samples = 0;
volatile uint32_t g_clipSamples = 0;
volatile uint32_t g_dropSamples = 0;
volatile uint32_t g_oversizeEvents = 0;
volatile int64_t g_sum = 0;
volatile uint64_t g_sumSquares = 0;
volatile int32_t g_peak = 0;
volatile uint32_t g_lastCallbackMs = 0;

uint32_t g_totalCallbacks = 0;
uint32_t g_totalSamples = 0;
uint32_t g_totalClipSamples = 0;
uint32_t g_totalDropSamples = 0;
uint32_t g_totalOverflowEvents = 0;
uint32_t g_lastReportMs = 0;
bool g_stallLatched = false;
bool g_pdmStarted = false;
bool g_serialWasOpen = false;

struct Window {
  uint32_t callbacks;
  uint32_t samples;
  uint32_t clips;
  uint32_t drops;
  uint32_t oversizeEvents;
  int64_t sum;
  uint64_t sumSquares;
  int32_t peak;
  uint32_t lastCallbackMs;
};

void onPdmData() {
  const int available = PDM.available();
  int requested = available;
  if (requested > kPdmBufferBytes) {
    requested = kPdmBufferBytes;
    ++g_oversizeEvents;
  }
  if (requested < 0) requested = 0;
  const int bytesRead = PDM.read(g_buffer, static_cast<size_t>(requested));
  ++g_callbacks;
  g_lastCallbackMs = millis();

  if (available > bytesRead) {
    g_dropSamples += static_cast<uint32_t>((available - bytesRead) / 2);
  }
  if ((bytesRead & 1) != 0) ++g_dropSamples;

  const int count = bytesRead / static_cast<int>(sizeof(int16_t));
  for (int i = 0; i < count; ++i) {
    const int32_t sample = g_buffer[i];
    const int32_t magnitude = sample < 0 ? -sample : sample;
    g_sum += sample;
    g_sumSquares += static_cast<uint64_t>(sample * sample);
    if (magnitude > g_peak) g_peak = magnitude;
    if (magnitude >= kClipThreshold) ++g_clipSamples;
  }
  g_samples += static_cast<uint32_t>(count);
}

Window takeWindow() {
  noInterrupts();
  const Window result = {
      g_callbacks, g_samples, g_clipSamples, g_dropSamples,
      g_oversizeEvents, g_sum, g_sumSquares, g_peak, g_lastCallbackMs};
  g_callbacks = 0;
  g_samples = 0;
  g_clipSamples = 0;
  g_dropSamples = 0;
  g_oversizeEvents = 0;
  g_sum = 0;
  g_sumSquares = 0;
  g_peak = 0;
  interrupts();
  return result;
}

void printReport(uint32_t now) {
  const Window window = takeWindow();
  const bool stalled = g_pdmStarted &&
      (window.lastCallbackMs == 0 || now - window.lastCallbackMs > kStallMs);
  if (stalled && !g_stallLatched) ++g_totalOverflowEvents;
  g_stallLatched = stalled;

  g_totalCallbacks += window.callbacks;
  g_totalSamples += window.samples;
  g_totalClipSamples += window.clips;
  g_totalDropSamples += window.drops;
  g_totalOverflowEvents += window.oversizeEvents;

  const double mean = window.samples
      ? static_cast<double>(window.sum) / window.samples : 0.0;
  const double rms = window.samples
      ? sqrt(static_cast<double>(window.sumSquares) / window.samples) : 0.0;
  const double rmsDbfs = rms > 0.0 ? 20.0 * log10(rms / 32768.0) : -120.0;
  const double peakDbfs = window.peak > 0
      ? 20.0 * log10(static_cast<double>(window.peak) / 32768.0) : -120.0;

  Serial.print(F("G0_REPORT elapsed_ms=")); Serial.print(now);
  Serial.print(F(" samples=")); Serial.print(g_totalSamples);
  Serial.print(F(" callbacks=")); Serial.print(g_totalCallbacks);
  Serial.print(F(" window_samples=")); Serial.print(window.samples);
  Serial.print(F(" dc=")); Serial.print(mean, 1);
  Serial.print(F(" rms=")); Serial.print(rms, 1);
  Serial.print(F(" rms_dbfs=")); Serial.print(rmsDbfs, 1);
  Serial.print(F(" peak=")); Serial.print(window.peak);
  Serial.print(F(" peak_dbfs=")); Serial.print(peakDbfs, 1);
  Serial.print(F(" clips=")); Serial.print(g_totalClipSamples);
  Serial.print(F(" drops=")); Serial.print(g_totalDropSamples);
  Serial.print(F(" overflow=")); Serial.print(g_totalOverflowEvents);
  Serial.print(F(" no_sample=")); Serial.println(window.samples == 0 ? 1 : 0);
}

void printBanner() {
  Serial.println(F("G0_BANNER image=g0_microphone_smoke sample_rate=16000 channels=1 gain=20"));
  Serial.println(F("G0_POLICY qspi=0 flash=0 filesystem=0 msc=0 commands=0 reset=0 host_write_required=0"));
  Serial.println(g_pdmStarted ? F("G0_BEGIN result=ok")
                              : F("G0_FATAL reason=pdm_begin_failed"));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 3000) yield();

  PDM.setBufferSize(kPdmBufferBytes);
  PDM.onReceive(onPdmData);
  PDM.setGain(kGain);
  g_pdmStarted = PDM.begin(kChannels, kSampleRate) == 1;
  g_lastReportMs = millis();
  g_serialWasOpen = static_cast<bool>(Serial);
  if (g_serialWasOpen) {
    printBanner();
  }
}

void loop() {
  const uint32_t now = millis();
  const bool serialOpen = static_cast<bool>(Serial);
  if (serialOpen && !g_serialWasOpen) {
    printBanner();
    g_lastReportMs = now;
  }
  g_serialWasOpen = serialOpen;
  if (serialOpen && now - g_lastReportMs >= kReportMs) {
    g_lastReportMs += kReportMs;
    printReport(now);
  }
  yield();
}
