# Contributing

Thank you for helping. Bug reports, fixes and improvements are welcome.

- **Bugs**: open an issue with the template; include versions, the device,
  what you did, and the relevant log lines (read them first, see INSTALL.md).
- **Security problems**: see [SECURITY.md](SECURITY.md); not in public issues.
- **Changes**: fork, branch, and open a pull request. Before you do:
  - `cmake --preset release && cmake --build --preset release && ctest --preset release`
    pass, and the build has no new warnings;
  - for threading or memory changes, also the `asan` or `tsan` preset;
  - a fix for a bug comes with a test in the matching smoke where one can be
    written (`tests/`);
  - changes to the UxPlay fork go in `uxplay/` as their own commits;
  - code follows the surrounding style (`.clang-format`, `.editorconfig`);
    comments say why, not what.
- Changes to discovery or connection (the server name, ports, the Bonjour
  profile, the fast-connect handshake) need a test on a real iPhone and a Mac
  sender, mirroring and AirPlay video, including connect/disconnect/rename.

By contributing you agree that your contribution is licensed under
GPL-3.0-or-later, like the rest of the project.
