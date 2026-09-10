# Addictol Crash Logger

Multi-Runtime version of Buffout 4 NG's Crash Logger with CrashLoggerSSE enhancements.<br>
Supports all major game versions [OG (1.10.163.0), NG (1.10.984.0) and AE (1.11.240.0)].

## Optional in-game UI

Installing the standalone
[DearModdingUI](https://github.com/Dear-Modding-FO4/DearModdingUI) host adds
Home, Reports, and Settings pages. The host is optional: if it is absent,
disabled, unavailable, or incompatible, crash logging and the existing
Ctrl+Shift+F12 thread-dump hotkey continue without an in-game UI. The Crash
Logger does not bundle DearModdingUI, Dear ImGui, or rendering hooks.

The UI client registers during normal-load `kPostPostLoad`, after the existing
earliest supported crash-handler installation stage. Its stable client ID is
`dear-modding.addictol-crash-logger`.

### Home

Home reports the active-session enablement and handler installation outcome,
the game and logger versions, the configured and actually resolved report
locations, and the actual `AddictolCrashLogger.log` startup/operational-log
path. A disabled logger can still browse existing reports. Open actions are
performed by the DearModdingUI host.

### Reports

Reports is a read-only, asynchronous browser for strict timestamp-named
`crash-*.log` and `threaddump-*.log` files in the resolved report directory.
It identifies a same-basename `.dmp`, but does not inspect dump contents.

The first release deliberately bounds work:

* at most 10,000 directory entries are inspected per refresh;
* at most the 1,000 newest matching reports are retained in the index;
* at most 2 MiB of selected text is loaded;
* previews are paged at 200 lines.

Partial indexes and truncated or encoding-normalized previews are labeled.
Files are checked before and after reading so ordinary replacement or
concurrent modification is rejected with a refresh/retry message. This is a
best-effort preview, not a forensic snapshot.

Copying is always an explicit user action. **Copy Loaded Text** is labeled as
potentially sensitive and may contain only the loaded prefix. **Copy Selected
Summary** previews and copies only a fixed whitelist: report kind and basename,
recorded timestamp, game/logger version, exception code or a recognized exception
name, and a structurally recognized module basename plus offset. Reports are never
uploaded, deleted,
automatically opened, or automatically copied by the UI.

### Settings

Settings exposes the existing General, Output, Capture, and Advanced options.
All edits apply on the **next game launch**. The active crash-handler settings
and REX setting store are never reloaded or mutated at runtime.

Reset uses immutable compiled defaults for exposed controls, Revert restores
the configured draft, and Apply updates only dirty owned keys in the logical
`Data\F4SE\Plugins\AddictolCrashLoggerCustom.toml`. Unrelated and hidden keys
are preserved. Leaving the page discards unapplied edits; an accepted save
continues off-thread and is reconciled when the page reopens.

Saving uses best-effort source fingerprints, a unique same-directory temporary
file, and checked Windows replacement. Detected external edits require reload
and review. The original file is preserved on errors before replacement, but
the UI does not claim protection against arbitrary racing editors, unsupported
virtual filesystems, or power loss. Normal logical paths are used so mod-manager
write routing remains authoritative; unsupported replacement paths report an
error instead of falling back to in-place truncation.

Pastebin upload settings remain file-only. The UI shows upload enablement and a
privacy notice but never displays or copies the API key. Manual diagnostic
capture from the UI is not part of this release.

### Requirements
* [XMake](https://xmake.io) [3.0.0+]
* C++23 Compiler (MSVC or Clang-CL)

## Getting Started
```bat
git clone --recurse-submodules https://github.com/Dear-Modding-FO4/AddictolCrashLogger
cd AddictolCrashLogger
```

### Build
To build the project, run the following command:
```bat
xmake build
```

> ***Note:*** *This will generate a `build/windows/` directory in the **project's root directory** with the build output.*

### Build Output (Optional)
If you want to redirect the build output, set one of the following environment variables:

- Path to a Mod Manager mods folder: `XSE_FO4_MODS_PATH`

  or

- Path to a Fallout 4 install folder: `XSE_FO4_GAME_PATH`

### Project Generation (Optional)
If you use Visual Studio, run the following command:
```bat
xmake project -k vsxmake
```

> ***Note:*** *This will generate a `vsxmakeXXXX/` directory in the **project's root directory** using the latest version of Visual Studio installed on the system.*

**Alternatively**, if you do not use Visual Studio, you can generate a `compile_commands.json` file for use with a laguage server like clangd in any code editor that supports it, like vscode:
```bat
xmake project -k compile_commands
```

### Upgrading Packages (Optional)
If you want to upgrade the project's dependencies, run the following commands:
```bat
xmake repo --update
xmake require --upgrade
```
