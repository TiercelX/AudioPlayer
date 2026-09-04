# E-AC-3/JOC renderer reference cache manifest

## BEAR main alignment (2026-08-31)

The active local reference is now the official BEAR `main` commit
`6127e897b941211051c2ad135ee09b00be2e6ae0`, checked out independently at
`tmp/reference/bear-main-6127e897`. Its recursively fetched submodule refs are
libear `10e071784d7f033fee8e456032458f10dd1da174`, Eigen
`3147391d946bb4b6c68edd901f2add6ac1f31f8c`, and xsimd
`31298c883bda0f3dfcf44170f45be242deb78f1a`. The existing
`tmp/reference/bear-git` checkout remains the old `5eececb` reference for A/B;
no old source or evidence was replaced.

The official `default_v1.1.tf` was downloaded to
`tmp/reference/bear-main-6127e897/data/default_v1.1.tf`: 235,633,724 bytes,
SHA-256 `171acae2159e60ffe9d705abc16a79be129ecd06d37186fae413a265b6ed71e8`.
The binding was configured with the existing VISR 0.13.0 install
(`ac48082f47109c766992a039787d3102075852ea`) and Boost 1.85, plus an isolated
nlohmann_json 3.11.3 dependency. CMake configure passed with
`CMAKE_POLICY_VERSION_MINIMUM=3.5`; the xsimd submodule emits only legacy
CMake-policy warnings. Build output is in
`tmp/reference/bear-main-6127e897/bear-main-build-final.log` and the generated
Python extension is under `build-visr-bear-6/python/Release`.
The CMake install target attempted to place the Python extension and data in
the host Python site-packages; those three generated items were moved to the
ignored `tmp/reference/bear-main-6127e897/site-packages-install/` quarantine,
so subsequent probes use only explicit build/VISR paths.

The focused front-centre impulse probe passed with finite 2x512 output and
peak-per-ear `[0.036141231656074524, 0.03963926434516907]`;
`tmp/reference/bear-main-6127e897/bear-main-front-center-impulse-final.json` is the
evidence. The initial CTest run reached `test_renderer` without the required
VISR runtime DLL directories in `PATH`; direct execution returned Windows
loader status `0xC0000135`, so that bounded wait was a harness-environment
problem rather than a test assertion. Re-running CTest with the wrapper's
explicit BEAR/VISR DLL paths passed all six tests in 11.60 s; `test_renderer`
passed all four cases in 2.21 s.

A comparable 99.987979-second / 4,799,423-frame Gate6C MONTERO object bundle
was rendered with the new reference. Provenance and raw/audition outputs are
under `tmp/reference/bear-main-6127e897/montero-100s-output-v2/` and identify
`bearSourceVersion=main@6127e897`, `bearDataVersion=default_v1.1`, and the hash
above. The same bundle rendered through the old pyd/default data and the new
main binding produced identical summary peaks/RMS/clipping; the raw A/B report
`montero-100s-output/ab-old-build-vs-new-data.json` measures difference RMS
`9.46e-9` and per-channel correlation above `0.99999999999998`. This is a
numeric object-bundle compatibility result only, not a listening or Dolby
equivalence claim. The old `bear-default.tf` remains the explicit legacy
choice via `-BearRoot tmp\reference\bear-git -BearData
docs\dev\reference-cache\bear-default.tf`.

### BEAR main IR-processing System H experiment (2026-08-31)

The existing BBC System H input is
`docs/dev/reference-cache/renderer-assets/bbcrdlr_systemH.sofa`, SHA-256
`09dc3414a5eb7d9a325e0ad750da87ce63c9d4baf270c980296c726e152c89fa`.
It is a finite 48 kHz `MultiSpeakerBRIR` with 180 listener views and 22
emitters in the `9+10+3` order. BEAR's default `9+10+3_extra_rear` layout has
24 channels; the two absent emitters are `B+135` and `B-135`. Official
`bear.process_irs.extract` rejects this mismatch by duplicate-IR detection for
both selection algorithms, rather than inventing missing data.

For a controlled explicit-layout experiment, the same official pipeline was
run with `--layout 9+10+3`. The v2 1000-step result is retained as feasibility
evidence. A v3 rerun used the recommended `--default-schedule --opt-steps
10000` for both delay stages. Its TensorFile is
`tmp/reference/bear-main-6127e897/ir-processing-system-h/v3/layout-9+10+3/system-h-22-v3.tf`,
SHA-256 `3f4e18d14b32d95a2d1b43a4820b25d7d5cdfc9cd6af5873021607c16d2dd47d`.
The report shows global last improvement near 7800/10000 and some view
optimisations reaching the limit at 10000, so v3 is
`INCONCLUSIVE_OPTIMISATION_NOT_CONVERGED`, not a calibrated candidate. The
structured audits, impulse report, processing PDF, and 100-second A/B report
are in the v3 directory. This data is experimental 22-channel evidence only;
it is not the 24-channel `default_v1.1.tf` and is not selected by the export
wrapper.

### BEAR main System H v4--v6 cross-seed stability (2026-09-01)

The v4/v5/v6 experiments retain the v2/v3 artifacts and use the explicit
22-emitter `9+10+3` layout; the missing default-layout channels remain
`B+135` and `B-135`, with no synthetic mapping. v5 and v6 both used the
official `--default-schedule` budgets of 90,000 view steps and 840,000 global
steps. v5 used seeds `6127`/`6128`; v6 used independent seeds `16127`/`16128`.

The v5-to-v6 delay comparison passed the declared numerical stability gates:
view max/RMS `1.043938`/`0.198061` samples, minimum correlation `0.999989905`;
global max/RMS `0.182405`/`0.0200646` samples, correlation `0.999997663`.
The final BRIR max/RMS/correlation were `0.226055`/`0.000591404`/`0.9989191625`.
The v6 TensorFile is
`tmp/reference/bear-main-6127e897/ir-processing-system-h/v6/layout-9+10+3/system-h-22-v6.tf`
with SHA-256
`8195b0b456f9172a709375f9da8e2a39c63d9d5f7b03aa02ee90359a8bffe7c9`, finite
`[180,22,2,2976]` BRIR/HOA data, and the official HOA decoder. Its seven
direction impulse binding probe passed. The same MONTERO bundle produced
per-ear correlation `0.9999895132`/`0.9999880578`, diff RMS `0.000451254`,
and tail ratio `0.999987829`; repeated raw and S24 renders had identical
hashes `bc1ba6541a2fcd156ee9eb3dae078623b99c85a234fdb918fa037d5eda0f5a9b`
and `3d296fc8c5dda7f00e10080c0ec21a9adc23eca49e65f26448d531a5c1d69802`.
`scripts\\validate-all.ps1` passed unit tests (10 suites), report schema, and
smoke. The complete v6 report and comparisons are under the v6 evidence root.

The resulting status is `SOLUTION_STABLE_WITH_LATE_STOCHASTIC_IMPROVEMENTS`,
meaning cross-seed numerical stability only. View improvements still occur
at `81000--89100/90000` and global improvement at `554400/840000`, so this is
not optimizer plateau convergence, formal calibration, listening evidence,
Dolby equivalence, or endpoint proof. v2--v5 remain preserved as historical
experiments and are not silently promoted to the default 24-channel data.

本清单记录 `docs/dev/reference-cache/` 中的本地规范参考缓存。PDF 均从
官方发布页面或官方公开 PDF 直链获取，并在 2026-08-25 以 `pypdf 6.16.2`
读取页数；每个文件的首字节签名均为 `%PDF-`。

## Cache policy

- `docs/dev/reference-cache/` 通过 `.git/info/exclude` 忽略，不属于提交内容。
- 本地缓存仅用于研发核对，不作为规范再分发，也不应复制到发布包。
- ITU、EBU、SMPTE 文档仍受各自版权、许可或终端用户协议约束；“官方免费
  下载/公开直链”不等于允许再分发。
- 现有 `docs/dev/` 中的 ETSI TS 102 366、TS 103 420 和
  `ts_103420_tables.c` 未在本次任务中重下或修改。

## Downloaded references

| 文件 | 版本/日期 | 当前用途 | 官方 URL | SHA-256 | 页数 | access/license note |
|---|---|---|---|---|---:|---|
| `ITU-R-BS.2076-3-2025.pdf` | BS.2076-3 (02/2025) | ADM 模型和 object/bed 元数据定义；不提供 E-AC-3 解码或 renderer 实现 | [ITU-R official PDF](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2076-3-202502-I!!PDF-E.pdf) | `337e78d026331dd8a5cc89f2e87e32b1ef31ac2443a6d21d3b849e847a0c79a1` | 123 | ITU 页面列为 Free Download；保留版权/不得擅自再分发 |
| `ITU-R-BS.2051-3-2022.pdf` | BS.2051-3 (05/2022) | 标准扬声器布局、位置和坐标；用于 virtual 7.1.4/9.1.6 参考 | [ITU-R official PDF](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2051-3-202205-I!!PDF-E.pdf) | `adb548c8465028580c257409d75beb32bed4d7a3adb8d5dff05abaacda2ca44b` | 22 | ITU 页面列为 Free Download；本地研发缓存，不再分发 |
| `ITU-R-BS.2127-1-2023.pdf` | BS.2127-1 (11/2023) | ADM reference renderer、object/DirectSpeakers/HOA 到目标布局；不等同 Windows/Dolby 耳机 renderer | [ITU-R official PDF](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2127-1-202311-I!!PDF-E.pdf) | `caf8e38880604b4abe71d172ddf384bd44c84c647313f76bc9505a75a58ef007` | 92 | ITU 页面列为 Free Download；版权和本地缓存限制同上 |
| `ITU-R-BS.2094-2-2025.pdf` | BS.2094-2 (02/2025) | ADM common definitions、Binaural 类型和布局命名互操作性核对 | [ITU-R official PDF](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2094-2-202502-I!!PDF-E.pdf) | `2cb89d086cad8d662c758dec1e38d685f7ee43d1c36bc7335d020338921b1658` | 41 | ITU 页面列为 Free Download；不构成私有 HRTF/BRIR 实现许可 |
| `ITU-R-BS.2125-1-2022.pdf` | BS.2125-1 (05/2022) | Serial ADM 表示背景和元数据互操作性核对 | [ITU-R official PDF](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2125-1-202205-I!!PDF-E.pdf) | `5f2875173fa3177f9d64b3d76a7566f5db1933165482a12074a82f44196caf85` | 48 | ITU 页面列为 Free Download；本地研发缓存，不再分发 |
| `ITU-R-BS.2466-1-2022.pdf` | BS.2466-1 (09/2022) | ITU ADM Renderer 使用、评估和 QA 背景；不是 OS API 合同 | [ITU-R official PDF](https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BS.2466-1-2022-PDF-E.pdf) | `12feb6f0cd5a3c6fc1f0810e38e7b0ebd6f15f04f4918bd0fb4a3763aab37ac4` | 17 | ITU Report 官方公开 PDF；保留 ©/all-rights-reserved 限制 |
| `EBU-Tech-3396-2023.pdf` | EBU Tech 3396 (27 Mar 2023) | BEAR binaural ADM renderer 的 direct/diffuse、BRIR、延迟和卷积参考 | [EBU publication page](https://tech.ebu.ch/publications/tech3396) / [official PDF](https://tech.ebu.ch/docs/tech/tech3396.pdf) | `130072029f79952b51bfa83b46ff6e06de55c69e78d00fe7c771767783b8c7a5` | 16 | EBU 官方公开 PDF；仅作本地研究缓存，遵守 EBU 版权/再分发限制 |
| `ITU-R-BS.2388-6-2025.pdf` | BS.2388-6 (2025) | ADM 和多声道音频文件的 usage guidelines；用于 ADM 文件/元数据 QA，不是 JOC bitstream 规范 | [ITU-R publication page](https://www.itu.int/pub/R-REP-BS.2388-6-2025) / [official PDF](https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BS.2388-6-2025-PDF-E.pdf) | `314c30298fc039a73b61da04521126052e2b3de159617713066f066ce3ec41cf` | 52 | ITU 页面列为 Free Download；本地研发缓存，不再分发 |
| `ITU-R-BS.2494-0-2021.pdf` | BS.2494-0 (11/2021) | advanced sound system 主观/定位/空间感测试材料说明；用于测试设计背景，不是解码器向量包 | [ITU-R publication page](https://www.itu.int/pub/R-REP-BS.2494) / [official PDF](https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BS.2494-2021-PDF-E.pdf) | `a3d71125b537192f56b0652184132a4a5a6fe91346027dfa304aee8b127e6f1e` | 21 | ITU 页面列为 In force、English-only；PDF/媒体素材均不得因本缓存而再分发 |
| `SMPTE-ST-2098-1-2018.pdf` | ST 2098-1:2018 (approved 18 Jun 2018) | immersive audio metadata、bed/object/renderer 术语背景；background-only | [SMPTE official PDF](https://pub.smpte.org/latest/st2098-1/st2098-1-2018.pdf) | `35b2a89efcf677a00b271369b6e2c1a1092cd5265be1cd2bfa41b8b5f12498d9` | 14 | 官方全文直链可读；仍遵守 SMPTE Standards Library EULA/版权，不作为当前 E-AC-3/JOC 规范依赖 |
| `SMPTE-ST-2098-2-2022.pdf` | ST 2098-2:2022 (approved 25 May 2022) | Immersive Audio Bitstream/IAB 的 bed/object/renderer interface 背景；background-only，不等于 Dolby JOC | [SMPTE official PDF](https://pub.smpte.org/latest/st2098-2/st2098-2-2022.pdf) | `dd0adc17259142707fa52f78edef7b5683ddd66679a6912a567f34827fe83ed1` | 61 | 官方全文直链可读；遵守 SMPTE EULA/版权，不把 IAB 与 E-AC-3/JOC 混为同一 bitstream |

## BEAR/EAR reference implementation baseline (2026-08-30)

The following archives were downloaded from the official EBU/ITU sources into
the ignored cache. The first archive is the exact BEAR revision named by EBU
Tech 3396 (v0.0.1-pre, commit `5eececb`); the second is the tagged EAR 2.1.0
source, which upstream identifies as the BS.2127 reference implementation
from version 2.0; the third is the official ITU companion ZIP shipped with
BS.2127-1, whose root is explicitly labelled `BS2127-0`.

| Cache file | Pinned edition/revision | Size (bytes) | SHA-256 | Official source | Contents/license check |
|---|---|---:|---|---|---|
| `bear-v0.0.1-pre-5eececb.zip` | BEAR `v0.0.1-pre`, commit `5eececb2c2671711c1f63a872e706a538a1d4a5a` (2022-01-31) | 3,323,738 | `d476a5835a7e5eabc888056d3f2718177a86ab8d683626c7021e1df8283aa0b9` | [ebu/bear commit archive](https://github.com/ebu/bear/archive/5eececb.zip) | Lists `bear/`, `visr_bear/`, `LICENSE`, and `visr_bear/LICENSE.md`; root README says Apache-2.0. Submodules are declared but not embedded. |
| `ear-2.1.0-a0e37d3.zip` | EBU EAR `2.1.0`, tag commit `a0e37d33f55ae7080b1aaccbce655680319f92ae` | 714,202 | `3525e0c0944ca754f4b06da91771320b3b12237d0f15d054dd142365866cdc41` | [ebu/ebu_adm_renderer 2.1.0 archive](https://github.com/ebu/ebu_adm_renderer/archive/refs/tags/2.1.0.zip) | Lists Python `ear/`, docs, tests, and `LICENSE`; license is BSD-3-Clause-Clear. Upstream README states 2.0+ is also the ITU BS.2127 reference implementation. |
| `ITU-BS2127-reference-implementation-R0A0700003E0001ZIPE.zip` | ITU companion package, root `BS2127-0` (the ZIP named by BS.2127-1 §1) | 682,234 | `123f4d86c200873f1ccac8f9b503f761f6213ff0bc1a6d9899531dd993246799` | [ITU official reference ZIP](https://www.itu.int/dms_pub/itu-r/oth/0a/07/R0A0700003E0001ZIPE.zip) | `Expand-Archive` succeeds; lists `iar/`, `README.md`, and `LICENSE`; license text is BSD-3-Clause-Clear. This is an ITU-provided BS2127-0 package, not evidence that the archive itself was revised to BS2127-1. |

Verification used `Invoke-WebRequest`, `Get-FileHash -Algorithm SHA256`,
`Expand-Archive`, and `tar -tf`; all three archives downloaded and listed
successfully, and all three license files were readable. These files remain
local research cache only and must not be staged or redistributed; BEAR also
depends on separately licensed VISR/libear submodules and no submodule payload
was fetched in this baseline.

### Reproducible EAR oracle setup

The runnable comparison uses an ignored local environment at
`tmp/reference/ear-2.1.0/venv` and imports the extracted pinned source directly;
no global package install is required. On this Windows host the environment
was created with `py -m venv`, then installed with `numpy`, `scipy`,
`attrs<22`, `ruamel.yaml~=0.15`, `six`, `lxml`, `multipledispatch`,
`importlib_resources`, and `setuptools<81` (the last pin supplies the legacy
`pkg_resources` API used by EAR 2.1.0). The checked-in entry point is
`tools/atmos-render/ear_bs2127_oracle.py`; it invokes the local panner's
`--vectors` interface and writes only an ignored JSON report.

### BEAR dependency/data boundary

The full pinned BEAR data file is cached at
`docs/dev/reference-cache/bear-default.tf` from the official release URL
`https://github.com/ebu/bear/releases/download/v0.0.1-pre/default.tf`;
size is 235,633,612 bytes and SHA-256 is
`c23a36289f246c96779fdce75e108187185d3ec7aeedd6afa25f7c3dc5e42131`, matching
the pinned `files.txt`. It remains an ignored local research cache under the
BEAR source/data license boundary.

The official BEAR Git checkout at `tmp/reference/bear-git` is detached at
`5eececb2c2671711c1f63a872e706a538a1d4a5a`. Its recursively fetched official
submodules are libear `8ae3bba00902539e6d86d634b596c63febcb47a3`, libear's
Eigen `939c22c74d799c55e944298de360d089f5dae8a9`, and rapidjson
`f54b0e47a08782a6131cc3d60f94d038fa6e0a51` (rapidjson's gtest
`0a439623f75c029912728d80cb7f1b8b48739ca4`). VISR 0.13.0 was cloned from
the official `s3a-spatialaudio/VISR` tag at commit
`ac48082f47109c766992a039787d3102075852ea`.

`visr_bear/data/default_small.tf` was downloaded from the official BEAR
release URL, is 1,252,613 bytes, and has SHA-256
`25f4ed69153f6245e494fdffca27be9be632cb0013ddd1de6e1ec20f22b44ddf`,
matching the pinned `files.txt`. The full `default.tf` is also cached and
hash-verified as documented at the start of this section.

An official Boost 1.85.0 source archive was downloaded to
`tmp/reference/boost_1_85_0.zip` from
`https://archives.boost.io/release/1.85.0/source/boost_1_85_0.zip`;
size is 214,707,279 bytes and SHA-256 is
`e712fe7eb1b9ec37ac25102525412fb4d74e638996443944025791f48f29408a`.
It is covered by the Boost Software License 1.0. The source was extracted
with `tar` under `tmp/reference/boost-tar/`; `bootstrap.bat vc143` generated
the local b2 engine. The first b2 attempt failed before compilation because
the VS18 developer environment was not initialized. An isolated retry used
`VsDevCmd.bat -arch=amd64` and an explicit b2 `vcvarsall.bat` setup, producing
the required x64 shared libraries; see
`tmp/reference/boost-build-shared.log`.

VISR 0.13.0 then configured, built, and installed to ignored
`tmp/reference/VISR-install`; exact logs are
`tmp/reference/visr-configure-shared.log`, `tmp/reference/visr-build.log`,
and `tmp/reference/visr-install.log`. Its generated import metadata omitted
the MSVC `kissfft_shared.lib` filename, so the local install copied the
corresponding generated `kissfft.lib` to that expected name. This is a local
packaging workaround, not an upstream source change. Pinned `visr_bear`
configured and built successfully using
`tmp/reference/bear-configure-installed-2.log` and
`tmp/reference/bear-build.log`; the first install attempt was rejected at
the default system prefix, then a local ignored prefix was used. The official
BEAR Python API produced `tmp/reference/bear-front-center-impulse.json`: a
front-center object impulse gave finite 2x512 binaural PCM with peak per ear
`[0.0407501757144928,0.03817449510097504]` and sum-of-squares
`0.017260704189538956`. This is a BEAR runtime boundary result only, not
Dolby equivalence or production integration.

The three exact artifacts written by the upstream install were moved to
ignored `tmp/reference/site-packages-quarantine`; their external paths were
verified absent and no unrelated packages were touched. The checked-in
`tools/atmos-render/run_bear_front_center_probe.py` passed with explicit local
BEAR/VISR/DLL/data paths and does not use user site-packages. Host Python and
NumPy remain runtime prerequisites and are not claimed to be fully hermetic.
The reusable full-file CLI and production integration are now the next step.

Phase 5 real-source evidence is also present under ignored
`tmp/listening/bear-montero/`: Gate6C export9 contains 4305 v2 batches and
`run_bear_montero_bundle.py` produced exact 10/15/11-second 48 kHz stereo
raw float32 WAV windows plus separate s24 audition copies. `provenance.json`
records the full default.tf hash, causal 6,610,944-frame processing, LFE
exclusion, and reserved warp_mode=3 non-rendering. Audition copies use one
fixed -2 dB gain; raw output is unmodified and preserved without clipping.
The actual-update queue accepted 64,560 deduplicated blocks, mapping
`rampDuration` to BEAR `interpolationLength`; source windows compensate an
empirical 167-sample latency. Raw files are named `*-raw-f32.wav`; fixed -2 dB
audition copies are named `*-audition-minus2dB-s24.wav`.

## Not downloaded

| 项目 | 原因 | 官方链接 |
|---|---|---|
| AES69-2022 (SOFA 2.1) / SOFA | AES69-2022 完整标准文本按 AES Standards Store/会员权限获取；不绕过购买或访问控制。公开 SOFA 资料只保留官方链接 | [AES Standards Store](https://aes.org/publications/standards-store/)；[AES69 preview](https://www.aes.org/publications/standards/preview.cfm?ID=99)；[SOFA Conventions](https://www.sofaconventions.org/) |
| ITU-R BS.2159-9 | 背景报告，不是当前 renderer 的必要实现依赖；保留链接即可 | [ITU-R BS.2159-9](https://www.itu.int/pub/R-REP-BS.2159-9-2022) |
