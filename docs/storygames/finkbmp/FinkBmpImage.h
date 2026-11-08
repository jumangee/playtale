#pragma once

#include <FreeInkUICore.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

/**
 * Owns pixel bytes decoded from a FINKBMP file.
 *
 * BitmapRef returned by bitmapRef() is a non-owning view. Keep this image
 * alive for as long as FreeInkUI may use that BitmapRef.
 */
class FinkBmpImage {
 public:
  FinkBmpImage() = default;
  ~FinkBmpImage() = default;

  FinkBmpImage(const FinkBmpImage&) = delete;
  FinkBmpImage& operator=(const FinkBmpImage&) = delete;

  FinkBmpImage(FinkBmpImage&&) noexcept = default;
  FinkBmpImage& operator=(FinkBmpImage&&) noexcept = default;

  bool valid() const { return data_ != nullptr && width_ != 0 && height_ != 0 && dataSize_ != 0; }

  uint16_t width() const { return width_; }
  uint16_t height() const { return height_; }
  BitmapFormat format() const { return format_; }

  const uint8_t* data() const { return data_.get(); }
  size_t dataSize() const { return dataSize_; }

  /**
   * Returns a non-owning FreeInkUI view. progmem=false because the buffer
   * is allocated in ordinary RAM, not stored in flash/PROGMEM.
   */
  BitmapRef bitmapRef() const {
    BitmapRef ref;
    ref.data = data_.get();
    ref.width = width_;
    ref.height = height_;
    ref.format = format_;
    ref.progmem = false;
    return ref;
  }

  void reset() {
    data_.reset();
    width_ = 0;
    height_ = 0;
    format_ = BitmapFormat::BW1;
    dataSize_ = 0;
  }

 private:
  friend class FinkBmpParser;

  void setMetadata(uint16_t width, uint16_t height, BitmapFormat format, size_t dataSize) {
    width_ = width;
    height_ = height;
    format_ = format;
    dataSize_ = dataSize;
  }

  void setData(std::unique_ptr<uint8_t[]> data) { data_ = std::move(data); }

  uint16_t width_ = 0;
  uint16_t height_ = 0;
  BitmapFormat format_ = BitmapFormat::BW1;
  std::unique_ptr<uint8_t[]> data_;
  size_t dataSize_ = 0;
};
