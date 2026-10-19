# Notes for working on this repo

- Hardware reference priority: Supermodel first, MAME second. See
  docs/REFERENCES.md. When adapting Supermodel code, keep its copyright
  notice and cite the source file.
- Never commit ROM data, dumps, or anything extracted from them. That
  includes recompiled code: it is generated into build/generated/.
- Build and test: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.
  CI builds with warnings as errors on Linux and macOS.
