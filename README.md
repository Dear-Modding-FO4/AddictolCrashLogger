# Addictol Crash Logger

Multi-Runtime version of Buffout 4 NG's Crash Logger with CrashLoggerSSE enhancements.<br>
Supports all major game versions [OG (1.10.163.0), NG (1.10.984.0) and AE (1.11.240.0)].

## Optional in-game UI

Install [DearModdingUI](https://github.com/Dear-Modding-FO4/DearModdingUI) separately
for three in-game pages:

* **Home:** logger status, versions, and report locations.
* **Reports:** browse, search, open, and copy saved crash/thread reports. Previews
  are limited to 2 MiB; open the original file for the full report.
* **Settings:** edit configuration with Reset, Revert, and Apply. Changes are saved
  to `Data\F4SE\Plugins\AddictolCrashLoggerCustom.toml` for the **next game launch**.
  Leaving the page discards unapplied edits.

Crash logging works without the UI. The UI never uploads or deletes reports;
Pastebin settings remain file-only, and manual capture uses the existing optional
Ctrl+Shift+F12 hotkey.

## Crash-log compatibility

The default text report retains the existing Bethesda crash-logger layout.
Headers, section order, indentation and frame/register/plugin line conventions
are a compatibility boundary for community tools such as CLASSIC. Improvements
should preserve that format; new presentation belongs in DearModdingUI or
separately approved opt-in diagnostics.

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
