# ReplayGain and loudness

## Application scope

Per ADR-0025, ReplayGain analysis, tag/sidecar storage, and local playback
gain application belong to Trackbench, the standalone local-file workstation
(milestone M7). Trackknife, the MPD/Melody client, does none of this itself:
it only exposes MPD's advertised `replay_gain_mode`/`replay_gain_status`
control, and the server owns the actual gain application. Everything below
describes Trackbench unless stated otherwise.

## Product contract

Trackbench must analyze ReplayGain for every format it can decode. Scanning and
storing are separate capabilities:

- **Analysis support** means the decoder can deliver the complete PCM signal
  with correct channels, sample rate, gapless boundaries, and segment limits.
- **Embedded persistence support** means a safe, interoperable format-specific
  mapping exists and the adapter can round-trip it.
- **Fallback persistence support** means results can be stored in a Trackbench
  sidecar and cached in the library when embedding is impossible or disabled.

A normal scan writes metadata only. It never changes encoded audio samples.
Permanently applying gain is a separate, conspicuously named conversion or
codec-global-gain operation.

## Loudness record

Keep measurement data typed rather than reparsing display strings:

```text
LoudnessRecord
  track_gain_db: optional float
  track_sample_peak: optional float       # 1.0 is digital full scale
  track_true_peak_dbtp: optional float
  album_gain_db: optional float
  album_sample_peak: optional float
  album_true_peak_dbtp: optional float
  integrated_loudness_lufs: optional float
  loudness_range_lu: optional float
  algorithm: bs1770 revision | classic_replaygain version
  target_loudness_lufs/reference: value
  peak_mode: sample | true_peak + oversampling details
  channel/layout policy
  scanner/version
  scanned_at
  album_group_id and grouping expression
  source_revision/fingerprint
  persistence provenance
```

Classic interoperable ReplayGain storage normally exposes four values:
`REPLAYGAIN_TRACK_GAIN`, `REPLAYGAIN_TRACK_PEAK`, `REPLAYGAIN_ALBUM_GAIN`, and
`REPLAYGAIN_ALBUM_PEAK`. Gains are signed dB adjustments. Traditional peaks are
linear amplitude ratios where values above `1.0` can occur. Extra true-peak,
LUFS, range, algorithm, and provenance data should be stored only through
documented extensions/sidecars without breaking other players.

## Analysis algorithms

### Default: BS.1770 family at ReplayGain reference

foobar2000's modern scanner uses ITU-R BS.1770/EBU-R128-style analysis but a
ReplayGain-compatible target of `-18 LUFS`, rather than broadcast R128's
`-23 LUFS`. Trackbench should use a current, validated BS.1770 implementation,
state its revision, and default to `-18 LUFS` for interoperable ReplayGain gains.

Conceptually:

```text
gain_db = target_loudness_lufs - measured_integrated_loudness_lufs
```

Do not implement the gated loudness algorithm from a brief summary. Use a
well-tested standards implementation and validate it against known vectors.
Channel weighting, K-weighting filters, absolute/relative gating, and block
overlap are correctness-critical.

### Compatibility option: Classic ReplayGain

foobar2000 exposes the older Classic ReplayGain analysis as an option. It may be
implemented after the modern scanner but must be labeled with its algorithm and
never mixed into an album record with BS.1770 results.

### High sample rates

foobar2000 can downsample material above 48 kHz before loudness analysis to keep
inaudible ultrasonic energy from influencing measurement. Trackbench should
provide a documented automatic policy and record the resampler/rate used. The
loudness result must be independent of the audio output device configuration.

### Peaks and true peaks

Sample peak is the maximum decoded sample magnitude. True peak estimates
inter-sample excursions by oversampling according to the chosen standard/tool.
foobar2000 exposes automatic 2x/4x/8x-like choices or a configured resampler.

Trackbench should calculate sample peak cheaply on every scan and optionally
calculate standards-compliant true peak. The UI must not label a sample peak as
true peak. For lossy codecs, decoder variation can affect peak estimates; record
decoder provenance where reproducibility matters.

## Track and album modes

Expose these operations clearly:

- **Scan tracks**: calculate/write track gain and peak for each logical track.
- **Scan selection as one album**: calculate all track values plus one album
  loudness/peak shared by the entire selected group.
- **Scan as albums by tags**: evaluate a title-format grouping expression for
  every item, partition by the exact result, then calculate track and group
  values.
- **Remove ReplayGain data**: remove the intended embedded and/or sidecar fields
  after preview.

The foobar2000 default album grouping pattern is:

```text
%album artist% | %date% | %album%
```

Trackbench should offer a better visible default that also deals with
multi-disc releases, but preserve the exact classic pattern as a preset and
accept existing patterns unchanged. The preview must show groups before analysis begins so
missing tags do not accidentally combine unrelated albums.

Album loudness is calculated over the album programme according to the selected
algorithm, not by averaging track gains. Album peak is at least the maximum peak
across its tracks. Preserve track results when writing album results.

Logical cue tracks are scanned only over their exact sample ranges. Album mode
must avoid redundant full-file decode where a decoder can stream shared source
segments correctly.

## Parallel scan engine

The scanner is a job graph, not one UI-thread loop:

```text
probe/group
  -> bounded decode tasks per physical source
  -> per-logical-track loudness + peak accumulators
  -> album reducers after group members finish
  -> result review
  -> bounded metadata/sidecar writers
  -> verification + library refresh
```

Requirements:

- Default worker count adapts to logical CPUs, storage behavior, and decoder
  safety; user can cap it.
- One physical source is not decoded repeatedly for each cue subsong when a
  single ordered pass can feed segment accumulators.
- Network/removable sources use separate conservative I/O limits.
- Decoder instances are isolated unless their library explicitly supports
  sharing.
- Progress is based on decoded samples/duration where known, with indeterminate
  state otherwise.
- Cancellation stops new work, asks decoders to stop, and retains completed
  analysis in the result set without writing unless the user chooses it.
- Decode errors are per item; an album with missing/failed members is marked
  incomplete and album tags are not silently written.
- The UI remains responsive and displays throughput, worker utilization, ETA,
  completed/failed/skipped counts, and current sources.
- Analysis results are tied to a source revision so a changed file cannot receive
  stale values.

Decode and tag-write phases should be separate by default. This enables review,
prevents many tiny concurrent rewrites on slow media, and makes “quiet mode” an
explicit preset rather than the only workflow.

**Trackbench decision (ADR-0054):** ReplayGain appears as an independent
checkable operation in the shared preparation workspace once the M7 scanner and
storage paths are qualified. It remains typed cancellable analysis rather than
a transformation-script action. Album/group boundaries are shown before
decoding; revision-bound results then join metadata and source-to-target path
effects in one final per-file preview. Its explicit embedded/sidecar persistence
is not disabled merely because ordinary **Save tags** is unchecked.

## Persistence policy

### Canonical versus cached data

Effective loudness may come from embedded tags, a Trackbench sidecar, the
library database, or an unavailable track's playlist snapshot. Each value has
provenance and a source revision.

Proposed precedence:

1. current explicit sidecar override when configured;
2. current embedded/container values;
3. current library-only result;
4. playlist snapshot only as an offline fallback.

A scan result should be written to the selected canonical target and then cached
in the common `TrackRef`. A playlist may serialize the cached values for fast
offline display, as classic FPL did, but that snapshot must not override a newer
canonical scan.

### Known container families

This table is a research starting point, not proof that the first release can
write every row:

| Format/container | Typical foobar/interoperable storage |
| --- | --- |
| FLAC / Ogg FLAC / Vorbis | Vorbis-comment `REPLAYGAIN_*` fields. |
| MP3/MP2 | ID3v2 user text frames or optional APEv2 policy. |
| WavPack / Monkey's Audio / TAK | APEv2 fields. |
| Musepack | Native header/tag mechanism depending format revision. |
| MP4/M4A (AAC/ALAC/etc.) | MP4 freeform/iTunes-style metadata. |
| Raw AAC | Often APEv2 in foobar; interoperability is limited, so sidecar may be safer by default. |
| WMA/ASF | Native ASF/WMA attributes. |
| WAV/RF64/AIFF | ID3 chunk in foobar-style workflows; preservation and interoperability require strict tests. |
| Matroska/WebM | Container tags. |
| Opus | Special case: Opus header output gain and RFC 7845 `R128_*` comments interact; follow the RFC and test player interoperability. |
| Untaggable/decoder-only | Trackbench sidecar + library cache. |

Do not copy gain fields blindly between formats, especially Opus. RFC 7845 says
`R128_TRACK_GAIN`/`R128_ALBUM_GAIN` are Q7.8 dB values relative to the Opus
header output gain and must be applied in addition to it; changing output gain
requires updating/removing the comments. Its normalization reference differs
from naive ReplayGain-tag assumptions. Implement one explicit Opus policy after
interoperability tests.

For all formats, metadata writes preserve audio essence and unrelated data.
Sidecar fallback must be offered when an embedded mapping is risky, lossy, or
not supported by the adapter.

## Playback application

This section covers Trackbench's local player only. For MPD playback the
server applies gain itself; Trackknife merely reads and sets the advertised
`replay_gain_mode` and never processes audio.

Expose source modes equivalent to:

- `none`;
- `track`;
- `album`;
- `automatic/by playback order` (album for coherent album playback, track for
  shuffled or unrelated sequences, with the exact Trackbench rule documented).

Expose processing modes:

- no ReplayGain processing;
- apply gain;
- apply gain and reduce it as needed to prevent clipping using peak metadata;
- prevent clipping from configured preamp even without normalization gain.

Let `g` be selected ReplayGain plus the relevant preamp. Predicted linear peak:

```text
predicted_peak = stored_linear_peak * 10^(g / 20)
```

For peak-based clipping prevention, reduce `g` so predicted peak does not exceed
the configured ceiling. If peak is absent, say so; do not claim protection.
True-peak-based protection should use true peak when available and expose which
peak type was selected.

Separate preamps for tracks with and without loudness data are useful. Apply
gain in floating point before later DSP/output conversion, and make limiter/
clipping behavior explicit. A clean bypass path must not alter samples.

Album mode depends on correct grouping/continuity. Queue jumps, crossfades,
shuffle, and mixed albums need a deterministic documented rule rather than a
hidden heuristic.

## Conversion and permanent gain

The converter may:

- apply selected track/album gain permanently to decoded PCM before encoding;
- apply a DSP chain after/before gain according to an explicit fixed order;
- transfer original ReplayGain only when audio loudness is unchanged;
- discard stale input values when PCM is modified;
- rescan all outputs, grouping them as albums, after conversion.

Any gain baked into output invalidates original gain/peak metadata. The safest
default is to scan the actual encoded output, especially for lossy encoders.

Codec-global-gain modifications (for example Opus output gain or reversible
MP3/AAC mechanisms) are expert operations separate from ordinary scanning and
must update dependent tags consistently.

## Result and review UI

Show one row per logical track with path/subsong, group, measured loudness,
suggested gain, sample peak, optional true peak, status, target persistence, and
warnings. Album values appear once per group but remain attributable to all
members.

Users can:

- sort/filter failures and untaggable formats;
- change embedded versus sidecar target where supported;
- write all valid results or selected rows;
- export results;
- retry failed scans;
- inspect algorithm/provenance;
- cancel tag writing safely.

## Validation

- Use standards/test vectors for integrated loudness and true peak.
- Compare against a named foobar2000 version for common mono, stereo, multichannel,
  high-rate, short, silent, and cue-segment material.
- Test albums with unequal track lengths; album gain must not be a mean of track
  gains.
- Test NaN, infinity, silence, corrupt/truncated decode, unknown channel layout,
  and changing files.
- Round-trip tags through each real container and inspect them in at least one
  independent tool/player.
- Assert metadata-only scans do not change decoded audio essence.
- Benchmark multicore scaling on SSD, HDD, and network storage without allowing
  unbounded I/O thrashing.
