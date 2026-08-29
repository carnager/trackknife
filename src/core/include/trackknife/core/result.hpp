// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"

#include <expected>

namespace trackknife::core {

template <typename T> using Result = std::expected<T, Error>;

} // namespace trackknife::core
