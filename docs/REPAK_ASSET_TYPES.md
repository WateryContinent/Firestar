# RePak asset authoring reference

This is the asset reference for the RePak build in this repository. It is based
on the handlers in `src/assets`, the handler table in
`src/logic/pakfile.cpp`, and working examples in `sdk_rpak`.

It describes what this build actually accepts. Some older RePak guides use
names or fields that are not valid in this fork.

## The short version

A RePak build has two layers:

1. A build map tells RePak which RPAK to create and lists its assets.
2. Most asset entries point to a source file under `assetsDir`.

For example:

```json
{
  "version": 8,
  "name": "my_assets",
  "assetsDir": "assets/",
  "outputDir": "build/",
  "keepDevOnly": false,
  "keepServerOnly": true,
  "keepClientOnly": true,
  "files": [
    {
      "_type": "txtr",
      "_path": "texture/example/my_texture.rpak"
    }
  ]
}
```

With that entry, RePak reads:

```text
assets/texture/example/my_texture.dds
```

and creates the logical RPAK asset:

```text
texture/example/my_texture.rpak
```

Run it with:

```powershell
.\repak.exe .\my_assets.json
```

JSON comments are accepted by this build, so the maps in `sdk_rpak` may contain
`// comments`.

## Build map fields

| Field | Required | Meaning |
| --- | --- | --- |
| `version` | Yes | `7` for Titanfall 2 or `8` for Apex/R5. |
| `name` | Recommended | Output name without `.rpak`. |
| `assetsDir` | Yes | Root directory containing source assets. Relative paths are resolved from the build map. |
| `outputDir` | Yes | Output directory. Relative paths are resolved from the build map. |
| `files` | Usually | Array of asset entries. |
| `keepDevOnly` | No | Keeps development names such as shader and UI-image names. Default `false`. |
| `keepServerOnly` | No | Includes server-scoped data. Default `true`. |
| `keepClientOnly` | No | Includes client/render data. Default `true`. |
| `showDebugInfo` | No | Enables verbose build logging. The `-v` CLI switch does the same globally. |
| `streamFileMandatory` | When needed | Runtime path of the mandatory `.starpak`. Required by packed audio and streamed texture data. |
| `streamFileOptional` | No | Runtime path of the optional `.starpak`. Apex/version 8 only. |
| `streamCache` | No | Existing `.starmap` used to reuse matching streamed data. |
| `streamCacheFilter` | No | Restricts reuse to the listed starpak paths. |
| `ioWorkers` | No | Parallel input-prefetch workers. `0` disables it; maximum is 64. |
| `fileCacheMB` | No | Input-prefetch memory limit in MiB. `0` disables it; maximum is 32768. |
| `compressLevel` | No | Zstandard compression level. `0` leaves the RPAK decoded. |
| `compressWorkers` | No | Worker count used when `compressLevel` is above zero. |
| `hasDynamicLibrary` | No | Marks the RPAK as having a same-named DLL, loaded before its assets. |

### Building several RPAKs together

A build-list JSON shares the top-level build and streaming settings, then names
the individual maps:

```json
{
  "version": 8,
  "outputDir": "build/",
  "streamFileMandatory": "paks/Win64/pc_sdk.starpak",
  "streamFileOptional": "paks/Win64/pc_sdk.opt.starpak",
  "paks": [
    "sdk_depot/common_sdk.json",
    "sdk_depot/ui_mainmenu.json"
  ]
}
```

Each listed map still supplies its own `name`, `assetsDir`, and `files`.
`sdk_rpak/build_list_core_wmaps.json` is the main working example.

## Fields shared by every asset

Every entry in `files` needs:

```json
{
  "_type": "txtr",
  "_path": "texture/example/my_texture.rpak"
}
```

- `_type` selects one of the handlers in the table below. Type names are
  case-sensitive.
- `_path` is the asset's logical engine name. It also supplies the default
  source-file path.
- `$guid` is an optional explicit 64-bit asset GUID. Without it, RePak hashes
  `_path`.

Example with an explicit GUID:

```json
{
  "_type": "txtr",
  "_path": "texture/example/my_texture.rpak",
  "$guid": "0x1234567890ABCDEF"
}
```

Use an explicit GUID only when reproducing an extracted asset whose original
name is unknown or whose reference must remain byte-compatible. For normal
custom assets, keep a stable path and let RePak hash it.

You can calculate a path GUID or UI-image hash with:

```powershell
.\repak.exe -pakguid "material/example/test_sknp.rpak"
.\repak.exe -uimghash "rui/example/icon"
```

## References, ordering, and automatic dependencies

An asset reference can normally be:

- a logical path string, which RePak hashes; or
- a numeric/hex GUID, which RePak uses directly.

For several asset types, a path string also lets RePak find and automatically
add the source asset. A GUID is only an external reference; RePak cannot infer a
source filename from it.

Use dependency-first order when practical:

```text
shdr -> shds -> txtr/txan -> matl -> aseq/arig -> mdl_ -> uimg/ui
```

Automatic adding means that exact order is not always required, but it remains
the clearest layout and produces better errors. Audio is stricter: an `awsr`
source must be listed before an `aevt` that plays it.

An external-reference warning does not mean the target was packed. It means the
current RPAK expects the target GUID to be supplied by another mounted RPAK.

## Supported asset types

| `_type` | Asset | Source input | v7 | v8 |
| --- | --- | --- | :---: | :---: |
| `anir` | Animation recording | `.anir` | Yes | Yes |
| `txtr` | Texture | `.dds`, optional `.json` | Yes | Yes |
| `txan` | Texture animation | `.txan` | No | Yes |
| `uimg` | UI image atlas metadata | Inline JSON plus `txtr` atlas | Yes | Yes |
| `rlcd` | LCD/screen effect | `.json` | Yes | Yes |
| `matl` | Material | `.json` plus `.uber` | Yes | Yes |
| `mt4a` | Materials-for-aspect table | `.json` | No | Yes |
| `shdr` | Compiled shader | `.msw` | Yes | Yes |
| `shds` | Shader set | `.msw` | Yes | Yes |
| `dtbl` | Data table | `.csv` | Yes | Yes |
| `stlt` | Settings layout | `.json` plus `.csv` | No | Yes |
| `stgs` | Settings instance | `.json` | No | Yes |
| `mdl_` | Respawn model | `.rmdl`, `.vg`, optional/required `.phy` | No | Yes |
| `aseq` | Animation sequence | `.rseq`, optional `.json` | No | Yes |
| `arig` | Animation rig | `.rrig` | No | Yes |
| `awsr` | Packed audio source | `.wav` or `.ogg` | No | Yes |
| `asrc` | Alias of `awsr` input handler | `.wav` or `.ogg` | No | Yes |
| `aevt` | Packed audio event | Inline JSON | No | Yes |
| `txls` | Texture list | `.json` | No | Yes |
| `Ptch` | Patch relationship metadata | Inline JSON | Yes | Yes |
| `ui` | Compiled RUI | `.ruip` | Yes | Yes |

`efct` is not a normal `files` handler in this build. The separate PCF-to-EFCT
command is documented near the end of this file.

---

## `txtr`: textures

### What it is

A `txtr` is a GPU texture asset. RePak reads a DDS file, creates the RPAK
texture header, and puts larger streamed mips into starpak storage when the
metadata asks for it.

### Entry and source files

```json
{
  "_type": "txtr",
  "_path": "texture/weapons/example/example_col.rpak"
}
```

Sources:

```text
assets/texture/weapons/example/example_col.dds
assets/texture/weapons/example/example_col.json   optional metadata
```

The map entry may force all mips into the RPAK:

```json
{
  "_type": "txtr",
  "_path": "texture/ui/example_atlas.rpak",
  "$disableStreaming": true
}
```

### Optional texture metadata

```json
{
  "streamLayout": [
    "optional",
    "mandatory",
    "permanent"
  ],
  "mipInfo": [
    0,
    0,
    0
  ],
  "resourceFlags": "0x0",
  "usageFlags": "0x0"
}
```

- `streamLayout` must describe **every mip except the base mip**. If a DDS has
  ten total mips, the array must have nine entries.
- Valid stream values are `permanent`, `mandatory`, and `optional`.
- `optional` streaming is Apex/version 8 only.
- `mipInfo`, if present, must cover the same non-base mip count.
- `resourceFlags` and `usageFlags` preserve extracted D3D metadata.
- Texture arrays are kept permanent rather than streamed.
- This build supports at most 13 mip levels.

For a newly authored texture, start without a metadata JSON or copy metadata
from a genuinely comparable extracted texture. Do not copy a stream layout
from a DDS with a different mip count.

### Practical DDS conventions

These are good material conventions, not hard RePak requirements:

| Texture role | Common format |
| --- | --- |
| Albedo/colour | BC7 sRGB |
| Normal map | BC5 unsigned, sometimes R8G8 |
| Gloss/roughness mask | BC4 unsigned |
| Specular/packed colour | BC7 sRGB |
| Emissive | BC7 sRGB |
| AO/cavity/opacity mask | BC4 unsigned |
| UI atlas | BC7 or another alpha-capable format, non-streamed |

The shader and material decide the real channel meanings. A DDS can build
successfully and still render incorrectly when its channels or colour space do
not match the chosen shader set.

## `txan`: texture animations

A `txan` stores compiled texture-animation data.

```json
{
  "_type": "txan",
  "_path": "texture_anim/example/example.rpak"
}
```

RePak reads the same stem with a `.txan` extension. There is no editable JSON
schema in this handler. A material can reference it through
`$textureAnimation`; a path reference may auto-add it.

`txan` is Apex/version 8 only.

## `txls`: texture lists

A texture list is a named array of texture GUIDs, commonly used when another
asset needs to select from a set.

Build entry:

```json
{
  "_type": "txls",
  "_path": "texture_list/example.rpak"
}
```

Source `texture_list/example.json`:

```json
{
  "textures": [
    "weapons/example/example_col",
    "weapons/example/example_nml"
  ]
}
```

Each entry is expanded to `texture/<entry>.rpak` and hashed. Do not include the
leading `texture/` or trailing `.rpak` in this JSON. The array must not be
empty.

`txls` is Apex/version 8 only.

---

## `shdr`: compiled shaders

### What it is

A `shdr` is one compiled vertex or pixel shader. The `.msw` contains the
compiled DXBC bytecode and its resource metadata. RePak packages that compiled
data; it does not compile HLSL source.

```json
{
  "_type": "shdr",
  "_path": "shader/0x0123456789ABCDEF.rpak"
}
```

Source:

```text
assets/shader/0x0123456789ABCDEF.msw
```

The MSW must identify itself as a shader wrapper. Extracted or converted shader
MSWs are the normal input. `keepDevOnly` controls whether development names are
retained in the packed shader.

## `shds`: shader sets

### What it is

A shader set joins the vertex shader and pixel shader used by a material and
records their texture/sampler/resource counts.

```text
material -> shader set -> vertex shader + pixel shader
```

The material does not normally reference the two `shdr` assets directly.

```json
{
  "_type": "shds",
  "_path": "shaderset/0x0123456789ABCDEF.rpak"
}
```

Source:

```text
assets/shaderset/0x0123456789ABCDEF.msw
```

A shader-set MSW may embed its shaders. In that case RePak can automatically
add the `shdr` assets. If it contains only shader GUIDs, those shaders must
already exist in this RPAK or another mounted dependency.

The source must be a shader-set MSW, not an ordinary shader MSW. `keepDevOnly`
controls preservation of its development name.

---

## `matl`: materials

### What it is

A material connects:

- a shader set;
- texture GUIDs in exact bind slots;
- render-state flags;
- depth/prepass/colour-pass materials;
- surface properties; and
- a raw shader constant buffer called the UBER.

A successful material build only proves the structure is valid. Rendering also
depends on the shader set, UBER layout, texture channels, model material type,
and every referenced dependency agreeing.

### Build entry and files

```json
{
  "_type": "matl",
  "_path": "material/models/weapons/example/example_sknp.rpak"
}
```

Sources:

```text
assets/material/models/weapons/example/example_sknp.json
assets/material/models/weapons/example/example_sknp.uber
```

Almost all material fields belong in the material's own JSON, not in the
top-level `files` entry.

### Material JSON shape

Use an exported material as the starting point. A simplified Apex example is:

```json
{
  "shaderType": "SKNP",
  "uberBufferFlags": 0,
  "width": 512,
  "height": 512,
  "depth": 1,
  "glueFlags": 0,
  "glueFlags2": 0,
  "name": "models/weapons/example/example",
  "surfaceProp": "metal",
  "surfaceProp2": "",
  "samplers": 0,
  "features": 0,
  "blendStateMask": 0,
  "depthStencilFlags": 7,
  "rasterizerFlags": 6,
  "blendStates": [
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0
  ],
  "shaderSet": "shaderset/0x0123456789ABCDEF.rpak",
  "$textures": {
    "0": "texture/weapons/example/example_col.rpak",
    "1": "texture/weapons/example/example_nml.rpak",
    "2": "texture/weapons/example/example_gls.rpak"
  }
}
```

Do not treat the numeric values above as a universal preset. They are only
showing the field types. Copy the structural values from a stock material with
the same shader type and purpose.

Required structural fields are:

- `shaderType`
- `uberBufferFlags` for Apex material v15
- `width`, `height`, `depth`
- `glueFlags`, `glueFlags2`
- `name`
- `surfaceProp`, `surfaceProp2`
- `samplers`
- `features`
- `blendStateMask`, `depthStencilFlags`, `rasterizerFlags`
- `blendStates`, with exactly 4 entries for Titanfall 2/v7 or 8 entries for
  Apex/v8
- `shaderSet`

### Texture bind slots

`$textures` is an object whose property names are numeric bind points:

```json
"$textures": {
  "0": "texture/example/albedo.rpak",
  "3": "0xAABBCCDDEEFF0011"
}
```

- A path string is hashed and its DDS may be auto-added.
- A hex GUID is an external reference.
- Bind points are shader-defined. Slot 0 is not guaranteed to mean albedo for
  every shader.
- `textureSlotCount` can force the array to include unused trailing slots:

```json
"textureSlotCount": 8
```

- `disableStreaming: true` in the material JSON makes auto-added textures
  permanent.

Never renumber an extracted `$textures` object simply to remove gaps. Those
numbers are bindings, not a list order.

### UBER files

An UBER is **not another RPAK asset type**. It is the raw material CPU constant
buffer placed inside the `matl` asset.

By default, RePak reads the same stem:

```text
example_sknp.json
example_sknp.uber
```

The material JSON can reuse a different UBER:

```json
"$uber": "material/models/weapons/base/base_sknp"
```

RePak appends `.uber`, so that example reads:

```text
assets/material/models/weapons/base/base_sknp.uber
```

Important UBER rules:

- The file is mandatory. This build deliberately errors instead of inventing
  a fallback buffer that would render incorrectly.
- It must match the chosen shader set and material version.
- It contains packed constants such as tints, UV transforms, scalar controls,
  emissive values, and feature parameters in the exact layout expected by the
  shader.
- RePak does not understand or rebuild a node graph when handling `matl`; it
  copies the compiled buffer.
- A same-sized but semantically wrong UBER often produces fullbright, black,
  oddly glossy, wrongly glowing, or UV-distorted output without causing a
  build error.

For a new material, begin with a complete JSON+UBER pair from the closest stock
material, then change texture paths and only the UBER fields you understand.

### Render-pass materials

Optional fields are:

```json
{
  "$depthShadowMaterial": "material/code_private/depth_shadow.rpak",
  "$depthPrepassMaterial": "material/code_private/depth_prepass.rpak",
  "$depthVSMMaterial": "material/code_private/depth_vsm.rpak",
  "$depthShadowTightMaterial": "material/code_private/depth_shadow_tight.rpak",
  "$colpassMaterial": "material/example/example_colpass.rpak"
}
```

String paths can be auto-added. Explicit GUIDs remain external. An explicit
zero such as `"0x0"` disables that pass. When a field is omitted, RePak may use
its code-private default according to shader and rasterizer type.

Using the wrong depth material can cause bad silhouettes, self-shadowing,
incorrect first-person rendering, or fullbright-looking surfaces even when the
colour textures are correct.

### Texture animation

```json
"$textureAnimation": "texture_anim/example/example.rpak"
```

A string can auto-add `txan`; a GUID only references an existing asset.

### Titanfall 2 material presets

Version 7 supports the legacy material type and these optional `$preset`
values:

```text
ironsight
epg_mag
hair
opaque
```

They set known render-state combinations. They do not replace the need for a
valid exported material and matching UBER.

## `mt4a`: materials for aspect

`mt4a` stores a material GUID for each supported rendering aspect/category.

Build entry:

```json
{
  "_type": "mt4a",
  "_path": "material_for_aspect/example.rpak"
}
```

Source `material_for_aspect/example.json`:

```json
{
  "materials": [
    "material/example/example_sknp.rpak",
    "0x0123456789ABCDEF"
  ]
}
```

Entries may be paths or GUIDs. The array must not be empty and must not exceed
the engine's material-shader-type count. Use an extracted table as the template
because index meanings are engine-defined.

`mt4a` is Apex/version 8 only.

---

## `dtbl`: data tables

### What it is

A data table is a typed CSV used directly by scripts and game systems.

```json
{
  "_type": "dtbl",
  "_path": "datatable/example/items.rpak"
}
```

RePak reads:

```text
assets/datatable/example/items.csv
```

### CSV structure

The first row contains column names. The **last row contains the column
types**:

```csv
"name","enabled","count","scale","origin","icon"
"first_item","true","3","1.5","<0,0,0>","rui/example/icon"
"second_item","false","8","0.75","<1,2,3>","rui/example/icon_2"
"string","bool","int","float","vector","asset"
```

Supported types:

| Type | Input |
| --- | --- |
| `bool` | `true`, `false`, `1`, or `0` |
| `int` | Integer |
| `float` | Floating-point number |
| `vector` | `<x,y,z>` |
| `string` | Ordinary string |
| `asset` | Asset path string plus a precache/dependency GUID |
| `asset_noprecache` | Asset path string without a dependency GUID |

Every row, including the type row, must have exactly the same number of
columns. A blank line can become a one-column row and cause:

```text
Expected N columns for data row #X, found 1
```

Remove stray blank rows, preserve commas for empty cells, and quote values that
contain commas.

Use `asset` only when the referenced RPAK asset must be resolved before the
datatable loads. Use `asset_noprecache` for a path that scripts will interpret
later.

---

## `stlt`: settings layouts

### What it is

A settings layout defines the typed schema used by one or more `stgs` assets.
It needs a JSON control file and a CSV field table.

```json
{
  "_type": "stlt",
  "_path": "settings_layout/settings_example_layout.rpak"
}
```

Sources:

```text
assets/settings_layout/settings_example_layout.json
assets/settings_layout/settings_example_layout.csv
```

### Layout JSON

Simple layout:

```json
{
  "extraDataSizeIndex": 1
}
```

Layout with nested arrays:

```json
{
  "elementCount": 1,
  "extraDataSizeIndex": 1,
  "subLayouts": [
    "settings_layout/settings_example_layout/tags.rpak"
  ]
}
```

- `extraDataSizeIndex` is required. Preserve it from an extracted comparable
  layout unless you understand the runtime consumer.
- `elementCount` defaults to 1. On a sub-layout it controls a static array's
  number of elements.
- `subLayouts` is required when the CSV contains `array` or `array_dynamic`
  fields. Its order must match the layout indices used in the CSV.
- Every sub-layout path has its own `.json` and `.csv` pair.

### Layout CSV

The CSV has exactly four columns:

```csv
"fieldName","dataType","layoutIndex","helpText"
"assetName","string","0",""
"enabled","bool","0","Whether this item is enabled."
"origin","float3","0","World-space origin."
"tags","array","0","Static tag array."
```

Supported field types:

```text
bool
int
float
float2
float3
string
asset
asset_noprecache
array
array_dynamic
```

`layoutIndex` matters only for the array types and selects an entry in
`subLayouts`.

Field ordering matters because the packed structure does not permit arbitrary
padding. Keep fields sorted from stricter/larger alignment to smaller
alignment, and use extracted layouts as the authoritative ordering.

`asset` stores a path and registers a dependency. `asset_noprecache` stores the
path only.

## `stgs`: settings instances

### What it is

A `stgs` asset supplies values for a `stlt` schema. Character skins, weapon
skins, item flavours, abilities, and many other engine records use settings
assets.

```json
{
  "_type": "stgs",
  "_path": "settings/example/my_item.rpak"
}
```

RePak reads:

```text
assets/settings/example/my_item.json
```

Example:

```json
{
  "layoutAsset": "settings_layout/settings_example_layout.rpak",
  "uniqueId": 123456789,
  "settings": {
    "assetName": "settings/example/my_item.rpak",
    "enabled": true,
    "count": 4,
    "scale": 1.0,
    "origin": "<0,0,0>",
    "model": "mdl/example/example.rmdl",
    "tags": [
      {
        "name": "custom"
      }
    ]
  }
}
```

- `layoutAsset` is required and must identify a valid `stlt`.
- `uniqueId` is optional and defaults to 0. Preserve extracted IDs when
  replacing stock content, and choose stable unique IDs for custom content.
- `settings` is required.
- Field names, types, nested object shapes, and static-array lengths must match
  the layout exactly.
- `float2` and `float3` values use strings such as `<1,2>` and `<1,2,3>`.
- Fields of layout type `asset` become dependencies; `asset_noprecache` remains
  a string.

### Settings modifiers

Some settings assets contain runtime modifier tables:

```json
{
  "$modNames": [
    "example_mod"
  ],
  "$modValues": [
    {
      "index": 0,
      "type": "number",
      "field": "count",
      "value": 8
    }
  ],
  "$modFlags": 0
}
```

`$modNames` and `$modValues` must either both be present or both be absent.
Each modifier's `index` selects a name. It targets either a `field` access path
or an absolute `offset`.

Accepted modifier names in this build are:

```text
int_add
int_multipy
float_add
float_multipy
bool
number
string
```

The `multipy` spelling is intentional in the current source. Modifier value
types must be compatible with the target layout field. Prefer `field` over
`offset`; it is readable and survives layout adjustments.

---

## `mdl_`: Respawn models

### What it is

`mdl_` packages an Apex RMDL v54 model and its render geometry. Note that the
map type is `mdl_`, not `rmdl`.

```json
{
  "_type": "mdl_",
  "_path": "mdl/weapons/example/example.rmdl",
  "$animrigs": [
    "animrig/weapons/example/example.rrig"
  ],
  "$sequences": [
    "animseq/weapons/example/example_idle.rseq"
  ]
}
```

Files:

```text
assets/mdl/weapons/example/example.rmdl
assets/mdl/weapons/example/example.vg
assets/mdl/weapons/example/example.phy   optional or model-required
```

Rules:

- The RMDL must be version 54 for this Apex handler.
- `.vg` is required when client/render data is kept.
- `.phy` is optional only when the RMDL does not declare required physics. If
  physics is declared, the file and expected size must match.
- `$animrigs` registers rig references. It does not auto-create missing RRIG
  files.
- `$sequences` may auto-add string-path `aseq` assets.
- Embedded material GUIDs are dependencies and their shader types must agree
  with the model's material slots.

`$materials` can override material GUID slots, but the source warns that it
does not rewrite the RMDL's internal path records and may cause runtime errors.
Fix or byte-edit the RMDL's material records when making a real port rather
than relying on this as a general remapping system.

Inventory the full model dependency closure: body materials, sights, optics,
suppressors, magazines, counters, reticles, attachment models, rigs,
sequences, and physics.

`mdl_` is Apex/version 8 only.

## `arig`: animation rigs

An animation rig contains the skeleton/rig used by models and sequences.

```json
{
  "_type": "arig",
  "_path": "animrig/weapons/example/example.rrig",
  "$sequences": [
    "animseq/weapons/example/example_idle.rseq"
  ]
}
```

RePak reads the `.rrig` path directly. `$sequences` is optional; string paths
may auto-add `aseq` assets and GUIDs remain references.

The rig's bones, ordering, and checksum relationships must match the model and
animation data. RePak does not retarget animation between skeletons.

`arig` is Apex/version 8 only.

## `aseq`: animation sequences

### Important naming correction

Use:

```json
{
  "_type": "aseq",
  "_path": "animseq/example/example.rseq"
}
```

The source extension is `.rseq`, but the registered RePak type is **`aseq`**.
`"_type": "rseq"` is not supported by this build.

An optional same-stem metadata file can add dependencies:

```text
assets/animseq/example/example.rseq
assets/animseq/example/example.json
```

```json
{
  "dependencies": [
    "mdl/example/prop.rmdl",
    "settings/example/effect.rpak",
    "0x0123456789ABCDEF"
  ]
}
```

RePak also inspects supported sequence event options for asset references and
registers autolayer sequence GUIDs. Metadata dependencies do not repair broken
bone animation, event payloads, or a sequence exported for the wrong skeleton.

`aseq` is Apex/version 8 only.

## `anir`: animation recordings

An animation recording is precompiled recording data:

```json
{
  "_type": "anir",
  "_path": "anim_recording/example/example.rpak"
}
```

RePak reads the same stem with `.anir`. The handler validates its binary header,
version, element count, and frame count, then marks the asset persistent. There
are no editable sidecar fields.

---

## `ui`: compiled RUI packages

### What it is

A `ui` asset is a compiled RUI program: arguments, defaults, transforms,
render jobs, styles, and optional keyframing data. RePak packages `.ruip`; it
does not compile a script-like source RUI in this handler.

```json
{
  "_type": "ui",
  "_path": "ui/example/example.rpak"
}
```

Source:

```text
assets/ui/example/example.ruip
```

The optional `$ruiVersion` selects a supported target:

```json
{
  "_type": "ui",
  "_path": "ui/example/example.rpak",
  "$ruiVersion": 39
}
```

This fork accepts target RUI versions 30 (Titanfall 2), 39 (Apex), and 40.
When explicitly targeting Apex v39, it can convert the known v40 and v42
render-job differences, and v42 transform records. Unsupported opcodes or
structures are rejected rather than silently producing a corrupt RUI.

The argument names and types exposed by the RUIP are its runtime API. A script
call such as `RuiSetString(rui, "weaponName", value)` will fail if `weaponName`
is absent or has another type. Check the compiled package's exposed arguments,
not just the script that creates it.

## `uimg`: UI image atlases

### What it is

A `uimg` maps named UI sprites to rectangles in one non-streamed atlas texture.
RUI code usually refers to the image name/hash, not directly to the DDS.

```json
{
  "_type": "uimg",
  "_path": "rui/example/example_atlas.rpak",
  "atlas": "texture/ui/example_atlas.rpak",
  "images": [
    {
      "path": "rui/example/icon_a",
      "posX": 0,
      "posY": 0,
      "width": 128,
      "height": 128
    },
    {
      "path": "rui/example/icon_b",
      "posX": 128,
      "posY": 0,
      "width": 256,
      "height": 128,
      "cropInsetLeft": 0.0,
      "cropInsetTop": 0.0,
      "startAnchorX": 0.0,
      "startAnchorY": 0.0,
      "endAnchorX": 1.0,
      "endAnchorY": 1.0,
      "scaleRatioX": 1.0,
      "scaleRatioY": 1.0
    }
  ]
}
```

- `atlas` is a texture path or an explicit 64-bit GUID.
- A string atlas path auto-adds its `txtr` and forces it non-streamed.
- `images` may contain at most 65536 records.
- `path` is a UI image name or an explicit 32-bit hash such as `0x12345678`.
- `width` and `height` are required.
- Normal authoring uses `posX` and `posY`.
- Crop, anchor, and scale fields default to the full untrimmed sprite.
- `keepDevOnly` keeps readable image names; otherwise the runtime hash remains.

For exact round-tripping of extracted retail UV floats, all four hexadecimal
bit fields may replace `posX`/`posY` UV calculation:

```json
{
  "path": "0x12345678",
  "width": 128,
  "height": 128,
  "uvMinXBits": "0x00000000",
  "uvMinYBits": "0x00000000",
  "uvSizeXBits": "0x3F000000",
  "uvSizeYBits": "0x3F000000"
}
```

All four `uv*Bits` fields must be present together.

Extracted atlases can also contain:

```json
{
  "unkCount": 1,
  "unknownDataHex": "..."
}
```

`unknownDataHex` must contain exactly `unkCount * 32` bytes, encoded as two hex
characters per byte. Preserve it for a retail round trip; do not invent it for
a simple custom atlas.

## `rlcd`: LCD/screen effects

Build entry:

```json
{
  "_type": "rlcd",
  "_path": "rui/lcd/example.rpak"
}
```

Source `rui/lcd/example.json`:

```json
{
  "pixelScaleX1": 1.0,
  "pixelScaleX2": 1.0,
  "pixelScaleY": 1.0,
  "brightness": 1.0,
  "contrast": 1.0,
  "waveOffset": 0.0,
  "waveScale": 0.0,
  "waveSpeed": 0.0,
  "wavePeriod": 1.0,
  "bloomAdd": 0.0,
  "doBloomLuminance": false,
  "pixelFlicker": 0.0
}
```

Every shown field is required. This describes a screen/LCD post-effect preset,
not the RUI layout itself.

---

## `awsr` and `asrc`: packed audio sources

### What they are

An audio source contains the real sound bytes and format information.
`awsr` is the normal custom packed-WAV source type in this fork. `asrc` is an
input alias that calls the same handler and produces the same source asset
format.

```json
{
  "_type": "awsr",
  "_path": "audio/weapons/example/fire_01.rpak",
  "wav": "audio/weapons/example/fire_01.wav"
}
```

OGG/Vorbis input is accepted and decoded to 16-bit PCM WAV during the build:

```json
{
  "_type": "awsr",
  "_path": "audio/weapons/example/fire_01.rpak",
  "ogg": "audio/weapons/example/fire_01.ogg"
}
```

`file` is a generic alternative:

```json
"file": "audio/weapons/example/fire_01.ogg"
```

If none of `wav`, `ogg`, or `file` is supplied, RePak reads the asset path with
its extension changed to `.wav`.

Optional virtual filename:

```json
"virtualName": "rpakwav/example_fire.wav"
```

Normally RePak generates a stable virtual name from the source GUID.

The source must be RIFF/WAVE PCM or IEEE float. OGG input must be Vorbis.
Packed bytes are placed in the mandatory starpak, so the build map must set
`streamFileMandatory`.

## `aevt`: packed audio events

An event is the name scripts or animation events trigger. It selects one or
more source assets:

```json
{
  "_type": "aevt",
  "_path": "Weapon_Example_Fire_1P",
  "sources": [
    "audio/weapons/example/fire_01.rpak",
    "audio/weapons/example/fire_02.rpak"
  ],
  "volume": 0.5,
  "pitch": 1.0,
  "music": false,
  "replaceSameEvent": false
}
```

Fields:

- `mode` defaults to `play`.
- `sources` is required and non-empty for `play`.
- `volume` and `pitch` default to `1.0`.
- `flags` supplies raw numeric event flags.
- `music` and `replaceSameEvent` set the corresponding known flags.

Sources may be path strings or GUIDs. For play events, the first referenced
source must already have been added as an `awsr`, so list source entries before
events.

Control modes are:

```text
stop
stop_events
stop_music
stop_all
stop_managed
```

`stop` is an alias of `stop_events`. A stop-events entry needs a non-empty
`targets` array:

```json
{
  "_type": "aevt",
  "_path": "Stop_Weapon_Example",
  "mode": "stop_events",
  "targets": [
    "Weapon_Example_Fire_1P"
  ]
}
```

The other control modes may operate without sources/targets.

---

## `Ptch`: patch relationships

The type name has an uppercase `P` and is case-sensitive:

```json
{
  "_type": "Ptch",
  "_path": "patch/example.rpak",
  "entries": [
    {
      "name": "common",
      "version": 1
    }
  ]
}
```

Each entry records a pak name and patch number. This is advanced RPAK patch
metadata, not a request to diff or merge the listed RPAKs. Use it only when
reproducing a known patch relationship.

---

## PCF plus JSON to `efct`

`efct` is not registered as a normal build-map `_type`. This fork provides a
separate template-backed compiler:

```powershell
.\bin\Release\repak.exe -pcf `
  "C:\path\effect.pcf" `
  "C:\path\s3_effects.decoded.rpak" `
  "C:\path\effects_custom.rpak" `
  "C:\path\effect.efct.json"
```

The inputs are:

1. A Source/Titanfall binary DMX v5 / PCF v2 effect.
2. A decoded Season 3 effects RPAK used as the native template source.
3. The output standalone effects RPAK.
4. An optional editable JSON project sidecar.

The first run can create the JSON. Later runs preserve it. It controls output
names, material references, particle counts, included systems, and validated
same-size compiled operator payload overrides.

This does not copy arbitrary PCF operators directly into Apex. Apex expects a
native compiled EFCT operator graph, so the compiler maps the PCF to compatible
Season 3 templates. Unsupported graph shapes must be repaired or given a
matching template.

The detailed PCF schema, operator meanings, limits, and examples are in
[PCF + JSON to Season 3 EFCT authoring](../../documentation/pcf-json-efct-authoring.md).

## A complete material/model folder example

```text
assets/
|-- shader/
|   |-- vertex_example.msw
|   `-- pixel_example.msw
|-- shaderset/
|   `-- example_sknp.msw
|-- texture/
|   `-- weapons/
|       `-- example/
|           |-- example_col.dds
|           |-- example_nml.dds
|           `-- example_gls.dds
|-- material/
|   `-- models/
|       `-- weapons/
|           `-- example/
|               |-- example_sknp.json
|               `-- example_sknp.uber
|-- animrig/
|   `-- weapons/
|       `-- example.rrig
|-- animseq/
|   `-- weapons/
|       |-- example_idle.rseq
|       `-- example_idle.json
`-- mdl/
    `-- weapons/
        `-- example/
            |-- example.rmdl
            |-- example.vg
            `-- example.phy
```

Example build order:

```json
{
  "version": 8,
  "name": "example_weapon",
  "assetsDir": "assets/",
  "outputDir": "build/",
  "streamFileMandatory": "paks/Win64/example_weapon.starpak",
  "streamFileOptional": "paks/Win64/example_weapon.opt.starpak",
  "files": [
    {
      "_type": "shdr",
      "_path": "shader/vertex_example.rpak"
    },
    {
      "_type": "shdr",
      "_path": "shader/pixel_example.rpak"
    },
    {
      "_type": "shds",
      "_path": "shaderset/example_sknp.rpak"
    },
    {
      "_type": "txtr",
      "_path": "texture/weapons/example/example_col.rpak"
    },
    {
      "_type": "txtr",
      "_path": "texture/weapons/example/example_nml.rpak"
    },
    {
      "_type": "txtr",
      "_path": "texture/weapons/example/example_gls.rpak"
    },
    {
      "_type": "matl",
      "_path": "material/models/weapons/example/example_sknp.rpak"
    },
    {
      "_type": "aseq",
      "_path": "animseq/weapons/example_idle.rseq"
    },
    {
      "_type": "arig",
      "_path": "animrig/weapons/example.rrig",
      "$sequences": [
        "animseq/weapons/example_idle.rseq"
      ]
    },
    {
      "_type": "mdl_",
      "_path": "mdl/weapons/example/example.rmdl",
      "$animrigs": [
        "animrig/weapons/example.rrig"
      ],
      "$sequences": [
        "animseq/weapons/example_idle.rseq"
      ]
    }
  ]
}
```

## Common failure patterns

### Builds, but the asset is black or fullbright

Check the whole material contract:

1. correct material shader type;
2. matching shader set;
3. both shader assets available;
4. correct UBER for that shader set;
5. texture bind slots unchanged;
6. DDS formats/channels/colour spaces correct;
7. depth and colpass materials suitable for the model;
8. model material slot expects the same shader type.

RePak cannot diagnose the visual meaning of compiled shader constants.

### "Expected N columns ... found 1"

The CSV contains a blank/malformed row or a value with an unquoted comma. Every
row must have the same number of fields.

### Texture stream-layout count mismatch

`streamLayout` must have one entry for every mip except the base mip. Regenerate
it for the actual DDS instead of reusing metadata from a different texture.

### Missing model index at runtime

The model is not mounted early enough or is not precached by the script/game
system that creates it. Packing a model is separate from registering or
precaching it.

### RUI "doesn't expose argument ..."

The compiled `.ruip` does not expose that exact argument with that exact type.
Change the script call or rebuild/replace the RUIP contract.

### Asset was "auto-added" but still missing

Only certain path-string references auto-add. Explicit GUIDs cannot reveal a
source filename, and some fields such as model `$animrigs` only register a
reference. Add important dependencies explicitly and inspect warnings.

### RPAK loads but the game crashes

Treat the build as structurally accepted, not runtime-proven. Check:

- target RPAK version;
- source asset version;
- model/rig/sequence compatibility;
- material and UBER sizes;
- dependency mount order;
- required starpak presence;
- server/client scope flags; and
- the first fresh runtime error or crash address.

## Before calling a pack complete

- Build the intended map or build list, not a nearby copy.
- Resolve all RePak errors and review every external-reference warning.
- Confirm the output RPAK and starpak timestamps changed.
- Keep RPAK and dependent DLL/starpak outputs together.
- Confirm each `_path` hashes to the GUID expected by scripts and other assets.
- Validate material JSON, UBER, shader set, shaders, and textures as one unit.
- Validate every model attachment, optic, suppressor, sight, reticle, counter,
  sequence, rig, and physics file.
- Restart the game when it already loaded an older RPAK, DLL, RUI, or script.
- Use a fresh log and screenshot for runtime visual validation.
