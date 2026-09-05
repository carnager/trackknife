// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"

#include <QImage>

#include <string>

namespace trackknife::ui {

// Worker-only local thumbnail reader shared by lists and the library. Loads
// embedded art first, then conventional sibling images; never uses the network.
[[nodiscard]] QImage loadLocalArtwork(const std::string& raw_path,
                                      const core::CancellationToken& cancellation = {});

} // namespace trackknife::ui
