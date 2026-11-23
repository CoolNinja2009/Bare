# The default font

`Inter-Regular.ttf` is [Inter](https://github.com/rsms/inter) 4.1 Regular by The Inter Project Authors, under the
SIL Open Font License 1.1 (`OFL.txt`). It's embedded into Bare at build time (`cmake/embed_file.cmake`); build with
`-DBARE_DEFAULT_FONT=OFF` to leave it out.

It is a subset: Latin-1, common punctuation and symbols (dashes, quotes, €, ™, arrows, ✓ ✕, minus), with only the
`kern` feature and no hinting (stb_truetype ignores hints), which takes it from 411 KB to 36 KB. Made with
fonttools from `extras/ttf/Inter-Regular.ttf` in the release zip:

```powershell
python -m fontTools.subset Inter-Regular.ttf --output-file=subset.ttf --layout-features=kern --no-hinting `
  --desubroutinize --unicodes="U+0000-00FF,U+0131,U+0152-0153,U+02BB-02BC,U+02C6,U+02DA,U+02DC,U+2000-206F,U+2074,U+20AC,U+2122,U+2190-2193,U+2212,U+2215,U+2713,U+2715,U+FEFF,U+FFFD"
```

Then its GPOS extension lookups (type 9) are unwrapped into plain pair-adjustment lookups (type 2), because
stb_truetype only reads the latter and would otherwise lose all kerning:

```python
from fontTools.ttLib import TTFont
f = TTFont("subset.ttf")
for lookup in f["GPOS"].table.LookupList.Lookup:
    if lookup.LookupType == 9:
        lookup.LookupType = lookup.SubTable[0].ExtensionLookupType
        lookup.SubTable = [s.ExtSubTable for s in lookup.SubTable]
        lookup.SubTableCount = len(lookup.SubTable)
f.save("Inter-Regular.ttf")
```
