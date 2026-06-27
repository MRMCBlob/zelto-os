# Coding Standards

Conventions for contributing to Zelto OS (C system code, the SDK, and Script in the shell).
For the public app-facing API conventions see
[../api-reference/conventions.md](../api-reference/conventions.md).

## C

### Style
- **C17**, compiled with `-Wall -Wextra -Werror` (warnings are errors).
- 4-space indent, no tabs. ~100-column soft limit.
- Braces on the same line; always brace `if`/`for`/`while` bodies.
- `snake_case` functions/variables, `Z`-prefixed `PascalCase` types, `Z_` SCREAMING enums
  (matches the public API, [../api-reference/conventions.md](../api-reference/conventions.md)).

### Naming & headers
- Public SDK symbols: `z_` prefix; declared in `sdk/include/zelto/*.h`. Changing these is a
  documented API change ([../api-reference/c/](../api-reference/c/)).
- Internal symbols: `static` or a module prefix; never exported.
- One module = one `.c` + `.h`; include what you use.

### Memory
- Document ownership at every boundary: who allocates, who frees.
- Per-frame UI nodes use the build **arena** (no manual free); long-lived objects are
  **reference-counted** (`z_retain`/`z_release`) ([sdk-internals.md](sdk-internals.md)).
- No leaks: every `malloc` has a clear owner; run ASan in CI.

### Errors
- Return a status / `NULL` / `false`; set the last error. Don't `abort()` in library code.
- Check every fallible call; never ignore returns silently.
- `z_assert` for programmer errors (debug-only), not for runtime/user errors.

### Concurrency
- UI + Script state is **app-loop only**. Cross-thread results post back via `z_post`
  ([../api-reference/conventions.md](../api-reference/conventions.md)).
- Guard shared state; prefer message passing over locks where practical.

## Zelto Script (shell & samples)

- Follow the app-facing conventions: PascalCase components, camelCase functions, hooks at
  the top level ([../zelto-script/language.md](../zelto-script/language.md)).
- Keep components small and pure; side effects in `useEffect`.
- Optional TypeScript-style annotations encouraged for shared modules.

## Commits & reviews

- Conventional, imperative commit subjects ("Add layer-shell anchor handling").
- One logical change per commit; keep diffs reviewable.
- Cross-component changes land together (mono-repo,
  [repo-layout.md](repo-layout.md)).
- PRs describe the change, the why, and how it was tested.

## Testing

- Unit tests for logic (`meson test`).
- UI smoke tests via the headless simulator
  ([../tooling/simulator.md](../tooling/simulator.md)).
- Add/extend tests with behavior changes; don't regress performance budgets
  ([../tooling/profiling.md](../tooling/profiling.md)).

## Documentation

- Update the relevant `docs/` page in the same PR as an API or behavior change.
- Public C headers and Script modules must match their reference pages
  ([../api-reference/](../api-reference/)).

## Formatting & linting

- `clang-format` (config in repo) for C; CI checks formatting.
- `zelto fmt` / lint for Script.
- CI gate: format check · `-Werror` build · unit tests · UI smoke.
