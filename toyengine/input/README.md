# toyengine/input

| File | Purpose |
|---|---|
| [`input_map.h`](input_map.h) | `InputMap` — named actions map to one or more GLFW key codes; `is_action_down(action, is_key_pressed_fn)` takes a key-state predicate rather than a `Window&`, so it's testable with a plain lambda mock. Ported from `pixengine/input/input_map.h` — the binding model is engine-agnostic. |
