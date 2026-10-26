#!/usr/bin/env python3
"""HTTP contract and optional real-audio tests for a running llama-tts-server."""

import argparse
import array
import base64
import hashlib
import io
import json
import math
import struct
import tempfile
import unittest
import urllib.error
import urllib.request
import uuid
import wave
from pathlib import Path


def multipart(audio, filename="sample.wav", **fields):
    boundary = "tts-" + uuid.uuid4().hex
    chunks = []
    for key, value in fields.items():
        chunks.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{key}"\r\n\r\n{value}\r\n'.encode())
    chunks.append(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{filename}"\r\n'
                  'Content-Type: audio/wav\r\n\r\n'.encode())
    chunks.extend([audio, f"\r\n--{boundary}--\r\n".encode()])
    return b"".join(chunks), f"multipart/form-data; boundary={boundary}"


def fixture_wav():
    out = io.BytesIO()
    with wave.open(out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(b"\0\0" * 1600)
    return out.getvalue()


class TTSCompatibility(unittest.TestCase):
    @classmethod
    def http(cls, method, path, data=None, content_type="application/json"):
        if isinstance(data, dict):
            data = json.dumps(data).encode()
        request = urllib.request.Request(cls.options.url.rstrip("/") + path, data=data, method=method,
                                         headers={"Content-Type": content_type})
        try:
            response = urllib.request.urlopen(request, timeout=240)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, response.headers, response.read()

    @classmethod
    def upload(cls, method, path, audio=None, filename="sample.wav", **fields):
        body, content_type = multipart(cls.fixture if audio is None else audio, filename, **fields)
        return cls.http(method, path, body, content_type)

    @classmethod
    def setUpClass(cls):
        cls.fixture = fixture_wav()
        cls.output = Path(cls.options.output_dir or tempfile.mkdtemp(prefix="tts-compat-audio-"))
        cls.output.mkdir(parents=True, exist_ok=True)
        cls.reference_text = "Hello, welcome to VoxCPM2!"
        if cls.options.reference_wav:
            cls.reference = Path(cls.options.reference_wav).read_bytes()
            cls.reference_text = cls.options.reference_text or cls.reference_text
        elif not cls.options.skip_synthesis:
            status, _, cls.reference = cls.http("POST", "/v1/audio/speech", {
                "model": "voxcpm2", "voice": "none", "input": cls.reference_text,
                "voxcpm": {"seed": 12345, "inference_timesteps": 10, "max_steps": 80},
            })
            if status != 200:
                raise RuntimeError(f"Reference synthesis failed: {status} {cls.reference!r}")
            cls.check_audio(cls.reference)
            (cls.output / "reference.wav").write_bytes(cls.reference)

    @staticmethod
    def check_audio(data, streaming=False):
        if streaming:
            data = bytearray(data)
            struct.pack_into("<I", data, 4, len(data) - 8)
            struct.pack_into("<I", data, 40, len(data) - 44)
        with wave.open(io.BytesIO(data), "rb") as wav:
            assert wav.getnchannels() == 1
            assert wav.getsampwidth() == 2
            assert wav.getframerate() == 48000
            pcm = wav.readframes(wav.getnframes())
            assert len(pcm) > 1000
            assert any(pcm)
        return bytes(data)

    def setUp(self):
        self.voice_id = "compat_" + uuid.uuid4().hex[:12]
        self.path = "/v1/audio/voices/" + self.voice_id

    def tearDown(self):
        self.http("DELETE", self.path)

    def register(self, reference_text="Hello!"):
        status, _, body = self.upload("POST", "/v1/audio/voices", voice_id=self.voice_id,
                                      reference_text=reference_text)
        self.assertEqual(status, 201, body)
        return json.loads(body)

    def assert_error(self, expected, response):
        status, headers, body = response
        self.assertEqual(status, expected, body)
        self.assertIn("application/json", headers.get("Content-Type", ""))
        self.assertIn("message", json.loads(body)["error"])

    def test_health_and_models(self):
        for path in ("/health", "/v1/health"):
            status, _, body = self.http("GET", path)
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(body)["status"], "ok")
        status, _, body = self.http("GET", "/v1/models")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"object": "list", "data": [
            {"id": "voxcpm2", "object": "model", "created": 0, "owned_by": "voxcpm2"}]})
        self.assertEqual(self.http("GET", "/v1/audio/speech/models")[0], 200)

    def test_crud(self):
        created = self.register()
        self.assertEqual(created["object"], "voice_file")
        self.assertEqual(created["filename"], self.voice_id + ".wav")
        self.assertEqual(created["bytes"], len(self.fixture))
        self.assertGreater(created["created_at"], 0)
        status, _, body = self.http("GET", "/v1/audio/voices")
        self.assertEqual(status, 200)
        voices = {voice["id"]: voice for voice in json.loads(body)["data"]}
        self.assertEqual(voices[self.voice_id]["object"], "voice")
        self.assertEqual(voices[self.voice_id]["ref_wav"], self.voice_id + ".wav")
        self.assertTrue(voices["none"]["no_ref"])
        self.assertIsNone(voices["none"]["ref_wav"])
        self.assertEqual(self.http("GET", self.path)[0], 200)
        self.assert_error(409, self.upload("POST", "/v1/audio/voices", voice_id=self.voice_id))
        self.assertEqual(self.upload("PUT", self.path, reference_text="Replacement")[0], 200)
        if self.options.voices_dir:
            txt = Path(self.options.voices_dir) / (self.voice_id + ".wav.txt")
            self.assertEqual(txt.read_text(), "Replacement")
        self.assertEqual(self.upload("PUT", self.path)[0], 200)
        if self.options.voices_dir:
            self.assertFalse(txt.exists())
        self.assertEqual(self.upload("PUT", self.path, reference_text="Again")[0], 200)
        status, _, body = self.http("DELETE", self.path)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"id": self.voice_id, "object": "voice_file", "deleted": True})
        if self.options.voices_dir:
            self.assertFalse(txt.exists())
        self.assert_error(404, self.http("GET", self.path))
        self.assert_error(404, self.http("DELETE", self.path))
        self.assert_error(404, self.upload("PUT", self.path))

    def test_filename_default(self):
        status, _, body = self.upload("POST", "/v1/audio/voices", filename=self.voice_id + ".WAV")
        self.assertEqual(status, 201, body)
        self.assertEqual(json.loads(body)["id"], self.voice_id)

    def test_reject_invalid_uploads_without_replacing_voice(self):
        self.register()
        for audio in (b"", b"not a wav", self.fixture[:-5]):
            self.assert_error(400, self.upload("PUT", self.path, audio=audio))
        for offset, value, fmt in ((34, 0, "<H"), (34, 32, "<H"), (40, 0xFFFFFFFF, "<I"),
                                   (16, 0xFFFFFFFF, "<I"), (22, 0, "<H"), (24, 0, "<I")):
            broken = bytearray(self.fixture)
            struct.pack_into(fmt, broken, offset, value)
            self.assert_error(400, self.upload("PUT", self.path, audio=bytes(broken)))
        self.assert_error(400, self.upload("POST", "/v1/audio/voices", filename="sample.mp3", voice_id=self.voice_id))
        self.assert_error(400, self.upload("POST", "/v1/audio/voices", voice_id="../escape"))
        self.assert_error(400, self.upload("POST", "/v1/audio/voices", voice_id="none"))
        self.assert_error(400, self.http("POST", "/v1/audio/voices", {}))
        self.assertEqual(json.loads(self.http("GET", self.path)[2])["bytes"], len(self.fixture))
        if self.options.voices_dir:
            self.assertEqual((Path(self.options.voices_dir) / (self.voice_id + ".wav.txt")).read_text(), "Hello!")

    def test_reject_bad_speech_requests(self):
        self.assert_error(400, self.http("POST", "/v1/audio/speech", b"{"))
        for values in ({"voxcpm": None}, {"voxcpm": {"seed": "bad"}},
                       {"voxcpm": {"seed": 4294967296}}, {"voxcpm": {"no_reference_text": "true"}},
                       {"model": "other"}, {"response_format": "mp3"}, {"voice": "../escape"},
                       {"voxcpm": {"reference_text": "orphan"}},
                       {"reference_audio": "broken"},
                       {"reference_audio": "base64", "voxcpm": {"reference_audio": "ref_123"}}):
            for route in ("/v1/audio/speech", "/v1/audio/speech/stream"):
                self.assert_error(400, self.http("POST", route, {"input": "Test.", "voice": "none", **values}))
        self.assert_error(404, self.http("POST", "/v1/audio/speech", {"input": "Test.", "voice": self.voice_id}))

    def synthesize(self, name, voice=None, voxcpm=None, streaming=False, **fields):
        body = {"model": "voxcpm2", "voice": voice or self.voice_id,
                "input": "こんにちは。音声合成のテストです。", "response_format": "wav",
                "voxcpm": {"seed": 12345, "cfg_value": 2.0, "inference_timesteps": 10,
                           "max_steps": 80, **(voxcpm or {})}, **fields}
        route = "/v1/audio/speech/stream" if streaming else "/v1/audio/speech"
        status, headers, data = self.http("POST", route, body)
        self.assertEqual(status, 200, data[:500])
        if body["response_format"] == "pcm" and not streaming:
            self.assertEqual(headers.get("Content-Type"), "audio/pcm")
            self.assertEqual(len(data) % 4, 0)
            self.assertGreater(len(data), 1000)
            samples = array.array("f", data)
            self.assertTrue(all(math.isfinite(sample) for sample in samples))
            self.assertTrue(any(samples))
            (self.output / (name + ".pcm")).write_bytes(data)
        else:
            self.assertIn("audio/wav", headers.get("Content-Type", ""))
            data = self.check_audio(data, streaming)
            (self.output / (name + ".wav")).write_bytes(data)
        return data

    def test_synthesis(self):
        if self.options.skip_synthesis:
            self.skipTest("--skip-synthesis")
        self.upload("POST", "/v1/audio/voices", audio=self.reference,
                    voice_id=self.voice_id, reference_text=self.reference_text)
        continuation = self.synthesize("continuation")
        explicit = self.synthesize("explicit-reference", voice="none", voxcpm={
            "reference_audio": self.voice_id, "reference_text": self.reference_text})
        self.assertEqual(continuation, explicit)
        clone = self.synthesize("clone", voxcpm={"no_reference_text": True})
        empty = self.synthesize("empty-transcript", voxcpm={"reference_text": ""})
        self.assertEqual(clone, empty)
        legacy = self.synthesize("legacy-clone", voice="default", model="voxcpm",
                                 reference_audio=base64.b64encode(self.reference).decode(),
                                 voxcpm={"no_reference_text": True})
        self.assertEqual(clone, legacy)
        self.assertNotEqual(continuation, clone)
        caption = "A young woman, gentle and sweet voice"
        no_ref = self.synthesize("caption-no-reference", voxcpm={"no_reference_audio": True, "caption": caption})
        none = self.synthesize("caption-none", voice="none", voxcpm={"caption": caption})
        self.assertEqual(no_ref, none)
        self.synthesize("caption-continuation", voxcpm={"caption": caption})
        streamed = self.synthesize("stream-clone", streaming=True, voxcpm={"no_reference_text": True})
        self.assertGreater(len(streamed), 1000)
        self.synthesize("pcm-no-reference", voice="none", response_format="pcm")
        manifest = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in self.output.iterdir() if path.suffix in (".wav", ".pcm")}
        (self.output / "sha256.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--voices-dir", help="Optional local directory for checking saved sidecars")
    parser.add_argument("--output-dir", help="Generated audio directory; defaults to a temporary directory")
    parser.add_argument("--reference-wav", help="Use an existing reference instead of synthesizing one")
    parser.add_argument("--reference-text", help="Transcript matching --reference-wav")
    parser.add_argument("--skip-synthesis", action="store_true", help="Run only HTTP management/error tests")
    TTSCompatibility.options = parser.parse_args()
    unittest.main(argv=[__file__], verbosity=2)
