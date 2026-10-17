# Credits

## Project

- **Hellcommander** — project author and maintainer.
- **Contributors** — see the repository history for implementation and review
  contributions.

## Tools and platform references

- **Microsoft Visual C++ Build Tools and Windows SDK** — used to build and test
  the x64 Windows code and inspect the executable import/export ABI.
- **Windows operating system APIs** — used for module loading, diagnostics,
  exception context, and local logging.

These tools and APIs are not included in this repository.

## Referenced software

- **Soulash 2** — the game targeted by this experimental project. Soulash 2,
  its executable, assets, documentation, and data files are not included here.
  This project is independent and is not affiliated with or endorsed by the
  game developers. The project maintainer reports that the game's author has
  said an extender is welcome and that the maintainer may do what they want
  with it; that is the maintainer's account, not a licence or a statement
  from the author, and it does not cover redistributing the game or BugSplat.
- **BugSplat** — the proxy's four exported symbol names and expected x64
  signatures were derived from the game's imports. The vendor DLL, its code,
  and its assets are not included. The original signed DLL must remain
  user-provided and unmodified; deployment instructions are in `README.md`.

- **Lua 5.4** - copyright PUC-Rio, MIT licence. Downloaded by the builder from
  <https://www.lua.org/ftp/> (see `scripts/fetch-lua.ps1`), not stored in this
  repository, and compiled into the extender DLL for the sandboxed script tier.
  Binary packages include its licence notice in `THIRD_PARTY_NOTICES.txt`.

## Test fixtures

- `tests/fake_bugsplat.cpp` and the synthetic extender/module DLLs are
  purpose-built test fixtures maintained in this repository. They do not
  contain or forward to vendor BugSplat code, and the tests do not execute the
  game or third-party Workshop modules.

## License

Original source code and test fixtures in this repository are provided under
the MIT License; see `LICENSE`. Referenced product names and trademarks remain
the property of their respective owners. No third-party binary, game asset, or
Workshop module is redistributed by this project.
