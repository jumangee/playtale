<?php

class StorygameCoverConvert
{
    /**
     * Largest output the device may ask for. A caller requests its own slot
     * size; this is the ceiling, not the size.
     */
    const MAX_WIDTH = 800;
    const MAX_HEIGHT = 600;

    const MAX_SOURCE_PIXELS = 20000000;


    /**
     * Convert a game cover into a 1-bit BMP that the PlayTale firmware draws
     * without any image decoder.
     *
     * The firmware reads this file through Bitmap::parseHeaders(), which
     * accepts 1 bpp BI_RGB, and draws it through GfxRenderer::drawBitmap1Bit(),
     * which takes the palette from the header. So the output is a plain
     * top-down 1-bit BMP with the standard palette: index 0 black, index 1
     * white, a set bit meaning white.
     *
     * The dithering is unchanged: Floyd-Steinberg, two error rows, 128
     * threshold. Only the packing differs from the old JSON payload, which
     * base64-encoded the same bits.
     *
     * @param string $filePath Absolute path to source image.
     * @param int    $toWidth  Requested width ceiling; 0 uses MAX_WIDTH.
     * @param int    $toHeight Requested height ceiling; 0 uses MAX_HEIGHT.
     *
     * @return string|false BMP bytes on success, false on any error.
     */
    public static function forPlayTale($filePath, $toWidth, $toHeight)
    {
        if (!function_exists('imagecreatefromstring')) {
            return false;
        }

        if (!is_file($filePath) || !is_readable($filePath)) {
            return false;
        }

        // Detect source image format and dimensions.
        $info = @getimagesize($filePath);

        if ($info === false) {
            return false;
        }

        $sourceWidth = $info[0];
        $sourceHeight = $info[1];
        $imageType = $info[2];

        if (
            $sourceWidth < 1 ||
            $sourceHeight < 1 ||
            ($sourceWidth * $sourceHeight) > self::MAX_SOURCE_PIXELS
        ) {
            return false;
        }

        $allowedTypes = array(
            IMAGETYPE_JPEG,
            IMAGETYPE_PNG,
            IMAGETYPE_GIF
        );

        if (!in_array($imageType, $allowedTypes)) {
            return false;
        }

        // Load source image.
        $imageBytes = @file_get_contents($filePath);

        if ($imageBytes === false) {
            return false;
        }

        $source = @imagecreatefromstring($imageBytes);

        if (!$source) {
            return false;
        }

        $targetWidth = self::clampDimension($toWidth, self::MAX_WIDTH);
        $targetHeight = self::clampDimension($toHeight, self::MAX_HEIGHT);

        // Calculate output dimensions.
        // Preserve aspect ratio and do not upscale.
        $scale = min(
            $targetWidth / $sourceWidth,
            $targetHeight / $sourceHeight,
            1
        );

        $width = max(1, (int)round($sourceWidth * $scale));
        $height = max(1, (int)round($sourceHeight * $scale));

        // Create white target image.
        $target = imagecreatetruecolor($width, $height);

        if (!$target) {
            imagedestroy($source);
            return false;
        }

        imagealphablending($target, true);
        imagesavealpha($target, false);

        $white = imagecolorallocate($target, 255, 255, 255);
        imagefill($target, 0, 0, $white);

        // Resize source image.
        $resized = imagecopyresampled(
            $target,
            $source,
            0, 0,
            0, 0,
            $width,
            $height,
            $sourceWidth,
            $sourceHeight
        );

        imagedestroy($source);

        if (!$resized) {
            imagedestroy($target);
            return false;
        }

        // Convert to 1bpp.
        $binary = self::imageTo1bpp($target, $width, $height);

        imagedestroy($target);

        if ($binary === false) {
            return false;
        }

        $stride = self::stride($width);
        $imageSize = $stride * $height;

        return self::bmpHeader($width, $height, $imageSize)
            . self::padRows($binary, $width, $height);
    }


    /**
     * A requested dimension is a ceiling: below the maximum it is honoured,
     * above it the maximum wins, and 0 or garbage means the maximum.
     */
    private static function clampDimension($requested, $maximum)
    {
        $value = intval($requested);

        if ($value < 1) {
            return $maximum;
        }

        return min($value, $maximum);
    }


    /**
     * BMP rows are padded to a 4-byte boundary, independently of the bit
     * width. For a width that is already a multiple of 32 this is the plain
     * bytes-per-row figure.
     */
    private static function stride($width)
    {
        return ((int)(($width + 31) / 32)) * 4;
    }


    /**
     * Pads every packed row from bytes-per-row up to the BMP stride. The
     * unused bits and the padding bytes stay zero, as the format requires.
     */
    private static function padRows($binary, $width, $height)
    {
        $rowBytes = (int)(($width + 7) / 8);
        $stride = self::stride($width);

        if ($stride === $rowBytes) {
            return $binary;
        }

        $padding = str_repeat("\0", $stride - $rowBytes);
        $padded = '';

        for ($y = 0; $y < $height; $y++) {
            $padded .= substr($binary, $y * $rowBytes, $rowBytes) . $padding;
        }

        return $padded;
    }


    /**
     * 62-byte header: file header, BITMAPINFOHEADER, 2-entry palette.
     * Same layout the firmware writes for its own 1-bit covers, including the
     * negative height that marks the rows as top-down.
     */
    private static function bmpHeader($width, $height, $imageSize)
    {
        $palette = "\x00\x00\x00\x00"   // index 0: black
                 . "\xFF\xFF\xFF\x00";  // index 1: white
        $offset = 14 + 40 + strlen($palette);

        return 'BM'
            . pack('V', $offset + $imageSize)      // bfSize
            . pack('V', 0)                         // bfReserved
            . pack('V', $offset)                   // bfOffBits
            . pack('V', 40)                        // biSize
            . pack('V', $width)                    // biWidth
            . pack('V', -$height & 0xFFFFFFFF)     // biHeight < 0: top-down
            . pack('v', 1)                         // biPlanes
            . pack('v', 1)                         // biBitCount
            . pack('V', 0)                         // biCompression: BI_RGB
            . pack('V', $imageSize)                // biSizeImage
            . pack('V', 2835)                      // biXPelsPerMeter
            . pack('V', 2835)                      // biYPelsPerMeter
            . pack('V', 2)                         // biClrUsed
            . pack('V', 2)                         // biClrImportant
            . $palette;
    }


    /**
     * Convert GD image to packed 1bpp bitmap.
     *
     * Uses Floyd-Steinberg dithering.
     *
     * Bit layout, matching the BMP palette above:
     *   1 = white (palette index 1)
     *   0 = black (palette index 0)
     *
     * Pixels are packed MSB first.
     * Every row starts at a new byte boundary; padRows() widens the rows to
     * the BMP stride afterwards.
     *
     * @return string|false Binary bitmap data.
     */
    private static function imageTo1bpp($image, $width, $height)
    {
        $binary = '';

        // Floyd-Steinberg error buffers.
        // Only two rows are stored in memory.
        $currentRow = array_fill(0, $width + 2, 0);
        $nextRow = array_fill(0, $width + 2, 0);

        for ($y = 0; $y < $height; $y++) {

            $byte = 0;
            $bitPosition = 7;

            for ($x = 0; $x < $width; $x++) {

                $color = imagecolorat($image, $x, $y);

                $r = ($color >> 16) & 0xFF;
                $g = ($color >> 8) & 0xFF;
                $b = $color & 0xFF;

                // Perceptual grayscale.
                $gray = (int)(
                    (299 * $r + 587 * $g + 114 * $b) / 1000
                );

                // Add accumulated error.
                $gray += $currentRow[$x + 1];

                if ($gray < 0) {
                    $gray = 0;
                } elseif ($gray > 255) {
                    $gray = 255;
                }

                // Quantize to black or white.
                if ($gray < 128) {
                    $newPixel = 0;
                } else {
                    $newPixel = 255;
                    $byte |= (1 << $bitPosition);
                }

                $error = $gray - $newPixel;

                // Floyd-Steinberg:
                //
                //          X   7/16
                // 3/16  5/16  1/16

                // Right.
                if ($x + 1 < $width) {
                    $currentRow[$x + 2] += $error * 7 / 16;
                }

                // Bottom-left.
                if ($x > 0 && $y + 1 < $height) {
                    $nextRow[$x] += $error * 3 / 16;
                }

                // Bottom.
                if ($y + 1 < $height) {
                    $nextRow[$x + 1] += $error * 5 / 16;
                }

                // Bottom-right.
                if ($x + 1 < $width && $y + 1 < $height) {
                    $nextRow[$x + 2] += $error * 1 / 16;
                }

                $bitPosition--;

                // Complete byte.
                if ($bitPosition < 0) {
                    $binary .= chr($byte);

                    $byte = 0;
                    $bitPosition = 7;
                }
            }

            // Flush incomplete byte.
            // Unused bits remain zero.
            if ($bitPosition != 7) {
                $binary .= chr($byte);
            }

            // Move next row into current row.
            $currentRow = $nextRow;
            $nextRow = array_fill(0, $width + 2, 0);
        }

        return $binary;
    }
}
