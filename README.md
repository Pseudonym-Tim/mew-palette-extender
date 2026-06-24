# Mew Palette Extender
A DLL dependency mod that allows other mods to add new custom color palettes to the game!

<img width="484" height="321" alt="preview" src="https://github.com/user-attachments/assets/9c333a8f-dfb5-46a4-84c2-ae351dcf6a28" />

# Making a Custom Palette Mod

To make a palette mod, install **MewPaletteExtender** once, then create your own mod folder next to it.

Your palette mod should include a `palette_rows.txt` file and, optionally, a `palettes` folder for PNG palette strips.

Here’s how the file layout should look:

```text
mods/
  MewPaletteExtender/
    MewPaletteExtender.dll
  YourPaletteMod/
    palette_rows.txt
    palettes/
      fairy_pink.png
```

Inside `palette_rows.txt`, add one palette per line.

You can use either **16 hex colors**:

```text
fairy_pink@420 = #1A0610 #321020 #4A1930 #632240 #7C2B50 #963460 #B03D70 #CA4680 #E45090 #F062A0 #F578B0 #FA8EC0 #FCA8D0 #FDC2DF #FEDCEF #FFF6FB
```

Or a **16x1 PNG strip**:

```text
fairy_pink@420 = palettes/fairy_pink.png
```

PNG strips must be exactly **16 pixels wide and 1 pixel tall**. Each pixel becomes one palette color, read from left to right.

Palette row IDs are assigned automatically if not specified. When assigning an explicit row for your palettes, make sure each row ID is a unique number between **256 and 1024**.

NOTE: Right now you just have to pick a number that you think is unique enough to avoid conflictions with any mods. In the future, perhaps I will update the mod so that it will dynamically avoid any potential conflictions, but this is so niche right now that I'm not going to bother. At least not until a proper in-general cat framework is complete/provided!
