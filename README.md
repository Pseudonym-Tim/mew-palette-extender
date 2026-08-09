# Mew Palette Extender
A DLL dependency mod that allows other mods to add new custom color palettes to the game!

<img width="484" height="321" alt="preview" src="https://github.com/user-attachments/assets/9c333a8f-dfb5-46a4-84c2-ae351dcf6a28" />

# Making a Custom Palette Mod

To make a palette mod, install [MewPaletteExtender](https://www.nexusmods.com/mewgenics/mods/369), then create your own mod folder next to it.

First, please ensure that in your description.json of your mod, that you require this mod as a dependency so players know to download it:

```text
"requirements": [
    "MewPaletteExtender>=1.1.0"
  ]
```

Your palette mod should include a `palette_rows.txt` file and, optionally, a `palettes` folder for PNG palette strips.

Here's how the file layout should look:

```text
mods/
  MewPaletteExtender/
    MewPaletteExtender.dll
  YourPaletteMod/
    description.json
    preview.png
    palette_rows.txt
    palettes/
      fairy_pink.png
```

Inside `palette_rows.txt`, add one palette per line.

You can use either **16 hex colors**:

```text
fairyPink = #1A0610 #321020 #4A1930 #632240 #7C2B50 #963460 #B03D70 #CA4680 #E45090 #F062A0 #F578B0 #FA8EC0 #FCA8D0 #FDC2DF #FEDCEF #FFF6FB
```

Or a **16x1 PNG strip**:

```text
fairyPink = palettes/fairy_pink.png
```

PNG strips must be exactly **16 pixels wide and 1 pixel tall**. Each pixel becomes one palette color, read from left to right.

Recommended palette naming convention:

```text
myMod.paletteID = palettes/my_palette.png
```

Once defined, you can then use these palettes in any .gon script files by doing:
```text
palette @myMod.paletteID
```
