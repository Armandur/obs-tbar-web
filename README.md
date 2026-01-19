# OBS T-bar Web

This project is an OBS plugin that exposes a small local web server to test and control OBS “T‑bar” / manual transitions in Studio Mode.

It includes:

- A **web UI** on `/` with a slider to drive transition progress
- A small **HTTP API** (`/tbar`, `/config`)
- Handling for both **Fade** (manual progress) and **Cut** (instant/fixed)

## Supported Build Environments

| Platform  | Tool   |
|-----------|--------|
| Windows   | Visual Studio 17 2022 |
| macOS     | XCode 16.0 |
| Windows, macOS  | CMake 3.30.5 |
| Ubuntu 24.04 | CMake 3.28.3 |
| Ubuntu 24.04 | `ninja-build` |
| Ubuntu 24.04 | `pkg-config`
| Ubuntu 24.04 | `build-essential` |

## Quick Start
### Build (Windows / Cursor CMake Tools)

- Make sure `ENABLE_FRONTEND_API=true` in your CMake configuration (required for Studio Mode functionality).
- Build normally in Cursor with CMake Tools (Configure + Build).

After the build you’ll find the plugin binaries here (example):

- `build_x64\RelWithDebInfo\obs-tbar-web.dll`
- `build_x64\rundir\RelWithDebInfo\obs-tbar-web.dll`

If you build from the command line, the target name is `obs-tbar-web`:

```bash
cmake --build build_x64 --config RelWithDebInfo --target obs-tbar-web
```

### Install into OBS

Option A (recommended): install via CMake:

```bash
cmake --install build_x64 --config RelWithDebInfo
```

Option B: manually copy the `.dll` to the OBS plugin folder (64bit).

### Run and test

1) Start OBS and enable **Studio Mode**.
2) Open the web UI in a browser:

- `http://127.0.0.1:<port>/`

Default port is **4455** unless you changed the config.

## API

### `GET /` (web-UI)

A test page with:

- Slider for manual progress (0..1023)
- “Save” for server settings (`enabled`, `port`)

### `GET /tbar`

Returns the last position (cached) in normalized form (0..1):

```json
{"position":0.5,"source":"cached"}
```

### `POST /tbar`

Send:

```json
{"position":0.5}
```

Optional (commit):

```json
{"position":1.0,"release":true}
```

**Behavior per transition:**

- **Fade / manual-capable transitions**: we start a manual transition towards the preview scene and drive progress using `manual_time`. On `release:true` near 1.0 we do a **program/preview swap** so Studio Mode behaves as expected.
- **Cut (fixed)**: there is no meaningful “in-between” position. We trigger a real transition on `release:true` near 1.0.

### `GET /config`

Returns:

```json
{"enabled":true,"port":4455}
```

### `POST /config`

Update the server settings:

```json
{"enabled":true,"port":4455}
```

Note: setting `enabled=false` disables the web server. Re-enable by editing the config file and restarting OBS.

### `GET /status`

Returns a small health/status payload:

```json
{"ok":true,"enabled":true,"port":4455,"manual_active":false,"last_position":0.0}
```

## Configuration

The plugin reads/writes a JSON file named:

- `obs-tbar-web.json`

It’s stored in the OBS “module config path” (the plugin’s config folder).

## Troubleshooting

- **Nothing happens when dragging**: verify Studio Mode is enabled and Preview ≠ Program.
- **Cut is “instant”**: expected; commit happens on `release:true` near max.
- **Port changes**: if you change the port in the web UI, open `http://127.0.0.1:<newport>/`.

## Future work

Gamepad/axis control is implemented in a separate project (client app). This plugin keeps the HTTP control surface and OBS-side transition handling.

## Documentation

All documentation can be found in the [Plugin Template Wiki](https://github.com/obsproject/obs-plugintemplate/wiki).

Suggested reading to get up and running:

* [Getting started](https://github.com/obsproject/obs-plugintemplate/wiki/Getting-Started)
* [Build system requirements](https://github.com/obsproject/obs-plugintemplate/wiki/Build-System-Requirements)
* [Build system options](https://github.com/obsproject/obs-plugintemplate/wiki/CMake-Build-System-Options)

## GitHub Actions & CI

Default GitHub Actions workflows are available for the following repository actions:

* `push`: Run for commits or tags pushed to `master` or `main` branches.
* `pr-pull`: Run when a Pull Request has been pushed or synchronized.
* `dispatch`: Run when triggered by the workflow dispatch in GitHub's user interface.
* `build-project`: Builds the actual project and is triggered by other workflows.
* `check-format`: Checks CMake and plugin source code formatting and is triggered by other workflows.

The workflows make use of GitHub repository actions (contained in `.github/actions`) and build scripts (contained in `.github/scripts`) which are not needed for local development, but might need to be adjusted if additional/different steps are required to build the plugin.

### Retrieving build artifacts

Successful builds on GitHub Actions will produce build artifacts that can be downloaded for testing. These artifacts are commonly simple archives and will not contain package installers or installation programs.

### Building a Release

To create a release, an appropriately named tag needs to be pushed to the `main`/`master` branch using semantic versioning (e.g., `12.3.4`, `23.4.5-beta2`). A draft release will be created on the associated repository with generated installer packages or installation programs attached as release artifacts.

## Signing and Notarizing on macOS

Basic concepts of codesigning and notarization on macOS are explained in the correspodning [Wiki article](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS) which has a specific section for the [GitHub Actions setup](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS#setting-up-code-signing-for-github-actions).
