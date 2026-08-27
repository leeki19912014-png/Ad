#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <ESP_I2S.h>

// Set these before uploading. SERVER_URL must be your PC's LAN address, never localhost.
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SERVER_URL = "http://192.168.0.15:8000/api/analyze-and-speak";

static constexpr int BUTTON_PIN = 1;
static constexpr int I2S_BCLK_PIN = 2;
static constexpr int I2S_LRCLK_PIN = 4;
static constexpr int I2S_DOUT_PIN = 5;
static constexpr uint32_t WIFI_TIMEOUT_MS = 20000;

// XIAO ESP32-S3 Sense camera pins.
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 10
#define SIOD_GPIO_NUM 40
#define SIOC_GPIO_NUM 39
#define Y9_GPIO_NUM 48
#define Y8_GPIO_NUM 11
#define Y7_GPIO_NUM 12
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 16
#define Y4_GPIO_NUM 18
#define Y3_GPIO_NUM 17
#define Y2_GPIO_NUM 15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM 47
#define PCLK_GPIO_NUM 13

struct WavInfo {
  uint16_t audioFormat;
  uint16_t channels;
  uint32_t sampleRate;
  uint16_t bitsPerSample;
  uint32_t dataSize;
};

I2SClass I2S;

bool connectWiFi();
bool initCamera();
bool initAudio();
void captureAndSend();
bool parseUrl(const String& url, String& host, uint16_t& port, String& path);
bool readResponseHeaders(WiFiClient& client, uint32_t& contentLength);
bool parseWavHeader(WiFiClient& client, WavInfo& wav);
bool playWavStream(WiFiClient& client, const WavInfo& wav);
bool readExact(WiFiClient& client, uint8_t* buffer, size_t length);
uint16_t readLE16(WiFiClient& client);
uint32_t readLE32(WiFiClient& client);
bool skipBytes(WiFiClient& client, uint32_t count);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nGPT GLASSES V1");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if (!initCamera() || !initAudio() || !connectWiFi()) {
    Serial.println("Initialization failed; restart after fixing configuration.");
    while (true) delay(1000);
  }
  Serial.println("READY: press the button to capture.");
}

void loop() {
  static bool previous = HIGH;
  const bool current = digitalRead(BUTTON_PIN);
  if (previous == HIGH && current == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      captureAndSend();
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
  }
  previous = current;
  delay(5);
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to %s", WIFI_SSID);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - started > WIFI_TIMEOUT_MS) {
      Serial.println(" timeout");
      return false;
    }
    Serial.print('.');
    delay(500);
  }
  Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  config.jpeg_quality = psramFound() ? 12 : 15;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    Serial.printf("Camera init error: 0x%x\n", result);
    return false;
  }
  return true;
}

bool initAudio() {
  I2S.setPins(I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DOUT_PIN, -1, -1);
  return I2S.begin(I2S_MODE_STD, 24000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
}

void captureAndSend() {
  if (WiFi.status() != WL_CONNECTED && !connectWiFi()) return;
  camera_fb_t* frame = esp_camera_fb_get();
  if (frame == nullptr) {
    Serial.println("Camera capture failed.");
    return;
  }
  Serial.printf("Captured %u JPEG bytes.\n", frame->len);

  String host, path;
  uint16_t port;
  if (!parseUrl(SERVER_URL, host, port, path)) {
    Serial.println("SERVER_URL must be an http:// URL.");
    esp_camera_fb_return(frame);
    return;
  }

  WiFiClient client;
  if (!client.connect(host.c_str(), port)) {
    Serial.println("Backend TCP connection failed.");
    esp_camera_fb_return(frame);
    return;
  }

  const String boundary = "----GPTGlassBoundary";
  const String partHeader = "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n";
  const String partFooter = "\r\n--" + boundary + "--\r\n";
  const size_t bodyLength = partHeader.length() + frame->len + partFooter.length();

  client.printf("POST %s HTTP/1.1\r\nHost: %s\r\n", path.c_str(), host.c_str());
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client.printf("Content-Length: %u\r\nAccept: audio/wav\r\nConnection: close\r\n\r\n", bodyLength);
  client.print(partHeader);
  for (size_t sent = 0; sent < frame->len;) {
    const size_t chunk = min(static_cast<size_t>(4096), frame->len - sent);
    const size_t written = client.write(frame->buf + sent, chunk);
    if (written == 0) {
      Serial.println("JPEG upload failed.");
      client.stop();
      esp_camera_fb_return(frame);
      return;
    }
    sent += written;
  }
  esp_camera_fb_return(frame);
  client.print(partFooter);

  uint32_t responseLength = 0;
  if (!readResponseHeaders(client, responseLength)) {
    Serial.println("Invalid or unsuccessful backend response.");
    client.stop();
    return;
  }
  WavInfo wav = {};
  if (!parseWavHeader(client, wav)) {
    Serial.println("Unsupported WAV response.");
    client.stop();
    return;
  }
  if (wav.dataSize > responseLength) {
    Serial.println("WAV data length exceeds the HTTP response length.");
    client.stop();
    return;
  }
  Serial.printf("Playing %u Hz, %u channel(s), %u bytes.\n", wav.sampleRate, wav.channels, wav.dataSize);
  if (!playWavStream(client, wav)) Serial.println("Audio playback failed.");
  client.stop();
}

bool parseUrl(const String& url, String& host, uint16_t& port, String& path) {
  if (!url.startsWith("http://")) return false;
  String authorityAndPath = url.substring(7);
  const int slash = authorityAndPath.indexOf('/');
  String authority = slash < 0 ? authorityAndPath : authorityAndPath.substring(0, slash);
  path = slash < 0 ? "/" : authorityAndPath.substring(slash);
  const int colon = authority.indexOf(':');
  host = colon < 0 ? authority : authority.substring(0, colon);
  port = colon < 0 ? 80 : authority.substring(colon + 1).toInt();
  return !host.isEmpty() && port != 0;
}

bool readResponseHeaders(WiFiClient& client, uint32_t& contentLength) {
  client.setTimeout(30000);
  const String status = client.readStringUntil('\n');
  Serial.printf("HTTP: %s", status.c_str());
  if (!status.startsWith("HTTP/1.1 200") && !status.startsWith("HTTP/1.0 200")) return false;

  bool chunked = false;
  contentLength = 0;
  while (true) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLength = line.substring(line.indexOf(':') + 1).toInt();
    if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) chunked = true;
  }
  // Backend uses FastAPI Response, which produces a fixed Content-Length.
  return !chunked && contentLength > 0;
}

bool parseWavHeader(WiFiClient& client, WavInfo& wav) {
  char id[4];
  if (!readExact(client, reinterpret_cast<uint8_t*>(id), 4) || memcmp(id, "RIFF", 4) != 0) return false;
  (void)readLE32(client);
  if (!readExact(client, reinterpret_cast<uint8_t*>(id), 4) || memcmp(id, "WAVE", 4) != 0) return false;

  bool hasFormat = false;
  while (readExact(client, reinterpret_cast<uint8_t*>(id), 4)) {
    const uint32_t size = readLE32(client);
    if (memcmp(id, "fmt ", 4) == 0) {
      if (size < 16) return false;
      wav.audioFormat = readLE16(client);
      wav.channels = readLE16(client);
      wav.sampleRate = readLE32(client);
      (void)readLE32(client);
      (void)readLE16(client);
      wav.bitsPerSample = readLE16(client);
      if (!skipBytes(client, size - 16)) return false;
      hasFormat = true;
    } else if (memcmp(id, "data", 4) == 0) {
      wav.dataSize = size;
      return hasFormat && wav.audioFormat == 1 && wav.bitsPerSample == 16 && (wav.channels == 1 || wav.channels == 2);
    } else if (!skipBytes(client, size)) {
      return false;
    }
    if ((size & 1U) && !skipBytes(client, 1)) return false;
  }
  return false;
}

bool playWavStream(WiFiClient& client, const WavInfo& wav) {
  I2S.end();
  const i2s_slot_mode_t mode = wav.channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
  if (!I2S.begin(I2S_MODE_STD, wav.sampleRate, I2S_DATA_BIT_WIDTH_16BIT, mode)) return false;

  uint8_t buffer[4096];
  uint32_t remaining = wav.dataSize;
  uint32_t lastActivity = millis();
  while (remaining > 0) {
    const size_t available = client.available();
    if (available == 0) {
      if (!client.connected() || millis() - lastActivity > 15000) return false;
      delay(2);
      continue;
    }
    const size_t requested = min(min(available, sizeof(buffer)), static_cast<size_t>(remaining));
    const size_t received = client.readBytes(buffer, requested);
    if (received == 0) continue;
    if (I2S.write(buffer, received) != received) return false;
    remaining -= received;
    lastActivity = millis();
  }
  delay(100);  // Let the final DMA buffer drain to the amplifier.
  return true;
}

bool readExact(WiFiClient& client, uint8_t* buffer, size_t length) {
  size_t offset = 0;
  uint32_t lastActivity = millis();
  while (offset < length) {
    if (client.available()) {
      const int value = client.read();
      if (value >= 0) {
        buffer[offset++] = static_cast<uint8_t>(value);
        lastActivity = millis();
      }
    } else {
      if (!client.connected() || millis() - lastActivity > 10000) return false;
      delay(1);
    }
  }
  return true;
}

uint16_t readLE16(WiFiClient& client) {
  uint8_t bytes[2];
  return readExact(client, bytes, sizeof(bytes)) ? bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8) : 0;
}

uint32_t readLE32(WiFiClient& client) {
  uint8_t bytes[4];
  return readExact(client, bytes, sizeof(bytes))
      ? bytes[0] | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24)
      : 0;
}

bool skipBytes(WiFiClient& client, uint32_t count) {
  uint8_t scratch[64];
  while (count > 0) {
    const size_t step = min(static_cast<uint32_t>(sizeof(scratch)), count);
    if (!readExact(client, scratch, step)) return false;
    count -= step;
  }
  return true;
}
