# toyengine/loaders

| File | Purpose |
|---|---|
| [`pixel_texture_loader.h`](pixel_texture_loader.h) | `PixelTextureLoader` — a near-verbatim fork of gfxcoopa's `TextureLoader`, forcing `VK_FILTER_NEAREST` + `CLAMP_TO_EDGE`. gfxcoopa's own loader hardcodes `VK_FILTER_LINEAR`, which blurs every pixel-art texture. |
