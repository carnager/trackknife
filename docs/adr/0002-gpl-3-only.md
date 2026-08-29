# ADR-0002: License Trackknife under GPL-3.0-only

- Status: accepted
- Date: 2026-08-23
- Owners: Trackknife project

## Context

Trackknife is a personal open-source project. It uses Qt 6 under Qt's
LGPLv3 option and will incorporate other open-source audio, metadata, database,
and Linux platform libraries. The project needs an explicit application license
before source code and contributions accumulate.

## Decision

License Trackknife as a whole under the GNU General Public License version 3
only (`GPL-3.0-only`). The repository root contains the complete license in
`LICENSE`.

- New original source files should carry `SPDX-License-Identifier:
  GPL-3.0-only` in the comment syntax of their language.
- Documentation and assets are covered by the repository license unless a file
  states a different compatible license explicitly.
- Dependencies retain their own licenses and notices.
- Contributions are accepted under the same `GPL-3.0-only` terms unless a
  future contribution policy says otherwise.

Qt remains dynamically consumed under LGPLv3; Trackknife being GPLv3 does not
change Qt's license or remove the obligation to ship its notices and permit
replacement of the LGPL-covered Qt libraries.

## Alternatives considered

- GPL-3.0-or-later: allows automatic adoption of later GPL versions, but was not
  explicitly selected.
- LGPL-3.0-only for Trackknife: permits proprietary derivatives of the
  application itself, which is not the chosen project policy.
- Permissive licenses: do not provide the desired copyleft for derivatives.

## Consequences

- Distributed derivative works of Trackknife must comply with GPLv3.
- Every dependency and bundled asset must be checked for GPLv3 compatibility.
- GPL-only Qt modules may be usable when their exact GPL version is compatible,
  but each module still requires a license audit rather than assumption.
- Source distributions must include the license and corresponding-source
  obligations must be met for binaries.

## Validation

- CI should eventually run a dependency/license inventory.
- Release packaging tests must confirm that `LICENSE`, dependency notices, and
  corresponding source/build information are present.

## Revisit when

Only revisit through an explicit relicensing decision with permission from all
relevant copyright holders.
