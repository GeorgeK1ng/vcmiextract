## vcmiextract

Extractor of Heroes 3 data files based on VCMI source code

Primary made for my own use, so nothing fancy

## Supported formats

Archives - all files will be extracted as it, with exception of images which will be converted to png
- .lod
- .pac
- .snd
- .vid
- .pak (HD Edition)

Animations - will be extracted as set of png files and additional text file that contains order of images in .def file
- .def
- .d32: used by HotA

Spritesheets (HD Edition) - partial support, extraction done as it, without resizing to match H3 sheet size
- .dds

Images - will be converted to .png format
- .pcx: custom format of Heroes 3, not related to well-known pcx format
- .p32: used by HotA

## Usage - Windows

Drag-and-drop file(s) that you want to extract on executable. Extracted files will be placed in a directory with same name as input file

## Building - Windows x86 (Visual Studio 2019)

Install the x86 static variants of the dependencies with vcpkg, then configure CMake for the Win32 platform and the Visual Studio 2019 (`v142`) toolset:

```bat
vcpkg install zlib libpng liblzma --triplet x86-windows-static
cmake -S . -B build-x86 -A Win32 -T v142 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x86-windows-static
cmake --build build-x86 --config Release
```

The resulting 32-bit executable is located at `build-x86\Release\vcmiextract.exe`.

## Usage - Command line

```
./vcmiextract [archive.lod]...
./vcmiextract [animation.def]...
```
