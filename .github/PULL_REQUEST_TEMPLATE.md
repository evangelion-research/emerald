<!--
  One logical change per PR. Include the test (golden, e2e, or Go) that pins it.
-->

## What and why

<!-- What does this PR do, and why is it needed? Link the issue. -->

## Stability boundary

Does this change anything inside the boundary in `docs/stability.md`
(syntax, stdlib signatures, CLI flags, `--json` schema, diagnostic codes,
the `-I` contract)?

- [ ] No
- [ ] Yes — explained above, changelog entry added

## Checklist

- [ ] `task test` passes locally (includes examples)
- [ ] New behavior has a test
- [ ] If goldens were blessed: `git diff` on `tests/**/*.expected` reviewed line by line
- [ ] Docs updated in the same PR
