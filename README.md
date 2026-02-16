# Addictol Crash Logger

Multi-Runtime version of Buffout 4 NG's Crash Logger.<br>
Supports OG (1.10.163.0), NG (1.10.984.0) and AE (1.11.191.0).

### Requirements
* [XMake](https://xmake.io) [2.8.2+]
* C++23 Compiler (MSVC, Clang-CL)

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

- Path to a Fallout 4 install folder: `XSE_FO4_GAME_PATH`

- Path to a Mod Manager mods folder: `XSE_FO4_MODS_PATH`

### Project Generation (Optional)
If you want to generate a Visual Studio project, run the following command:
```bat
xmake project -k vsxmake
```

> ***Note:*** *This will generate a `vsxmakeXXXX/` directory in the **project's root directory** using the latest version of Visual Studio installed on the system.*

### Upgrading Packages (Optional)
If you want to upgrade the project's dependencies, run the following commands:
```bat
xmake repo --update
xmake require --upgrade
```
