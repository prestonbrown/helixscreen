# Regenerate assets/filaments.json from the OrcaSlicer filament library.
# Shallow-clones Orca at a pinned tag into scratch; ships nothing AGPL.
ORCA_TAG ?= v2.4.1
ORCA_TMP := $(CURDIR)/build/orca-profiles

.PHONY: regen-filaments
regen-filaments:
	@echo "==> Fetching OrcaSlicer profiles at $(ORCA_TAG)"
	@rm -rf "$(ORCA_TMP)"
	@git clone --depth 1 --branch $(ORCA_TAG) --filter=blob:none --sparse \
		https://github.com/SoftFever/OrcaSlicer "$(ORCA_TMP)"
	@cd "$(ORCA_TMP)" && git sparse-checkout set resources/profiles
	@python3 scripts/import_orca_filaments.py \
		--orca "$(ORCA_TMP)/resources/profiles" \
		--cfs-seed scripts/fixtures/cfs_seed.json \
		--type-ranges scripts/fixtures/type_ranges.json \
		--orca-tag $(ORCA_TAG) \
		--out assets/filaments.json
	@rm -rf "$(ORCA_TMP)"
	@echo "==> Regenerated assets/filaments.json"
