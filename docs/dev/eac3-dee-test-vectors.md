# DEE E-AC-3 test vectors

This note records the local Dolby Encoding Engine (DEE) vectors used for the
Gate 8 audio work. The vectors are differential/coverage samples only; they
are not a normative Dolby conformance set and are not a runtime dependency.

## Environment and source

- DEE: `5.2.1-5994839` (`E:\Tool\dolby_encoding_engine\dee.exe`).
- Source: `media\POWDER SNOW Live V9.8.6.wav`.
- Source format: Logic Pro ADM BWF, 48 kHz, 24-bit, 18 interleaved channels:
  10 bed channels plus 8 object channels.
- All generated media, logs, and the generated XSD are local-only under
  `media\gate8-dee-vectors\`. They must not be added to Git.

The XML entry points are DEE's `encode_to_atmos_ddp` filter with an `<adm>`
audio input and an `<ec3>` output for Atmos vectors, and the local
`media\gate8-dee-vectors\adm_pcm_to_ddp_ec3.xml` configuration for ordinary
5.1 vectors. A representative CLI shape (license options intentionally
omitted) is:

```text
dee.exe --xml <job.xml> --input-audio <ADM.wav> --output <out.ec3> \
  --add-elem data_rate=<kbps> --add-elem start=00:01:00.00000 \
  --add-elem end=00:01:05.00000 --temp <local-temp> --log-file <log>
```

## Atmos vectors

Each vector is a five-second ADM-to-Atmos E-AC-3 encode. The probe was run on
the first 100 access units with `--emdf --joc --joc-math-self-test`.

| File | Rate | Size | SHA-256 | EMDF 11 / 14 | Config | Object count |
|---|---:|---:|---|---:|---:|---:|
| `adm_joc_5s_384k.ec3` | 384 kbps | 241,152 | `3783A4E1C870303CC61377450DBBE166B10BC0964748B2AEDE0E18F9D9D3C4C8` | 100 / 100 | 3 | 11 |
| `adm_joc_5s_448k.ec3` | 448 kbps | 281,344 | `C18DDB8F7BB5B97E6589ACE33AAAA6C1FE2B8B22AC90AE99DB9E7FD5DC83E0AB` | 100 / 100 | 3 | 15 |
| `adm_joc_5s_576k.ec3` | 576 kbps | 361,728 | `C3FA144D628F1ECA02FE6FEA593B074AFD5E809D96BBB66832313A476E637AE0` | 100 / 100 | 3 | 15 |
| `adm_joc_5s_640k.ec3` | 640 kbps | 401,920 | `7A159C30AACA6DC6A1C342D2208840E1878D3BE25C4F5AFB0E4AA009238032A9` | 100 / 100 | 3 | 15 |
| `adm_joc_5s_768k.ec3` | 768 kbps | 482,304 | `311765C1C315ECC2F71BE214BDF2968B04C867AF1FC019510017DD7185DA714E` | 100 / 100 | 3 | 15 |
| `adm_joc_5s_1024k.ec3` | 1024 kbps | 643,072 | `6A15211C603A825BCA95BA89E1C0183AFFD25135EA9ACC22E9ED1EED4098851A` | 100 / 100 | 3 | 15 |

For all six vectors, the first 100 AUs reported `independentFrames=100`,
`dependentFrames=0`, EMDF payloads `1:100,2:100,11:100,14:100`, and
`jocDownmixConfigs=3:100`. The bounded JOC syntax and coefficient math stages
both passed; the current probe reports the complete phase as unsupported rather
than malformed. `dependentFrames=0` is not a no-JOC result: the JOC/EMDF payload
is present in the independent substream for these files.

## DEE Media Encoder Blu-ray-profile EB3 vectors

The DEE Media Encoder generated the following local Blu-ray-profile EB3
coverage files. The rate is the nominal encoded rate; size and SHA-256 are
recorded so the ignored media can be revalidated without entering Git.

| File | Nominal rate | Size (bytes) | SHA-256 |
|---|---:|---:|---|
| `eb3/powder_active_5s_1152k.eb3` | 1,152 kbps | 43,350,000 | `5F72E8E38911F9B3BB61614A260F9A438526F08A7165DFBE61BAF6A159A53CC7` |
| `eb3/powder_active_5s_1280k.eb3` | 1,280 kbps | 48,150,000 | `3D9FE5C2F9A9CF522D9CE2780815FA0E9A4AEAA130950D4F3917A207CE101F1A` |
| `eb3/powder_active_5s_1408k.eb3` | 1,408 kbps | 52,950,000 | `DFBF1D6A9F750C68053C82678B36D751ADE663B276AB483A454E86F8A98AD0C4` |
| `eb3/powder_active_5s_1512k.eb3` | 1,512 kbps | 56,850,000 | `22A857B213B84C5E34B365BFE1BE02C9C2D8CB06E3AEF087A31CC8BC81B70A04` |
| `eb3/powder_active_5s_1536k.eb3` | 1,536 kbps | 57,750,000 | `F7A0B32887252E185B49C69B59A3A13BDF369597C2FEB3B4B103995B2153D239` |
| `eb3/powder_active_5s_1664k.eb3` | 1,664 kbps | 62,550,000 | `C90B6F3C5B2D2DB02237D1608E7B7C8E34A66535B74851C1A881F342DCFBFC98` |

Although the filenames contain `5s`, each file contains 9,375 access units:
`9375 * 1536 / 48000 = 300` seconds. Every access unit has one 16-byte
wrapper, one legacy AC-3 frame, and one dependent E-AC-3 frame: 18,750 total
frames and 9,375 wrappers. In the first 100 access units there are 200 frames,
100 AUs, and 100 dependent frames; BSI `chanmap=0xA010`, EMDF payload types 11
and 14 each occur 100 times, and the stream reports config 4 with 15 objects.
The JOC syntax and coefficient-math checks report `total=100, covered=100`;
semantic phase reports `total=100, covered=0` with structured `unsupported`
for all inspected AUs. That counter belongs to the base `--joc` phase only;
the existing Gate 6C/7 diagnostic path already handles config 4 when those
later gates are requested. It is not evidence that config 4 metadata is an
unresolved blocker for the established FFmpeg-backed differential chain.

The first-AU compressed byte sizes in rate order are 4,608 / 5,120 / 5,632 /
6,048 / 6,144 / 6,656. Wrapper bytes 0..3 are fixed at `01 10 00 01`, bytes
4..11 are dynamic carriage metadata kept opaque by the parser, and bytes
12..15 are fixed at `00 08 80 00`. The legacy raw EB3 first-100 regression
still passes with wrapper count 0. These media remain ignored local assets;
runtime DRC remains disabled (`off`).

## Ordinary 5.1 vectors

These files use the ADM input through `pcm_to_ddp` and are channel-based 5.1,
not Atmos/JOC. The XML has both `line_mode_drc_profile` and
`rf_mode_drc_profile` set to `film_light`. The runtime decoder policy for this
project records the DRC metadata but keeps DRC application disabled (`off`),
so these samples must not be judged by applying DEE's film-light gain curve.

| File | Rate | Size | SHA-256 |
|---|---:|---:|---|
| `adm_pcm51_active_5s_192k.ec3` | 192 kbps | 120,576 | `0D1AADB5D068FD553959CCCE93A5A4CF8668674783525DFD86BA12CF84A82B99` |
| `adm_pcm51_active_5s_448k.ec3` | 448 kbps | 281,344 | `822CC31C41D6E6659DF6EAA2C58BBA73F8FAA4ADAF19CCC05FEDA8C05AD4F28A` |
| `adm_pcm51_active_5s_640k.ec3` | 640 kbps | 401,920 | `0B376FD1A7AE76B40644347136C3D977432F89F2F84FC40DF4A641EA7AD431A3` |

## Online/Blu-ray profile boundary

DEE 5.2.1's CLI help exposes only generic XML, input, output, and
`--add-elem` controls; it has no `online`, `blu-ray`, `media`, or target-profile
selector. The generated official schema confirms that `encode_to_atmos_ddp`
contains bitrate/DRC/downmix/time-range fields, while the `<ec3>` output only
contains file/storage fields. Atmos bitrates in that schema are
`384, 448, 576, 640, 768, 1024` kbps. The Atmos documentation describes E-AC-3
output and optional MP4 or MPEG-2 transport-stream multiplexing, but does not
define a separate Blu-ray Atmos mode.

The separate `pcm_to_ddp` `encoder_mode=ddp71` path is ordinary Dolby Digital
Plus 7.1. It must not be treated as a Blu-ray Atmos path: attempting to feed the
ADM master produced the DEE error `Atmos input is not supported in DD+ 7.1.`
(`media\gate8-dee-vectors\adm_pcm71_canary384.log`). The documentation's note
that a 640 kbps Dolby Digital rate is Blu-ray-compliant is a rate/use-case note
for that non-Atmos encoder, not evidence of a Blu-ray Atmos profile.

Consequently, the six Atmos files are labeled Atmos E-AC-3/JOC coverage
vectors, not “Blu-ray Atmos” vectors. No unsupported or guessed XML parameter
was added to manufacture that label.
