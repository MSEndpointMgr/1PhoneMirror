# Project Governance

This document describes how decisions are made for **1PhoneMirror** and
who is authorised to approve release artifacts for code signing.

## Maintainers

| Role               | Person                  | GitHub handle         |
|--------------------|-------------------------|------------------------|
| Project lead       | Simon Skotheimsvik      | @SimonSkotheimsvik     |

The project lead has final say on all technical and release decisions
and is the sole maintainer at this time. Additional maintainers may be
added by the project lead.

## Decision making

- **Code changes**: accepted through GitHub pull requests, reviewed and
  merged by the project lead.
- **Releases**: cut from `main` by the project lead using the local,
  signed release routine (`scripts/release.ps1`, Azure Trusted Signing),
  then published to GitHub Releases by hand. CI does not build or release
  the MSI — see [DISTRIBUTION.md](DISTRIBUTION.md).
- **Security advisories**: handled per [SECURITY.md](SECURITY.md).

## Release signing

Official `1PhoneMirror-*.msi` artifacts are **signed via Azure Trusted
Signing** (Public Trust). The MSI and every bundled executable/DLL carry an
Authenticode signature with an RFC 3161 timestamp. Signing is performed
locally as part of `scripts/release.ps1`; no signing certificate lives in CI.
Users can additionally verify the SHA-256 hash published on the GitHub
Release page before installing — see [SECURITY.md](SECURITY.md#release-integrity).
