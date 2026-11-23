# iyipack: Asset and Resource Packer for IYIEngine

`iyipack` is a standalone CLI tool that packages game assets and project files into an [rres](https://github.com/raysan5/rres) resource archive (`.rres`) for shipping distribution.

## Overview

- **Format Compliance**: Packages files as standard `RRES_DATA_RAW` (`RAWD`) resource chunks preceded by a valid `rresFileHeader`.
- **Omitted Central Directory**: The Central Directory (`CDIR`) is deliberately omitted (`cdOffset = 0`), preventing plaintext asset filenames and directory hierarchies from leaking into the distribution archive.
- **Lookup by Deterministic ID**: Every resource is identified solely by a 32-bit CRC32 ID computed from its normalized, lowercase, forward-slash relative path. The runtime computes the exact same ID at lookup time.
- **Collision Detection**: If two files produce identical CRC32 hashes, `iyipack` aborts immediately and prints both colliding paths.
- **Compression**: Per-extension compression policies using standard DEFLATE (`RRES_COMP_DEFLATE`) linked directly with raylib's `CompressData`, matching raylib's `DecompressData` in the engine runtime.
- **Encryption**: Authenticated encryption using XChaCha20-Poly1305 (`RRES_CIPHER_XCHACHA20_POLY1305`) via Monocypher. The key is derived once per archive (Argon2i, one salt stored in every chunk, a random nonce per chunk), so the runtime derives it once at the first read.
- **Key Safety**: Encryption keys are never accepted as raw command-line arguments, never printed to console or logs, and never stored inside the output archive. Sensitive key buffers are wiped in memory using `crypto_wipe`.

---

## Usage

```sh
iyipack [options] <manifest.json>
```

### Options

| Flag | Argument | Description |
|---|---|---|
| `-m`, `--manifest` | `<path>` | Path to the packaging manifest JSON file. |
| `-i`, `--input` | `<path>` | Override input root directory specified in manifest. |
| `-o`, `--output` | `<path>` | Override output archive path specified in manifest. |
| `--key-env` | `<NAME>` | Environment variable holding the encryption key/passphrase. |
| `--key-file` | `<path>` | File containing the encryption key/passphrase. |
| `--encrypt` | *(none)* | Force enable encryption. |
| `--no-encrypt` | *(none)* | Force disable encryption. |
| `-h`, `--help` | *(none)* | Print command-line usage and exit. |

### Security Requirements for Keys

`iyipack` forbids passing keys directly as command-line arguments (e.g. `--key secret` / `-k` is rejected with an error). Keys must be passed via:
1. An environment variable (e.g. `set IYI_ENCRYPTION_KEY=...` and `--key-env IYI_ENCRYPTION_KEY`).
2. A key file (e.g. `--key-file path/to/secret.key`).

Trailing carriage returns (`\r`) and newlines (`\n`) in key files or environment variables are automatically trimmed.

---

## Manifest JSON Schema

Manifests configure input paths, output destinations, file pattern filters, compression rules, and encryption settings.

### Schema Fields

- `input_root` *(string, optional, default: `"."`)*: The base directory containing files to package. Relative paths resolve against the directory containing `manifest.json`.
- `output_file` *(string, optional, default: `"game.rres"`)*: Target archive path. Relative paths resolve against the directory containing `manifest.json`.
- `include` *(array of strings, optional, default: `["**/*"]`)*: Glob patterns specifying files to include.
- `exclude` *(array of strings, optional, default: `[]`)*: Glob patterns specifying files to exclude (takes precedence over `include`).
- `compression` *(object or string, optional, default: `"deflate"`)*:
  - When string: `"deflate"` or `"none"` applied globally.
  - When object:
    - `default` *(string)*: Default algorithm (`"deflate"` or `"none"`).
    - `extensions` *(object)*: Map of file extensions (with or without leading dot) to compression policies (`"deflate"` or `"none"`).
- `encryption` *(object or boolean, optional, default: `false`)*:
  - When boolean: Enable or disable encryption using default environment variable `IYI_ENCRYPTION_KEY`.
  - When object:
    - `enabled` *(boolean)*: Enable or disable encryption.
    - `key_env` *(string)*: Name of environment variable holding the passphrase (default: `"IYI_ENCRYPTION_KEY"`).
    - `key_file` *(string)*: Path to file containing the passphrase.

### Example `manifest.json`

```json
{
  "input_root": "game",
  "output_file": "dist/game.rres",
  "include": [
    "**/*"
  ],
  "exclude": [
    "**/*.pyc",
    "**/__pycache__/**",
    "**/.git*",
    "**/.DS_Store",
    "**/Thumbs.db"
  ],
  "compression": {
    "default": "deflate",
    "extensions": {
      ".png": "none",
      ".jpg": "none",
      ".jpeg": "none",
      ".ogg": "none",
      ".mp3": "none",
      ".wav": "deflate",
      ".json": "deflate",
      ".py": "deflate",
      ".txt": "deflate",
      ".fs": "deflate",
      ".vs": "deflate"
    }
  },
  "encryption": {
    "enabled": true,
    "key_env": "IYI_ENCRYPTION_KEY",
    "key_file": ""
  }
}
```

---

## Binary Archive Layout

`iyipack` writes standard rres format data:

1. **`rresFileHeader` (16 bytes)**:
   - `id`: `"rres"` (0x73657272)
   - `version`: `100` (rres v1.0)
   - `chunkCount`: Number of resource chunks (uint16, max 65535)
   - `cdOffset`: `0` (Central Directory omitted)
   - `reserved`: `0`

2. **Resource Chunks (`RRES_DATA_RAW`)**:
   For each file, an `rresResourceChunkInfo` (32 bytes) followed immediately by `packedSize` data bytes:
   - `type`: FourCC `"RAWD"` (`RRES_DATA_RAW`)
   - `id`: CRC32 of normalized relative path
   - `compType`: `RRES_COMP_NONE` (0) or `RRES_COMP_DEFLATE` (10)
   - `cipherType`: `RRES_CIPHER_NONE` (0) or `RRES_CIPHER_XCHACHA20_POLY1305` (73)
   - `flags`: `0`
   - `packedSize`: Total size on disk (after compression and encryption overhead)
   - `baseSize`: Original uncompressed payload size (`20 + file_size`)
   - `nextOffset`: `0`
   - `reserved`: `0`
   - `crc32`: CRC32 of the packed data bytes on disk

3. **Unpacked Payload Format (`RRES_DATA_RAW`)**:
   - `props[0]`: Original uncompressed file byte length
   - `props[1]`: Big-endian extension letters 1–4 (e.g. `".png"` -> `0x2e706e67`)
   - `props[2]`: Big-endian extension letters 5–8 (or `0` if shorter)
   - `props[3]`: Reserved (`0`)
   - Followed immediately by raw file bytes.

4. **Encrypted Payload Format (`RRES_CIPHER_XCHACHA20_POLY1305`)**:
   - Matches `rres-raylib.h:618-685`:
   - `[ciphertext: plain_size] [salt: 16 bytes] [nonce: 24 bytes] [mac: 16 bytes]`
   - Key derivation: Argon2i (`nb_blocks = 16384`, `nb_passes = 3`, `nb_lanes = 1`).

---

## Runtime Resource ID Calculation

The engine runtime locates resources without a Central Directory by hashing the asset's project-relative path.

### 1. Path Normalization Algorithm

Before computing the CRC32 the path is reduced to its archive key (`IYI::ResourceKey` in `include/IYI/ResourcePath.h`, shared by the engine and `iyipack`), so Windows `assets\Sprites\Hero.PNG` and Linux `assets/sprites/hero.png` give the same ID:

1. **Backslashes to slashes**: Replace all `\` with `/`.
2. **Lexical cleanup**: Normalize redundant components (`.` and `..`) using `std::filesystem::path::lexically_normal().generic_string()`.
3. **Strip root / prefix**: Remove leading `/` or `./`. A result of `"."` becomes `""`.
4. **ASCII lowercase**: Lowercase the result. Only the archive key is lowercased; filesystem paths keep their case (`ResourceFsPath`).

### 2. CRC32 Calculation

Hash the normalized path bytes using the standard ISO 3309 / ITU-T V.42 / IEEE 802.3 CRC32 polynomial (`0xEDB88320`), exactly as implemented by `rresComputeCRC32`:

```cpp
// CRC32 calculation as defined in rres.h
uint32_t ComputeResourceId(const std::string& normalizedRelPath)
{
    return rresComputeCRC32(
        reinterpret_cast<const unsigned char*>(normalizedRelPath.data()),
        static_cast<int>(normalizedRelPath.length())
    );
}
```

### 3. Runtime Lookup Code Example

```cpp
#include "rres.h"
#include "rres-raylib.h"

// 1. Archive key of the query path
std::string normPath = IYI::ResourceKey("assets/textures/player.png");

// 2. Compute resource ID
uint32_t resourceId = rresComputeCRC32(
    reinterpret_cast<const unsigned char*>(normPath.data()),
    static_cast<int>(normPath.length())
);

// 3. Load chunk directly from archive
rresResourceChunk chunk = rresLoadResourceChunk("game.rres", resourceId);
if (chunk.info.id == resourceId) {
    // 4. Decompress and decrypt (if password was set via rresSetCipherPassword)
    if (UnpackResourceChunk(&chunk) == 0) {
        unsigned int size = 0;
        void* data = LoadDataFromResource(chunk, &size);
        // data contains original file bytes of length size
    }
}
```
