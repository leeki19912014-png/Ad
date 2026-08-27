# GPT Glasses V1

XIAO ESP32-S3 Sense의 버튼을 누르면 JPEG를 FastAPI 서버에 보내고, 서버가 장면을 한국어로 설명한 뒤 WAV 음성을 돌려주는 프로토타입입니다.

## 구성

```text
XIAO ESP32-S3 Sense -> FastAPI -> OpenAI Vision + TTS -> WAV/PCM I2S -> MAX98357A
```

첫 구현은 MP3 대신 16-bit PCM WAV를 사용합니다. ESP32가 MP3 디코더 없이 WAV의 PCM 데이터를 MAX98357A로 전송할 수 있기 때문입니다.

## Backend 시작

```bash
cd backend
python -m venv .venv
source .venv/bin/activate  # Windows: .venv\\Scripts\\activate
pip install -r requirements.txt
cp .env.example .env
# .env에 OPENAI_API_KEY 설정
uvicorn main:app --host 0.0.0.0 --port 8000
```

개발 PC에서 다음을 순서대로 확인합니다.

```bash
curl http://localhost:8000/health
curl -X POST http://localhost:8000/api/analyze -F "image=@test.jpg"
curl -X POST "http://localhost:8000/api/tts?text=안녕하세요" --output test.wav
curl -X POST http://localhost:8000/api/analyze-and-speak -F "image=@test.jpg" --output answer.wav
```

## 배선

카메라가 사용하지 않는 핀을 사용합니다.

| XIAO ESP32-S3 | 연결 대상 |
| --- | --- |
| GPIO 1 (D0) | 버튼의 한쪽; 다른 쪽은 GND |
| GPIO 2 (D1) | MAX98357A BCLK |
| GPIO 4 (D3) | MAX98357A LRC/LRCLK |
| GPIO 5 (D4) | MAX98357A DIN |
| 5V | MAX98357A VIN |
| GND | MAX98357A GND |

MAX98357A의 `SPK+`와 `SPK-`에는 4Ω 스피커를 연결합니다. 버튼은 펌웨어에서 `INPUT_PULLUP`으로 읽으므로 누르면 LOW입니다.

## 펌웨어 업로드

Arduino IDE에서 **esp32 by Espressif Systems**를 설치하고 XIAO ESP32-S3 Sense 보드를 선택합니다. `firmware/gpt_glasses/gpt_glasses.ino`에서 Wi-Fi 정보와 `SERVER_URL`을 바꿉니다. 서버 URL에는 `localhost`가 아닌 개발 PC의 같은 LAN IP를 넣어야 합니다.

```cpp
const char* SERVER_URL = "http://192.168.0.15:8000/api/analyze-and-speak";
```

개발용 HTTP는 같은 신뢰된 LAN에서만 사용하세요. 외부에 배포할 때는 HTTPS, 인증, 서버 측 비밀 관리가 필요합니다.
