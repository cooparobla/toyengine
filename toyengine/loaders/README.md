# toyengine/loaders

Empty as of the sampler-parameter change to gfxcoopa's `TextureLoader`. This directory used to
hold `pixel_texture_loader.h`, a near-verbatim fork of gfxcoopa's `TextureLoader` that only
differed in forcing `VK_FILTER_NEAREST` + `CLAMP_TO_EDGE` (gfxcoopa's own loader hardcoded
`VK_FILTER_LINEAR`, which blurs pixel-art textures). `TextureLoader` now takes a `SamplerDesc`
parameter, so `core/engine.h` registers it directly with `SamplerDesc::pixel_art()` instead of
forking the whole decode/upload path for one sampler difference.
