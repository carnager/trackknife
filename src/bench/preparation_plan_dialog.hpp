// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/operations/preparation_plan.hpp"

#include <functional>
#include <memory>

class QDialog;
class QWidget;

namespace trackknife::bench {

using PreparationPlanApplyCallback =
    std::function<void(std::shared_ptr<const operations::PreparationPlan>)>;

[[nodiscard]] QDialog*
createPreparationPlanDialog(std::shared_ptr<const operations::PreparationPlan> plan,
                            PreparationPlanApplyCallback apply, QWidget* parent);

} // namespace trackknife::bench
