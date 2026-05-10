## Summary

Describe the change and the user-visible behavior it affects.

## Testing

List the commands you ran.

```bash
cmake --preset ci
cmake --build --preset ci --parallel
ctest --preset ci

cmake --preset dev-tidy
cmake --build --preset dev-tidy --parallel

cmake --preset dev-sanitize
cmake --build --preset dev-sanitize --parallel
ctest --preset dev-sanitize
```

## Checklist

- [ ] Tests were added or updated where behavior changed.
- [ ] Documentation was updated where user-facing behavior changed.
- [ ] No generated build output, private agent files, or prompt/spec artifacts are included.
