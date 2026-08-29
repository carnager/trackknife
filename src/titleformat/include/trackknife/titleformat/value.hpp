// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>

namespace trackknife::titleformat {

struct EvalValue {
    std::string text;

    [[nodiscard]] bool truthy() const noexcept { return !text.empty(); }

    friend bool operator==(const EvalValue&, const EvalValue&) = default;
};

} // namespace trackknife::titleformat
