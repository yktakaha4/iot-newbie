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
constexpr int kJpegQuality = 15;
}  // namespace CameraSettings

namespace Network {
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint16_t kHttpPort = 80;
}  // namespace Network

WebServer server(Network::kHttpPort);

bool cameraReady = false;
bool wifiReady = false;
esp_err_t lastCameraError = ESP_OK;
uint32_t captureCount = 0;
uint32_t captureFailCount = 0;
size_t lastCaptureBytes = 0;

const char* frameSizeName(framesize_t frameSize) {
  switch (frameSize) {
    case FRAMESIZE_QQVGA:
      return "QQVGA";
    case FRAMESIZE_QCIF:
      return "QCIF";
    case FRAMESIZE_HQVGA:
      return "HQVGA";
    case FRAMESIZE_240X240:
      return "240X240";
    case FRAMESIZE_QVGA:
      return "QVGA";
    case FRAMESIZE_CIF:
      return "CIF";
    case FRAMESIZE_HVGA:
      return "HVGA";
    case FRAMESIZE_VGA:
      return "VGA";
    case FRAMESIZE_SVGA:
      return "SVGA";
    case FRAMESIZE_XGA:
      return "XGA";
    case FRAMESIZE_HD:
      return "HD";
    case FRAMESIZE_SXGA:
      return "SXGA";
    case FRAMESIZE_UXGA:
      return "UXGA";
    case FRAMESIZE_FHD:
      return "FHD";
    case FRAMESIZE_P_HD:
      return "P_HD";
    case FRAMESIZE_P_3MP:
      return "P_3MP";
    case FRAMESIZE_QXGA:
      return "QXGA";
    case FRAMESIZE_QHD:
      return "QHD";
    case FRAMESIZE_WQXGA:
      return "WQXGA";
    case FRAMESIZE_P_FHD:
      return "P_FHD";
    case FRAMESIZE_QSXGA:
      return "QSXGA";
    default:
      return "UNKNOWN";
  }
}

framesize_t configuredFrameSize() {
  return ESP.getPsramSize() > 0 ? CameraSettings::kFrameSizeWithPsram
                                : CameraSettings::kFrameSizeWithoutPsram;
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
  config.fb_count = ESP.getPsramSize() > 0 ? 2 : 1;
  config.fb_location = ESP.getPsramSize() > 0 ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = ESP.getPsramSize() > 0 ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

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

void handleRoot() {
  String html;
  html.reserve(900);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>AtomS3R Camera</title>");
  html += F("<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:20px;background:#111;color:#eee}");
  html += F("img{max-width:100%;height:auto;border:1px solid #444}a{color:#8cf}button{font-size:16px;padding:8px 12px}</style>");
  html += F("</head><body><h1>AtomS3R Camera</h1>");
  html += F("<p><button onclick='reloadImage()'>Capture</button> <a href='/capture.jpg'>Open JPEG</a> <a href='/status'>Status</a></p>");
  html += F("<img id='capture' src='/capture.jpg' alt='capture'>");
  html += F("<script>function reloadImage(){document.getElementById('capture').src='/capture.jpg?t='+Date.now();}</script>");
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleStatus() {
  char json[384];
  snprintf(json,
           sizeof(json),
           "{\"camera\":%s,\"cameraError\":\"0x%x\",\"cameraErrorName\":\"%s\",\"wifi\":%s,\"ip\":\"%s\",\"frameSize\":\"%s\",\"jpegQuality\":%d,\"captures\":%lu,\"failures\":%lu,\"lastBytes\":%u,\"heap\":%u,\"psramSize\":%u,\"freePsram\":%u}",
           cameraReady ? "true" : "false",
           static_cast<unsigned int>(lastCameraError),
           esp_err_to_name(lastCameraError),
           wifiReady ? "true" : "false",
           wifiReady ? WiFi.localIP().toString().c_str() : "",
           frameSizeName(configuredFrameSize()),
           CameraSettings::kJpegQuality,
           static_cast<unsigned long>(captureCount),
           static_cast<unsigned long>(captureFailCount),
           static_cast<unsigned int>(lastCaptureBytes),
           static_cast<unsigned int>(ESP.getFreeHeap()),
           static_cast<unsigned int>(ESP.getPsramSize()),
           static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  server.send(200, "application/json", json);
}

void handleCapture() {
  if (!cameraReady) {
    server.send(503, "text/plain", "camera is not ready");
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    ++captureFailCount;
    server.send(500, "text/plain", "capture failed");
    return;
  }

  if (fb->format != PIXFORMAT_JPEG) {
    ++captureFailCount;
    esp_camera_fb_return(fb);
    server.send(500, "text/plain", "captured frame is not jpeg");
    return;
  }

  ++captureCount;
  lastCaptureBytes = fb->len;
  Serial.printf("capture %lu: %u bytes\n",
                static_cast<unsigned long>(captureCount),
                static_cast<unsigned int>(fb->len));

  WiFiClient client = server.client();
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  client.write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
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
