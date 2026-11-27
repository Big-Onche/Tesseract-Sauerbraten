# Installing Sauerract

Sauerract is an overlay for **Cube 2: Sauerbraten**. It does not include the original maps and assets, so install a compatible copy of Sauerbraten first.

## Requirements

- A Sauerbraten installation containing `packages/` and `data/`
- A GPU and driver with OpenGL 3.0 support
- About 2 GB of free space in addition to the original game

Keep the vanilla game installed. Sauerract uses its assets but has a separate executable, configuration, and user-data directory, so both versions can coexist.

## Install a release build

1. Download the repository as a ZIP.
2. Extract it to a temporary directory.
3. Copy all extracted files and folders into the **Sauerbraten installation root**.
4. Check that the following paths now exist in the same directory:

   ```text
   data/               (Sauerbraten installation)
   packages/           (Sauerbraten installation)
   sauerract/          (Sauerract's files)
   binaries/           (Sauerract's executables)
   sauerract.bat       (Windows)
   sauerract.sh        (Linux)
   sauerract.command   (macOS)
   ```

Do not put Sauerract in a subfolder inside the Sauerbraten directory.

### Start the game

- **Windows:** double-click `sauerract.bat`.
- **Linux:** open a terminal in the installation root and run:

  ```sh
  chmod +x sauerract.sh binaries/linux/sauerract
  ./sauerract.sh
  ```

- **macOS:** open a terminal in the installation root and run:

  ```sh
  chmod +x sauerract.command binaries/mac/sauerract
  ./sauerract.command
  ```

On the first macOS launch, Gatekeeper may block an unsigned downloaded binary. Open **System Settings > Privacy & Security**, confirm that you trust the file, and choose **Open Anyway**.

## Compile from source

The source is in `src_sauerract/`. Build from the complete overlay after placing it in the Sauerbraten installation root. A successful build writes the client directly to the appropriate directory under `binaries/`.

### Windows (MinGW-w64, recommended)

Visual Studio 2022 builds are supported through `src_sauerract/vcpp/`, but they offer lower performance and are not recommended.

1. Install [MSYS2](https://www.msys2.org/) and open its **MinGW x64** terminal.
2. Install the 64-bit compiler and Make:

   ```sh
   pacman -Syu
   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-make
   ```

   If the update asks you to close the terminal, reopen the MinGW x64 terminal, run `pacman -Syu` again, and then install the packages.

3. Change to the Sauerbraten installation root and compile Sauerract:

   ```sh
   cd /c/path/to/sauerbraten
   mingw32-make -C src_sauerract PLATFORM=MINGW64 -j"$(nproc)" client
   ```

The result is `binaries/windows/sauerract.exe`. Keep it beside the supplied DLLs and start it with `sauerract.bat`.

#### Code::Blocks alternative

Code::Blocks is also supported and provides an easy graphical build process, but be aware that the IDE is not actively maintained anymore. Install a version bundled with MinGW-w64, open `src_sauerract/vcpp/sauerract.cbp`, select the `client64` build target, and choose **Build > Build**. The executable is written to `binaries/windows/sauerract.exe`.

### Linux

Install a C++ compiler, GNU Make, and the SDL2, SDL2_image, OpenAL, libsndfile, zlib, OpenGL, and X11 development packages. For Debian or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential libsdl2-dev libsdl2-image-dev libopenal-dev libsndfile1-dev zlib1g-dev libgl1-mesa-dev libx11-dev
```

For Fedora:

```sh
sudo dnf install gcc-c++ make SDL2-devel SDL2_image-devel openal-soft-devel libsndfile-devel zlib-devel mesa-libGL-devel libX11-devel
```

Build and launch:

```sh
make -C src_sauerract -j"$(nproc)" client
chmod +x sauerract.sh binaries/linux/sauerract
./sauerract.sh
```

The result is `binaries/linux/sauerract`.

### macOS

The current Makefile produces an **x86_64 (Intel)** binary. On Apple silicon, the build and the game therefore require Rosetta 2 plus x86_64-compatible dependencies.

1. Install the Xcode command-line tools:

   ```sh
   xcode-select --install
   ```

2. Make sure the original `sauerbraten.app` is in the installation root. The build uses its `SDL2.framework` and `SDL2_image.framework` from `sauerbraten.app/Contents/Frameworks/`.
3. Install an x86_64-compatible `libsndfile` (for example with Intel Homebrew when building on Intel or under Rosetta).
4. Build and launch:

   ```sh
   make -C src_sauerract -j"$(sysctl -n hw.logicalcpu)" client
   chmod +x sauerract.command binaries/mac/sauerract
   ./sauerract.command
   ```

The result is `binaries/mac/sauerract`. If the compiler cannot find `sndfile`, add the include and library paths used by your package manager to the build environment.

## Rebuild or clean

After changing source files, run the platform build command again. To remove generated object and precompiled-header files before a full rebuild:

```sh
make -C src_sauerract clean
```

On Windows, run `mingw32-make -C src_sauerract PLATFORM=MINGW64 clean`.

## Troubleshooting

- **The launcher cannot find the executable:** compile the client or install the release binary for your operating system.
- **A shared library or DLL is missing:** keep the binary in its original `binaries/<platform>/` directory and reinstall the platform dependencies listed above.
- **Graphics fail during startup:** update the GPU driver and verify that the system supports OpenGL 3.0 or newer.
