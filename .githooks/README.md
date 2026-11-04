# Git hooks

These hooks keep Cursor / `cursoragent` co-author trailers out of commit messages.

- `prepare-commit-msg` — strips `Co-authored-by` (and Cursor `Signed-off-by`) lines
- `commit-msg` — rejects the commit if any `Co-authored-by` line remains

## Install (once per clone)

```bash
cp -f .githooks/prepare-commit-msg .githooks/commit-msg .git/hooks/
chmod +x .git/hooks/prepare-commit-msg .git/hooks/commit-msg
```

Or point Git at this directory:

```bash
git config core.hooksPath .githooks
```
