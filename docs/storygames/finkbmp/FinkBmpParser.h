#pragma once

#include "FinkBmpImage.h"

#include <cstddef>
#include <cstdint>

/**
 * Incremental parser for the FINKBMP v1 wire format.
 *
 * Header (18 bytes, all multi-byte integers little-endian):
 *   0..7   magic: "FINKBMP\\0"
 *   8      version: 1
 *   9      BitmapFormat numeric value (FreeInkUI enum)
 *   10..11 width: uint16
 *   12..13 height: uint16
 *   14..17 dataSize: uint32
 *   18..   pixel data
 *
 * The parser copies each incoming chunk directly into the final image buffer;
 * it never accumulates the complete HTTP body in a second buffer.
 */
class FinkBmpParser {
 public:
  static constexpr size_t HEADER_SIZE = 18;
  static constexpr uint8_t VERSION = 1;

  // Conservative defaults for embedded targets. Callers may supply larger
  // limits when their memory budget permits it.
  struct Limits {
    uint16_t maxWidth = 800;
    uint16_t maxHeight = 600;
    size_t maxDataSize = 256 * 1024;
  };

  enum class Error : uint8_t {
    None,
    InvalidArgument,
    InvalidMagic,
    UnsupportedVersion,
    UnsupportedFormat,
    InvalidDimensions,
    ImageTooLarge,
    InvalidDataSize,
    OutOfMemory,
    TrailingData,
    Incomplete,
    AlreadyFailed,
  };

  FinkBmpParser();
  explicit FinkBmpParser(const Limits& limits);

  FinkBmpParser(const FinkBmpParser&) = delete;
  FinkBmpParser& operator=(const FinkBmpParser&) = delete;

  /**
   * Feed the next HTTP/body chunk.
   *
   * Returns false on the first parse/allocation error. Once failed, the parser
   * remains failed until reset(). A zero-length chunk is accepted.
   */
  bool consume(const uint8_t* data, size_t size);

  /**
   * Marks end-of-input and verifies that the complete header and exactly
   * dataSize pixel bytes were received.
   */
  bool finish();

  bool complete() const { return finished_; }
  bool failed() const { return error_ != Error::None; }
  Error error() const { return error_; }

  size_t receivedDataSize() const { return receivedData_; }
  size_t expectedDataSize() const { return expectedData_; }

  /**
   * Moves the decoded image out. Succeeds only after finish() returned true.
   * The parser becomes empty and can be reused.
   */
  bool takeImage(FinkBmpImage& outImage);

  void reset();

 private:
  enum class State : uint8_t { Header, Pixels, Complete, Failed };

  bool parseHeader();
  bool fail(Error error);
  static bool decodeFormat(uint8_t value, BitmapFormat& outFormat);
  static uint16_t readLe16(const uint8_t* p);
  static uint32_t readLe32(const uint8_t* p);

  Limits limits_;
  State state_ = State::Header;
  Error error_ = Error::None;

  uint8_t header_[HEADER_SIZE] = {};
  size_t headerUsed_ = 0;

  FinkBmpImage image_;
  size_t expectedData_ = 0;
  size_t receivedData_ = 0;
  bool finished_ = false;
};
