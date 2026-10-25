#pragma once

// Native display-list processing has no desktop renderer or logger dependency.
#include <dolphin/gx.h>
#include <cstring>
#include <type_traits>

namespace aurora::gx {
u8 comp_type_size(GXAttr attr, GXCompType type) noexcept;
u8 comp_cnt_count(GXAttr attr, GXCompCnt cnt) noexcept;

template <typename T> T unaligned_load(const void* ptr) {
  T value;
  std::memcpy(&value, ptr, sizeof(value));
  return value;
}

template <typename T> T read_bits(const void* ptr) {
  static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);
  const auto* bytes = static_cast<const u8*>(ptr);
  T value = 0;
  for (unsigned i = 0; i < sizeof(T); ++i) {
    value = static_cast<T>((value << 8) | bytes[i]);
  }
  return value;
}
} // namespace aurora::gx
