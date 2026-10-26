---
status: active
---

# Reimplement this contract on a new upstream HEAD

You are updating a local VoxCPM2 server extension. Read the adjacent `SPEC.md` and
`test_api.py` first. They define the user-approved behavior. Preserve the contract
while adapting the implementation to current upstream source. Inspect repository
instructions and contribution policy before editing. Do not commit, push or
submit a PR unless the human explicitly requests it.
Do not include `TASKS.md` or machine-specific progress notes in the implementation
patch. Keep the reusable requirements and tests in this compatibility folder.

1. Record the upstream URL and exact HEAD. Inspect local changes before choosing
   a fresh `codex/` branch. Never discard unrelated changes. Work from the current
   upstream source; old implementation patches are optional reference material,
   not the specification.
2. Locate the standalone TTS server, its HTTP library, VoxCPM2 generation methods,
   audio loaders, argument handling and relevant tests. Reuse features upstream
   has added; do not duplicate them or blindly restore old implementation files.
3. Implement the SPEC's voice CRUD/storage, nested options, no-reference flags,
   caption translation and model discovery in the smallest sensible server-side
   change. Preserve legacy model IDs/Base64 input, synchronous audio, real WAV
   streaming and cancellation. Use existing dependencies. Keep model inference
   untouched. Do not add runtime concurrency limits, inference locks, queues or
   overload responses: they are explicitly outside this task.
4. Validate added HTTP/file boundaries. Reject invalid IDs, symlink escapes,
   malformed audio and bad fields before storage/inference. Failed uploads must
   not replace existing audio or leave stale transcripts. Keep storage operations
   coherent, without building a database or runtime-specific voice cache.
5. Add focused C++ tests for request precedence and storage behavior, including
   the four flag combinations, explicit empty text, seed bounds, legacy inputs,
   missing voices and persistence. Run the adjacent HTTP tests against the real
   compiled server, including synthesis; use existing GGUF paths supplied by the
   user/environment. Put temporary logs/audio in the OS temporary directory.
6. Verify startup/health, stop/restart persistence and the foreground `$PORT`
   launch needed by Passenger. If Passenger is available, test its launch and
   stop cycle without disturbing other services. Check actual proxy reachability
   separately; a dry-run/status output alone does not prove HTTP success.
7. Review the complete diff. Report branch, build/test results, any exclusions,
   server URL/PID if left running, and sample/log paths. Distinguish audio format
   correctness from subjective clone/style quality. Do not change the external
   contract to make tests pass without explaining and obtaining user direction.

Supply a foreground command of this form, using verified local paths:

```sh
VOXCPM_VOICES_DIR=/absolute/voices \
  /absolute/build/bin/llama-tts-server \
  --host 127.0.0.1 --port "$PORT" \
  --voxcpm2-base-lm /absolute/BaseLM.gguf \
  --voxcpm2-acoustic /absolute/Acoustic.gguf \
  --voxcpm2-n-gpu-layers -1
```

The existing server's no-certificate HTTP issue with OpenSSL enabled is not part
of this extension. For the local HTTP build, use `-DLLAMA_OPENSSL=OFF`.
