# Contributing Guide

This document covers code standards, workflow practices, and submission guidelines for contributing to the HelixScreen prototype.

## Development Workflow

### Git Workflow

**Current branch structure:**
- **`main`** - Stable releases and integration
- **`ui-redesign`** - Active development branch for prototype work
- **Feature branches** - For specific features or fixes

**Recommended workflow:**
1. Create feature branch from `ui-redesign`
2. Make focused commits with clear messages
3. Test thoroughly before submitting
4. Submit pull request to `ui-redesign` branch

### Daily Development Cycle

1. **Edit code** in `src/` or `include/`
2. **Edit XML** in `ui_xml/` (layout/styling changes - no rebuild needed)
3. **Build** with `make -j` (parallel incremental build)
4. **Test** with `./build/bin/helix-ui-proto [panel_name]`
5. **Screenshot** with `./scripts/screenshot.sh` or press **S** in UI
6. **Commit** with working incremental changes

### XML vs C++ Development

**XML file changes (immediate feedback):**
- Layout, styling, colors, text content
- Edit `ui_xml/*.xml` files
- Restart application to see changes
- No recompilation needed

**C++ file changes (requires build):**
- Business logic, subject bindings, event handlers
- Edit `src/*.cpp` and `include/*.h` files
- Run `make -j` after changes
- Test with application restart

## Code Standards

### Production Safety Rules

**CRITICAL: Mock implementations must NEVER be used in production builds.**

See **[CLAUDE.md](CLAUDE.md)** Critical Rule #4 for the complete production safety requirements.

**Key Points:**
- No automatic fallbacks to mock implementations
- Mocks require explicit `--test` command-line flag
- Production mode must fail gracefully when hardware unavailable
- Test mode must have clear visual indicators

**Testing with Mocks:**
```bash
./build/bin/helix-ui-proto --test              # Full mock mode
./build/bin/helix-ui-proto --test --real-wifi  # Mix real and mock
```

**Production (No Mocks Ever):**
```bash
./build/bin/helix-ui-proto  # Will fail gracefully if hardware unavailable
```

### Copyright Headers

**All new source files MUST include GPL v3 copyright header.**

See **[docs/COPYRIGHT_HEADERS.md](docs/COPYRIGHT_HEADERS.md)** for required templates:
- C/C++ source files
- XML component files
- Shell scripts

### Logging Standards

**CRITICAL:** All debug, info, warning, and error messages must use **spdlog** for console output.

```cpp
#include <spdlog/spdlog.h>

// Log levels (use appropriately):
spdlog::trace("Very detailed tracing");           // Function entry/exit
spdlog::debug("Debug information");               // Development details
spdlog::info("General information");              // Normal operation
spdlog::warn("Warning condition");                // Recoverable issues
spdlog::error("Error condition");                 // Failed operations
```

**Formatting:**
- Use **fmt-style formatting**: `spdlog::info("Value: {}", val);`
- **NOT** printf-style: ~~`spdlog::info("Value: %d", val);`~~
- Enums must be cast: `spdlog::debug("Panel ID: {}", (int)panel_id);`

**Do NOT use:**
- `printf()` / `fprintf()` - Use spdlog instead
- `std::cout` / `std::cerr` - Use spdlog instead
- `LV_LOG_*` macros - Use spdlog instead

### Naming Conventions

**C++ Functions:**
- `snake_case` for function names: `ui_panel_home_init()`
- `snake_case` for variable names: `temp_target_subject`
- Prefix with component/module: `ui_nav_*`, `ui_panel_*`

**XML Components:**
- `kebab-case` for file names: `nozzle-temp-panel.xml`
- `snake_case` for component names: `nozzle_temp_panel` (matches filename)
- Explicit `name` attributes for instances: `<nozzle_temp_panel name="nozzle_temp_panel"/>`

**Constants:**
- `UPPER_SNAKE_CASE` for preprocessor defines: `#define MAX_TEMP 300`
- `snake_case` for globals.xml constants: `primary_color`, `nav_width`

### Critical Code Patterns

**Component Names:** Always add explicit `name` attributes to component instantiations:

```xml
<!-- app_layout.xml -->
<lv_obj name="content_area">
  <controls_panel name="controls_panel"/>  <!-- Required for lv_obj_find_by_name() -->
  <home_panel name="home_panel"/>
</lv_obj>
```

**Widget Lookup:** Use name-based lookup instead of index-based:

```cpp
// GOOD: Robust against layout changes
lv_obj_t* label = lv_obj_find_by_name(panel, "temp_display");

// BAD: Breaks when XML structure changes
lv_obj_t* label = lv_obj_get_child(panel, 3);
```

**LVGL Public API Only:** Never use private LVGL interfaces:

```cpp
// ❌ WRONG - Private interfaces:
lv_obj_mark_dirty()              // Internal layout/rendering
obj->coords.x1                   // Direct structure access
_lv_* functions                  // Underscore-prefixed internals

// ✅ CORRECT - Public API:
lv_obj_get_x()                   // Public getters/setters
lv_obj_update_layout()           // Public layout control
lv_obj_invalidate()              // Public redraw trigger
```

## UI Development Standards

### XML Attribute Syntax

**LVGL 9 XML uses simplified flag syntax** - never use `flag_` prefix:

```xml
<!-- CORRECT: -->
<lv_obj hidden="true" clickable="true" scrollable="false"/>

<!-- WRONG: -->
<lv_obj flag_hidden="true" flag_clickable="true" flag_scrollable="false"/>
```

**Flex alignment uses three separate properties:**

```xml
<!-- CORRECT: -->
<lv_obj style_flex_main_place="center"
        style_flex_cross_place="center"
        style_flex_track_place="start"/>

<!-- WRONG: -->
<lv_obj flex_align="center center center"/>  <!-- This attribute doesn't exist -->
```

### Subject Binding Patterns

**Text bindings** use `bind_text` attribute:

```xml
<lv_label bind_text="temp_current_text"/>
```

**Conditional visibility** uses child elements:

```xml
<lv_obj>
    <lv_obj-bind_flag_if_eq subject="print_state" flag="hidden" ref_value="idle"/>
</lv_obj>
```

**Subject initialization order** is critical:

```cpp
// CORRECT ORDER:
lv_xml_register_component_from_file("A:/ui_xml/globals.xml");
ui_panel_home_init_subjects();  // Initialize BEFORE XML creation
lv_xml_create(screen, "home_panel", NULL);  // NOW create UI
```

## Screenshot & Documentation Standards

### Screenshot Workflow

**Always use the screenshot script** instead of manual BMP conversion:

```bash
# CORRECT: Use the automated script
./scripts/screenshot.sh helix-ui-proto output-name [panel] [options]

# Examples:
./scripts/screenshot.sh helix-ui-proto home-screen home
./scripts/screenshot.sh helix-ui-proto motion-panel motion -s small

# WRONG: Manual BMP handling
# magick /tmp/screenshot.bmp output.png  # Don't do this
```

**Script features:**
- Panel name validation
- Automated BMP → PNG conversion
- Multi-display positioning
- Error handling with helpful messages

### Documentation Updates

When adding new features:

1. **Update relevant documentation** - don't just edit code
2. **Add examples** to appropriate reference docs
3. **Update CLAUDE.md** if patterns change
4. **Screenshot new UI** using the standard workflow
5. **Test documentation** - verify links and examples work

## Asset Management

### Icon Workflow

**FontAwesome icons** are auto-generated from C++ constants:

1. **Edit icon definitions** in `include/ui_fonts.h`
2. **Regenerate constants:**
   ```bash
   python3 scripts/generate-icon-consts.py
   ```
3. **Rebuild fonts if needed:**
   ```bash
   make generate-fonts
   ```

**Custom icons** use PNG format:
- Store in `assets/images/`
- Use consistent naming: `icon-name-size.png`
- Document purpose and usage

### Application Icon Generation

Generate platform-specific icons:

```bash
make icon  # Creates .icns (macOS) and .png (all platforms)
```

**Requirements:** ImageMagick installed via system package manager

## Testing Guidelines

### Manual Testing

**Before submitting:**
1. **Test all affected panels** - don't just test the code you changed
2. **Test both screen sizes** - small and large if applicable
3. **Test navigation** - ensure back buttons and nav bar work
4. **Check console output** - no unexpected errors or warnings

**Testing commands:**
```bash
# Test specific panel
./build/bin/helix-ui-proto --panel home

# Test with different screen sizes
./build/bin/helix-ui-proto -s small --panel motion
./build/bin/helix-ui-proto -s large --panel controls

# Test with verbose logging
./build/bin/helix-ui-proto -v --panel nozzle-temp
```

### Build Validation

**Always verify build succeeds:**
```bash
make clean && make -j  # Full clean build
```

**Check for warnings:**
```bash
make V=1  # Verbose mode shows all compiler warnings
```

### Memory Testing

For complex changes, run memory analysis:

```bash
# Check for leaks (basic)
valgrind --leak-check=yes ./build/bin/helix-ui-proto

# Performance profiling
./scripts/memory_profile.sh  # If available
```

See **[docs/MEMORY_ANALYSIS.md](docs/MEMORY_ANALYSIS.md)** for detailed testing procedures.

## Submission Guidelines

### Commit Standards

**Commit message format:**
```
type(scope): description

Optional detailed explanation
of what changed and why.
```

**Examples:**
```
feat(ui): add temperature control overlay panel
fix(build): resolve SDL2 linking on macOS Sequoia
docs(readme): update build instructions for npm dependencies
refactor(nav): simplify history stack management
```

**Types:** `feat`, `fix`, `docs`, `style`, `refactor`, `test`, `chore`

### Pull Request Guidelines

**Before submitting:**
1. **Rebase on latest `ui-redesign`** branch
2. **Test build and runtime** thoroughly
3. **Update documentation** if patterns changed
4. **Add screenshots** for UI changes
5. **Check for unintended changes** (don't commit debugging code)

**PR description should include:**
- **What** changed (brief summary)
- **Why** it was needed (context/problem solved)
- **How** to test the changes
- **Screenshots** for visual changes
- **Breaking changes** if any

### Code Review Focus

**Reviewers should check:**
- **Architecture compliance** - follows XML/Subject patterns
- **Error handling** - proper logging, null checks
- **Performance** - no blocking operations in UI thread
- **Documentation** - updated for significant changes
- **Testing** - manually verified or automated tests added

## Development Environment

### Required Setup

See **[DEVELOPMENT.md](DEVELOPMENT.md)** for complete setup instructions.

**Quick verification:**
```bash
make check-deps  # Verify all dependencies
make compile_commands  # Generate IDE support
make -j  # Test build
./build/bin/helix-ui-proto  # Test runtime
```

### IDE Configuration

**Recommended:** Use LSP-compatible editors with `compile_commands.json`

**VS Code:**
- C/C++ extension pack
- clangd extension
- Files → Preferences → Settings → search "clangd"

**Vim/Neovim:**
- LSP client (nvim-lspconfig, coc.nvim, etc.)
- Point to generated `compile_commands.json`

## Getting Help

### Documentation References

- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System design and patterns
- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Build system and daily workflow
- **[LVGL 9 XML Guide](docs/LVGL9_XML_GUIDE.md)** - Complete XML syntax reference
- **[Quick Reference](docs/QUICK_REFERENCE.md)** - Common patterns and gotchas
- **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)** - Build configuration details

### Common Issues

**Build problems:** Check **[DEVELOPMENT.md](DEVELOPMENT.md)** troubleshooting section

**XML issues:** Read **[docs/LVGL9_XML_GUIDE.md](docs/LVGL9_XML_GUIDE.md)** troubleshooting first

**UI behavior:** Review **[ARCHITECTURE.md](ARCHITECTURE.md)** for subject binding patterns

**Performance issues:** See **[docs/MEMORY_ANALYSIS.md](docs/MEMORY_ANALYSIS.md)**

### Where to Ask Questions

1. **Documentation first** - check relevant guides above
2. **Search existing issues** in the repository
3. **Create new issue** with:
   - Clear problem description
   - Steps to reproduce
   - Build environment details
   - Relevant log output

Remember: Good documentation and clear examples help everyone contribute more effectively!