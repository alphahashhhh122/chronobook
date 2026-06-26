#pragma once

#include "core/Order.h"
#include "core/SlabPool.h"

namespace chronobook {

using OrderPool = SlabPool<Order>;

} // namespace chronobook
