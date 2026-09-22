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

## Decoding

`app/media/image_codec.*` is the same two-path shape as the cover art decoder
(see [`metadata.md`](metadata.md#decoding-a-cover)) with the box and the source
generalised: baseline JPEG goes through `jpeg_ppa_pipeline`, and PNG, a
progressive JPEG or a picture the hardware rejects falls back to
image_framework's row-streaming decoder through the resizer. It is a separate
module from `artwork_codec` rather than a shared one because the tuning pulls
the other way: a cover is a 56 px square from bytes already in memory, a
picture here is a screen-sized rectangle read off the card.

- **The strip buffers are sized to the source width, not fixed.** A cover is
  capped at 1365 px by its 64 KB strips; photographs are routinely 3000-4000 px
  wide, and a strip has to hold 16 rows of the padded width (about 192 KB at
  4096 px). The pipeline is therefore rebuilt when a wider picture arrives and
  released with `image_codec_close()`, so the buffers only exist while the
  viewer is open.
- **PPA cannot land on the wanted size**, so the pipeline runs the smallest
  sixteenth that still overshoots and a second pass resizes exactly onto the
  target and converts to the panel format. That pass is a stretch onto the
  size the fit already decided, not another contain, so rounding cannot drift
  away from it.
- **The target box is clamped to the source size before the fit**, which is
  what turns "contain" into "shrink only".
- **A file up to 16 MB is read into PSRAM in chunks**; the hardware decoder
  wants the whole stream as contiguous bytes, and chunking is what lets a read
  be abandoned part way. Anything larger is decoded straight off the card
  through image_framework's file stream, which gives up the hardware path but
  never holds the file.
- **Interrupting a decode is a property of the software path only.** The
  `cancel` flag is polled between read chunks and between rows, and the stream
  wrapper returns an error to unwind the decoder. `jpeg_ppa_pipeline_process()`
  is one blocking call and runs to completion.

## The metadata cache gives way

`onEnter` stops the metadata worker and `onExit` starts it again, like the
video player does. It frees the browser's JPEG engine (only one is worth
holding) and the internal RAM behind it, and it stops thumbnail prefetching
from competing for the card with the picture being opened. Browser thumbnails
are dropped by the stop and asked for again when Home reappears.

Image files are not probed for metadata at all: they carry no tags and no cover
art, so `FileBrowserPage` skips them when it asks the cache for rows.

## Test material

`simulator/sdcard/Pictures/` (gitignored) is what
`simulator/verify/image_viewer.txt` walks. The samples cover both aspects, a
picture smaller than the screen, and the two fallback formats:

```sh
cd simulator/sdcard/Pictures
ffmpeg -f lavfi -i "testsrc2=size=3000x2000" -frames:v 1 a_landscape_3000.jpg
ffmpeg -f lavfi -i "mandelbrot=size=1200x1600" -frames:v 1 b_portrait_1200.jpg
ffmpeg -f lavfi -i "testsrc2=size=320x240" -frames:v 1 c_small_320.jpg
ffmpeg -f lavfi -i "smptebars=size=1024x768" -frames:v 1 tmp.ppm
cjpeg -progressive -quality 90 -outfile d_progressive_1024.jpg tmp.ppm && rm tmp.ppm
ffmpeg -f lavfi -i "testsrc2=size=1500x1000" -frames:v 1 e_wide_1500.png
ffmpeg -f lavfi -i "rgbtestsrc=size=600x900" -frames:v 1 f_portrait_600.png
```

ffmpeg's MJPEG encoder cannot write a progressive stream, which is why that one
goes through `cjpeg`; its PPM input is because the libjpeg-turbo in the flake
has no PNG reader.
