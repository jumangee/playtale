# FINKBMP v1 parser

Files:
- `FinkBmpImage.h` — owns decoded pixel bytes and exposes a non-owning `BitmapRef`.
- `FinkBmpParser.h/.cpp` — incremental parser; accepts arbitrarily fragmented input.

## Wire format

All multi-byte integers are little-endian. The fixed header is 18 bytes:

| Offset | Size | Field | v1 value / meaning |
|---:|---:|---|---|
| 0 | 8 | Magic | `FINKBMP\0` |
| 8 | 1 | Version | `1` |
| 9 | 1 | Format | `0=Mask1`, `1=BW1`, `2=Gray2`, `3=NativeBmp` |
| 10 | 2 | Width | `uint16` |
| 12 | 2 | Height | `uint16` |
| 14 | 4 | DataSize | pixel payload byte count (`uint32`) |
| 18 | DataSize | Data | opaque payload for the selected format |

For `Mask1` and `BW1`, FreeInkUI's current contract defines row-major, MSB-first,
byte-aligned rows, with `bytesPerRow = (width + 7) / 8` and
`DataSize = bytesPerRow * height`. `Mask1` uses bit 0 = draw / bit 1 = transparent;
`BW1` uses bit 1 = draw.

`Gray2` and `NativeBmp` are accepted as opaque payloads. Their internal byte
layout is intentionally not interpreted or validated by this parser yet.

## Use with streamed HTTP data

```cpp
FinkBmpParser parser;

// In HttpDownloader::DataCallback:
const bool ok = parser.consume(chunk, chunkSize);

// After fetchUrl() has completed successfully:
if (!ok || !parser.finish()) {
  // inspect parser.error(); discard the response
}

FinkBmpImage image;
if (!parser.takeImage(image)) {
  // no complete image
}

BitmapRef bitmap = image.bitmapRef();
// Keep `image` alive while the UI uses `bitmap`.
```

The parser copies chunks straight into the final allocation owned by
`FinkBmpImage`; it does not keep a second copy of the response body.

## Limits

Default parser limits are 800x600 pixels and 256 KiB of payload. Supply a
`FinkBmpParser::Limits` value to raise or lower them for a particular caller.
The limits are checked before allocating the payload buffer.

`BitmapRef::progmem` is set to `false`, because the decoded buffer is allocated
in ordinary RAM. `BitmapRef` itself does not own the buffer.
