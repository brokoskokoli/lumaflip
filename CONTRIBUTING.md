<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Contributing to Lumaflip

Thanks for your interest! Bug reports, translations, documentation fixes and code are all welcome.
By participating you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

- **Found a bug?** Open a [bug report](https://github.com/brokoskokoli/lumaflip/issues/new?template=bug_report.yml).
- **Have an idea?** Start a [feature request](https://github.com/brokoskokoli/lumaflip/issues/new?template=feature_request.yml)
  or a thread in [Discussions](https://github.com/brokoskokoli/lumaflip/discussions).
- **Speak another language?** See [translations/README.md](translations/README.md).
- **Security issue?** Please don't open a public issue — see [SECURITY.md](SECURITY.md).

## Development setup

Requirements: an Apple Silicon Mac with macOS 15 or later, Xcode command line tools and
[Homebrew](https://brew.sh).

```sh
brew install cmake ninja qt jpeg-turbo googletest clang-format resvg
git clone https://github.com/brokoskokoli/lumaflip.git && cd lumaflip
tools/run.sh [image]                   # build only the app (build/run) and start it
tools/check.sh --quick --no-gui        # build + unit tests, opens no windows
```

- Qt 6.11 comes from Homebrew. `resvg` is optional (SVG falls back to Qt SVG without it).
- `tools/fetch_sparkle.sh` (optional) downloads Sparkle 2 into `third_party/` for the auto-updater;
  without it the app builds with the updater disabled.
- Build directories live under `build/`. Don't delete them; use `cmake --fresh` if a cache is stale.
- The core also builds without the macOS layer:
  `cmake -B build/generic -DLF_FORCE_GENERIC_PLATFORM=ON …` (similar to the Linux CI job, which builds only the core).

### Windows

Visual Studio 2022 (C++ workload) or the Build Tools, CMake ≥ 3.28, Ninja, Qt 6.11 for
`msvc2022_64` (with *Qt Image Formats* and *Qt Shader Tools*), Python 3 and
[vcpkg](https://github.com/microsoft/vcpkg) (`VCPKG_ROOT`; dependencies come from `vcpkg.json`).
In an *x64 Native Tools Command Prompt*:

```bat
set CMAKE_PREFIX_PATH=C:\Qt\6.11.2\msvc2022_64
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release -L unit
```

Windows code goes into `src/platform/windows/` (`Win<Role>.cpp` replaces the generic
`Generic<Role>.cpp`) and `src/app/<Name>_win.cpp` next to `<Name>_mac.mm`; CMake parts into
`src/platform/windows/*.cmake` or `if(WIN32)` blocks. Own sources build with `/W4 /WX`. Packaging:
`tools/windows_deploy.py` + `packaging/windows/Lumaflip.iss`. More in [docs/WINDOWS.md](docs/WINDOWS.md).

### Checks

| Command | What it does |
|---|---|
| `tools/check.sh --quick --no-gui` | Build + unit tests (no windows). Run this before every commit. |
| `tools/check.sh --quick` | Build + all tests, including integration tests (`tst_*`) that open real windows |
| `tools/check.sh` | Everything CI runs: format check, SPDX headers, spec coverage, benchmarks |
| `tools/check.sh --quick --sanitize` | ASan/UBSan build |
| `tools/check.sh --quick --tidy` | Additionally clang-tidy on changed files |
| `tools/check.sh --format-all` | clang-format check for all files (default: only lines changed vs. `main`) |
| `python3 tools/spec_coverage.py --verbose` | Shows which test covers which requirement ID |

Integration tests and benchmarks open windows and load the machine — run them when you can leave
the Mac alone for a few minutes. CI runs the full set on every pull request.

## Project layout

| Path | Contents |
|---|---|
| `src/core/` | Platform-neutral core (C++20, QtCore/QtGui only): collection, sorting, decoder registry, image provider (prefetch/cache), file operations, settings, input bindings, `.DS_Store` parser |
| `src/core/Platform.h` | Platform interfaces and factory functions |
| `src/platform/mac/` | macOS implementations in Objective-C++ (`.mm`, ARC): ImageIO decoder, Finder sort order, name collation, Trash, FSEvents, shell |
| `src/platform/generic/` | Portable fallbacks (used by the Linux CI build) |
| `src/app/` | Qt Widgets UI: application, viewer window, image view (QRhi/Metal), panels, preferences |
| `src/app/shaders/` | GLSL shaders |
| `src/helper/` | `lumaflip-decoder`, the sandboxed decoder helper process |
| `tests/unit/` | GoogleTest unit tests (link `lf_core`) |
| `tests/integration/tst_*.cpp` | Qt Test integration tests (link `lf_ui`, one binary per file) |
| `tests/bench/` | Benchmarks for the performance requirements (`lf_bench`) |
| `tests/corpus/` | Format corpus; `make_corpus.sh` generates files into `tests/corpus/generated/` |
| `translations/` | Qt Linguist catalogues (`.ts`), one per language |
| `packaging/` | `Info.plist`, entitlements, localized `InfoPlist.strings`, licence texts, Homebrew cask template |
| `tools/` | Build, check, packaging and release scripts |
| `docs/` | Manual test plans, release process, FAQ — see [docs/README.md](docs/README.md) |

New source files are picked up automatically (CMake glob with `CONFIGURE_DEPENDS`).

## Spec-first: requirements and IDs

The binding specification is [SPEC.md](SPEC.md) (German). Every requirement has an ID such as
`F-SORT-01` or `P-03` and a milestone (M1–M4).

- **Proposing a feature:** open a feature request first. If it is accepted, the spec gets a new
  or changed requirement (with ID and milestone) **before** the implementation. A PR may include the
  spec change, but discuss larger features in an issue first so nobody wastes work.
- **Tests name the requirement ID** in the test name — only names count, not comments:
  `TEST(SortEngine, F_SORT_01_FinderNameOrder)`, `void F_NAV_01_arrowKeys();`, `LF_BENCH(P_05_…)`.
- Requirements that cannot reasonably be tested automatically go into
  [docs/MANUAL_CHECKS.md](docs/MANUAL_CHECKS.md) with a reason.
- `tools/spec_coverage.py` checks that every ID of the current stage is covered.

## Coding conventions

- Namespace `lf`, C++20. Prefer Qt types (`QString`, `QVector`, …). No exceptions across module
  boundaries.
<!-- REUSE-IgnoreStart -->
- Every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`
  (the repository follows [REUSE](https://reuse.software); see `REUSE.toml`).
<!-- REUSE-IgnoreEnd -->
- Formatting: `.clang-format` (checked on changed lines); static analysis: `.clang-tidy`.
  Own sources compile with `-Werror`.
- **The GUI never blocks:** file system access, decoding and metadata only on worker threads
  (`QThreadPool` / `QtConcurrent`).
- Make errors visible (error card, status message) instead of swallowing them.
- Public headers in `src/core/*.h` are interfaces between modules: keep changes small and explain
  them in the commit message.
- No private user data in test data — generate synthetic files (e.g. `.DS_Store`) with scripts.

## Internationalization

UI strings are written in **German** (the source language) and always go through `tr()`:

- Whole sentences with `%1` placeholders instead of concatenation; plurals with
  `tr("%n Bilder", nullptr, n)`.
- Outside `QObject` classes use `struct Tr { Q_DECLARE_TR_FUNCTIONS(lf::Context) };`; in `.mm` files
  `QCoreApplication::translate("lf::…", …)` or `lf::mac::translate()`.
- Tables use `QT_TRANSLATE_NOOP` and are translated when shown. Numbers, sizes and dates go through
  `core/LocaleFormat.h`.
- After changing strings: `cmake --build build/check --target lf_lupdate` updates
  `translations/*.ts`. `tools/i18n_check.py` (part of `check.sh`) flags strings without `tr()` and
  stale catalogues.

If German isn't your language, write the string as well as you can and mention it in the PR — the
maintainer will polish it. Details: [translations/README.md](translations/README.md).

## Commit messages

- One logical change per commit. Summary line in the imperative or as a short description,
  ≤ 72 characters, no trailing period; English or German are both fine.
- In the body, explain *why*, and reference requirement IDs (`F-CULL-02`) and issues (`#123`).
- Don't add generated trailers or e-mail addresses beyond your own author identity. Using your
  GitHub `noreply` address as the author e-mail is recommended.

## Licensing of contributions

Lumaflip is licensed under **GPL-3.0-or-later**. Contributions are accepted under the same licence
(*inbound = outbound*): by opening a pull request you agree that your contribution is licensed
under GPL-3.0-or-later. There is **no CLA**. A `Signed-off-by` line
([Developer Certificate of Origin](https://developercertificate.org)) is welcome but not required.

Only contribute code you wrote yourself or that is under a GPL-3.0-compatible licence — say so in
the PR when you include third-party code, and add its licence to `LICENSES/` / `REUSE.toml`.

## Pull request checklist

- [ ] The change is described in an issue or in the spec (for features)
- [ ] `tools/check.sh --quick --no-gui` passes locally
- [ ] New behaviour has tests named after the requirement ID, or an entry in `docs/MANUAL_CHECKS.md`
- [ ] UI strings go through `tr()`, and `lf_lupdate` was run if strings changed
- [ ] New files carry an SPDX header
- [ ] User-visible changes are noted in [CHANGELOG.md](CHANGELOG.md) under *Unreleased*
- [ ] Docs updated (README, SPEC, docs/) where needed

The [pull request template](.github/PULL_REQUEST_TEMPLATE.md) contains the same list.
