<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Security Policy

Lumaflip opens files that may come from anywhere — downloads, memory cards, network shares. A
crafted image must never compromise your Mac, so we take parser and sandbox bugs seriously.

## Supported versions

Only the latest release receives security fixes. Updates are delivered through the built-in
updater (Sparkle) and on the [Releases](https://github.com/brokoskokoli/lumaflip/releases) page.

| Version | Supported |
|---|---|
| 1.1.x (latest) | ✅ |
| < 1.1 | ❌ (internal milestones, never published) |

## Reporting a vulnerability

**Please do not open a public issue, discussion or pull request for security problems.**

Report privately via GitHub: open the repository's **Security** tab and click
**[Report a vulnerability](https://github.com/brokoskokoli/lumaflip/security/advisories/new)**
(private vulnerability reporting / security advisories). Only the maintainer can see the report.

Please include:

- the Lumaflip version (*About Lumaflip*) and macOS version, Mac model;
- a description of the issue and its impact;
- a minimal proof-of-concept file or steps to reproduce (attach it to the advisory — never post it publicly);
- any crash log (`~/Library/Logs/DiagnosticReports/Lumaflip*` or `lumaflip-decoder*`).

What to expect:

- acknowledgement within **7 days**;
- an assessment and, for confirmed issues, a fix plan within **30 days**;
- coordinated disclosure: we publish a GitHub security advisory together with the fixed release and
  credit you unless you prefer to stay anonymous.

This is a volunteer project; there is no bug bounty.

## Scope

In scope — please report:

- **Decoder and parser bugs** reachable by opening a file: memory corruption, out-of-bounds reads,
  infinite loops/hangs, excessive memory use. This includes Lumaflip's own parsers
  (`.DS_Store` reader, XMP reader/writer, QOI, PCX, PNM/PFM, SGI, OpenRaster, Krita, XCF, PSB, the
  decoder IPC protocol) and how Lumaflip drives Qt image plugins and resvg.
- **Decoder helper sandbox escapes**: `lumaflip-decoder` runs non-system decoders in a separate
  process with a sandbox that denies network access and file writes. Anything that lets a file
  break out of it, or makes the app accept an unsandboxed helper, is in scope.
- **File operations** that touch the wrong file: e.g. trash/move/rename/rating writes that follow
  symlinks or escape the intended folder, non-atomic writes that can lose data.
- **Update channel**: anything that bypasses Sparkle's EdDSA signature check or the appcast
  verification.
- **Privacy**: network traffic other than the documented update check and map tiles (which load
  only after you click "Show Map" / open the folder map), or data leaving the machine unexpectedly.

Out of scope:

- Bugs in Apple ImageIO or other macOS frameworks — report those to
  [Apple Product Security](https://security.apple.com). Tell us too if Lumaflip could mitigate them.
- Vulnerabilities in upstream libraries (Qt, resvg, libjpeg-turbo, Sparkle) that Lumaflip does not
  make worse — please report upstream; we will ship updated versions.
- Crashes in the decoder helper that are correctly contained (the app shows an error card and keeps
  working) — these are regular bugs, please use a
  [bug report](https://github.com/brokoskokoli/lumaflip/issues/new?template=bug_report.yml) with a
  sample file.
- Self-built or modified binaries, ad-hoc-signed development builds.
