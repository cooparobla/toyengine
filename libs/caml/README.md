# caml

**A header-only C++20 library and CLI that stores YAML as compressed, encrypted `.caml` files.**

caml takes a YAML document, compresses it with Zstandard, and encrypts it with AES-256-GCM.
The result is a small binary file that can't be read or edited by hand without the key.
Decoding gives back the exact original text. The library is one header,
`includes/caml/caml.h`, and the repo also builds a small `caml` command-line tool. One
example consumer is the toyengine game engine, which packages its game assets to `.caml`.

## Features

- **Compression.** Zstandard at level 3.
- **Authenticated encryption.** AES-256-GCM with a random 12-byte nonce per file and a
  16-byte tag. The magic bytes `CAML` are authenticated as associated data. A wrong key or
  a changed byte makes decoding throw.
- **Passphrase keys.** `derive_key()` turns a passphrase into a 32-byte key with
  PBKDF2-HMAC-SHA256 (10,000 iterations). You can also pass your own 32-byte key.
- **Byte-exact round trip.** `encode()` and `decode()` work on the raw YAML string, so
  comments and formatting are kept.
- **YAML map wrapper.** `caml::CAMLMap` wraps an [fkYAML](https://github.com/fktn-k/fkYAML)
  node. It loads and saves both `.yaml` and `.caml`, and gives you the node to edit.
- **Header-only.** Include one header. There is nothing to link except zstd and OpenSSL's
  libcrypto.
- **CLI.** `caml encode` and `caml decode` convert files from the shell.

## Getting started

### 1. Dependencies

- A C++20 compiler and CMake 3.16 or newer.
- **OpenSSL** (libcrypto). CMake finds it with `find_package(OpenSSL REQUIRED)`.
- **zstd.** CMake looks for `libzstd` with pkg-config. If it is not found, CMake fetches
  zstd v1.5.6 from GitHub and builds it as a static library.
- **fkYAML** 0.4.2 is bundled in `includes/fkYAML/node.hpp`. You don't install it.

caml does not depend on any other library.

On Debian or Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libzstd-dev libssl-dev
```

On macOS with Homebrew:

```bash
brew install cmake openssl@3 zstd pkg-config
```

### 2. Build

```bash
cmake -B build
cmake --build build -j
```

This builds one executable, `build/caml`. If CMake can't find OpenSSL on macOS, add
`-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`.

`install.sh` does the same on Debian or Ubuntu. It installs packages with `sudo apt` first.
`uninstall.sh` deletes `build/` and purges those packages with `sudo apt purge`, so read it
before you run it.

### 3. Install (optional)

```bash
cmake --install build --prefix /usr/local
```

This copies the `caml` executable to `bin/` and the `caml/` header folder to `include/`.

### 4. Use the CLI

```bash
./build/caml encode config.yaml config.caml --passphrase "my passphrase"
./build/caml decode config.caml config.yaml --passphrase "my passphrase"
./build/caml --help
```

Short forms are `-e`, `-d` and `-p` (`--key` and `-k` also work for the passphrase).

### 5. Use the library

Add `includes/` to your include path, then link zstd and OpenSSL's libcrypto. The repo does
not export a CMake target, so you set this up in your own build.

```cpp
#include <caml/caml.h>
#include <iostream>

int main() {
    // Parse a YAML file and edit it through the fkYAML node.
    caml::CAMLMap config = caml::CAMLMap::load_yaml("config.yaml");
    config.get_raw_node()["version"] = 2;

    // Write it as .caml, then read it back with the same passphrase.
    config.save_caml("config.caml", "my passphrase");
    caml::CAMLMap loaded = caml::CAMLMap::load_caml("config.caml", "my passphrase");
    std::cout << loaded.to_yaml_string();

    // Or work on raw strings and bytes.
    std::vector<uint8_t> key = caml::derive_key("my passphrase");
    std::vector<uint8_t> bytes = caml::encode("name: demo\n", key);
    std::cout << caml::decode(bytes, key);
}
```

The full API, all in namespace `caml`:

| Function | What it does |
|---|---|
| `derive_key(passphrase, salt)` | Passphrase to 32-byte key (PBKDF2-HMAC-SHA256) |
| `encode(yaml, key)` / `decode(bytes, key)` | String to `.caml` bytes and back |
| `encode_file(in, out, passphrase)` / `decode_file(in, out, passphrase)` | The same, file to file |
| `CAMLMap::load_yaml` / `load_caml` / `from_yaml_string` | Build a map from a file or string |
| `CAMLMap::save_yaml` / `save_caml` / `to_yaml_string` | Write a map out |
| `CAMLMap::get_raw_node()` | The underlying `fkyaml::node` |

Errors are reported as `std::runtime_error` or `std::invalid_argument`.

## File format

Every `.caml` file has a 41-byte header followed by the ciphertext. Version 1 is the only
version.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic bytes `CAML` (`43 41 4D 4C`) |
| 4 | 1 | Format version, `0x01` |
| 5 | 12 | AES-GCM nonce (random) |
| 17 | 16 | AES-GCM authentication tag |
| 33 | 8 | Payload length, unsigned 64-bit, big-endian |
| 41 | n | Ciphertext: the Zstandard frame of the YAML text, encrypted with AES-256-GCM |

The associated data for GCM is the 4 magic bytes.

## Testing

```bash
./build/caml test
```

This runs a small self-test. It encodes and decodes a sample document, checks that the
text matches, and round-trips it through `CAMLMap`. Running `caml` with no arguments, with
fewer than three arguments, or with an unknown command also runs the self-test.

## Project layout

```
caml/
├── includes/
│   ├── caml/caml.h        the whole library
│   └── fkYAML/node.hpp    bundled fkYAML 0.4.2 (MIT)
├── test.cpp               the caml CLI and its self-test
├── CMakeLists.txt         builds the caml executable
├── install.sh             apt install + build (Debian/Ubuntu)
└── uninstall.sh           deletes build/ and purges the apt packages
```

## Notes

- **Encryption is always on.** If you don't give a key or passphrase, the library uses the
  built-in passphrase `caml::DEFAULT_PASSPHRASE` (`"coopa-caml-key"`). That only hides the
  data from casual reading. Use your own passphrase if the content must stay secret.
- **The CLI has a different default.** Without `-p`, the CLI uses `"default_caml_key"`, not
  `DEFAULT_PASSPHRASE`. A file encoded by the CLI without `-p` can't be read by
  `load_caml()` or `decode_file()` with their default argument. Pass the same passphrase on
  both sides.
- **The salt is fixed.** `derive_key()` uses one built-in salt unless you pass your own, so
  the same passphrase always gives the same key. Derive the key once and reuse it if you
  decode many files.
- **The bundled fkYAML is patched.** Integer conversion treats any unsigned 64-bit type as
  `uint64_t`, so reading a `size_t` works on macOS.
- **Whole-file only.** Files are read into memory and decoded in one pass. There is no
  streaming API.
