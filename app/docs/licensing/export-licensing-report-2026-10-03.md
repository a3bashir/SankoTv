# SankoTV: MP4 export and third-party licensing

Research report, 3 October 2026. Prepared for review by a lawyer before
SankoTV is sold.

**This is research, not legal advice.** It records what was measured on the
development machine and what the cited primary sources say. Where a
conclusion depends on reading a licence, that is said, and the source is
given so the licence can be read in full. Licence texts are summarised here,
not reproduced: read the originals.

Status of every open item is tracked in `HANDOFF.md`, section "BEFORE
SELLING". Nothing there is closed without the owner's word.

---

## Addendum, 3 October 2026 (after the report below was written)

**The MP4 encoder was changed from option 1 to option 4: Windows Media
Foundation called directly.** The owner decided this the same day. The body
of the report is left as written; where it says the Qt recorder was chosen,
read this addendum instead.

Why. Measured on real music and speech, the Qt recorder produced the sound
track as HE-AAC at 16 kbit/s whatever bit rate was asked for. Qt passes no
audio bit rate to the Windows AAC encoder in the mode the picture needs, and
the mode that does pass it lowers picture quality for the first second of
every panel. Calling Media Foundation directly sets both. It was proved in a
separate test program first: the same picture at the same sizes, frame times
exact over a one-hour film, and AAC-LC sound at 192 kbit/s.

What this changes for licensing: **nothing new ships.**

- The encoders are the same ones: Microsoft's H.264 and AAC encoders, which
  are part of Windows. Sections 6.1 and 7 (the patent question about using
  the operating system's encoders, and Windows N editions) apply unchanged.
- Windows now also writes the MP4 file itself (the Media Foundation "Sink
  Writer"). SankoTV links Windows system libraries for this (`mfplat`,
  `mfreadwrite`, `mfuuid`); it redistributes none of them.
- The MP4 export no longer passes through FFmpeg at all. **The FFmpeg
  libraries still ship**, because Qt Multimedia uses them to play the
  animatic's audio track and to decode an imported audio file for the
  export. Sections 3, 5 and 6.2 therefore still apply as written.
- One path was measured and deliberately **not** taken: feeding the Qt
  recorder floating-point audio makes Qt use FFmpeg's own AAC encoder
  instead of Windows'. That would have made code shipped with SankoTV the
  AAC encoder, sharpening the question in section 6.2. SankoTV does not do
  this.
- The exported file now carries a colour tag (BT.709) and the picture is
  converted by SankoTV's own code. No third-party component is involved.

Additional sources for the addendum:

- Microsoft, "AAC Encoder" (Media Foundation; profiles and the supported bit
  rates of 96, 128, 160 and 192 kbit/s):
  <https://learn.microsoft.com/en-us/windows/win32/medfound/aac-encoder>
- Microsoft, "Tutorial: Using the Sink Writer to Encode Video":
  <https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video>

The measurements behind the addendum are archived on the development
machine in `app/tests/_backups/` (a local folder, not part of the
repository) as `export_probe_audio_20261003*` and `export_probe_mf_20261003*`.

---

## 1. Summary

- SankoTV exports MP4 (H.264 video, AAC audio) through Qt Multimedia's
  `QMediaRecorder`. No encoder, codec or FFmpeg program was added for this.
- On Windows the compression is done by **Microsoft's own encoders, which are
  part of Windows**. Qt's FFmpeg libraries only pass frames to them.
- **FFmpeg libraries ship with SankoTV already**, and did before export was
  rebuilt: Qt Multimedia uses them to play the animatic's audio track. They
  are an LGPL build.
- **Qt is used under its open-source licence (LGPL version 3).** Selling a
  closed-source application on that basis is permitted but carries
  obligations that have not yet been met (section 4).
- Two patent questions are open (section 6): whether a paid, professional
  application may rely on the H.264 and AAC encoders Windows provides, and
  whether shipping FFmpeg's own H.264 / AAC code (decoders and an AAC
  encoder) makes SankoTV a product that needs pool licences of its own.
- Windows "N" editions have no media components until the user adds
  Microsoft's free Media Feature Pack. SankoTV detects this and says so; it
  does not fall back to another codec.

## 2. What was decided, and what was rejected

Four ways to make an MP4 were compared.

| | Ships a new binary | Customer installs something | Licence consequences |
|---|---|---|---|
| **1. Qt Multimedia `QMediaRecorder`** (chosen) | No | No (N editions: Media Feature Pack) | None beyond what already ships |
| 2. Bundle an `ffmpeg.exe` | Yes | No | LGPL or GPL obligations for the program. A build with x264 is GPL and makes SankoTV the supplier of an H.264 encoder |
| 3. Require the customer's own FFmpeg (the old behaviour) | No | Yes | None for FFmpeg; fails the product requirement |
| 4. Windows Media Foundation called directly | No | No (N editions: Media Feature Pack) | Same as option 1; Windows-only code |

Option 1 was chosen by the owner on 3 October 2026, with the encoder placed
behind an interface (`app/src/export/MovieEncoder.h`) so that option 4 or an
Apple implementation can replace it later.

Not used, and why:

- **x264** is licensed under the GPL (a commercial licence is sold
  separately). No GPL component may enter SankoTV without the owner's
  explicit approval.
- **Cisco OpenH264**: Cisco's patent licence covers only the binary Cisco
  distributes, and only when the end user's device downloads it separately
  and the user can disable it. Bundling it in an installer falls outside
  those terms. Source: OpenH264 binary licence.

## 3. What ships with SankoTV today

Measured in the application's build folder (`app/build/Release`), as placed
there by Qt's own deployment tool (`windeployqt`).

Qt libraries: `Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Svg`, `Qt6Multimedia`,
`Qt6Network` (a dependency of Qt Multimedia; SankoTV itself contains no
network code), plus Qt plugins.

FFmpeg, version 7.1.3, as built and supplied by The Qt Company:

| File | Size |
|---|---|
| `avcodec-61.dll` | 13.9 MB |
| `avformat-61.dll` | 2.6 MB |
| `avutil-59.dll` | 1.2 MB |
| `swscale-8.dll` | 0.75 MB |
| `swresample-5.dll` | 0.25 MB |
| `multimedia/ffmpegmediaplugin.dll` (Qt's plugin) | 0.65 MB |

The FFmpeg build identifies its own licence as LGPL version 2.1 or later.
Its build configuration, read from the library, is:

```
--disable-debug --disable-decoder=truemotion1 --disable-doc --disable-lzma
--disable-programs --disable-static --disable-v4l2-m2m --disable-vulkan
--enable-network --enable-pic --enable-shared --enable-zlib
```

There is no `--enable-gpl` and no `--enable-nonfree`. Checked in the library
itself: it contains no x264, x265, OpenH264, NVENC, QuickSync or AMF encoder.

What codec code it does contain (confirmed by the codec names present in
`avcodec-61.dll`):

- H.264 and HEVC **decoders** (FFmpeg's own).
- AAC **encoder and decoder** (FFmpeg's own).
- MPEG-4 Part 2 encoder and decoder, MP3 decoder, MJPEG, and others.
- H.264, HEVC and AAC **encoders only as wrappers** around Windows' Media
  Foundation encoders (`h264_mf`, `hevc_mf`, `aac_mf`).

Not examined in this report: the other third-party files in the deployment
folder (the software OpenGL library `opengl32sw.dll`, Microsoft's
`D3Dcompiler_47.dll`, `dxcompiler.dll`, `dxil.dll`) and the components Qt
Multimedia lists in its attributions besides FFmpeg (DR Libs, Signalsmith
Stretch, TLSF, Boost, libjpeg, zlib). A full third-party notice inventory is
a separate task.

## 4. Qt licensing

**Which licence is in use.** The Qt installation on the build machine is the
open-source one: `C:\Qt\licenseInfo.txt` records the licence type as
open source, and the installer log records that no commercial licence was
found. Qt's own software bill of materials, installed with Qt 6.11.1, lists
Qt Multimedia, its FFmpeg plugin and the other Qt libraries SankoTV links as
available under Qt's commercial licence, LGPL version 3, or GPL version 2 or
3. SankoTV therefore uses them under **LGPL version 3**.

**What LGPLv3 asks of a closed-source application**, as The Qt Company
summarises it (source: "Obligations of the GPL and LGPL"):

1. Provide the complete source code of the Qt libraries used (or a written
   offer of it), including any changes made to Qt.
2. Ship the LGPL and GPL licence texts, and state prominently that Qt is
   used under the LGPL.
3. Link dynamically, so the application itself can stay closed. SankoTV
   does: Qt is a set of DLLs beside the executable.
4. Let the user replace the Qt libraries with a modified version and still
   run the application; do not forbid reverse engineering done for that
   purpose.

**Questions for the lawyer.**

- **Signed or sealed packages.** If SankoTV is distributed as an MSIX
  package or through the Microsoft Store, the user cannot replace a DLL
  inside the package. Does that satisfy obligation 4, and if not, what form
  of distribution does?
- **Private Qt interfaces.** SankoTV's build links Qt's `GuiPrivate`
  interfaces. These are not stable between Qt versions, so a replacement Qt
  must be the same version. Is that acceptable under the LGPL's relinking
  requirement?
- **Build tool.** The shader compiler `qsb` is licensed GPL version 3 with a
  Qt exception. It runs when SankoTV is built and is not shipped; SankoTV
  does not link the Qt Shader Tools library at run time. Confirm that its
  output is unaffected.
- **The alternative** is a Qt commercial licence, which removes the LGPL
  obligations for Qt (not for FFmpeg). Its terms for a project begun on
  open-source Qt have not been checked.
- **The end-user licence agreement** for SankoTV must not contradict
  obligations 2 and 4.

## 5. FFmpeg licensing

FFmpeg is LGPL version 2.1 or later unless built with GPL parts; the build
Qt supplies is not. Qt's documentation states that the pre-built FFmpeg
libraries it provides include only features compatible with the permissive
licences it lists.

FFmpeg publishes a compliance checklist for applications that ship its
libraries (source: FFmpeg "License and Legal Considerations"). In summary:
build without GPL or non-free parts, link dynamically, distribute the exact
source of the FFmpeg used and say where it came from, state in the
application's "About" information that it uses FFmpeg under the LGPL, do not
rename the libraries to hide them, and do not forbid reverse engineering in
the EULA.

These obligations apply to SankoTV **as it stands**, because the libraries
already ship. The export changes nothing about them.

Not yet done: none of the notices, source offers or "About" text exist in
SankoTV.

## 6. H.264 and AAC patents

Qt's documentation says plainly that video compression standards such as
H.264 may be covered by patents and incur royalties, and that Qt's licences
do not cover them. FFmpeg's legal page declines to advise on patents.

### 6.1 Using the encoders that are part of Windows

SankoTV's MP4 export calls Microsoft's H.264 and AAC encoders. Microsoft
licenses those from the patent pools and ships them in Windows.

The Windows licence terms (Windows 11, OEM, April 2024, section 14(b))
carry the notice the AVC pool requires. It licenses the product for the
"personal and non-commercial use of a consumer" to encode video to the
standard, and says no licence is granted or implied for other use.

The pool's own summary of its licence (Via LA, AVC Patent Portfolio License
briefing) describes the right that comes with a licensed encoder as use by
the end user for personal and consumer purposes, expressly including
internal business use, without remuneration.

**Open question.** A storyboard artist exporting an animatic for a client or
a studio is working professionally. Is that inside the use the Windows
encoder is licensed for? If not, who needs a licence: the artist, the
publisher of the application that calls the encoder, or neither? The same
question applies to every Windows application that encodes through Media
Foundation, and to options 1 and 4 equally.

Separately, the pool charges for **distributing AVC video** to end users for
payment (per title or by subscription), with free internet video exempt.
That concerns whoever distributes the finished video, not the tool. An
animatic is normally a working document.

### 6.2 The codec code inside the FFmpeg libraries

`avcodec-61.dll` contains FFmpeg's own H.264 and HEVC decoders and an AAC
encoder and decoder (section 3). SankoTV uses the AAC or MP3 decoder when
the artist imports an audio file in those formats. It does not use FFmpeg's
own AAC encoder for export (Windows' encoder was observed in use), but the
code is present in the shipped file.

**Open question.** Does shipping that file make SankoTV an "end product"
containing an H.264 decoder and an AAC encoder/decoder, so that SankoTV's
publisher needs pool licences in its own name?

Published pool terms, for reference:

- **AVC / H.264** (Via LA): for products sold to end users, no royalty on
  the first 100,000 units a year, then US $0.20 a unit, falling to $0.10
  above 5 million; an annual enterprise cap applies. The published briefing
  is dated March 2022 and describes the licence term ending 31 December
  2025, renewable in five-year periods. **The terms of the current term
  should be obtained from Via LA.** Whether the zero-royalty tier requires
  signing the licence is a question for the lawyer.
- **AAC** (Via LA): the published schedule has **no free tier**. As read on
  3 October 2026 it lists a per-unit fee starting at US $0.98 for the first
  500,000 units a year, an initial fee of US $15,000 (US $1,000 for small
  entities), and a separate "PC software" category whose rates are not
  published. Verify these figures on the page.

**A way to remove the question**, not yet done and requiring the owner's
approval because it means building FFmpeg ourselves: build Qt's FFmpeg with
only the Windows-encoder wrappers and patent-free decoders (PCM, FLAC,
Vorbis, Opus, and MP3 if its patents are confirmed expired). SankoTV would
then ship no H.264, HEVC or AAC code of its own at all.

### 6.3 Other platforms

On macOS, Qt's FFmpeg backend is expected to use Apple's encoders
(VideoToolbox and AudioToolbox). **Not measured**: there is no Mac in the
project yet. The same two questions would apply with Apple in Microsoft's
place.

## 7. Windows "N" editions

Microsoft states that N editions of Windows exclude Media Foundation and the
H.264, AAC and other codecs, and that the free Media Feature Pack restores
them (Settings, Apps, Optional features; a restart is needed).

SankoTV checks for the Windows media components before offering to export an
MP4. If they are absent it shows a message naming the Media Feature Pack and
where to add it, and exports nothing. PNG and PDF export are unaffected.

**Not tested on an N edition.** The detection looks for three Windows
system files (`mfplat.dll`, `mfh264enc.dll`, `mfAACEnc.dll`); that these are
exactly what an N edition lacks is taken from Microsoft's description, not
observed.

## 8. What was measured

On Windows 11 Pro, through `QMediaRecorder` with frames supplied from
memory:

| Film | Encode time | Frames in file |
|---|---|---|
| 1080p, 24 fps, 10 s, with audio | 1.1 s | 240 of 240 |
| 1080p, 30 fps, 60 s, no audio | 7.9 s | 1800 of 1800 |
| 3840x2160, 24 fps, 60 s, with audio | 25.6 s | 1440 of 1440 |

The encoders named in the log were "H264 Encoder MFT" and "Microsoft AAC
Audio Encoder MFT". The H.264 encoder was Microsoft's software encoder; the
graphics card's encoder was not used.

The probes are archived on the development machine in `app/tests/_backups/`
(a local folder, not part of the repository) as
`export_probe_qmediarecorder_20261003*`.

## 9. Sources

Primary sources, all read on 3 October 2026:

1. Qt Multimedia overview (backends, licences, the patent statement,
   third-party attributions): <https://doc.qt.io/qt-6/qtmultimedia-index.html>
2. Qt, `QVideoFrameInput` (FFmpeg backend only, since Qt 6.8):
   <https://doc.qt.io/qt-6/qvideoframeinput.html>
3. The Qt Company, "Obligations of the GPL and LGPL":
   <https://www.qt.io/licensing/open-source-lgpl-obligations>
4. FFmpeg, "License and Legal Considerations" (LGPL/GPL, the compliance
   checklist, the patent statement): <https://ffmpeg.org/legal.html>
5. Microsoft, "H.264 Video Encoder" (Media Foundation):
   <https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder>
6. Microsoft, "Media Feature Pack for Windows N":
   <https://support.microsoft.com/en-us/windows/experience/platform-variants/media-feature-pack-for-windows-n>
7. Microsoft Software License Terms, Windows Operating System (OEM
   pre-installed, last updated April 2024), section 14(b):
   <https://www.microsoft.com/content/dam/microsoft/usetm/documents/windows/11/oem-(pre-installed)/UseTerms_OEM_Windows_11_English.pdf>
8. Via Licensing Alliance, AVC/H.264 programme:
   <https://www.via-la.com/licensing-programs/avc-h-264/>
9. Via Licensing Alliance, "AVC Patent Portfolio License Briefing"
   (version dated 16 March 2022):
   <https://via-la.com/wp-content/uploads/2025/09/avcweb.pdf>
10. Via Licensing Alliance, AAC programme:
    <https://www.via-la.com/licensing-2/aac/>
11. Cisco, OpenH264 binary licence:
    <https://www.openh264.org/BINARY_LICENSE.txt>

Local evidence:

- `C:\Qt\licenseInfo.txt` and the Qt installer log (which Qt licence).
- Qt's software bill of materials installed with Qt 6.11.1 (module
  licences).
- The licence and configuration strings embedded in
  `C:\Qt\6.11.1\msvc2022_64\bin\avcodec-61.dll`.

Licence texts that should be read in full and were not fetched for this
report: the GNU LGPL version 3 and version 2.1, and the GNU GPL version 3
(<https://www.gnu.org/licenses/>).
