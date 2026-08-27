"""HTTP backend for the GPT Glasses V1 prototype."""

import base64
import os

from dotenv import load_dotenv
from fastapi import FastAPI, File, HTTPException, UploadFile
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import Response
from openai import OpenAI

load_dotenv()

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")
VISION_MODEL = os.getenv("VISION_MODEL", "gpt-4.1-mini")
TTS_MODEL = os.getenv("TTS_MODEL", "gpt-4o-mini-tts")
TTS_VOICE = os.getenv("TTS_VOICE", "alloy")
MAX_IMAGE_SIZE = 5 * 1024 * 1024
ALLOWED_IMAGE_TYPES = {"image/jpeg", "image/jpg", "image/png", "image/webp"}

if not OPENAI_API_KEY:
    raise RuntimeError("OPENAI_API_KEY is not set. Copy .env.example to .env first.")

client = OpenAI(api_key=OPENAI_API_KEY)
app = FastAPI(title="GPT Glass API", version="1.0.0")

VISION_PROMPT = """
너는 스마트 글라스의 AI 시각 비서다. 현재 카메라가 바라보는 장면을 분석해라.

반드시 다음 규칙을 지켜라.
- 한국어로 답한다.
- 음성으로 들었을 때 자연스럽게, 1~3문장으로 짧게 답한다.
- 중요한 정보부터 말한다.
- 확실하지 않은 내용은 추측하지 않는다.
- 보이지 않는 것은 보인다고 말하지 않는다.
- 장면의 핵심적인 물체와 상황을 우선 설명한다.
""".strip()


@app.get("/")
async def root() -> dict[str, str]:
    return {"status": "ok", "service": "GPT Glass"}


@app.get("/health")
async def health() -> dict[str, str]:
    return {"status": "healthy"}


async def read_image(image: UploadFile) -> bytes:
    """Validate and read an uploaded camera image."""
    if image.content_type not in ALLOWED_IMAGE_TYPES:
        raise HTTPException(status_code=400, detail="Unsupported image type.")

    # Read one byte beyond the limit so an oversized upload is rejected without
    # first allocating its whole body in application memory.
    data = await image.read(MAX_IMAGE_SIZE + 1)
    if not data:
        raise HTTPException(status_code=400, detail="Empty image.")
    if len(data) > MAX_IMAGE_SIZE:
        raise HTTPException(status_code=413, detail="Image too large (maximum 5 MB).")
    if not looks_like_image(data, image.content_type):
        raise HTTPException(status_code=400, detail="Image data does not match its content type.")
    return data


def looks_like_image(data: bytes, content_type: str) -> bool:
    """Perform a cheap signature check before forwarding bytes to the model."""
    signatures = {
        "image/jpeg": (b"\xff\xd8\xff",),
        "image/jpg": (b"\xff\xd8\xff",),
        "image/png": (b"\x89PNG\r\n\x1a\n",),
        "image/webp": (b"RIFF",),
    }
    if not data.startswith(signatures[content_type]):
        return False
    # A RIFF container must identify itself as WebP at byte offset 8.
    return content_type != "image/webp" or len(data) >= 12 and data[8:12] == b"WEBP"


def analyze_image(image_bytes: bytes, content_type: str) -> str:
    """Return a short Korean description generated from an image."""
    encoded = base64.b64encode(image_bytes).decode("utf-8")
    response = client.responses.create(
        model=VISION_MODEL,
        input=[
            {
                "role": "user",
                "content": [
                    {"type": "input_text", "text": VISION_PROMPT},
                    {
                        "type": "input_image",
                        "image_url": f"data:{content_type};base64,{encoded}",
                        "detail": "low",
                    },
                ],
            }
        ],
    )
    answer = response.output_text.strip()
    if not answer:
        raise RuntimeError("The vision model returned an empty response.")
    return answer


def text_to_speech(text: str) -> bytes:
    """Generate a complete WAV response, including its RIFF header."""
    response = client.audio.speech.create(
        model=TTS_MODEL,
        voice=TTS_VOICE,
        input=text,
        response_format="wav",
        speed=1.0,
    )
    audio = response.read()
    if not audio:
        raise RuntimeError("The TTS model returned an empty response.")
    return audio


@app.post("/api/analyze")
async def analyze(image: UploadFile = File(...)) -> dict[str, object]:
    image_bytes = await read_image(image)
    answer = await run_in_threadpool(
        analyze_image, image_bytes, image.content_type or "image/jpeg"
    )
    return {"success": True, "text": answer}


@app.post("/api/analyze-and-speak")
async def analyze_and_speak(image: UploadFile = File(...)) -> Response:
    image_bytes = await read_image(image)
    answer = await run_in_threadpool(
        analyze_image, image_bytes, image.content_type or "image/jpeg"
    )
    print(f"[GPT] {answer}")
    audio = await run_in_threadpool(text_to_speech, answer)

    # Response provides Content-Length, avoiding chunked transfer on the ESP32 V1 client.
    return Response(
        content=audio,
        media_type="audio/wav",
        headers={
            "Content-Disposition": 'inline; filename="answer.wav"',
        },
    )


@app.post("/api/tts")
async def tts(text: str) -> Response:
    text = text.strip()
    if not text:
        raise HTTPException(status_code=400, detail="Empty text.")
    if len(text) > 4096:
        raise HTTPException(status_code=400, detail="Text too long.")
    audio = await run_in_threadpool(text_to_speech, text)
    return Response(content=audio, media_type="audio/wav")
