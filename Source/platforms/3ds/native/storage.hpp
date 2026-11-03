#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>

namespace dusk::pica {

struct FreeStorage {
    template<class T> void operator()(T* p) const noexcept { std::free(p); }
};

template<class T> using Storage = std::unique_ptr<T[], FreeStorage>;

template<class T> Storage<T> allocateStorage(std::size_t count) noexcept {
    static_assert(std::is_trivially_destructible_v<T>);
    static_assert(std::is_nothrow_default_constructible_v<T>);
    if (!count || count > std::numeric_limits<std::size_t>::max() / sizeof(T)) return {};
    auto* data = static_cast<T*>(std::malloc(count * sizeof(T)));
    if (!data) return {};
    std::uninitialized_value_construct_n(data, count);
    return Storage<T>{data};
}

} // namespace dusk::pica
