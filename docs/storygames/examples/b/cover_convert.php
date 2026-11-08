<?php

class StorygameCoverConvert
{
    /**
     * PlayTale image parameters.
     */
    const MAX_WIDTH = 800;
    const MAX_HEIGHT = 600;

    const BPP = 1;
    const BIT_ORDER = 'MSB_FIRST';
    const BLACK_BIT = 1;

    const MAX_SOURCE_PIXELS = 20000000;


    /**
     * Convert game cover to PlayTale MONO_1BPP JSON.
     *
     * @param string $filePath Absolute path to source image.
     *
     * @return string|false JSON string on success, false on error.
     */
    public static function forPlayTale($filePath, $toWidth, $toHeight)    {
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
		
		$targetWidth = intval($toWidth) ? intval($toWidth) : self::MAX_WIDTH;
		$targetHeight = intval($toHeight) ? intval($toHeight) : self::MAX_HEIGHT;

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

        $stride = (int)(($width + 7) / 8);

        // Build JSON response.
        $result = array(
            'width' => $width,
            'height' => $height,
            'stride' => $stride,
            'bpp' => self::BPP,
            'bit_order' => self::BIT_ORDER,
            'black_bit' => self::BLACK_BIT,
            'format' => 'MONO_1BPP',
            'data' => base64_encode($binary),
			'size' => strlen($binary)
        );

        $json = json_encode($result);

        if ($json === false) {
            return false;
        }

        return $json;
    }


    /**
     * Convert GD image to packed 1bpp bitmap.
     *
     * Uses Floyd-Steinberg dithering.
     *
     * Bit layout:
     *   1 = black
     *   0 = white
     *
     * Pixels are packed MSB first.
     * Every row starts at a new byte boundary.
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
                    $byte |= (1 << $bitPosition);
                } else {
                    $newPixel = 255;
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