# Audio fixture provenance

These Base64 files materialize real encoded audio containers during the formats
test. They are text-encoded only so repository tooling can review and reproduce
the exact binary inputs; the test never invokes an `ffmpeg` subprocess.

## Gapless tone set

The source is exactly 4,800 mono samples (100 ms) of a 997 Hz sine at 48 kHz.
It was generated and encoded with FFmpeg n9.0.1 / libavcodec 63.1.101:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=0.1" \
  -c:a pcm_s16le source.wav
ffmpeg -i source.wav -c:a aac -b:a 128k tone.m4a
ffmpeg -i source.wav -c:a libmp3lame -b:a 128k tone.mp3
ffmpeg -i source.wav -c:a libopus -b:a 96k tone.opus
```

| Encoded fixture | Binary SHA-256 | Relevant delay/padding metadata |
| --- | --- | --- |
| `gapless-tone-aac-m4a.b64` | `9f73d9159487333ba3acd16d12cdf614e9a4951a732cd40f1417c4710aaeef1a` | MP4 edit list; 1,024 initial AAC samples skipped |
| `gapless-tone-mp3.b64` | `096dcc043a0b1cbb10d2d67086c9b399b84b62e72d041aab555a7ab919c0b4a4` | LAME/Xing delay and padding; 1,105 initial and 1,007 final samples skipped |
| `gapless-tone-opus.b64` | `e69f0e640b80074e5774cc4bb17fa89aa2e8300da560df214a0bdf9cbf2a2aa7` | Opus pre-skip of 312 samples plus final granule trim |

Each fixture independently decodes to exactly 4,800 frames with FFmpeg's
documented packet skip-sample semantics. The Trackknife test additionally
requires a non-silent first and last window, preventing encoder priming or
trailing padding from passing merely because it happens to contain zeros.

## Vorbis timeline fixture

`vorbis-positive-start.b64` is 4,410 mono samples (100 ms) of the same 997 Hz
sine at 44.1 kHz. It was encoded by vorbis-tools `oggenc` 1.4.3 with a fixed Ogg
serial number:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=44100:duration=0.1" \
  -c:a pcm_s16le source.wav
oggenc -s 12345 -q 4 -o vorbis-positive-start.ogg source.wav
```

The binary SHA-256 is
`fb683e56206f5f0f66bb3199a72630e5d1195de325870eaba1df004c52b5ac28`.
FFmpeg exposes its first decoded frame at timestamp 128 even though the logical
audition starts at sample zero; other valid Vorbis encoders can also expose
overlapping or gapped best-effort frame timestamps. The regression requires the
4,282 decodable PCM frames to be published on one contiguous logical timeline
starting at zero.
