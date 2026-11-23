# Video encoding: GPU or CPU

How IYITrailer (`docs/IYITRAILER.md`) and the HR orientation tape choose their H.264 encoder.
Since 2026-11-06, encoding uses a GPU encoder when one works and falls back to the CPU otherwise.

## In short

- `--encoder auto` (the default) tries `nvenc` (NVIDIA), then `amf` (AMD), then `qsv` (Intel). Each one
  gets a 0.2 s test encode, and the first that works is used. If none works, `cpu` (libx264) is used.
- FFmpeg's encoder list shows all of these whether or not the machine has the GPU and driver, so
  the list alone proves nothing. Only the test encode does.
- A forced encoder (`--encoder nvenc`) that does not work is an error. It never falls back silently.
- Override: `--encoder auto|nvenc|amf|qsv|cpu` on `tools/trailer.py` and on HR's `pipeline.py`, or set
  `IYI_ENCODER` for both. `result.json` reports the encoder that was used under `encoder`.

## Where each step runs

| Step | Runs on |
|---|---|
| Drawing the frame (raylib / OpenGL) | GPU |
| Reading the frame back (`rlReadScreenPixels`) | synchronous GPU to CPU copy |
| Pass 1: live lossless capture (`libx264rgb ultrafast -qp 0`) | CPU, on purpose: it is the master |
| Pass 2: final `.mp4` (`src/Trailer.cpp`, config `video_args`) | chosen encoder |
| GIF and `.mpg` (`tools/trailer.py`) | CPU |
| HR tape: normalize (`capture.py`, quality 18; stream copy when already right) | chosen encoder |
| HR tape: picture rendering / final encode (`render_edit.py`) | Chromium/ANGLE frames; external FFmpeg probes NVENC, AMF, QSV; CPU last with an explicit fallback report |
| HR tape: loudnorm remux | CPU |

## How it fits together

The final HR edit renders JPEG frames through Remotion and encodes them with system FFmpeg.
It tests NVIDIA NVENC, AMD AMF and Intel QSV; working hardware is preferred, with CPU as the final
fallback. `reports/encoders.json` records probe failures; each picture receipt records the actual
encoder and attempts. This avoids Remotion's own vendor limitations. Explicit encoder overrides
are honored. Picture/audio caches are separate: sound-only edits stream-copy the existing picture.
A checked 10-second preview and a processed-speech quality gate precede the full picture render.

- `tools/trailer.py` `ENCODERS` maps each name to its FFmpeg arguments at one CRF-like quality number
  (lower means better). `pick_encoder(ffmpeg, choice)` returns `(name, error)` and caches the result
  for the process.
- `trailer.py render` passes `ENCODERS[name](18, "slow")` to the engine as `video_args`. The engine's
  `TrailerSettings::VideoArgs` defaults to libx264 CRF 18 preset slow, which applies when a game
  calls `engine.trailer.start` without `trailer.py`.
- HR: `common.encoder()` resolves the choice. `capture.py` passes it to `trailer.py` and uses it for
  normalize. `build_index.scene_inputs` adds a GPU encoder's name to the capture hash, so footage
  made with a different encoder is recaptured. `cpu` is left out of the hash, so clips captured
  before this change stay current (a `shortcut:` comment in the code marks this).
- To add an encoder, add one `ENCODERS` entry. `auto` tries the entries in order, except `cpu`.

## Things to know

- At the same quality number, encoders differ in file size and look. A hardware encoder needs more
  bits than libx264 slow for the same picture. market_spike (9 s 1080p25, quality 18): 2.4 MB CPU,
  3.18 MB AMF.
- AMF settings (measured 2026-11-06 with VMAF against a CRF 12 copy). Flat CQP 18 gave 6.70 MB at VMAF
  96.68. CQP with `-bf 3`, I and P at q and B at q+4, gave 5.19 MB at VMAF 96.62, so about 23%
  smaller for the same quality. That is the setting now. libx264 CRF 18 gave 3.13 MB at VMAF 96.76.
  QVBR was worse at every level tried (for example level 44: 3.61 MB at VMAF 93.5), and preanalysis
  with PAQ/TAQ changed nothing under CQP. GPUs without H.264 B-frames (AMD before RDNA3) should get a
  warning from FFmpeg and encode without them; this is not tested.
- The GPU encoder speeds up only the encode that runs after the shot. The take itself runs in real
  time. Measured on the Ryzen 7 8845HS / Radeon 780M machine: a 9 s 1080p encode took 1.6 s on AMF
  and 2.1 s on libx264, and the whole render took 19 s with either.
- HR `capture --jobs 4` runs four encodes at once. AMF and current NVIDIA drivers allow that many
  sessions.
- Tested: `amf` and `cpu` on the 780M machine; capture and final Remotion `nvenc` on the RTX 4070
  machine. The final render log confirms `h264_nvenc`, with stream copy during the final mux. `qsv` is untested.
- Remotion now selects ANGLE. Alternative Chrome GL backends and asynchronous (PBO) capture readback
  have not been benchmarked.
