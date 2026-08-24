# Welcome to HelixScreen

HelixScreen is an LVGL 9.5 touchscreen UI for Klipper 3D printers. This guide gets
a new contributor from a fresh checkout to a first change.

You should have arrived here from [CONTRIBUTING.md](../../CONTRIBUTING.md). The
path continues: **here** (environment + build + mental model) →
[YOUR_FIRST_CONTRIBUTION.md](YOUR_FIRST_CONTRIBUTION.md) (walkthrough of a real
contribution) → [the architecture guide](architecture/README.md) (pick your
subsystem).

## Prerequisites

- A working C++ toolchain and `make` (the build is a pure Makefile — no CMake/Ninja).
- The repository cloned locally: https://github.com/prestonbrown/helixscreen
- After cloning, initialize the submodules — a fresh clone is missing
  `lib/helix-xml/` (the XML UI engine) and the other dependencies:

```bash
git submodule update --init --recursive
```

## Environment Setup

### macOS (Homebrew)
```bash
brew install cmake bear imagemagick python3 node shellcheck bats-core
npm install         # lv_font_conv and lv_img_conv
make venv-setup     # Python venv with pypng/lz4
```
**Minimum:** macOS 10.15 (Catalina) for CoreWLAN/CoreLocation WiFi APIs.

### Debian/Ubuntu
```bash
sudo apt install cmake bear imagemagick python3 python3-venv clang make npm \
    shellcheck bats libnl-3-dev libnl-genl-3-dev libssl-dev
npm install && make venv-setup
```

### Fedora/RHEL
```bash
sudo dnf install cmake bear ImageMagick python3 clang make npm \
    ShellCheck bats libnl3-devel openssl-devel
npm install && make venv-setup
```

### Dependencies

| Category | Components | Notes |
|----------|------------|-------|
| **Required** | clang, cmake 3.16+, make, python3, node/npm | Core build tools |
| **Auto-built** | SDL2, spdlog, libhv | Built from submodules if not system-installed |
| **Always submodule** | lvgl | Project-specific patches required |
| **Optional** | bear, imagemagick, shellcheck, bats-core | IDE support, screenshots, shell linting/testing |

```bash
make check-deps      # Check what's missing
make install-deps    # Auto-install (interactive)
```

## Build & Run

Before compiling, check for existing build processes (`pgrep -f 'make|c\+\+'`) —
concurrent compilations thrash the machine.

```bash
make -j                              # Build ONLY the program binary (not tests)
./build/bin/helix-screen --test -vv  # Run against a mock printer with DEBUG logs
```

Always run with verbosity when debugging: `-v` = INFO, `-vv` = DEBUG, `-vvv` = TRACE
(default is WARN). Debugging without at least `-vv` wastes time.

### Tests

```bash
make test                            # Build tests only (does NOT run them)
make test-run                        # Build AND run tests in parallel
./build/bin/helix-tests "[tag]"      # Run a specific test tag
```

Note: `make -j` builds only `helix-screen`, not the tests. Run `make test` before
`./build/bin/helix-tests` or you will be testing a stale binary.

### XML changes need no rebuild

`ui_xml/*.xml` is loaded at runtime. Edit the XML, then relaunch the binary to see
the change — no `make` needed. Hot reload is ON by default for native builds: with
the app running, saving an XML file re-registers the component and rebuilds the
active panel/overlay/modal in place within ~500ms. (Override with
`HELIX_HOT_RELOAD=0`/`1` — see [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md).)

### Screenshots and driving the UI

Press `S` in the UI, or run `./scripts/screenshot.sh helix-screen output-name [token]`.
To remote-control a running instance — navigate, click widgets, read exact widget
text/geometry, bring up any panel — use `helix-screen ctl`. See
[HELIXCTL.md](HELIXCTL.md).

## The 15-minute mental model

Before your first change, get the whole-app picture: one pattern everywhere —
**XML → Subjects → C++** — plus a map of the subsystems and a "pick your
subsystem" table. It lives in one place, the router:

→ **[ARCHITECTURE.md](ARCHITECTURE.md)** — the 15-minute model, routing into the
15-chapter [architecture guide](architecture/README.md).

## Workflow Tips

- **Read `CLAUDE.md` before touching UI code.** The declarative-UI rules (no
  `lv_obj_add_event_cb`, no imperative visibility, design tokens instead of hardcoded
  colors/spacing, etc.) are strict and easy to violate if you're coming from
  imperative LVGL.
- **Use worktrees for multi-file or risky work.** When a worktree is warranted —
  and when it isn't — is decided in [DEVELOPMENT.md](DEVELOPMENT.md) § "Worktrees"
  (`scripts/setup-worktree.sh feature/my-branch`).
- **Start fresh per task.** Keep unrelated bugs/features in separate sessions rather
  than letting one session sprawl across a whole day of work.

## Next

- **Ready to write code?** → [YOUR_FIRST_CONTRIBUTION.md](YOUR_FIRST_CONTRIBUTION.md)
  — an annotated walkthrough of a real settings overlay, plus a pattern tour for
  bigger features.
- **Rather explore by subsystem?** → [architecture/README.md](architecture/README.md)
  — the "I want to work on..." index into the 15-chapter architecture guide.
- **Looking for an issue?** Browse the [open issues](https://github.com/prestonbrown/helixscreen/issues)
  and pick one that looks approachable. Debug/fix work is a fast way to get familiar
  with the codebase and its patterns — no specific ticket required, just find
  something you can reproduce and investigate.
- **Daily-workflow reference** (run flags, logging, config, IDE setup):
  → [DEVELOPMENT.md](DEVELOPMENT.md).
