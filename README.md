<div align="center">
   <img width="340" height="170" src="/assets/text_logo.png" alt="Logo">
</div>
<div align="center">
  </div>

Firestar is a Windows editor for creating and building Apex Legends RPAKs.

It can open normal RePak JSON files and build lists, browse their assets, edit
asset values, replace source files, and build the project directly. Source files
are checked before importing so obvious type or version mismatches are caught
early.

Firestar also includes a UI atlas builder and a small mod folder creator.

## Basic use

1. Open a Firestar project, import a RePak JSON file, or create a new RPAK from the home page.
2. Add files with **+ Asset** or import a whole asset folder.
3. Select an asset to edit its fields or inspect its source data.
4. Save the `.fsp` project and click **Build RPAK**. Export a standalone RePak JSON from the **File** menu when needed.

The Original UI atlas tool is available under **Tools**. If an RPAK Project is already open, its
texture and UIMG entries can be added straight to that project.

Deployment is optional and must be configured in **Settings**. Firestar always
shows the target and asks before copying a built RPAK or changing preload.rson.

## Building Firestar

Open `Firestar.sln` in Visual Studio and build `Release|x64`. The executable is
written to `bin/Release/Firestar.exe`.

## License

Firestar is released under the [MIT License](LICENSE). The included RePak source
keeps its MIT License in `third_party/RePak-LICENSE.txt`.

[Credit for the Logo](https://www.deviantart.com/hilsonity/art/Logo-Firestar-1065611272)
