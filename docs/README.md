# docs/

Hand-written guides to specific engine behaviour — the kind of thing that's
correct in the code but not obvious from reading it (a config key that lands
in a differently-named struct, a field that looks live but isn't, a value
that's hardcoded where you'd expect a uniform). Cross-cutting explanations
live here; a class or module's own usage notes stay in its subdirectory's
README (see the root [README](../README.md)'s Layout section).

This is distinct from `.docs/`, the HTML API reference generated from
in-source Doxygen-style comments by `coopadocs build` (see the root
`CLAUDE.md` for that workflow) — `.docs/` is generated output and shouldn't
be hand-edited; everything in this folder is.

## Guides

- [ambient-lighting.md](ambient-lighting.md) — the global ambient/indirect
  light brightness and colour knobs (`ambient_intensity`, `sky_zenith`/
  `sky_horizon`/`sky_ground`), how they reach the GPU, what GI exists in this
  engine, and a look-alike setting that doesn't do what it appears to.
