<!-- SPDX-License-Identifier: GPL-3.0-only -->

# M1 foobar2000 reference probes

These probes resolve compatibility behavior that the public title-formatting
reference does not specify precisely. Record the exact foobar2000 version,
Windows version, evaluation surface, visible output, and whether the script was
accepted. Do not add results to the black-box corpus without that provenance.

Use a local test track whose filename without extension is `fallback-title`.
Create these metadata variants if the tag editor permits them:

- `multi`: ARTIST has two ordered values, `First` and `Second`.
- `empty`: ALBUM ARTIST and TITLE are present but empty; ARTIST is
  `Fallback Artist`; PRESENT is `yes`.
- `case`: ALBUM ARTIST is `same`; ARTIST is `SAME`; PRESENT is `yes`.

If foobar2000 removes rather than preserves an empty value, record that fact and
skip the two present-empty probes rather than substituting a missing tag.

## Field and metadata probes

| ID | Fixture | Script | Record |
| --- | --- | --- | --- |
| `field.multi.display` | `multi` | `X%artist%Y` | Exact visible text. |
| `field.multi.truth` | `multi` | `$if(%artist%,T,F)` | `T` or `F`. |
| `field.empty.album-artist` | `empty` | `X%album artist%Y$if(%album artist%,T,F)` | Whether fallback occurs and truth. |
| `field.empty.title` | `empty` | `X%title%Y$if(%title%,T,F)` | Whether filename fallback occurs and truth. |
| `field.track-artist.case` | `case` | `X[%track artist%]Y` | Whether case-only differences count. |
| `meta.test.text` | `multi` | `X$meta_test(artist)Y$if($meta_test(artist),T,F)` | Whether true emits text as well as truth. |
| `meta.missing` | any | `X$meta(missing)Y$if($meta(missing),T,F)` | Missing output and truth. |
| `meta-num.missing` | any | `X$meta_num(missing)Y$if($meta_num(missing),T,F)` | Missing count output and truth. |

## Evaluation-order probes

Run these on a fixture with PRESENT set to `yes`.

| ID | Script | Interpretation |
| --- | --- | --- |
| `bool.and.order` | `$puts(x,start)$and($puts(x,A),$puts(x,B))$get(x)` | `A` means short-circuit after false; `B` means both arguments ran left-to-right. |
| `bool.or.order` | `$puts(x,start)$or($puts(x,A)%present%,$puts(x,B))$get(x)` | `A` means short-circuit after true; `B` means both arguments ran left-to-right. |
| `function.argument.order` | `$puts(x,start)$strcmp($puts(x,A),$puts(x,B))$get(x)` | `B` means left-to-right; `A` means right-to-left. |

## Case-folding probes

The bundled 2.25.10 syntax reference defines `$stricmp()` as case-insensitive
but does not specify its Unicode case-folding rules. Record the exact output of:

```text
X$stricmp('Ä','ä')Y$if($stricmp('Ä','ä'),T,F)
X$stricmp('Straße',STRASSE)Y$if($stricmp('Straße',STRASSE),T,F)
```

## Integer and zero-argument probes

Record exact output or rejection for each script:

```text
$div(-7,3)
$muldiv(1,1,2)
$muldiv(-1,1,2)
$muldiv(1,2,0)
$add(9223372036854775807,1)
$add(+12,0)
$iflonger('',-1,T,F)
$char(0)
$char(55296)
$char(1114112)
$if($and(),T,F)
$if($or(),T,F)
$if($xor(),T,F)
$add()
$mul()
```

Also record the exact default indicator emitted by
`$fix_eol(first$crlf()second)`.

## Result template

```text
foobar2000 version:
Windows version:
evaluation surface:
fixture preparation notes:

probe ID | accepted | exact output | notes
```
