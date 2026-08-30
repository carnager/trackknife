# ADR-0069: Source-tag authority for path-only preparation

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Supersedes: ADR-0067's naming-only draft context when Save tags is off
- Extends: ADR-0054 unified preparation plans and ADR-0067 immutable
  preparation review

## Context

ADR-0067 allowed manual draft values and checked automatic transformations to
drive Rename/Move while **Save tags** was off. The review labeled those values
as **Naming only**, but the resulting pathname did not represent the metadata
stored in the source file. A file could therefore be named as though a tag had
one value while retaining a different embedded value.

That is not an acceptable interpretation of independent operation choices.
Turning tag persistence off also turns uncommitted tag transformations off for
path evaluation. A changed tag may drive a path only when that tag change is
part of the same immutable reviewed operation.

## Decision

- A Rename/Move plan with **Save tags** off materializes its naming document
  from the captured source metadata snapshot with an empty patch set.
- Manual draft edits remain in the Properties workspace, but they are excluded
  from the path plan. Checked automatic chains are not evaluated for that plan.
- The captured metadata remains revision-qualified. Fresh filesystem preflight
  rejects a source whose content or identity changed after capture, so a stale
  metadata snapshot cannot reach Apply.
- `PreparationPlan` rejects any path-only input that carries a metadata-context
  change count or a metadata write plan. This makes the rule a Qt-free planner
  invariant instead of relying on UI labels.
- When **Save tags** is on, manual and automatic changes still form the final
  metadata context. Changed tags plus paths remain blocked until the direct
  destination-artifact executor can publish both effects as one operation.

No schema migration is required.

## Alternatives considered

### Label transformed values as “Naming only”

Rejected. A more prominent label does not make a filename derived from
nonexistent tag values truthful.

### Apply automatic chains but ignore manual edits

Rejected. Both are uncommitted synthetic metadata. Giving either source hidden
authority would make operation selection inconsistent.

### Silently write the transformed tags

Rejected. That would violate the explicit **Save tags** choice and the
preview/commit boundary.

## Consequences

- A path-only preview always describes the metadata actually captured from the
  source and never a temporary transformation result.
- Users who want transformed tags and matching paths must select both effects;
  Apply remains unavailable until combined destination publication is proven.
- An unsaved manual draft may coexist with a path-only operation without being
  discarded or influencing it.

## Validation

- Qt-free preparation tests reject metadata context when **Save tags** is off.
- Offscreen UI coverage stages a fake title and enables an automatic chain with
  another fake title, then proves the reviewed and applied target uses the
  source file's actual title.

## Revisit when

- direct destination-artifact publication qualifies changed tags plus paths;
- a future operation explicitly offers a non-tag-derived naming input with a
  distinct, truthful label and contract.
