#include <WebServer.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>

#include "secrets.h"

namespace {
namespace CameraPins {
constexpr int kPwdn = -1;
constexpr int kReset = -1;
constexpr int kXclk = 21;
constexpr int kSiod = 12;
constexpr int kSioc = 9;
constexpr int kY9 = 13;
constexpr int kY8 = 11;
constexpr int kY7 = 17;
constexpr int kY6 = 4;
constexpr int kY5 = 48;
constexpr int kY4 = 46;
constexpr int kY3 = 42;
constexpr int kY2 = 3;
constexpr int kVsync = 10;
constexpr int kHref = 14;
constexpr int kPclk = 40;
constexpr int kPower = 18;
}  // namespace CameraPins

namespace CameraSettings {
constexpr framesize_t kFrameSizeWithPsram = FRAMESIZE_VGA;
constexpr framesize_t kFrameSizeWithoutPsram = FRAMESIZE_QVGA;
constexpr int kJpegQuality = 10;
constexpr uint8_t kDiscardFramesBeforeCapture = 1;
constexpr uint32_t kFrameSettleDelayMs = 30;
// キャプチャは1リクエストあたり100ms超かかるため、TTL内の連続アクセスには
// 直近フレームを再利用してカメラアクセスを1回に集約する。
constexpr uint32_t kFrameCacheTtlMs = 500;
}  // namespace CameraSettings

namespace Network {
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint16_t kHttpPort = 80;
}  // namespace Network

const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>AtomS3R Camera</title>
<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:20px;background:#111;color:#eee}
img{max-width:100%;height:auto;border:1px solid #444}a{color:#8cf}button{font-size:16px;padding:8px 12px}
table{border-collapse:collapse;margin:16px 0}td{border-bottom:1px solid #333;padding:4px 12px 4px 0}
td:first-child{color:#aaa}.muted{color:#aaa}</style>
</head><body><h1>AtomS3R Camera</h1>
<p><button onclick='reloadImage()'>Capture</button> <a href='/capture.jpg'>Open JPEG</a> <a href='/status'>Status JSON</a></p>
<img id='capture' src='/capture.jpg' alt='capture'>
<table><tbody id='status'><tr><td>Status</td><td class='muted'>Loading...</td></tr></tbody></table>
<script>
const keys=['camera','cameraError','cameraErrorName','wifi','ip','rssi','frameSize','jpegQuality','discardFramesBeforeCapture','captures','failures','lastBytes','lastAcquireMs','lastWriteMs','lastTotalMs','heap','psramSize','freePsram'];
function renderStatus(s){document.getElementById('status').innerHTML=keys.map(k=>`<tr><td>${k}</td><td>${s[k]}</td></tr>`).join('');}
async function loadStatus(){try{const r=await fetch('/status?t='+Date.now());renderStatus(await r.json());}catch(e){document.getElementById('status').innerHTML='<tr><td>Status</td><td>Failed to load</td></tr>';}}
function reloadImage(){const img=document.getElementById('capture');img.onload=loadStatus;img.src='/capture.jpg?t='+Date.now();}
document.getElementById('capture').onload=loadStatus;loadStatus();
</script>
</body></html>)HTML";

struct CaptureStats {
  uint32_t captures = 0;
  uint32_t failures = 0;
  size_t lastBytes = 0;
  uint32_t lastAcquireMs = 0;
  uint32_t lastWriteMs = 0;
  uint32_t lastTotalMs = 0;
};

// 直近のJPEGフレームを保持するバッファ。PSRAMがあればPSRAMに置く。
struct FrameCache {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t len = 0;
  uint32_t capturedAtMs = 0;

  bool store(const uint8_t* src, size_t srcLen, uint32_t nowMs) {
    if (srcLen > capacity) {
      uint8_t* grown = static_cast<uint8_t*>(
          heap_caps_malloc(srcLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (grown == nullptr) {
        grown = static_cast<uint8_t*>(malloc(srcLen));
      }
      if (grown == nullptr) {
        return false;
      }
      free(data);
      data = grown;
      capacity = srcLen;
    }
    memcpy(data, src, srcLen);
    len = srcLen;
    capturedAtMs = nowMs;
    return true;
  }

  bool isFresh(uint32_t nowMs) const {
    return len > 0 && nowMs - capturedAtMs <= CameraSettings::kFrameCacheTtlMs;
  }
};

WebServer server(Network::kHttpPort);

bool cameraReady = false;
bool wifiReady = false;
esp_err_t lastCameraError = ESP_OK;
CaptureStats stats;
FrameCache frameCache;

const char* frameSizeName(framesize_t frameSize) {
  switch (frameSize) {
    case CameraSettings::kFrameSizeWithPsram:
      return "VGA";
    case CameraSettings::kFrameSizeWithoutPsram:
      return "QVGA";
    default:
      return "UNKNOWN";
  }
}

framesize_t configuredFrameSize() {
  return ESP.getPsramSize() > 0 ? CameraSettings::kFrameSizeWithPsram
                                : CameraSettings::kFrameSizeWithoutPsram;
}

camera_fb_t* captureFreshFrame() {
  for (uint8_t i = 0; i < CameraSettings::kDiscardFramesBeforeCapture; ++i) {
    camera_fb_t* staleFrame = esp_camera_fb_get();
    if (staleFrame == nullptr) {
      return nullptr;
    }
    esp_camera_fb_return(staleFrame);
    delay(CameraSettings::kFrameSettleDelayMs);
  }

  return esp_camera_fb_get();
}

bool refreshFrameCache() {
  const uint32_t startedMs = millis();
  camera_fb_t* fb = captureFreshFrame();
  if (fb == nullptr) {
    ++stats.failures;
    Serial.println("capture failed: no frame");
    return false;
  }

  const bool isJpeg = fb->format == PIXFORMAT_JPEG;
  const bool stored = isJpeg && frameCache.store(fb->buf, fb->len, millis());
  esp_camera_fb_return(fb);

  if (!stored) {
    ++stats.failures;
    Serial.println(isJpeg ? "capture failed: cache alloc" : "capture failed: not jpeg");
    return false;
  }

  ++stats.captures;
  stats.lastAcquireMs = millis() - startedMs;
  stats.lastBytes = frameCache.len;
  Serial.printf("capture %lu: %u bytes acquired=%lums\n",
                static_cast<unsigned long>(stats.captures),
                static_cast<unsigned int>(frameCache.len),
                static_cast<unsigned long>(stats.lastAcquireMs));
  return true;
}

void setupCameraPower() {
  pinMode(CameraPins::kPower, OUTPUT);
  digitalWrite(CameraPins::kPower, LOW);
  delay(500);
}

bool setupCamera() {
  setupCameraPower();

  camera_config_t config = {};
  config.pin_pwdn = CameraPins::kPwdn;
  config.pin_reset = CameraPins::kReset;
  config.pin_xclk = CameraPins::kXclk;
  config.pin_sccb_sda = CameraPins::kSiod;
  config.pin_sccb_scl = CameraPins::kSioc;
  config.pin_d7 = CameraPins::kY9;
  config.pin_d6 = CameraPins::kY8;
  config.pin_d5 = CameraPins::kY7;
  config.pin_d4 = CameraPins::kY6;
  config.pin_d3 = CameraPins::kY5;
  config.pin_d2 = CameraPins::kY4;
  config.pin_d1 = CameraPins::kY3;
  config.pin_d0 = CameraPins::kY2;
  config.pin_vsync = CameraPins::kVsync;
  config.pin_href = CameraPins::kHref;
  config.pin_pclk = CameraPins::kPclk;
  config.xclk_freq_hz = 20000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = configuredFrameSize();
  config.jpeg_quality = CameraSettings::kJpegQuality;
  config.fb_count = 1;
  config.fb_location = ESP.getPsramSize() > 0 ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  lastCameraError = esp_camera_init(&config);
  if (lastCameraError != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x (%s)\n",
                  lastCameraError,
                  esp_err_to_name(lastCameraError));
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_vflip(sensor, 1);
    sensor->set_hmirror(sensor, 1);
  }

  Serial.println("Camera init succeeded");
  return true;
}

bool setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to WiFi SSID: %s\n", WIFI_SSID);
  const uint32_t startedMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedMs < Network::kWifiConnectTimeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi connect failed: status=%d\n", static_cast<int>(WiFi.status()));
    return false;
  }

  Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void sendNoStoreHeaders() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
}

void handleRoot() {
  sendNoStoreHeaders();
  server.send_P(200, "text/html; charset=utf-8", kIndexHtml);
  server.client().stop();
}

void handleStatus() {
  char json[512];
  snprintf(json,
           sizeof(json),
           "{\"camera\":%s,\"cameraError\":\"0x%x\",\"cameraErrorName\":\"%s\",\"wifi\":%s,\"ip\":\"%s\",\"rssi\":%d,\"frameSize\":\"%s\",\"jpegQuality\":%d,\"discardFramesBeforeCapture\":%u,\"captures\":%lu,\"failures\":%lu,\"lastBytes\":%u,\"lastAcquireMs\":%lu,\"lastWriteMs\":%lu,\"lastTotalMs\":%lu,\"heap\":%u,\"psramSize\":%u,\"freePsram\":%u}",
           cameraReady ? "true" : "false",
           static_cast<unsigned int>(lastCameraError),
           esp_err_to_name(lastCameraError),
           wifiReady ? "true" : "false",
           wifiReady ? WiFi.localIP().toString().c_str() : "",
           wifiReady ? WiFi.RSSI() : 0,
           frameSizeName(configuredFrameSize()),
           CameraSettings::kJpegQuality,
           CameraSettings::kDiscardFramesBeforeCapture,
           static_cast<unsigned long>(stats.captures),
           static_cast<unsigned long>(stats.failures),
           static_cast<unsigned int>(stats.lastBytes),
           static_cast<unsigned long>(stats.lastAcquireMs),
           static_cast<unsigned long>(stats.lastWriteMs),
           static_cast<unsigned long>(stats.lastTotalMs),
           static_cast<unsigned int>(ESP.getFreeHeap()),
           static_cast<unsigned int>(ESP.getPsramSize()),
           static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  sendNoStoreHeaders();
  server.send(200, "application/json", json);
  server.client().stop();
}

void handleCapture() {
  if (!cameraReady) {
    server.send(503, "text/plain", "camera is not ready");
    return;
  }

  const uint32_t startedMs = millis();
  if (!frameCache.isFresh(startedMs) && !refreshFrameCache()) {
    server.send(500, "text/plain", "capture failed");
    return;
  }

  WiFiClient client = server.client();
  client.setNoDelay(true);
  sendNoStoreHeaders();
  server.setContentLength(frameCache.len);
  server.send(200, "image/jpeg", "");
  const uint32_t writeStartedMs = millis();
  client.write(frameCache.data, frameCache.len);
  stats.lastWriteMs = millis() - writeStartedMs;
  stats.lastTotalMs = millis() - startedMs;
  client.stop();

  Serial.printf("capture %lu done: write=%lums total=%lums rssi=%d\n",
                static_cast<unsigned long>(stats.captures),
                static_cast<unsigned long>(stats.lastWriteMs),
                static_cast<unsigned long>(stats.lastTotalMs),
                WiFi.RSSI());
}

void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture.jpg", HTTP_GET, handleCapture);
  server.on("/status", HTTP_GET, handleStatus);
  server.onNotFound([]() {
    server.send(404, "text/plain", "not found");
  });
  server.begin();
  Serial.println("HTTP server started");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("CameraWebServerAtomS3RCamera started");
  Serial.printf("PSRAM size: %u bytes, free: %u bytes\n",
                static_cast<unsigned int>(ESP.getPsramSize()),
                static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

  cameraReady = setupCamera();

  wifiReady = setupWifi();

  if (wifiReady) {
    setupServer();
    Serial.printf("Open http://%s/\n", WiFi.localIP().toString().c_str());
  }
}

void loop() {
  if (wifiReady) {
    server.handleClient();
  }

  delay(2);
}
