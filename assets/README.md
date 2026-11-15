# CED identity

The blue angular C with a terminal cursor is the project's primary logo.

- `ced.png`: 1024 × 1024 transparent PNG for the README and general use.
- `ced-256.png`: 256 × 256 PNG embedded for the empty editor pane and macOS CLI icon, and installed in the Linux icon theme.
- `../packaging/ced.icns`: macOS bundle icon, containing standard and Retina sizes from 16 to 1024 pixels.

The chosen concept was generated with the built-in ImageGen tool. Its white presentation background was removed with ImageGen, preserving the dark tile and blue mark. PNG sizes were exported with `sips`; the macOS icon was assembled with `iconutil`.

## Generation prompt

```text
A bold blue angular C ending in a terminal underscore on a dark rounded-square tile, flat geometric style.
```

## Background removal prompt

```text
Remove only the white exterior presentation background outside the dark rounded square, making those exterior areas transparent. Preserve the dark tile and blue angular C plus underscore: silhouette, colors, proportions, placement, edge angles, rounded corners and spacing. Keep the square canvas and existing margin. No text or additions.
```

## Exporting updated assets on macOS

Run from the repository root after replacing `assets/ced.png` with a square 1024px transparent PNG:

```sh
sips -z 256 256 assets/ced.png --out assets/ced-256.png
mkdir -p build/ced.iconset
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" assets/ced.png --out "build/ced.iconset/icon_${size}x${size}.png"
    double=$((size * 2))
    sips -z "$double" "$double" assets/ced.png --out "build/ced.iconset/icon_${size}x${size}@2x.png"
done
iconutil -c icns build/ced.iconset -o packaging/ced.icns
make
```
