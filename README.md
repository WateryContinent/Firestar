# Firestar

Firestar is a UI atlas builder for Apex Legends Season 3. It packs images into a
DDS texture atlas and creates the matching RePak JSON for the texture and UIMG
assets.

## Features

- Drag and drop image importing
- Live atlas preview
- Automatic power-of-two atlas sizing
- Configurable padding with edge-pixel extrusion
- Editable RUI image paths
- DDS and RePak JSON export
- Save and reopen projects as `.fsa` files

## Usage

1. Add or drop your source images into Firestar.
2. Check the generated RUI paths.
3. Choose the output folder and packing options under **Settings**.
4. Click **Export DDS + JSON**.
5. Build the exported JSON with RePak.

## Building

Firestar is a C++20, DirectX 11 application for 64-bit Windows. It requires
Visual Studio, the MSVC v145 toolset, a Windows SDK and Dear ImGui with the
Win32 and DirectX 11 backends.

Build the `Release|x64` configuration in Visual Studio, or run:

```powershell
MSBuild.exe Firestar.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
```

The executable will be written to `bin/Release/Firestar.exe`.

## License

Firestar is released under the [MIT License](LICENSE).
