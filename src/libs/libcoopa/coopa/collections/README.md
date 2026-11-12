# Collections Module

The `collections` module provides wrappers around YAML parsing, configuration handling, and structured data serialization.

## YAML Configuration

The main component is `YAMLMap` (defined in [`yaml_map.h`](./yaml_map.h)), which encapsulates a `fkyaml::node` map to simplify configuration parsing:

- **Constructors**: Supports default empty mappings or wrapping pre-existing `fkYAML` nodes.
- **I/O Functions**: Loads configuration files safely (`load`) and serializes modifications back to files (`save`).
- **Data Access & Mutators**: Offers template-based getters (`get`) and setters (`set`) supporting standard scalars as well as sequences/vectors of elements.
- **Diagnostics logging**: Integration with `coopa::debug::Logger` for formatting/dumping configuration states.

## Basic Usage Example

```cpp
#include <coopa/collections/yaml_map.h>
#include <coopa/debug/logger.h>

void load_and_log_config() {
    coopa::debug::Logger logger("App");
    
    // Load config
    auto config = coopa::collections::YAMLMap::load("config.yaml");
    
    // Read values with default fallbacks
    std::string api_url = config.get<std::string>("api_url", "http://localhost");
    int retry_count = config.get<int>("retries", 3);
    
    // Retrieve lists
    std::vector<std::string> ports = config.get_vector<std::string>("ports");
    
    // Mutate and save
    config.set<int>("retries", retry_count + 1);
    config.save("config_updated.yaml");
}
```
