#pragma once
#include <cstddef>

namespace dusk::native {
// Called once, before JKR initialization or worker creation. The owner retains
// the backing allocation until every game heap and worker has been destroyed.
bool configureGameArena(void* memory, std::size_t size);
}
