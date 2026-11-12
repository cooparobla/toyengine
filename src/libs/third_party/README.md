# third_party

Vendored single-header libraries shared by more than one coopa module, kept in one place so a
header cannot exist in two copies on the same include path.

| Directory | Used by |
|---|---|
| `stb/` | `stb_image.h` + `stb_image_write.h` (gfxcoopa, mapcoopa), `stb_truetype.h` (uicoopa) |

Each consumer adds this directory to its own include path (`../third_party`) and includes the
headers as `<stb/stb_image.h>`.

**Implementation macros stay with the consumer, not here.** `gfxcoopa/src/gfx_impl.cpp` compiles
`STB_IMAGE_IMPLEMENTATION` and `STB_IMAGE_WRITE_IMPLEMENTATION` once for its whole dependency
graph; `uicoopa/src/ui_impl.cpp` compiles `STB_TRUETYPE_IMPLEMENTATION`; and
`mapcoopa/coopa/maps/image_writer.h` compiles the PNG writer with `STB_IMAGE_WRITE_STATIC` so its
copy has internal linkage and cannot collide with gfxcoopa's. Sharing one header file does not
change any of that: the linkage is decided by those macros at compile time.
