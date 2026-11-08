# Example photos

All four examples are CC0 (public domain dedication) images from Wikimedia Commons, downscaled to
at most 1600 px.

| File | Subject | Author / source | License |
|---|---|---|---|
| `statue.jpg` | Statue of Leda and the Swan | J. Paul Getty Museum, Open Content Program · [Commons](https://commons.wikimedia.org/wiki/File:Statue_of_Leda_and_the_Swan_-_Getty_Museum_(70.AA.110).jpg) | CC0 |
| `livingroom.jpg` | Living room interior | Pixabay, via [Commons](https://commons.wikimedia.org/wiki/File:Living_Room_Interior_Designer_-_Picker_Online.jpg) | CC0 |
| `bedroom.jpg` | Bedroom, Arundel Castle, 2019 | DimiTalen · [Commons](https://commons.wikimedia.org/wiki/File:Bedroom,_Arundel_Castle,_2019.jpg) | CC0 |
| `stilllife.jpg` | *Still Life: Fruit*, Severin Roesen, 1855 | The Metropolitan Museum of Art · [Commons](https://commons.wikimedia.org/wiki/File:Still_Life-_Fruit_MET_DT301875.jpg) | CC0 |

## Cached depth (`*.depth.bin`)

The examples open instantly because their AI depth is stored next to them, so visitors do not
have to download the model just to try an example. Format: `uint32 width`, `uint32 height`
(little endian), then `width × height` `uint16` values (normalized disparity × 65535, row 0 = top),
written and read by `encode_depth16` / `decode_depth16` in [core/src/depth.cpp](../core/src/depth.cpp).
That is 16 bits instead of the 8 bits a PNG would usually give: 8-bit depth shows visible terraces
in the computed normals. The file holds the depth *before* filtering, so the filter and sharpening
settings can change without re-running the network.

To add an example: put the photo in `samples/`, add an entry to `manifest.json` (lamp positions
and slider presets are optional), then run

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bake.ps1
```

This opens `index.html?bake` in headless Chrome. The page runs the same AI path a visitor's photo
takes and posts each result to `scripts/serve.ps1`, which writes `samples/<id>.depth.bin`.
