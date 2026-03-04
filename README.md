# OpenAstro PHD2 Alpaca Build (Unofficial)

<img src="icons/oa512.png" alt="OpenAstro Logo" width="125">

## Overview

This repository contains an OpenAstro-maintained build derived from PHD2 that has ASCOM Alpaca Support. It is intended for use with [**AlpacaBridge**](https://github.com/open-astro/AlpacaBridge) and related Alpaca-based workflows.

## Important Notice

- This is **not** an official PHD2 release.
- It is a **private build** supported by OpenAstro, not by the PHD2 developers or community.
- Support requests should be directed to OpenAstro.

## Scope

- Alpaca-only device support
- Designed to work with AlpacaBridge

## Code formatting

CI runs a clang-format check. To fix formatting locally before pushing:

```bash
# Install clang-format 18 (Ubuntu/Debian)
sudo apt-get install -y clang-format-18

# Format all C++ sources
./build/run-clang-format
git add -A && git diff --cached --exit-code || git commit -m "Apply clang-format"
```

## License

This project remains under the original PHD2 licensing terms. See [LICENSE.txt](LICENSE.txt) for details.
