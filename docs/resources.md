# Resources (fonts, icons, images)

Fonts, icon fonts and images are generated at build time by esp-devkit's
resgen; the definition format, the pack format and the build integration are
in `esp-devkit/docs/resgen.md`.

`app/CMakeLists.txt` picks up `app/resources/resources.json` through a
`CONFIGURE_DEPENDS` glob and calls `resgen_add_resources()` if it exists, in
partition mode: the generated data goes to the 4M `resources` partition in
`esp32p4/partitions.csv`, so code changes never move those 2.2 MB and
differential flashing only rewrites the app. `idf.py app-flash` does not write the
partition at all; firmware that finds a partition from another build aborts at
boot with a `resgen:` log line.

| path | what it is |
|---|---|
| `app/resources/resources.json` | the definition |
| `app/resources/tabler/` | Tabler Icons SVGs for the icon fonts |
| `app/resources/fonts/` | the NotoSansJP subset and its glyph list (see [`fonts.md`](fonts.md)) |

To inspect the output by hand:

```sh
nix develop -c sh -c '$RESGEN_PYTHON esp-devkit/tools/resgen/resgen.py all app/resources/resources.json /tmp/out'
```
