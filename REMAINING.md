# What remains before Emerald is a publishable language and ecosystem

> Implementation pass of 2026-09-18: the P0 release blockers (broken tarball,
> shell injection, no CI/tags), the P1 fixes (pme add/why/update/--locked and
> I/O forwarding, emlsp README truth, VS Code scaffold, stdlib filesystem/env
> builtins, `#line` emission, examples gating, community files) and the
> documentation-accuracy defects have landed. The original 561-line audit with
> its evidence log is in git history (`0bb29b7`); this file lists only what is
> still open.



## Language and stdlib

- [ ] Stdlib breadth beyond filesystem/env: time/date, JSON, hashing, random
      with explicit RNG state. (Net/crypto/compression are out of scope until
      the FFI decision in [`docs/stability.md`](docs/stability.md) is revisited.)
- [ ] The proof-mode soundness work, recursive data types, and constrained
      generics tracked in [`docs/REMAINING_FEATURES.md`](docs/REMAINING_FEATURES.md).
