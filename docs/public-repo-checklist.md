# Public Repo Checklist

- Run `ctest --test-dir build`.
- Check for accidental credentials with a dedicated scanner or a targeted text
  search before publishing.
- Check for local absolute paths by searching for machine-specific home and
  temporary-directory prefixes.
- Check for stale scratch artifacts:
  `find . -maxdepth 3 -type f \( -name "*.tmp" -o -name "*.log" \)`
- Keep generated build outputs out of git.
