# editor/node

The node-graph canvas's geometry and graph edits, as pure functions. No `imm` drawing, no
`EditorApp`, no GPU — so every decision here is unit-tested without a window, and
[`../app/ui/node_graph.h`](../app/ui/node_graph.h) is left turning clicks into calls. The same
split [`../ui/curve_math.h`](../ui/curve_math.h) and `curve_widget.h` use.

The graph itself is **not** ours: it is
[`coopa::nodegraph::Graph`](../../libs/libcoopa/coopa/nodegraph/graph.h), the same type the
evaluator and `toyengine/procgraph/` consume. Nothing here is specific to procedural geometry —
node types, socket types and the conversions between them all come from the library's registries,
so a second kind of graph needs no code in this directory.

| File | Purpose |
|---|---|
| [`node_view.h`](node_view.h) | `NodeView{pan, zoom}` — graph space to screen pixels, and cursor-anchored zoom. `imm` has no transform of any kind, so the canvas does it by hand. |
| [`node_layout.h`](node_layout.h) | Where a node's body and each socket sit, in graph units. **One function, called by both the renderer and the hit-tester** — computing it twice is how wires end up attaching a few pixels from where you grabbed them. |
| [`node_wire.h`](node_wire.h) | A link's cubic bezier, flattened to points. Horizontal tangents, so a backwards link reads as a loop rather than a diagonal through its own node. |
| [`node_hit.h`](node_hit.h) | What is under the pointer, in the fixed priority **socket > node > wire > background**. Returns graph-space picks; the panel converts once. |
| [`node_edit.h`](node_edit.h) | Connect / disconnect / add / delete / duplicate / move, plus the graph's own interface (add / remove / rename / retype / reorder a socket), each returning a `NodeGraphEdit{changed, active, finished, key}` the panel turns into one undo step. |

## Why the interface edits are not just vector operations

A `GroupInput`'s output sockets and a `GroupOutput`'s input sockets **are** the graph's interface —
`Graph::interface_declaration()` synthesises them by name, and those two node types are in no
registry. A link stores only an identifier, so renaming an interface socket silently orphans every
link through it and nothing in the library checks. `rename_interface_socket()` re-points them,
`remove_` drops them, `retype_` drops only the ones `can_convert()` stops accepting, and
`move_` touches none (names do not move). Editing `graph.inputs()` directly skips all of that.

## Why `can_connect()` lives here

`ConnectVerdict can_connect(graph, from, to)` is the single rule for whether a wire may be dropped,
and it asks `TypeRegistry::can_convert()` — **the same predicate `Graph::validate()` uses**. So a
connection the canvas highlights as legal cannot later fail validation: the two are physically
unable to disagree. It also refuses the three things `Graph::add_link()` does not check at all
(output-to-output, a socket the current declaration no longer has, and a link that would close a
cycle), and reads the target's `multi_input()` flag so callers never have to remember which
overload to use.

## Units

Everything is in **graph units** — what `Node::ui_position` is in and what the file stores as
`pos:`. A node is therefore the same size in the document at every zoom, and `NodeView` converts
once at draw time. Pick tolerances are the exception worth noting: the panel passes them already
divided by zoom, so a socket stays the same size in *screen* pixels however far out you are.
