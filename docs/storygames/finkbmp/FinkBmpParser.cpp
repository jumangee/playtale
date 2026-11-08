#include "FinkBmpParser.h"

#include <limits>
#include <new>
#include <utility>

namespace {
constexpr uint8_t MAGIC[8] = {'F', 'I', 'N', 'K', 'B', 'M', 'P', '\0'};

bool checkedMul(size_t a, size_t b, size_t& result) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) return false;
  result = a * b;
  return true;
}
}  // namespace

FinkBmpParser::FinkBmpParser() = default;

FinkBmpParser::FinkBmpParser(const Limits& limits) : limits_(limits) {}

uint16_t FinkBmpParser::readLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t FinkBmpParser::readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool FinkBmpParser::decodeFormat(uint8_t value, BitmapFormat& outFormat) {
  // FINKBMP v1 wire values intentionally match the current FreeInkUI enum:
  // Mask1=0, BW1=1, Gray2=2, NativeBmp=3.
  switch (value) {
    case 0: outFormat = BitmapFormat::Mask1; return true;
    case 1: outFormat = BitmapFormat::BW1; return true;
    case 2: outFormat = BitmapFormat::Gray2; return true;
    case 3: outFormat = BitmapFormat::NativeBmp; return true;
    default: return false;
  }
}

bool FinkBmpParser::fail(Error error) {
  error_ = error;
  state_ = State::Failed;
  image_.reset();
  expectedData_ = 0;
  receivedData_ = 0;
  return false;
}

bool FinkBmpParser::parseHeader() {
  for (size_t i = 0; i < sizeof(MAGIC); ++i) {
    if (header_[i] != MAGIC[i]) return fail(Error::InvalidMagic);
  }

  if (header_[8] != VERSION) return fail(Error::UnsupportedVersion);

  BitmapFormat format;
  if (!decodeFormat(header_[9], format)) return fail(Error::UnsupportedFormat);

  const uint16_t width = readLe16(header_ + 10);
  const uint16_t height = readLe16(header_ + 12);
  const uint32_t dataSize32 = readLe32(header_ + 14);
  const size_t dataSize = static_cast<size_t>(dataSize32);

  if (width == 0 || height == 0) return fail(Error::InvalidDimensions);
  if (width > limits_.maxWidth || height > limits_.maxHeight) return fail(Error::ImageTooLarge);
  if (dataSize == 0 || dataSize > limits_.maxDataSize) return fail(Error::ImageTooLarge);

  // The two 1-bpp formats have a fully specified storage layout in FreeInkUI:
  // row-major, MSB-first, one independently byte-aligned row after another.
  // Gray2 and NativeBmp are accepted as opaque payloads for now.
  if (format == BitmapFormat::Mask1 || format == BitmapFormat::BW1) {
    const size_t bytesPerRow = (static_cast<size_t>(width) + 7u) / 8u;
    size_t expected = 0;
    if (!checkedMul(bytesPerRow, static_cast<size_t>(height), expected))
      return fail(Error::InvalidDataSize);
    if (dataSize != expected) return fail(Error::InvalidDataSize);
  }

  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[dataSize]);
  if (!buffer) return fail(Error::OutOfMemory);

  image_.setMetadata(width, height, format, dataSize);
  image_.setData(std::move(buffer));
  expectedData_ = dataSize;
  receivedData_ = 0;
  state_ = State::Pixels;
  return true;
}

bool FinkBmpParser::consume(const uint8_t* data, size_t size) {
  if (state_ == State::Failed) return false;
  if (finished_ || state_ == State::Complete) return size == 0 ? true : fail(Error::TrailingData);
  if (size == 0) return true;
  if (data == nullptr) return fail(Error::InvalidArgument);

  size_t offset = 0;

  if (state_ == State::Header) {
    const size_t need = HEADER_SIZE - headerUsed_;
    const size_t copy = size < need ? size : need;
    for (size_t i = 0; i < copy; ++i) header_[headerUsed_ + i] = data[i];
    headerUsed_ += copy;
    offset += copy;

    if (headerUsed_ < HEADER_SIZE) return true;
    if (!parseHeader()) return false;
  }

  if (offset < size) {
    if (state_ != State::Pixels) return fail(Error::TrailingData);

    const size_t remaining = expectedData_ - receivedData_;
    const size_t incoming = size - offset;
    if (incoming > remaining) return fail(Error::TrailingData);

    for (size_t i = 0; i < incoming; ++i) {
      // Write directly into the final owned pixel buffer.
      image_.data_[receivedData_ + i] = data[offset + i];
    }
    receivedData_ += incoming;

    if (receivedData_ == expectedData_) state_ = State::Complete;
  }

  return true;
}

bool FinkBmpParser::finish() {
  if (state_ == State::Failed) return false;
  if (finished_) return true;

  if (headerUsed_ != HEADER_SIZE) return fail(Error::Incomplete);
  if (receivedData_ != expectedData_) return fail(Error::Incomplete);
  if (state_ != State::Complete) return fail(Error::Incomplete);

  finished_ = true;
  return true;
}

bool FinkBmpParser::takeImage(FinkBmpImage& outImage) {
  if (!finished_ || !image_.valid()) return false;
  outImage = std::move(image_);
  reset();
  return true;
}

void FinkBmpParser::reset() {
  image_.reset();
  state_ = State::Header;
  error_ = Error::None;
  headerUsed_ = 0;
  for (size_t i = 0; i < HEADER_SIZE; ++i) header_[i] = 0;
  expectedData_ = 0;
  receivedData_ = 0;
  finished_ = false;
}
