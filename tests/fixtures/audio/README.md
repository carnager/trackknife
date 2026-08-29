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

## Tagged FLAC fixture

`tagged-tone-flac.b64` is 4,410 mono samples (100 ms) of the same 997 Hz sine
at 44.1 kHz with five Vorbis-comment tags. It was generated and encoded with
FFmpeg n9.0.1 / libavcodec 63.1.101 using `-bitexact` for reproducibility:

```sh
ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=44100:duration=0.1" \
  -c:a pcm_s16le source44.wav
ffmpeg -i source44.wav -metadata title="Fixture Tone" \
  -metadata artist="Trackknife Project" \
  -metadata album="Trackbench Fixtures" -metadata date="2026" \
  -metadata track="3" -c:a flac -bitexact tagged-tone.flac
```

The binary SHA-256 is
`f126f1b8100ad2b80ed1d8fab3e19b6834d2cb7f42c1ec15fc7388f46b04ea41`.
The probe test requires the five tags to survive projection through
`formats::MediaProbe` and the complete decode to publish exactly 4,410
contiguous frames from sample zero with non-silent first and last windows.

## Embedded-artwork FLAC fixture

`art-tone-flac.b64` is the tagged FLAC tone with a 64×64 orange PNG attached
as its cover picture, generated with FFmpeg n9.0.1 / libavcodec 63.1.101:

```sh
ffmpeg -f lavfi -i "color=c=orange:size=64x64:duration=0.04:rate=25" \
  -frames:v 1 cover-orange.png
ffmpeg -i tagged-tone.flac -i cover-orange.png -map 0:a -map 1:v \
  -c:a copy -c:v png -disposition:v:0 attached_pic -bitexact art-tone.flac
```

The binary SHA-256 is
`402d86b052891ee6d76c04357215087b5d457f13983afffab656195bbf4ef611`.
The artwork test requires the attached picture to surface as its exact PNG
bytes and the tagless lookup on `tagged-tone-flac.b64` to report a typed
not-found instead of inventing artwork.

## Tagged WavPack fixture

`tagged-tone-wavpack.b64` is the same 4,410-sample 44.1 kHz mono tone in a
WavPack container with two APEv2 tags, generated with FFmpeg n9.0.1 /
libavcodec 63.1.101:

```sh
ffmpeg -i source44.wav -metadata title="Fixture Tone" \
  -metadata artist="Trackknife Project" -c:a wavpack -bitexact tagged-tone.wv
```

The binary SHA-256 is
`07b9afb934a54ca0113b056c68a8a7f7731b0c11c12e8d65456e4b53ce249073`.
The probe test requires both tags to survive projection and the complete
decode to publish exactly 4,410 contiguous frames from sample zero with
non-silent first and last windows.

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
