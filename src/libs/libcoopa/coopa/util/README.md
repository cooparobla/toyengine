# Utility Module

The `util` module provides cross-platform helpers for algebra matrices, paths resolution, string algorithms, and unique identifier generators.

## Module Details

### 1. Matrix Mathematics (`math.h`)
- **`Mat4`**: A lightweight 4x4 matrix struct with operators for matrix multiplication, translation transformations, console formatting logs, and seamless translation to/from `glm::mat4`.
- **`MathUtil`**: Static utility math wrapper functions for `get_max`, `get_min`, and `lerp` linear interpolations.

### 2. Relative File Resolvers (`file.h`)
- **`FileUtil`**: Checks files existence (`does_path_exist`), and resolves target asset paths (`get_asset_path`, `does_asset_exist`) or resource paths (`get_resource_path`, `does_resource_exist`) relative to either environment variables, build outputs, or project source directories.

### 3. String Algorithms (`string.h`)
- **`StringUtil`**: Standard header-only operations including `split` (tokenizes strings), `replace` (performs substring replacements), `join` (concatenates vectors with delimiters), and `lower`/`upper` casing modifications.

### 4. Thread-Safe ID Generators (`id.h`)
- **`IdUtil`**: A static thread-safe counter using `std::atomic<unsigned int>` to generate unique runtime identifiers.

### 5. Transforms (`transform.h`)
- **`coopa::util::Transform`**: Hierarchical position / rotation (Euler degrees or quaternion) / scale with parent-child links, dirty-flag propagation and a lazily recomputed world matrix. `TransformComponent` in `coopa/scene/components/` wraps it.

### 6. Frame Timing (`time.h`)
- **`coopa::util::Time`**: High-resolution frame timer: call `update()` once per frame, then read the delta time, total elapsed time and frame count.

## Basic Example

```cpp
#include <coopa/util/math.h>
#include <coopa/util/id.h>
#include <coopa/util/string.h>

void execute() {
    // Unique ID
    unsigned int component_id = IdUtil::get_unique_id();
    
    // Matrix transform
    Mat4 transform = Mat4::translation(0.0f, 10.0f, 0.0f);
    
    // String join
    std::vector<std::string> parts = {"Part", "A"};
    std::string name = StringUtil::join(parts, "_");
}
```
