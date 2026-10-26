# Building and playing on Windows

Step by step from zero to the game running, starting from the source code. **To just play, use the release** (the zip in
Releases, see the [README](../README.md#how-to-play)): it needs none of this.

It takes about half an hour, almost all of it waiting for downloads and the build.

## What you need
- Windows 10 or 11, 64-bit
- A CPU with SSE4.2 (Intel from 2008 / Nehalem, AMD from 2011 / Bulldozer); with AVX2 (Intel from
  2013 / Haswell, AMD from 2015 / Excavator) the game code runs faster. `make` builds the
  game code for `GENARCH` (default `-march=x86-64-v3`; `GENARCH=-march=x86-64-v2` for a CPU without AVX2, deleting
  `build/obj/` first: the objects do not depend on the flags); the start screen picks on its own based on the CPU
- A GPU with Vulkan 1.3 and an up-to-date driver (recent NVIDIA, AMD or Intel)
- About 10 GB free (game files, generated code and build)
- [Git for Windows](https://git-scm.com/download/win) (includes Git Bash)
- **Your own game files**: *Pokémon: Let's Go, Eevee!* or *Pikachu!*, extracted from your copy (Eevee
  **v1.0.0** is the most tested; the experimental options require 1.0.0 of either one). This
  project does not provide these files or the keys, nor does it teach how to obtain them.

## 1. Download the project
In Git Bash, in a folder with free space (preferably outside `Arquivos de Programas`):

```bash
git clone https://github.com/raulzrrs/letsgo-recomp.git
```

## 2. Install the tools (once)
In PowerShell, inside the project folder:

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1
```

The script installs:
- **MSYS2** in `C:\msys64`, with clang, make, SDL3, FFmpeg, Vulkan and shaderc;
- **Python 3.12**, with the code generator's environment in `.venv`.

It also configures the clone for LF line endings. You can run it again if something fails partway through.

## 3. Build the runtime
Open **Git Bash** in the project folder and put MSYS2 on the PATH (applies to that window):

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -j20 build/LetsGoRecomp.exe
```

This produces `build/LetsGoRecomp.exe`, the runtime (the launcher and everything that replaces the Switch). Changes to the runtime
rebuild in seconds. On Windows, this name is also the one the game library imports; run `LetsGoRecomp.exe` to avoid loading two runtimes. The old `build/lasrecomp.exe` target still copies the executable for compatibility with earlier build commands.

## 4. Set up the game through the launcher (the simplest way)

```bash
./build/LetsGoRecomp.exe
```

In the launcher, click **Select game folder...** and choose the folder with the extracted game (the one that has `exefs` and
`romfs`, or a `romfs.bin`). It checks whether it is Eevee or Pikachu (program id), copies the files to `extracted/`, generates the game code
with the `.venv` and builds it with MSYS2's clang (`extracted/lasgame.dll`). It is the same flow as the release, with the
development tools in place of the frozen generator and Zig. Then click **Play**.

## 4b. Or by hand
Create the `extracted/` folder at the project root with:

```
extracted/
  exefs/      main, main.npdm, rtld, sdk, subsdk0, subsdk1   (o ExeFS do jogo)
  romfs.bin   a imagem RomFS do jogo (uns 4,4 GB)
  romfs/      opcional: o RomFS extraído em pastas (útil para mods; o jogo lê o romfs.bin)
```

The generated code depends on the exact executable: the generator's patches (`tools/patches.py`) and the runtime's hooks are only
addresses of the 1.0.0 `main` (a table per build: Eevee and Pikachu, `tools/addrmap.py`) and stay disabled
for any other. `extracted/` is
in `.gitignore` and must never go into git.

Translate the game code to C (the output goes to `generated/`, outside git) and build. **Always with `-j`**: without
it, the ~920 game files build one at a time and take over an hour.

```bash
make gen
make -j20
```

This also produces `build/lasgame.dll` (the game code), in 5 to 20 minutes depending on the CPU.

## 5. Play

```bash
./build/LetsGoRecomp.exe
```

- Saves go in `save/`, settings in `larecomp.ini` and shader caches in `cache/`, all in the
  project folder.
- `--play` skips the launcher: `./build/LetsGoRecomp.exe extracted --play`.
- `make jogo` assembles in `build/jogo/` a copy that opens with a double click (it includes the MSYS2 DLLs) and lets you rebuild
  `build/` while playing.
- The first time in each area compiles the shaders. Some objects may appear late for a moment; from the second
  time on it is smooth.

## 6. The release (the zip for those who just want to play)

```bash
make deps-windows          # FFmpeg só LGPL, com os decodificadores que o jogo usa (precisa do nasm do MSYS2)
make gen-helper            # o gerador congelado com o PyInstaller (sem Python no PC do jogador)
tools/fetch_zig.sh         # o compilador Zig, versão fixa e conferida por sha256
make dist                  # build/dist/LetsGoRecomp-<versão>-windows.zip
```

The zip contains `LetsGoRecomp.exe`, the DLLs, the generator, the slimmed-down Zig (`tools/zig_slim.sh`) and the headers of the
game code. Nothing from the game is included: the code is generated on the player's PC, from their own copy. The version comes from
`RT_APP_VERSION` in `runtime/paths.h`.

## Optional
- **Translations / mods:** in the launcher, **Mods** page, **Install mod...** and choose the downloaded file (.zip, .rar, .7z);
  it comes already checked in the list (you can use several together; if two replace the same file, the one on top wins). You can also copy the mod's `romfs` to `mods/<nome>/romfs` by hand (**Open the
  mods folder**).
- **Switch system font:** see [fonts/README.md](../fonts/README.md).
- **Better image:** with a GPU that has headroom, choose a resolution above your screen's (for example 4K on a 1080p screen):
  the game is rendered larger and scaled down, which removes almost all aliasing.

## Common problems
| Symptom | What to do |
|---|---|
| "avutil-…dll / SDL3.dll not found" when opening the exe | run it from Git Bash with MSYS2 on the PATH, or use the copy from `make jogo` (it includes the DLLs) |
| the launcher asks to set up the game again | the generator or the headers changed: choose the folder again (or, by hand, `make gen` and `make -j20`) |
| `make gen` fails | check the `.venv` (run `setup_windows.ps1` again) and whether `extracted/exefs/main` exists |
| black screen or Vulkan error | update the GPU driver; Vulkan 1.3 is required |
| the game closes on its own | send the terminal output (or the release's `lasrecomp.log`) in an issue (see [CONTRIBUTING.md](../CONTRIBUTING.md)) |

## For those who will work on the code
- [DEBUGGING.md](DEBUGGING.md): diagnostic variables and `tools/repro.sh`, which runs the game with scripted input and
  takes screenshots at chosen frames.
- [HISTORY.md](HISTORY.md): how each problem was solved. It is the best way to understand the runtime.
- Changed `runtime/cpu.h` or `runtime/gamelib.h`? The game DLL needs to be rebuilt (`make -j20`).
