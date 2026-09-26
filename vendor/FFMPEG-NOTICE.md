# Bundled FFmpeg

The `avcodec`, `avformat`, `avutil` and `swresample` DLLs shipped beside
the binaries are a minimal shared build of **FFmpeg 9.0.2**, compiled by
`scripts\build-ffmpeg.ps1` with exactly these options:

```
./configure --prefix=/C/Users/Mihir/git/ac3spdif-win/vendor/ffmpeg --enable-shared --disable-static --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-network --disable-autodetect --enable-w32threads --extra-ldflags=-static --disable-everything --enable-encoder=ac3,dca --enable-decoder=ac3,dca --enable-muxer=spdif --enable-demuxer=spdif --enable-parser=ac3,dca --enable-protocol=file
```

FFmpeg is licensed under the **LGPL-2.1-or-later** in this configuration (no
`--enable-gpl`, no `--enable-nonfree`). The ac3spdif binaries link these
DLLs dynamically; they are separate files, and you may replace them with any
ABI-compatible build of the same FFmpeg major versions. This project's MIT
licence does not apply to FFmpeg.

Corresponding source: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz

Ship this notice (and `LGPL-2.1.txt`) with any redistribution of the DLLs.
