# GPT Glasses V1

버튼 한 번으로 주변 장면을 음성으로 설명하는 **XIAO ESP32-S3 Sense 기반 스마트 글라스 프로토타입**입니다. 카메라는 JPEG를 촬영해 같은 Wi-Fi의 FastAPI 서버로 보내고, 서버는 OpenAI Vision으로 짧은 한국어 설명을 만든 뒤 TTS WAV를 보드에 돌려줍니다. 보드는 WAV의 16-bit PCM 오디오를 I²S로 MAX98357A 앰프에 전송합니다.

> **V1의 범위:** 버튼, 카메라, Wi-Fi, 장면 설명, TTS, 스피커까지입니다. 마이크, 웨이크 워드, 음성 질문, OCR 특화 모드, 로그인/인증, 배터리 측정 및 OTA는 아직 포함하지 않습니다.

## 1. 동작 구조

```text
[버튼] -> [XIAO ESP32-S3 Sense: JPEG 촬영]
                      |
                      | HTTP multipart/form-data (같은 LAN)
                      v
              [FastAPI backend]
                      |
                      v
         [OpenAI Vision -> 짧은 한국어 문장]
                      |
                      v
              [OpenAI TTS -> WAV]
                      |
                      | HTTP audio/wav
                      v
[XIAO: WAV 헤더 파싱 -> PCM I2S] -> [MAX98357A] -> [4Ω speaker]
```

MP3 대신 WAV/PCM을 선택했습니다. MP3는 ESP32에서 별도 디코딩이 필요하지만, 이 프로젝트는 WAV의 PCM 바이트를 바로 I²S로 보낼 수 있어 첫 제작과 문제 해결이 단순합니다.

## 2. 필요한 것

* XIAO ESP32-S3 Sense, 데이터 전송 가능한 USB-C 케이블, tactile 버튼.
* MAX98357A I²S mono 앰프 모듈과 4Ω 소형 스피커.
* Python 3.10 이상인 개발 PC와 OpenAI API 키.
* PC와 ESP32가 연결할 **동일한 2.4 GHz Wi-Fi**. 많은 ESP32 보드는 5 GHz 전용 Wi-Fi에 연결할 수 없습니다.
* Arduino IDE와 `esp32 by Espressif Systems` 보드 패키지.

## 3. Backend를 먼저 실행하기

하드웨어보다 서버를 먼저 확인하면 문제를 훨씬 쉽게 분리할 수 있습니다.

### 3-1. 환경 파일 만들기

```bash
cd backend
python -m venv .venv
source .venv/bin/activate       # Windows PowerShell: .venv\Scripts\Activate.ps1
pip install -r requirements.txt
cp .env.example .env            # Windows CMD: copy .env.example .env
```

Windows에서 실행 정책 때문에 가상환경 활성화가 막히면, PowerShell에서 `Set-ExecutionPolicy -Scope Process Bypass`를 실행한 뒤 다시 시도하세요.

`backend/.env`를 열어 실제 키를 넣습니다. **키는 펌웨어에 넣지 말고, `.env` 파일을 Git에 커밋하지 마세요.**

```env
OPENAI_API_KEY=sk-여기에_실제_API_키
VISION_MODEL=gpt-4.1-mini
TTS_MODEL=gpt-4o-mini-tts
TTS_VOICE=alloy
```

### 3-2. 서버 실행 및 PC 테스트

```bash
uvicorn main:app --host 0.0.0.0 --port 8000
```

다른 터미널에서, 서버와 API 키를 검증합니다. `test.jpg`는 실제 JPEG 파일로 바꾸세요.

```bash
curl http://localhost:8000/health
curl -X POST http://localhost:8000/api/analyze -F "image=@test.jpg"
curl -X POST "http://localhost:8000/api/tts?text=안녕하세요" --output test.wav
curl -X POST http://localhost:8000/api/analyze-and-speak -F "image=@test.jpg" --output answer.wav
```

성공 기준은 `/health`의 `{"status":"healthy"}`, `/api/analyze`의 한국어 `text`, 그리고 PC에서 재생되는 `test.wav`/`answer.wav`입니다. 이 단계가 성공하기 전에는 ESP32 문제를 찾지 마세요.

## 4. 배선

Sense 보드의 카메라는 여러 GPIO를 이미 사용합니다. 아래 핀은 카메라 핀과 겹치지 않도록 선택한 외부 버튼/I²S 핀입니다.

| XIAO ESP32-S3 | 연결 대상 | 설명 |
| --- | --- | --- |
| GPIO 1 (D0) | 버튼 한쪽 | 버튼의 다른 쪽은 GND |
| GPIO 2 (D1) | MAX98357A BCLK | I²S bit clock |
| GPIO 4 (D3) | MAX98357A LRC/LRCLK | I²S word-select |
| GPIO 5 (D4) | MAX98357A DIN | ESP32에서 앰프로 가는 오디오 데이터 |
| 5V | MAX98357A VIN | 모듈 실크/사양도 확인 |
| GND | MAX98357A GND | 반드시 공통 접지 |

MAX98357A의 `SPK+`와 `SPK-`에 스피커를 연결합니다. 스피커 단자는 GND에 연결하지 않습니다. 버튼은 `INPUT_PULLUP`으로 읽으므로 평소 HIGH, 누르면 LOW입니다. 앰프 보드의 핀 이름이 `LRC`, `LRCLK`, `DIN`, `SD` 등으로 다를 수 있으므로 구매한 보드의 실크 인쇄를 확인하세요.

## 5. 펌웨어 설정 및 업로드

1. Arduino IDE의 Board Manager에서 **esp32 by Espressif Systems**를 설치합니다.
2. 보드에서 XIAO ESP32-S3 Sense를 선택하고, 필요하면 USB CDC on Boot를 활성화합니다.
3. `firmware/gpt_glasses/gpt_glasses.ino`를 열어 아래 세 값을 바꿉니다.

```cpp
const char* WIFI_SSID = "내_와이파이_이름";
const char* WIFI_PASSWORD = "내_와이파이_비밀번호";
const char* SERVER_URL = "http://192.168.0.15:8000/api/analyze-and-speak";
```

`192.168.0.15`에는 **서버를 실행하는 PC의 LAN IPv4 주소**를 넣어야 합니다. `localhost`나 `127.0.0.1`은 ESP32 자신의 주소이므로 사용하면 안 됩니다. Windows는 `ipconfig`, macOS/Linux는 `ip addr` 또는 `ifconfig`로 PC IP를 확인할 수 있습니다.

4. 스케치를 업로드한 뒤 Serial Monitor를 **115200 baud**로 엽니다.
5. `READY: press the button to capture.`가 보이면 버튼을 누릅니다. 성공하면 JPEG 크기, HTTP 200, WAV sample rate, 재생 로그가 순서대로 보입니다.

## 6. 문제 해결 순서

| 증상 | 먼저 확인할 것 |
| --- | --- |
| Wi-Fi 연결 timeout | SSID/비밀번호, 2.4 GHz Wi-Fi, 공유기 거리 |
| Backend TCP connection failed | PC와 보드가 같은 LAN인지, `SERVER_URL` IP/포트, PC 방화벽에서 Python/8000 허용 여부 |
| HTTP 4xx/5xx | PC의 Uvicorn 로그와 `.env` API 키, JPEG 업로드 크기 |
| Unsupported WAV response | TTS가 `response_format="wav"`인지, backend와 firmware가 같은 커밋인지 |
| 스피커 무음 | BCLK/LRCLK/DIN/GND 배선, MAX98357A 전원, 스피커를 SPK+/SPK-에만 연결했는지 |
| 재생이 끊김 | Wi-Fi RSSI, 전원 품질, 작은 프레임 크기(QVGA)로 테스트 |

## 7. 보안 및 다음 단계

현재 `http://192.168.x.x:8000`은 **신뢰된 로컬 개발망 전용**입니다. 인터넷에 공개하지 마세요. 제품화 전에는 HTTPS 인증서 검증, 장치/사용자 인증, 요청 제한, 비밀 관리, 오류 모니터링을 추가해야 합니다.

V2에서는 PDM 마이크 입력, push-to-talk 또는 wake word, 음성 질문, OCR/번역 모드, Wi-Fi provisioning, 배터리 측정과 OTA 업데이트를 추가하는 순서가 좋습니다.
