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
  light knob (`ambient_intensity`), how it reaches the GPU, and two
  look-alike settings that don't do what they appear to.
