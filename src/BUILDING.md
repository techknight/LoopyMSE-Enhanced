# Building LoopyMSE-Enhanced

You need CMake 3.30 or newer and a C++17 compiler. SDL2 and SDL2_image are
fetched and built by CMake itself; there are no other dependencies.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target install
```

The finished program, its DLLs or libraries, and the default `loopymse.ini`
end up in `build/dist`. Put the BIOS files (`bios.bin` and `soundbios.bin`)
beside the program, or name them with `--bios` and `--sound-bios`.

## Windows

Either compiler works:

- **Visual Studio 2022** (Community is enough): open a Developer PowerShell
  and run the two commands above; CMake picks the Visual Studio generator.
  The program is a console program, so its log appears in the terminal you
  start it from.
- **MSYS2 MinGW64**: install the toolchain with
  `pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake`, then add
  `-G "MinGW Makefiles"` to the configure command.

## macOS

Install the Xcode command line tools and CMake (for example
`brew install cmake`), then run the commands above. The app bundle ends up in
`build/dist/LoopyMSE.app`.

## Linux

Install a compiler, CMake and SDL2's build dependencies (on Debian and
Ubuntu: `sudo apt install build-essential cmake libx11-dev libxext-dev
libasound2-dev libpulse-dev`), then run the commands above.
