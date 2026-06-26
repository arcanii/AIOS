# Vendored third-party source

These trees are vendored **unmodified** for reproducibility. AIOS compiles a curated subset of
their sources against `libaios` (the AIOS-ABI libc) with the `-nostdinc` shadow headers — proving
that *real, unmodified* POSIX-utility source compiles and runs on the AIOS userspace kernel. The
build rules live in `uk/Makefile`; the sbase sources themselves are never patched. When a utility
needs a libc feature AIOS does not have yet, the feature is added to `libaios` — not worked around
in the vendored tree.

## sbase

- Upstream: <https://git.suckless.org/sbase>
- Commit: `c546c3a5724c81cee9a11d816a38ccdf17472129` (2026-05-25)
- License: MIT (see `sbase/LICENSE`)
- Vendored as-is, only the `.git` directory removed.

To re-vendor at the pinned revision:

```sh
git clone https://git.suckless.org/sbase
cd sbase && git checkout c546c3a5724c81cee9a11d816a38ccdf17472129 && rm -rf .git
```

sbase's own `Makefile`/`config.mk` are **not** used (they target a hosted libc). AIOS builds each
curated utility as `sbase-<name>` from its source plus the `libutil`/`libutf` objects it needs plus
`lib/libaios.c`, with `$(LIBC_CFLAGS) -Ivendor/sbase`.

## dash (the operational shell)

- Upstream: <https://git.kernel.org/pub/scm/utils/dash/dash.git> (mirror
  <https://github.com/tklauser/dash>)
- Version: 0.5.11 (commit `057cd650`)
- License: BSD (3-clause)
- The dash **sources are unmodified upstream**. Two kinds of file under `vendor/dash/` are AIOS
  build inputs, not upstream:
  - `config.h` — the AIOS cross-compile config (force-included via `-include`). It tunes `HAVE_*`
    to what libaios provides (and lets dash self-provide the rest via `system.c`), and sets
    `JOBS 0` / `SMALL` / `GLOB_BROKEN`.
  - the **generated** sources in `src/` — `token.h`, `token_vars.h`, `syntax.{c,h}`, `nodes.{c,h}`,
    `signames.c`, `builtins.{c,h}`, `builtins.def`, `init.c` — produced by dash's own generators
    (`mktokens`, `mksyntax`, `mknodes`, `mksignames`, `mkbuiltins`, `mkinit`) run with the host
    compiler against `config.h`. Regenerate by running those generators in `src/` (see the recipes
    in `src/Makefile.am`).

AIOS builds the `dash` binary in one `cc` invocation over the dash CFILES + the generated sources +
`lib/libaios.c`, with `$(LIBC_CFLAGS) -Ivendor/dash/src -include vendor/dash/config.h -DSHELL -DSMALL
-DGLOB_BROKEN -D_GNU_SOURCE -Dalloca=__builtin_alloca` (see the `dash` rule in `uk/Makefile`). dash
runs builtins, arithmetic, control flow, loops, pipelines, command substitution, and redirection on
the AIOS kernel.
