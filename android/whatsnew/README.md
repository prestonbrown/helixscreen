# Play Store "What's new" text

One file per release that ships to Google Play: `<version>.txt`, named for the exact
string in `VERSION.txt` (`1.1.0.txt`). Written by hand and committed in the release
commit.

- **Plain text**, no markdown. Play shows it as-is.
- **At most 500 characters**, counted the way Play counts them (UTF-16 code units, so
  an emoji can cost two). `scripts/generate-whatsnew.sh` uses the file verbatim and
  fails the release when it is longer; it never truncates.
- **For a Play Store reader**: what changed for them in this release, in a few lines.
  The full story stays in `CHANGELOG.md`.

Betas, alphas and release candidates need no file: they never go to Play, and the
script distills their `CHANGELOG.md` section instead. A stable release with no file
fails the release workflow, naming the file to write.

Check a file before tagging:

```bash
scripts/generate-whatsnew.sh /tmp/whatsnew-preview.txt && cat /tmp/whatsnew-preview.txt
```
