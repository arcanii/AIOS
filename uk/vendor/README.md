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
