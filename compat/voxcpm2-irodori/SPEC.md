---
status: active
---

# VoxCPM2 Irodori-compatible API contract

This is a local extension to the standalone `llama-tts-server`. It reuses upstream
VoxCPM2 inference and remains a single application process. The source baseline
for the first implementation is `a64932ec615e1463158129feaf2f0752d0c07a2e`.

## Scope

Implement voice selection/storage, Irodori-shaped voice management, nested VoxCPM
options and model discovery. Preserve existing synchronous and WAV streaming
inference. Do not change runtime concurrency, inference locking, queues, overload
responses or the model implementation. HTTP tests must issue synthesis requests
sequentially.

Irodori LoRA, latent files, multi-reference voices, speaker inversion, aliases in
`voices.json`, SSE and automatic text chunking are excluded. This is API usability
compatibility for WAV voices, not complete Irodori feature parity.

## Configuration and storage

`VOXCPM_VOICES_DIR` chooses the voices directory; default: `voices` relative to the
working directory. Scan when resolving/listing voices, so manually added files
and transcripts work without restarting. Do not create the directory on reads.

Store a voice as `<voice_id>.wav` and its optional transcript as
`<voice_id>.wav.txt`. WAV input is RIFF/WAVE, 16-bit integer PCM; stereo/multichannel
input is averaged to mono by the existing loading path. Reject malformed/truncated
WAVs before storage or inference. File data/chunk sizes, sample rate, channel count,
alignment and bit depth must be checked before reading samples.

IDs contain only ASCII letters, digits, underscores and hyphens. Empty IDs and
reserved no-reference IDs cannot be uploaded. Do not follow symbolic links or
accept directories as voice files or sidecars. Files remain usable after restart;
do not store runtime-specific latent caches. Voice storage may synchronize its own
file operations; this is separate from the explicitly excluded runtime locking.

## Speech endpoints

`POST /v1/audio/speech` accepts:

```json
{
  "model": "voxcpm2",
  "voice": "ref_392",
  "input": "こんにちは。音声合成のテストです。",
  "response_format": "wav",
  "voxcpm": {
    "seed": 12345,
    "cfg_value": 2.0,
    "inference_timesteps": 10,
    "caption": "A young woman, gentle and sweet voice",
    "no_reference_text": false,
    "no_reference_audio": false
  }
}
```

- Keep model IDs `voxcpm2` and `voxcpm`; absent/empty model retains upstream behavior.
- `input` is a nonempty string.
- Nested `voxcpm` must be an object. Nested `seed`, `cfg_value`,
  `inference_timesteps`, `max_steps`, `temperature` override their legacy top-level
  equivalents. Defaults remain upstream's (42, 2.0, 10, 200, 1.0 respectively).
- Seed is an integer in `[0, 4294967295]`; retain upstream's seed-zero behavior.
  Timesteps and max_steps are positive integers representable as `int`.
  CFG is nonnegative and temperature positive; both must be finite float values.
- `voxcpm.caption` is an optional string. Pass `"(" + caption + ")" + input` to
  upstream synthesis when nonempty, without changing the transcript.
- `voice` accepts a string or `{ "id": "ref_392" }`. Missing/empty voice and the
  reserved IDs `default`, `none`, `no_ref`, `no-ref`, `null`, `text-only` select no
  reference. Keep `default` working for existing clients.
- `voxcpm.reference_audio` is an optional registered voice ID and overrides `voice`.
  Legacy top-level `reference_audio` remains Base64 WAV and overrides `voice`.
  Two nonempty explicit audio sources together are HTTP 400.
- `voxcpm.reference_text` overrides the saved transcript for this request only.
  Explicit empty string suppresses the saved transcript. Existing top-level
  `prompt_text`, `reference_text`, `ref_text` remain supported, in that order,
  before the stored transcript. Strip outer whitespace from saved transcripts.
- `voxcpm.no_reference_audio` and `voxcpm.no_reference_text` are booleans, default
  false. These flags take precedence over reference selection. The former skips
  all audio and transcript resolution, including missing IDs. The latter ignores
  all explicit and saved transcripts. Neither disables caption. There is no
  `voxcpm.no_ref` option.
- A nonempty, enabled transcript without resolved reference audio is HTTP 400.
- Reference WAV + transcript uses existing continuation inference; WAV alone
  uses existing clone inference; neither uses existing ordinary inference.
- Do not promise that caption + continuation sounds identical to Irodori or to
  the Python VoxCPM implementation. Validate it by generating sample audio.

Output remains upstream's: mono 48 kHz PCM16 WAV, or headerless float32 PCM for
`response_format: "pcm"`, with `audio/wav` or `audio/pcm` respectively. No compressed
formats or resampling features are added.

Apply the same reference/options resolution to existing
`POST /v1/audio/speech/stream`. Keep its chunked PCM16 WAV response with the
upstream unknown-size WAV header; do not replace it with Irodori SSE. Validate
reference input before starting the response. Existing disconnection cancellation
must continue to work.

## Voice management

Match the local Irodori server's paths, metadata and status codes for supported
WAV files. `reference_text` is our optional extension to multipart registration.

| Method | Path | Response |
| --- | --- | --- |
| GET | `/v1/audio/voices` | 200 list of resolved voices, including `none` |
| POST | `/v1/audio/voices` | 201 new voice-file metadata |
| GET | `/v1/audio/voices/{voice_id}` | 200 voice-file metadata, not audio bytes |
| PUT | `/v1/audio/voices/{voice_id}` | 200 replaced voice-file metadata |
| DELETE | `/v1/audio/voices/{voice_id}` | 200 deletion acknowledgement |

POST takes multipart `file` (required), `voice_id` (optional; defaults to filename
stem), `reference_text` (optional). PUT takes `file` and optional `reference_text`;
its ID comes from the URL. Accept `.wav` case-insensitively on upload, normalize
the saved extension to `.wav`. POST duplicate IDs are 409. Missing IDs on GET,
PUT or DELETE are 404. Bad IDs, unsupported formats, bad WAVs, invalid JSON/field
types and missing multipart file are 400. Errors use an OpenAI-shaped object:

```json
{"error":{"message":"...","type":"invalid_request_error"}}
```

Metadata:

```json
{"id":"ref_392","object":"voice_file","filename":"ref_392.wav","bytes":12345,"created_at":0}
```

`created_at` is the file's last-modification time in Unix seconds, matching Irodori.
POST/PUT persist nonempty UTF-8 `reference_text`. PUT with missing or empty text
removes the old sidecar. DELETE removes both files. Validate before replacing any
existing file; failed validation must retain the previous pair. Stage uploads and
retain the previous pair for rollback on ordinary I/O errors. A WAV+TXT pair is
not a crash-atomic database transaction.

Voice list shape (sort by ID):

```json
{"object":"list","data":[{"id":"ref_392","object":"voice","ref_wav":"ref_392.wav","ref_wavs":null,"ref_latent":null,"ref_latents":null,"ref_embed":null,"no_ref":false}]}
```

The `none` entry has null reference fields and `no_ref: true`.
Return only the filename in `ref_wav`, regardless of whether the configured voices
directory is relative or absolute. Never expose the server's directory in this
field. This intentionally differs from Irodori's configured-path representation.

## Discovery and health

Add `GET /v1/models`:

```json
{"object":"list","data":[{"id":"voxcpm2","object":"model","created":0,"owned_by":"voxcpm2"}]}
```

Preserve `/v1/audio/speech/models`, `/v1/voxcpm2/init`, `/health`, `/v1/health`.
Reuse the existing health body; Irodori's engine-specific health fields are not
required. Do not change authentication or add a separate web server.

## Validation and operation

Build `llama-tts-server` using Metal and `LLAMA_OPENSSL=OFF` for local HTTP. Reuse
existing GGUF paths; do not copy or reconvert models. Start foreground, bind to
loopback for direct tests, and pass Passenger's `$PORT` as `--port` when managed.
Use `exec` in a shell launcher, with no `nohup`, background wrapper or CLI child
per inference. Check health after cold start and verify graceful stop/restart.

The pure C++ storage/resolver tests run with:

```sh
cmake --build build-voxcpm2 --target test-tts-voices
ctest --test-dir build-voxcpm2 -R '^test-tts-voices$' --output-on-failure
```

`test_api.py` is a standalone Python standard-library HTTP contract test. It
creates and deletes unique `compat_*` voices; synthesis requests are sequential.
It can synthesize its own reference or accept an existing WAV and transcript.
Use `--skip-synthesis` for management/error checks only. Generated samples go to
`--output-dir`, or an OS temporary directory by default. Streaming samples saved
by the test have their WAV length fields repaired for playback.

Keep this folder outside the clean upstream checkout when regenerating changes.
Preserve this specification, the prompt and HTTP tests across upstream updates;
only regenerate implementation glue and adapt internal unit tests as necessary.
