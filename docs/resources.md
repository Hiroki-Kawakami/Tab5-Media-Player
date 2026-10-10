# Resources (fonts, icons, images)

Fonts, icon fonts and images are generated at build time by esp-devkit's
resgen; the definition format, the pack format and the build integration are
in `esp-devkit/docs/resgen.md`.

`app/CMakeLists.txt` picks up `app/resources/resources.json` through a
`CONFIGURE_DEPENDS` glob and calls `resgen_add_resources()` if it exists.

| path | what it is |
|---|---|
| `app/resources/resources.json` | the definition |
| `app/resources/tabler/` | Tabler Icons SVGs for the icon fonts |
| `app/resources/fonts/` | the NotoSansJP subset and its glyph list (see [`fonts.md`](fonts.md)) |

To inspect the output by hand:

```sh
nix develop -c sh -c '$RESGEN_PYTHON esp-devkit/tools/resgen/resgen.py all app/resources/resources.json /tmp/out'
```
