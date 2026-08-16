# ABI contract layout

`scan_mediaplayer_requirements.py` emits the contract consumed by the runtime
resolver. The canonical form has `schema_version: 1`, source identity fields,
`header_paths`, and `platforms.windows.requirements` plus
`platforms.linux.requirements`. Each requirement has an ordered `ordinal`, an
`ES_*` `name`, `kind`, `value_type`, and source `location`. The scanner's
`diagnostic_original_value` is provenance-only source context; resolution uses
only runtime report values.

Runtime report `environment.source_ref` and `environment.source_commit` identify
the Endstone runtime and are kept separate from the contract's `mediaplayer`
repository/ref/commit/fingerprint. Primary reports must be `complete: true` for
final resolution. Optional fallback reports use the same environment shape but
may only carry compile/static provenance; they may not claim runtime provenance.
Intermediate single-platform manifests are not final artifacts.
