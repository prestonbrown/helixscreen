# esp_websocket_client (vendored)

Espressif's WebSocket client, version **1.8.0**, from
[espressif/esp-protocols](https://github.com/espressif/esp-protocols/tree/master/components/esp_websocket_client)
at commit `70bf122fc8bc74622dcf7e15233b5b2af9088df5`, Apache-2.0 (`LICENSE`).

It is a local component so the firmware can carry changes to it: ESP-IDF builds a local
component in place of the managed one of the same name, and nothing declares the managed
dependency. The import is the component's sources, headers, build files and licence only,
byte-identical to the registry package; `examples/`, `tests/` and the changelog are left out.

Our changes are the commits on top of the import commit, so `git log -- <this directory>`
lists them. To move to a newer upstream: re-import that version as one commit, then replay
those commits.
