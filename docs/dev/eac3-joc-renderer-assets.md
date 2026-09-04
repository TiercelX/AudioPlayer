# E-AC-3/JOC renderer asset manifest

本清单记录用于 renderer/BRIR 读取和布局验证的 BBC R&D `bbcrd-brirs`
样本。二进制和随附许可证/README 位于本地 ignored cache，不属于提交内容。

## Cache policy

- `docs/dev/reference-cache/` 由 `.git/info/exclude` 忽略；缓存不提交、不进入发布包，
  也不因本清单而再分发。
- URL 和文件名来自 BBC 官方仓库的下载脚本：
  [bbc/bbcrd-brirs/sofa/get_sofa_files.sh](https://github.com/bbc/bbcrd-brirs/blob/master/sofa/get_sofa_files.sh)。
- 官方脚本当前使用 `http://data.bbcarp.org.uk/`；本次 HEAD 探测返回 HTTP 200、
  `application/octet-stream` 和 Range 支持。下载时间为 2026-08-25。
- 仓库 README 的通用声明为 CC BY-SA 4.0；`bbcrdlr_systemH.sofa` 自身的 SOFA
  `License` 属性明确标为 CC BY-NC-SA 4.0，因此对该二进制采取更严格的
  CC BY-NC-SA 4.0 解释。商业使用和再分发需另行确认；本地缓存仅用于研发验证。

## Candidate probe

| 文件 | 官方 URL | HEAD status | Content-Length | 选择结论 |
|---|---|---:|---:|---|
| `bbcrdlr_systemH.sofa` | [BBC data host](http://data.bbcarp.org.uk/bbcrd-brirs/sofa/bbcrdlr_systemH.sofa) | 200 | 464,099,203 | 选用：低于 1 GB；包含 BS.2051 Array H (9+10+3)，覆盖中层、上层、顶层和下层扬声器 |
| `bbcrdlr_systemA.sofa` | [BBC data host](http://data.bbcarp.org.uk/bbcrd-brirs/sofa/bbcrdlr_systemA.sofa) | 200 | 42,283,000 | 未下载：较小，但不是本次优先的完整高度布局 |
| `bbcrdlr_all_speakers.sofa` | [BBC data host](http://data.bbcarp.org.uk/bbcrd-brirs/sofa/bbcrdlr_all_speakers.sofa) | 200 | 674,116,862 | 未下载：覆盖更广但大于最小验证资产 |

## Downloaded asset

| 文件 | 来源/版本 | 用途 | SHA-256 | 大小 | 结构验证 | License |
|---|---|---|---|---:|---|---|
| `renderer-assets/bbcrdlr_systemH.sofa` | BBC R&D `bbcrd-brirs` master；文件服务器 Last-Modified 2021-09-14 12:25:15 GMT | Gate 8 renderer 的 MultiSpeakerBRIR/HRTF-BRIR 读取、前后/高度布局映射和卷积测试 | `09dc3414a5eb7d9a325e0ad750da87ce63c9d4baf270c980296c726e152c89fa` | 464,099,203 bytes | HDF5 signature；`SOFAConventions=MultiSpeakerBRIR`, version 0.3；`Data.IR=(M=180,R=2,E=22,N=16384)`；`Data.SamplingRate=48000 Hz`；`ListenerPosition=1`、`ReceiverPosition=2`、`EmitterPosition=22` | File metadata: CC BY-NC-SA 4.0 ([license URL](http://creativecommons.org/licenses/by-nc-sa/4.0/)); local research cache only |

### Metadata extracted with `h5py`

The file was opened successfully with temporary `h5py 3.16.0`/`numpy 2.5.2`
under Python 3.12. The important dimensions are:

```text
Data.IR             (180 orientations, 2 receivers, 22 emitters, 16384 samples)
Data.SamplingRate   48000 Hz
Data.Delay          (1, 2, 22), units=samples; all 44 values are exactly 0
ListenerPosition    (1, 3)
ReceiverPosition    (2, 3, 1)
EmitterPosition     (22, 3, 1), spherical degrees/degrees/metres
```

`Data.Delay` follows the SOFA delay field definition in the [SOFA
specifications](https://www.sofaconventions.org/mediawiki/index.php/SOFA_specifications);
this asset has no non-zero delay to carry into the cache.

The embedded `SourceDescription` and `Comment` identify the source as
**[ITU-R BS.2051](https://www.itu.int/rec/R-REC-BS.2051) loudspeaker array H
(9+10+3)**. The emitter list includes
front/side/rear middle-layer positions (`M+000`, `M+180`, `M+/-090`), upper
positions (`U+000`, `U+180`, `U+/-090`, `U+/-045`, `U+/-135`), a top position
(`T+000`) and lower positions (`B+000`, `B+/-045`). This is sufficient to test
front/back and elevation mapping without treating the SOFA file as a Dolby
renderer or as an E-AC-3/JOC reference decoder.

The cached companion files are the BBC repository copies:

- `renderer-assets/bbcrd-brirs-README.md` — SHA-256
  `763790e6fec59133d52a8fdefb5fc559d45432ddb793643b11b568efc0d7074f`.
- `renderer-assets/bbcrd-brirs-LICENSE.txt` — SHA-256
  `7abe19ec9bb73b36141b999b861d24ad855e808bafe0f81e84cce28556f6c297`.

These files are provenance records only; implementation must still validate
SOFA convention, sample rate, dimensions, units, coordinate convention and
layout mapping at load time.

## R2A1 extracted cache (local-only)

`scripts/extract-system-h-brir.py` uses an isolated h5py environment to validate
the SOFA metadata and extract only the unique identity-listener M slice. The
native cache is little-endian and versioned; no HDF5 runtime is required by the
C++ loader. Fixed header layout (520 bytes) is:

```text
magic[8] = "R2A1BRIR", version:u32, headerSize:u32,
sampleRate:u32, emitterCount:u32, receiverCount:u32, irLength:u32,
listenerViewM:i32, sourceSha256Length:u32, sourceSha256[32],
systemHToSofaEmitter[22]:u32, leftReceiverIndex:u32, rightReceiverIndex:u32,
delay[2][22]:f64.
```

Payload is exactly `[ear=left,right][systemH emitter 0..21][sample 0..16383]`
float32 little-endian. The extractor maps emitters by azimuth and measured
elevation against the BS.2051 System H catalog (layer-constrained, azimuth exact;
this asset's measured maximum elevation error is 10 degrees and is an audited
acceptance bound, not a normative SOFA tolerance; radius finite and positive),
and maps receiver +Y to left / -Y to right per MultiSpeakerBRIR 0.3. The loader
rejects invalid magic/version/header/counts, duplicate or out-of-range mapping,
non-finite delay/IR, truncation, trailing bytes, and malformed hash fields.
Delay and IR payloads are reordered into canonical System H emitter order, while
the two receiver indices retain their raw SOFA provenance. Generated caches remain
under ignored `tmp/` and are never committed.

Offline usage (never part of the player runtime): create the ignored venv with
`python -m venv tmp/r2a-h5py-venv` and `tmp/r2a-h5py-venv/Scripts/python.exe -m
pip install h5py`; run `...extract-system-h-brir.py --self-test`, then the
extract command against the local SOFA asset. Load the resulting cache with
`build-mm/Debug/Eac3SofaBrirCacheProbe.exe --self-test` or its real-cache path.
