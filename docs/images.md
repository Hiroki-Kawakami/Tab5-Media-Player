# Image viewer

JPEG and PNG files in a directory, shown one at a time, scaled down to fit the
screen with the aspect ratio kept. Nothing is ever enlarged, and there is no
zoom: a picture smaller than the screen is shown at its own size in the middle
of it.

## Why it is an ordinary screen

`ImageViewerScreen` renders on the main display like `AudioPlayerScreen`, not
on a display of its own like `VideoPlayerScreen`. The picture is a buffer of
panel-format pixels in PSRAM behind an `lv_image`, so LVGL owns every pixel on
the screen and the bars are plain objects on top of it — none of the video
player's machinery applies (no shared SRAM handover, no presenter, no UI
insets, no invalidation suppression). Rotation is `LV_EVENT_SIZE_CHANGED` on
the root, because `ui_orientation` keeps rotating the main display while the
viewer is open.

The bars start hidden and only a tap toggles them: nothing is playing, so
there is no reason to show controls until they are asked for, and nothing to
hide them again for.

The count in the bottom bar is aligned to the bar rather than laid out between
the buttons, so it sits in the middle of the screen whatever the buttons on
either side are.

A swipe is acted on while the finger is still down, as soon as the horizontal
travel passes `kSwipeThreshold` and exceeds the vertical one — waiting for the
release made a deliberate swipe feel like it had been missed. The same press
becomes a tap only if no swipe was recognised.

## The two panels

Media Info and Settings are the video player's panels reused: the same dark
shell (`app/screens/player_panel.*`, which also holds the row helpers both info
panels build from) in the same places — the bottom 640 px in portrait, the
right 560 px in landscape — and the same way of leaving them, either the close
button or a tap on the picture beside them.

- **Settings is the player's panel with the Sound section left out.** The panel
  takes a section mask rather than being copied, so the rows that can be
  changed while media is open stay defined in one place. Color Mode is not
  there for the same reason it is not there during playback: it decides the
  format the pixels were decoded into, so changing it would mean decoding the
  picture again.
- **Media Info is its own panel**, because a picture has nothing a
  `MediaSummary` describes: the rows are the file, its format and size, the
  resolution the header gave and the size the picture is actually shown at. It
  is rebuilt every time it is shown and whenever the picture behind it changes,
  since a swipe beside the panel moves to the next file. With EXIF it is longer
  than the panel, and it scrolls as an ordinary LVGL container: the video
  player's info panel renders itself into an off-screen buffer because scrolling
  live objects over a playing video is what costs, and nothing is playing here.

## What EXIF adds

`app/media/image_exif.*` reads the tags a camera leaves in a picture, and the
Media Info panel shows them as a Camera and an Exposure section.

- **It is parsed by the probe, out of the window it already read.** The 128 KB
  the probe pulls in for the format and the id starts at the file's first byte,
  which is where a JPEG's `APP1` and a PNG's `eXIf` are, so the tags cost a
  parse and no card access at all. A picture whose tags run past the window is
  parsed as far as the window goes — every offset is checked against the bytes
  in hand rather than against the length the segment claims.
- **The result hangs off `MediaEntry` as a pointer**, allocated only when
  something was found, so the 512 entries the meta store may hold do not each
  grow by an `ImageExif`. It survives `media_cache_stop()` with the rest of the
  meta entries.
- **Only ASCII is kept from a text tag**, and only the first 256 bytes of one
  are looked at. The panel's font has no glyphs beyond ASCII, and a camera may
  write anything into `Model` or `LensModel`, so the bytes that cannot be drawn
  are dropped at parse time rather than handed to LVGL; the cap is what keeps a
  file claiming a 64 KB string in every one of its entries from costing the
  probe a scan per entry.
- **Orientation is shown, not applied.** Turning the picture would mean the
  decode, the cache key (an entry is keyed by the box it was fitted into) and
  the PPA stand-in a rotation shows all agreeing on the rotation, which is a
  different change from reading a tag.

GPS, the maker notes and everything else are skipped: the parser walks IFD0 and
follows the Exif SubIFD pointer once, and takes the dozen tags the two sections
show.

## Where the picture comes from

Nothing is read or decoded on the LVGL thread: the viewer is a `media_cache`
client like the browser and the audio screen, and an image file is a kind of
entry in that cache rather than a mechanism of its own (see
[`metadata.md`](metadata.md#image-files)). What the viewer adds is the box, the
priorities and when to give up on a request:

- **The box is the whole screen**, so the entry is keyed by the rotation in
  effect. Rotating asks for the other box and the first one stays cached, which
  is why rotating back is instant.
- **The current picture is `Blocking` and the neighbours are `Idle`**, and the
  idle queue is dropped before the neighbours are re-requested, so a walk
  through a folder never leaves a queue of pictures nobody is looking at.
- **Moving on cancels the request for the picture being left**, which drops it
  from the queue and withdraws it if it is already being read or decoded. The
  prefetch token is deliberately not cancelled: the picture being swiped to is
  often the one the idle queue is decoding right then.
- **The decoded pixels are held by the screen for as long as they are shown**,
  and the cache never evicts an entry someone still holds.
- **A rotation shows the picture it already has, rescaled by PPA**, while the
  size the screen now wants is decoded. Reading the card and decoding again is
  hundreds of milliseconds; a PPA blit of pixels that are already in PSRAM is
  not, and leaving the screen blank for that long is what a rotation would
  otherwise cost. PPA quantizes its scale to sixteenths, so the stand-in is up
  to 6% smaller than the box — it is replaced by the real decode, and the
  alternative (overshooting) would clip the picture instead. It runs on the
  LVGL thread while the worker may be decoding; the two are separate PPA
  clients and the driver serializes them.

Fitting into the box is `image_codec`'s "contain, but never enlarge": the box
is clamped to the source size before the fit is computed, so a picture smaller
than the screen comes out at its own size.

A failure is read back off the entry rather than carried in a message: nothing
read (the file is gone), read but not an image, an image that ran out of memory
to decode, or one that would not decode for another reason. The label in the
middle of the screen then carries the file name, what the header said the
picture is (`3023x3231 progressive JPEG`) and the reason, because the size and
the kind are most of the explanation when the answer is "too large to decode".
A completion for the current file that carries no pixels is not a failure by
itself — after a rotation the request for the previous box completes the same
way — so the entry has to say so.

## What does not fit

A progressive JPEG has no hardware path and image_framework must hold int16
coefficients for the whole frame before it can emit a row — about 3 bytes per
pixel at 4:2:0 — so the ceiling is roughly 7 Mpx with the stores and the arenas
already in PSRAM. A measured example: a 3023x3231 (9.8 Mpx) progressive JPEG
wants some 29 MB against 22 MB free, and is reported as too large rather than
as a broken file. The same picture as baseline JPEG or PNG opens, because both
of those are decoded a band or a row at a time.

## Test material

`simulator/sdcard/Pictures/` (gitignored) is what
`simulator/verify/image_viewer.txt` walks. The samples cover both aspects, a
picture smaller than the screen, the two fallback formats, and a file that is
not an image at all (`head -c 4096 /dev/urandom > g_broken.jpg`):

```sh
cd simulator/sdcard/Pictures
ffmpeg -f lavfi -i "testsrc2=size=3000x2000" -frames:v 1 a_landscape_3000.jpg
ffmpeg -f lavfi -i "mandelbrot=size=1200x1600" -frames:v 1 b_portrait_1200.jpg
ffmpeg -f lavfi -i "testsrc2=size=320x240" -frames:v 1 c_small_320.jpg
ffmpeg -f lavfi -i "smptebars=size=1024x768" -frames:v 1 tmp.ppm
cjpeg -progressive -quality 90 -outfile d_progressive_1024.jpg tmp.ppm && rm tmp.ppm
ffmpeg -f lavfi -i "testsrc2=size=1500x1000" -frames:v 1 e_wide_1500.png
ffmpeg -f lavfi -i "rgbtestsrc=size=600x900" -frames:v 1 f_portrait_600.png
ffmpeg -f lavfi -i "mandelbrot=size=2048x1536" -frames:v 1 tmp.ppm
cjpeg -progressive -quality 88 -outfile i_progressive_2048.jpg tmp.ppm && rm tmp.ppm
```

`j_exif_camera.jpg` and `k_exif_600.png` carry the tags the two panel sections
show, the first in a JPEG `APP1` and the second in a PNG `eXIf` chunk. Pillow
(`$RESGEN_PYTHON` in the flake has it) writes both, but a plain tuple makes it
guess `SHORT` for a rational tag and the file comes out malformed —
`IFDRational` is what gives `ExposureTime` and `FNumber` their proper type:

```python
from PIL import Image
from PIL.TiffImagePlugin import IFDRational

image = Image.open("tmp.jpg")
exif = Image.Exif()
exif[0x010F] = "ExampleCorp"          # Make
exif[0x0110] = "ExampleCorp X-1"      # Model
exif[0x0131] = "Tab5 Media Player"    # Software
exif[0x0112] = 1                      # Orientation
sub = exif.get_ifd(0x8769)
sub[0x9003] = "2026:03:14 09:26:53"   # DateTimeOriginal
sub[0x829A] = IFDRational(1, 250)     # ExposureTime
sub[0x829D] = IFDRational(28, 10)     # FNumber
sub[0x8827] = 400                     # ISO
sub[0x920A] = IFDRational(35, 1)      # FocalLength
sub[0xA405] = 52                      # FocalLengthIn35mmFilm
sub[0x9204] = IFDRational(1, 3)       # ExposureBiasValue
sub[0x9209] = 16                      # Flash
sub[0xA434] = "EX 35mm F1.8"          # LensModel
image.save("j_exif_camera.jpg", exif=exif, quality=90)
```

The PNG is the same call on a `rgbtestsrc` picture saved as `.png`, with
`Orientation` 6 so the row that is not `Normal` is on screen somewhere.

`h_exif_3000.jpg` is `a_landscape_3000.jpg` with three maximum-size APP1
segments inserted after the SOI, which puts its frame header ~190 KB into the
file, past the window the metadata probe reads:

```python
src = open("a_landscape_3000.jpg", "rb").read()
seg = b"".join(b"\xff\xe1" + (65533).to_bytes(2, "big") + bytes(65531) for _ in range(3))
open("h_exif_3000.jpg", "wb").write(src[:2] + seg + src[2:])
```

ffmpeg's MJPEG encoder cannot write a progressive stream, which is why that one
goes through `cjpeg`; its PPM input is because the libjpeg-turbo in the flake
has no PNG reader.

`image_exif_parse()` takes a buffer and nothing else, so it can be fuzzed on
the host by compiling it with a `main` that mmaps each input so that it ends
exactly at a `PROT_NONE` page: a read past the bytes it was given is then a
SIGSEGV. That is how it is checked rather than with AddressSanitizer, whose
runtime hangs in its own initialiser on the macOS in this flake.
