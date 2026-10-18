# Where these headers come from

Build-only copies; nothing here is compiled into the plugin except through
`#include`.

- `libobs/`: the public headers (`*.h` only) of `libobs/` from the OBS Studio
  **32.0.4** source release (`obs-studio-32.0.4`, tag `32.0.4`, GPL-2.0-or-later;
  `LIBOBS_API_MAJOR_VER 32`, `MINOR 0`, `PATCH 4` in `obs-config.h`).
  `obsconfig.h` is not part of the release (OBS generates it at build time);
  the one here is a minimal hand-written version for plugins.
- `simde/`: SIMDe **0.8.4** (`SIMDE_VERSION_*` in `simde/simde-common.h`), the
  `simde/` header tree plus `COPYING` (MIT) and `README.md`. libobs's
  `util/sse-intrin.h` uses it to emulate SSE on Apple silicon. The exact
  upstream commit was not recorded when it was vendored.

To update: replace both trees with the headers of the new OBS release (and
the SIMDe version that release pins in `deps/`), update `obsconfig.h` and the
versions above, and build. CMake never downloads or writes into this folder.
