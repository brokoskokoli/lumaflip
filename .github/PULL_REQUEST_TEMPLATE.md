<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
## What and why

<!-- What does this change, and why? Link the issue: "Fixes #123". -->

Requirement IDs: <!-- e.g. F-CULL-02, P-03 — or "none (docs/build)" -->

## How it was tested

<!-- Commands you ran, manual steps, sample files (no private photos). -->

## Checklist

- [ ] The change is described in an issue or in [SPEC.md](../SPEC.md) (for features: spec first)
- [ ] `tools/check.sh --quick --no-gui` passes locally
- [ ] New behaviour has tests named after the requirement ID, or an entry in `docs/MANUAL_CHECKS.md`
- [ ] UI strings go through `tr()`; `lf_lupdate` was run if strings changed
- [ ] New files carry an SPDX header (`GPL-3.0-or-later`)
- [ ] User-visible changes are noted in `CHANGELOG.md` under *Unreleased*
- [ ] Docs updated where needed (README, SPEC, docs/)

By submitting this pull request I agree that my contribution is licensed under GPL-3.0-or-later
(see [CONTRIBUTING.md](../CONTRIBUTING.md#licensing-of-contributions)).
