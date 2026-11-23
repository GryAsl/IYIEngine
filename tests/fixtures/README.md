# Test fixtures

Small media files for `tests/engine_tests.cpp` (`TestVideo`). Regenerate with FFmpeg:

```bat
ffmpeg -y -f lavfi -i testsrc=size=64x48:rate=25:duration=1 -f lavfi -i sine=frequency=440:duration=1:sample_rate=44100 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 64k -ac 2 -f mpeg video_sound.mpg
ffmpeg -y -f lavfi -i testsrc=size=64x48:rate=25:duration=1 -an -c:v mpeg1video -q:v 4 -f mpeg video_silent.mpg
```

`video_sound.mpg`: 1 s, 64x48, 25 fps MPEG-1 with MP2 stereo audio. `video_silent.mpg`: the same without audio.
