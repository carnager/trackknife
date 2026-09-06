// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/playback_order.hpp"

namespace trackknife::audio {

PlaybackOrder::PlaybackOrder(const std::uint32_t seed) : generator_(seed) {}

void PlaybackOrder::startCycle() {
    excluded_ = current_;
    remaining_ = count_ - 1;
    swaps_.clear();
    history_ = {current_};
    cursor_ = 0U;
    pending_.reset();
    pending_wrap_ = false;
}

void PlaybackOrder::reset(const int count, const int current, const bool random) {
    count_ = count;
    current_ = current;
    random_ = random;
    startCycle();
}

int PlaybackOrder::draw() {
    const auto slot = std::uniform_int_distribution<int>{0, remaining_ - 1}(generator_);
    const auto value = [&](const int index) {
        const auto found = swaps_.find(index);
        return found == swaps_.end() ? index : found->second;
    };
    const auto result = value(slot);
    --remaining_;
    if (slot != remaining_) {
        swaps_[slot] = value(remaining_);
    }
    swaps_.erase(remaining_);
    return result >= excluded_ ? result + 1 : result;
}

std::optional<int> PlaybackOrder::adjacent(const int direction, const bool repeat) {
    if (count_ <= 0 || current_ < 0 || current_ >= count_) {
        return std::nullopt;
    }
    if (!random_) {
        const auto row = current_ + direction;
        if (row >= 0 && row < count_) {
            return row;
        }
        return repeat ? std::optional{direction < 0 ? count_ - 1 : 0} : std::nullopt;
    }
    if (direction < 0) {
        return cursor_ > 0U ? std::optional{history_[cursor_ - 1U]} : std::nullopt;
    }
    if (cursor_ + 1U < history_.size()) {
        return history_[cursor_ + 1U];
    }
    if (pending_) {
        return pending_wrap_ && !repeat ? std::nullopt : pending_;
    }
    if (remaining_ <= 0) {
        if (!repeat) {
            return std::nullopt;
        }
        // Retain the completed cycle until this candidate is committed, so a
        // status refresh never erases Previous's history.
        pending_wrap_ = true;
        pending_ =
            count_ == 1
                ? current_
                : static_cast<int>((static_cast<std::int64_t>(current_) +
                                    std::uniform_int_distribution<int>{1, count_ - 1}(generator_)) %
                                   count_);
    } else {
        pending_ = draw();
    }
    return pending_;
}

void PlaybackOrder::advance(const int row, const int direction) {
    if (row == current_) {
        return;
    }
    if (!random_) {
        current_ = row;
        return;
    }
    if (direction < 0 && cursor_ > 0U && history_[cursor_ - 1U] == row) {
        --cursor_;
    } else if (cursor_ + 1U < history_.size() && history_[cursor_ + 1U] == row) {
        ++cursor_;
    } else if (pending_ == row) {
        if (history_.size() == static_cast<std::size_t>(count_)) {
            current_ = row;
            startCycle();
            return;
        }
        history_.push_back(row);
        ++cursor_;
        pending_.reset();
    } else {
        reset(count_, row, random_);
        return;
    }
    current_ = row;
}

} // namespace trackknife::audio
