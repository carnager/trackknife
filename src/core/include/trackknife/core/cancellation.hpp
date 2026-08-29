// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>
#include <memory>

namespace trackknife::core {

class CancellationToken final {
  public:
    CancellationToken() = default;

    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return state_ && state_->load(std::memory_order_acquire);
    }

  private:
    explicit CancellationToken(std::shared_ptr<const std::atomic_bool> state)
        : state_(std::move(state)) {}

    std::shared_ptr<const std::atomic_bool> state_;
    friend class CancellationSource;
};

class CancellationSource final {
  public:
    CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}

    [[nodiscard]] CancellationToken token() const { return CancellationToken{state_}; }
    void request_cancellation() noexcept { state_->store(true, std::memory_order_release); }
    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return state_->load(std::memory_order_acquire);
    }

  private:
    std::shared_ptr<std::atomic_bool> state_;
};

} // namespace trackknife::core
