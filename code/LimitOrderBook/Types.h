#pragma once

#include <cstdint>

enum class Side {
  BUY,
  SELL
};

using Id = std::uint64_t;
using Price = std::uint32_t;
using Quantity = std::uint32_t;
