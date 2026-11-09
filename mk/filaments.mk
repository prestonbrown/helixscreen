# Regenerate assets/filaments.json from the OrcaSlicer filament library.
# Shallow-clones Orca at pinned tags into scratch; ships nothing AGPL.
# Temps and the product list come from ORCA_TAG, the orca_ids from ORCA_ID_TAG
# (OrcaSlicer 2.5 minted a new filament_id for every product). The importer
# refuses to change the ids tag a catalog names unless ALLOW_ID_TAG_CHANGE=1.
ORCA_TAG ?= v2.4.1
ORCA_ID_TAG ?= v2.5.0-beta
ORCA_TMP := $(CURDIR)/build/orca-profiles
ORCA_ID_TMP := $(CURDIR)/build/orca-id-profiles

define orca_sparse_clone
	@rm -rf "$(2)"
	@git clone --depth 1 --branch $(1) --filter=blob:none --sparse \
		https://github.com/SoftFever/OrcaSlicer "$(2)"
	@cd "$(2)" && git sparse-checkout set resources/profiles
endef

.PHONY: regen-filaments
regen-filaments:
	@echo "==> Fetching OrcaSlicer profiles at $(ORCA_TAG), filament ids at $(ORCA_ID_TAG)"
	$(call orca_sparse_clone,$(ORCA_TAG),$(ORCA_TMP))
	$(call orca_sparse_clone,$(ORCA_ID_TAG),$(ORCA_ID_TMP))
	@python3 scripts/import_orca_filaments.py \
		--orca "$(ORCA_TMP)/resources/profiles" \
		--orca-ids "$(ORCA_ID_TMP)/resources/profiles" \
		--orca-id-tag $(ORCA_ID_TAG) \
		$(if $(ALLOW_ID_TAG_CHANGE),--allow-id-tag-change) \
		--seed scripts/fixtures/filament_seed.json \
		--orca-tag $(ORCA_TAG) \
		--out assets/filaments.json
	@rm -rf "$(ORCA_TMP)" "$(ORCA_ID_TMP)"
	@echo "==> Regenerated assets/filaments.json"
