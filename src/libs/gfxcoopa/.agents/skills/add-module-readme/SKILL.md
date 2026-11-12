---
name: add-module-readme
description: Generates or refreshes a standardized README.md for system modules, featuring high-level architecture overviews, ASCII pipeline diagrams, data layout tables, clickable file breakdowns, and usage examples matching blendy standard conventions.
---

# add-module-readme

This skill provides step-by-step instructions to create or refresh `README.md` documentation for system modules in the repository. Modules should always strictly follow the structure defined in [`src/blendy/render/README.md`](file:///home/coopa/git/blendy/src/blendy/render/README.md).

If a `README.md` already exists in the target module directory, it must be refreshed to reflect any recent code changes, new files, updated signatures, or revised architecture.

---

## Standard Module README Specification

Every module `README.md` must contain the following sections in exact order:

### 1. Title & High-Level Summary Header
- **Title**: `# <Module Display Name> (<namespace::module_name>)`
- **Overview**: 1-2 concise paragraphs explaining the module's core responsibility, key features, core algorithms or technologies implemented (e.g. Vulkan, PBR, Physics, Scene Graph), and how it fits into the broader application/library.

### 2. Architecture / Data Flow Diagram
- **Section**: `## <Module Name> Architecture` (or `## <Module Name> Pipeline Architecture`)
- **ASCII Diagram**: A clean ASCII box-and-arrow visual inside a ` ```text ` code block showing:
  - Input objects / data structures (e.g. `Scene Graph`, `Camera`, `Buffers`).
  - Pipeline stages, sub-passes, or processing phases with key parameters.
  - Output targets / present modes (e.g. `HDR Offscreen Pass`, `Swapchain`, `Output Buffer`).
- Use unicode box-drawing characters (`┌`, `┐`, `└`, `┘`, `├`, `┤`, `┬`, `┴`, `│`, `─`, `▲`, `▼`, `◄`, `►`) for crisp visual structure.

### 3. Structural Specifications / Layout Tables (If Applicable)
- **Section**: e.g., `## Descriptor Set Layout Architecture`, `## Data Model Architecture`, or `## Component Architecture`
- **Markdown Table**: A formatted matrix detailing layout schemas, bindings, memory configurations, or state models.
  - Example columns: `| Set | Layout Owner | Contents / Bindings |` or `| Component | Type | Responsibility |`.

### 4. File Breakdown
- **Section**: `## File Breakdown`
- **File Entries**: For **every** header (`.h`/`.hpp`) and source (`.cpp`/`.c`) file directly inside or scoped to the module:
  - Header: `### [`filename.h`](file:///absolute/path/to/filename.h)` (must use absolute `file:///` markdown links, without surrounding backticks around the link).
  - Bulleted list detailing:
    - High-level role of the file within the module.
    - Key classes, structs, functions, or interfaces defined in the file.
    - Important implementation details, mathematical formulas, algorithms, or interactions with external subsystems (e.g. `gfxcoopa::engine`, Vulkan APIs).

### 5. Usage Example
- **Section**: `## Usage Example`
- **Code Block**: A complete, runnable C++ code snippet inside a ` ```cpp ` block demonstrating:
  - Required header `#include`s.
  - Configuration struct initialization.
  - Instantiation of the primary module class/orchestrator.
  - Main loop call or execution invocation.

---

## Execution Workflow

When instructed to add or refresh a module's `README.md`:

### Step 1: Inspect the Target Module
1. Identify the target module directory (e.g., `src/blendy/scene`, `src/blendy/core`).
2. List all files and subdirectories in the module using `list_dir`.
3. View each header and source file using `view_file` to extract:
   - Module namespace and primary classes.
   - Architecture, data flow, descriptor/binding structures.
   - File-by-file responsibilities and key methods.
   - Typical usage pattern for the module.

### Step 2: Check for Existing `README.md`
- If `README.md` exists in the target directory, view its current contents.
- Plan to replace/refresh it completely with up-to-date information matching the standard layout.

### Step 3: Generate the README Content
- Draft each section adhering to the standard specification above.
- Ensure all file paths in the File Breakdown section use absolute `file:///` URLs (e.g. `[pbr_render_pipeline.h](file:///home/coopa/git/blendy/src/blendy/render/pbr_render_pipeline.h)`).
- Ensure the ASCII diagram accurately reflects the module's actual control flow or data hierarchy.
- Ensure code snippets in Usage Example are clean, idiomatic, and valid C++.

### Step 4: Write/Overwrite the `README.md`
- Write the final document to `<module_directory>/README.md` (use `write_to_file` with `Overwrite: true`).

### Step 5: Verify Formatting
- Verify all links use the correct `file:///` scheme.
- Verify code blocks and ASCII diagrams render cleanly without formatting glitches.
