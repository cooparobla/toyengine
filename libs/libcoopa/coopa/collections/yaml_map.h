/**
 * @file yaml_map.h
 * @brief Provides the YAMLMap wrapper class for managing YAML configurations.
 */

#ifndef COOPA_COLLECTIONS_YAML_MAP_H
#define COOPA_COLLECTIONS_YAML_MAP_H

#include <fkYAML/node.hpp> // Correct include
#include <string>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>
#include <type_traits> // For std::is_same_v

#include <coopa/debug/logger.h>
#include <coopa/yaml/document.h>

/**
 * @namespace coopa
 * @brief Root namespace for the libcoopa library.
 */
namespace coopa {
/**
 * @namespace coopa::collections
 * @brief Namespace containing collection utilities.
 */
namespace collections {

/**
 * @class YAMLMap
 * @brief A wrapper class for managing YAML configuration using fkYAML.
 */
class YAMLMap {
public:
    // --- Constructors ---

    /**
     * @brief Constructs an empty YAMLMap with a mapping root node.
     */
    YAMLMap() : root_node_(fkyaml::node::mapping()) {} 

    /**
     * @brief Constructs a YAMLMap from an existing fkYAML node.
     * @param node The fkYAML node to wrap.
     */
    YAMLMap(fkyaml::node node) : root_node_(std::move(node)) {}

    // --- Core File I/O ---

    /**
     * @brief Loads a YAML file from the specified path.
     * @param filepath Path to the YAML file.
     * @return A YAMLMap instance representing the deserialized configuration.
     * @throws std::exception If deserialization fails.
     */
    static YAMLMap load(const std::string& filepath) {
        YAMLMap config;
        
        const std::filesystem::path resolved = coopa::yaml::resolve_variant(filepath);
        if (!std::filesystem::exists(resolved)) {
            std::cerr << "YAMLMap: Error loading file '" << filepath << "'. Creating empty config." << std::endl;
            return config; 
        }

        try {
            config.root_node_ = coopa::yaml::load_document(resolved);
        } catch (const std::exception& e) {
            std::cerr << "YAMLMap: Error parsing file '" << filepath << "': " << e.what() << std::endl;
            throw; 
        }
        return config;
    }

    /**
     * @brief Saves the current configuration to the specified file path.
     * @param filepath Path where the YAML file should be saved.
     * @throws std::runtime_error If the file cannot be opened for writing.
     */
    void save(const std::string& filepath) const {
        std::ofstream fout(filepath);
        if (fout.is_open()) {
            fout << root_node_;
            fout.close();
        } else {
            throw std::runtime_error("YAMLMap: Could not open file for writing: " + filepath);
        }
    }
    
    // --- Read Functionality ---

    /**
     * @brief Retrieves a scalar value associated with the given key.
     * @tparam T The expected type of the value.
     * @param key The key of the item to retrieve.
     * @param default_value The value to return if the key doesn't exist or type conversion fails.
     * @return The retrieved value, or default_value if not found.
     */
    template<typename T>
    T get(const std::string& key, const T& default_value) const {
        if (!root_node_.is_mapping() || !root_node_.contains(key)) {
            return default_value;
        }
        
        try {
            return root_node_.at(key).get_value<T>();
        } catch (const std::exception& e) {
            std::cerr << "YAMLMap::get() type conversion error for key '" << key << "': " << e.what() << std::endl;
            return default_value;
        }
    }

    /**
     * @brief Retrieves a nested YAMLMap node.
     * @param key The key of the sub-node to retrieve.
     * @return A YAMLMap wrapping the sub-node, or an empty YAMLMap if not found/not a container.
     */
    YAMLMap get_node(const std::string& key) const {
        if (!root_node_.is_mapping() || !root_node_.contains(key)) {
            return YAMLMap(); 
        }

        fkyaml::node sub_node = root_node_[key];
        
        if (sub_node.is_mapping() || sub_node.is_sequence()) {
            return YAMLMap(sub_node); 
        }
        return YAMLMap(); 
    }

    /**
     * @brief Checks if a key exists in the current YAML map.
     * @param key The key to look up.
     * @return True if the key exists and the root node is a mapping, false otherwise.
     */
    bool exists(const std::string& key) const {
        if (!root_node_.is_mapping()) {
            return false;
        }
        return root_node_.contains(key);
    }
    
    // --- Write Functionality ---

    /**
     * @brief Sets a value associated with the given key.
     * @tparam T The type of the value to write.
     * @param key The key of the item.
     * @param value The value to set.
     */
    template<typename T>
    void set(const std::string& key, const T& value) {
        if (!root_node_.is_mapping()) {
            root_node_ = fkyaml::node::mapping();
        }
        root_node_[key] = value;
    }

    // --- Vector/Sequence Helpers ---

    /**
     * @brief Retrieves a list/sequence of elements from a key.
     * @tparam T The element type.
     * @param key The key of the sequence (or empty string to read the root sequence).
     * @return A std::vector containing the deserialized elements.
     */
    template<typename T>
    std::vector<T> get_vector(const std::string& key) const {
        std::vector<T> result;
        if (key == "") {
            if (!root_node_.is_sequence()) {
                return result; 
            }

            for (const auto& item_node : root_node_) { 
                if constexpr (std::is_same_v<T, YAMLMap>) {
                    result.emplace_back(item_node);
                } else {
                    try {
                        result.push_back(item_node.get_value<T>()); 
                    } catch (const std::exception& e) {
                        std::cerr << "YAMLMap::get_vector() type conversion error: " << e.what() << std::endl;
                    }
                }
            }
            return result;

        } else {
            if (!root_node_.is_mapping() || !root_node_.contains(key)) {
                return result; 
            }

            const auto& node = root_node_.at(key);
            if (!node.is_sequence()) {
                return result; 
            }

            for (const auto& item_node : node) { 
                if constexpr (std::is_same_v<T, YAMLMap>) {
                    result.emplace_back(item_node);
                } else {
                    try {
                        result.push_back(item_node.get_value<T>()); 
                    } catch (const std::exception& e) {
                        std::cerr << "YAMLMap::get_vector() type conversion error: " << e.what() << std::endl;
                    }
                }
            }
            return result;
        }
    }

    /**
     * @brief Sets a key in the YAML map to a std::vector<T>. (CORRECTED)
     * @tparam T The element type.
     * @param key The key of the item to set.
     * @param vec The vector of elements.
     */
    template<typename T>
    void set_vector(const std::string& key, const std::vector<T>& vec) {
        if (!root_node_.is_mapping()) {
            root_node_ = fkyaml::node::mapping();
        }

        // --- START FIX ---

        // 1. Create a std::vector of fkyaml::node
        std::vector<fkyaml::node> node_vec;
        node_vec.reserve(vec.size()); // Optional optimization

        for (const T& item : vec) {
            if constexpr (std::is_same_v<T, YAMLMap>) {
                // Add the raw node to the std::vector
                node_vec.push_back(item.get_raw_node()); 
            } else {
                // Add the scalar value to the std::vector
                node_vec.push_back(item); 
            }
        }
        
        // 2. Create the sequence node *from* the vector
        fkyaml::node seq_node = fkyaml::node::sequence(node_vec);
        
        // --- END FIX ---
        
        root_node_[key] = std::move(seq_node);
    }

    // --- Advanced Accessors ---
    
    /**
     * @brief Retrieves the raw fkYAML node reference.
     * @return Reference to the underlying fkyaml::node.
     */
    fkyaml::node& get_raw_node() {
        return root_node_;
    }

    /**
     * @brief Retrieves the raw fkYAML node reference (const version).
     * @return Const reference to the underlying fkyaml::node.
     */
    const fkyaml::node& get_raw_node() const {
        return root_node_;
    }

    /**
     * @brief Gets the size of the YAMLMap (number of mapping keys or sequence elements).
     * @return The size of the node.
     */
    size_t size() const {
        if (!root_node_.is_mapping() && !root_node_.is_sequence()) {
            return 0;
        }
        return root_node_.size();
    }

    /**
     * @brief Dumps the current YAML node's content to the logger.
     * @param logger Logger instance to print to.
     */
    void log(coopa::debug::Logger& logger) const {
        logger.info("YAMLMap Content:\n" + to_string_());
    }

private:
    fkyaml::node root_node_; /**< The underlying fkYAML node. */

    /**
     * @brief Serializes the root node to a string representation.
     * @return The serialized string.
     */
    std::string to_string_() const {
        std::stringstream ss;
        ss << root_node_;
        return ss.str();
    }
};

} // namespace collections
} // namespace coopa

#endif // COOPA_COLLECTIONS_YAML_MAP_H