# ADR-0065: Pasted rule-script translation

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0048 versioned previewed metadata transformation chains and
  ADR-0064 typed keep-first metadata transformation

## Context

Users often already have a compact cleanup script describing many tag rules.
Recreating every line through individual controls is slow and obscures the
useful migration path from MusicBrainz Picard. Directly executing or persisting
Picard code would conflict with Trackbench's repository-owned deterministic
language and declarative preview requirements.

The first concrete script needs unconditional field removal, removal based on
`TOTALDISCS`, conditional derivation of `DATE`, and four-character date
prefixes. Existing chains express all but conditional field removal.

Picard also distinguishes `$unset` from `$delete`: current Picard documentation
says `$delete` marks an existing tag for deletion while `$unset` only unsets the
script variable. Trackbench has one staged current document rather than
Picard's old/new metadata layers, so that distinction cannot be copied
silently.

## Decision

- The transformation editor adds **Paste script…**. A window translates the
  pasted text, displays source-positioned errors and warnings, and can append
  or replace the current editable rule list.
- This is a bounded Picard-style convenience translator, not a Picard runtime,
  persisted compatibility dialect, or claim that arbitrary Picard scripts run.
  The source is discarded after translation; saved chains contain only normal
  Trackbench declarative actions and `tkfmt-1` expressions.
- The initial mutation subset is `$unset`, `$delete`, `$set`, and `$if`.
  Pure expressions accept fields plus `$if`, `$if2`, `$and`, `$or`, `$not`,
  `$eq`, `$ne`, and positive-count `$left` behavior that validates as
  `tkfmt-1`. Unsupported calls are errors and generate no usable import.
- A translated `$if` accepts Picard's two-argument form with no false branch as
  well as the three-argument form. Adjacent mutation calls form one branch and
  execute in order; they are not comma-separated because commas delimit the
  `$if` arguments themselves. Arity diagnostics report the argument count.
- Layout-only ASCII whitespace around structural expressions is ignored. This
  deliberately makes indented pasted scripts convenient and differs from
  Picard's whitespace-sensitive evaluation. A top-level comma or unmatched
  closing parenthesis following a complete rule is ignored with a visible
  warning, allowing recovery from common pasted statement punctuation.
- `$unset(name)` and `$delete(name)` both generate Trackbench's actual
  **Remove exact native field** action per ADR-0066. ASCII case follows the
  adapter, while separators and punctuation remain identity. The importer
  explanation and a source-positioned `$unset` warning state this semantic
  difference before rules can be accepted. Wildcard target names are rejected.
- `$set(field,$left(%field%,N))` generates the typed keep-first action. Other
  supported value expressions generate a scalar `tkfmt-1` format action.
  Matching field assignments across `$if` branches become one conditional
  scalar format action. A true-branch-only self-prefix cleanup guarded by the
  same field becomes an ordinary keep-first action: it is already a no-op when
  that field is absent. Other branch-only assignments fail closed.
- Picard's exact default-comment target `comment:` maps to Trackbench's
  conventional `COMMENT` field with a visible warning. Wildcard targets such as
  `comment:*` remain unsupported rather than silently broadening a deletion.
- Conditional unset/delete branches generate the new typed
  `MetadataRemoveFieldIfAction`. Its versioned `tkfmt-1` condition evaluates
  against the working document at that exact chain position; a non-empty result
  removes the target and an empty result leaves it unchanged. The action is
  also directly available as **Remove field when condition matches**.
- Reversible SQLite migration 18 reserves action code 15 for conditional
  removal and stores its condition plus complete `tkfmt-1` dialect identity.
  Downgrade to schema 17 refuses while a code-15 row remains.
- Reversible migration 19 adds the exact-native match mode to unconditional
  and conditional removal without reinterpreting previously saved actions.
- Import is limited to 1 MiB of valid UTF-8 source, 4,096 syntax nodes, 64
  nesting levels, and 256 generated actions. Translation and chain validation
  are Qt-free. Generated rules still require normal chain preview, optional
  staging, final fresh write-plan review, and explicit Apply.

## Alternatives considered

### Execute Picard scripts directly

Rejected. It would introduce a second mutation runtime, inherit external
language changes and whitespace quirks, and make persisted behavior depend on
an implementation Trackbench does not own.

### Store the pasted source as one opaque action

Rejected. Users could not inspect, reorder, or edit the generated behavior,
and preview diagnostics could not identify the actual typed operation that
changed a field.

### Silently skip unsupported calls

Rejected. Partial cleanup scripts are dangerous because the preview may look
plausible while an intended deletion or condition was omitted. Unsupported
syntax disables import until corrected.

### Treat `$unset` as a no-op for existing fields

Rejected. Trackbench has no Picard-style newly loaded metadata layer, and the
purpose of the imported cleanup list is to remove the named fields. The
translator makes the stronger Trackbench meaning explicit instead.

## Consequences

- The supplied cleanup script expands into 20 visible rules rather than 20
  repetitive manual entries or one opaque program.
- Generated rules can be renamed, reordered, removed, saved, checked for
  automatic tagging, and previewed exactly like hand-authored rules.
- Full Picard scripting and native chain serialization import/export remain
  separate work. Extending the paste subset requires explicit translation
  semantics and tests; it never changes `tkfmt-1` itself.

## Validation

- Qt-free tests translate the supplied script, including adjacent mutations in
  both `$if` branches and the default-comment mapping, into the expected
  unconditional, conditional, formatted, and keep-first actions; three metadata
  shapes prove the disc/date results.
- Unsupported mutation functions and invalid conditional expressions fail
  closed.
- Persistence restart coverage round-trips action code 15 and its complete
  dialect identity through schema 18; ADR-0066's schema 19 additionally retains
  the importer's exact-native match mode.
- Offscreen editor coverage pastes a smaller cleanup sample, observes three
  readable generated rules, replaces the list, and retains the existing saved
  chain preview/stage workflow.

## References

- [Picard assignment functions](https://picard-docs.musicbrainz.org/en/latest/functions/list_by_type.html)
- [Picard `$if`](https://picard-docs.musicbrainz.org/en/latest/functions/func_if.html)
- [Picard `$unset`](https://picard-docs.musicbrainz.org/en/latest/functions/func_unset.html)
- [Picard `$delete`](https://picard-docs.musicbrainz.org/en/latest/functions/func_delete.html)
- [Picard tag mapping](https://picard-docs.musicbrainz.org/en/latest/appendices/tag_mapping.html)

## Revisit when

- another concrete script justifies more pure or mutation calls;
- native chain import/export receives a stable interchange schema;
- wildcard performer/comment/lyrics cleanup has a Trackbench-owned typed rule;
- conditional set needs a directly editable typed action beyond same-target
  `$if` branch translation.
