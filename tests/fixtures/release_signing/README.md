# Release-signing test fixtures

Signatures over `manifest.json` made with throwaway minisign keys whose secret halves were
deleted after signing; nothing here relates to the real release key.

| File | Made with |
|------|-----------|
| `test.pub` | `minisign -G -W` (the key the tests trust) |
| `other.pub` | a second `minisign -G -W` key |
| `good.minisig` | `minisign -S` with the test key (prehashed) |
| `legacy.minisig` | `minisign -S -l` with the test key (legacy, not prehashed) |
| `other.minisig` | `minisign -S` with the other key |

Every signature carries the trusted comment `file:manifest.json channel:dev`. Changing
`manifest.json` invalidates them; regenerate all three with a fresh pair of keys.
