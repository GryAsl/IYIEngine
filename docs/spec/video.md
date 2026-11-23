# IYI Engine contract: Video (`engine.video`)

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 23. Video playback (`engine.video`)

Plays an MPEG-1 video with optional MP2 audio in an MPEG-PS file (`.mpg`) through pl_mpeg
(`third_party/pl_mpeg/pl_mpeg.h`, MIT, commit c871f2b): one C++ implementation (`include/IYI/Video.h`,
`src/Video.cpp`), the same on Windows, Linux, macOS and the browser, no runtime libraries to ship.
MPEG-1 was chosen over H.264 (2026-11-04): no codec DLLs, licence or patent review, and no separate
browser player. Convert a finished film offline with the FFmpeg the trailer tool already uses:
`ffmpeg -i film.mp4 -vf scale=in_color_matrix=bt709:out_color_matrix=bt601 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -ar 48000 -f mpeg film.mpg`.
pl_mpeg converts YUV with BT.601; an HD (BT.709) master without that `-vf` shows clipped greens and pale reds.

```python
v = engine.video.open(rel_path) -> engine.video.Video   # never raises: state "error" + error says why
v.play()          # from the start (ready, ended) or resume (paused)
v.pause(); v.resume()
v.stop()          # rewind: state "ready"
v.close()         # frees decoder, texture, stream now; safe to repeat; every later call does nothing
v.set_volume(0..1)                  # times the "master" and "music" buses
v.draw_fit(x, y, width, height)     # black over the area, the frame aspect-fit in its middle
image.video = v                     # or show it in an IYIUI image (None: off)
v.state           # "ready" | "playing" | "paused" | "ended" | "error" | "closed"
v.duration, v.position, v.width, v.height, v.has_audio, v.error
```

- **Sides (§8a).** `open` runs on the game side and reads the headers, so sizes and duration are known
  at once. A loose file (development, `ResourceStore::GetPhysicalPath`) streams from disk through
  pl_mpeg's own small buffer, as loose music does (§20.1), opened with every share mode: an open video
  never blocks replacing, renaming or deleting its file (a re-export while the game runs). A packed entry
  is read whole through the ResourceStore, as packed music is: iyipack encrypts and authenticates each
  file as one chunk, so no byte can be used before all of it is read (streaming one would need chunked
  encryption in iyipack; worth it only for a large shipped video). A video that fails
  to open holds no file or buffer. `duration` is the container's estimate (the last timestamp): it
  can be short by up to one GOP (~0.5 s); `ended` is exact.
- **In the UI.** `image.video = v` (IYIUI image, `SPEC-IYIUI.md`) shows the current frame in a UI
  image with its tint, opacity, `preserve_aspect` and `source_rect`, so a video is a normal UI element
  (under a button, in a panel). Nothing draws before the first frame.
  It refuses a missing file, a file that is not MPEG-PS with an MPEG-1 video stream, a frame larger
  than 3840x2160 and a zero duration. The other calls record their render-side work and show their
  state at once; the render side ticks every started video once per frame after the AudioManager.
- **Decoding.** A worker thread decodes ahead into bounded queues (3 RGBA frames, 0.25 s of audio)
  and converts YUV to RGBA on the CPU; the browser build has no threads, so the tick decodes inline
  there. One texture per video (bilinear), updated on the render side with the frame that is due;
  late frames are dropped. The video only advances while ticked, never while paused.
- **Clock and sound.** A wall clock from play, frozen while paused. Audio goes to a raylib audio
  stream (~32 ms sub-buffers) fed in step with it; an empty queue feeds silence. `ended` comes once
  the last frame and sample are out plus 0.1 s for the stream's buffer. With no audio device the
  video plays silently.
- **Lifetime.** A `Video` is shared with the commands that draw it and deleted on the render side
  (`RenderOwned`); `close()` or the last reference frees it. Hot reload does not touch it.
