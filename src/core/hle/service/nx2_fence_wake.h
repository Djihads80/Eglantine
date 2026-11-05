// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace Service::Nx2 {

inline int CaptureFenceTimeoutMilliseconds(std::string_view text) {
    constexpr int default_timeout = 4000;
    if (text.empty()) {
        return default_timeout;
    }
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value >= default_timeout &&
                   value <= 120000
               ? value
               : default_timeout;
}

// A syncpoint callback can outlive a timed-out wait. Cancel waits for any in-flight signal and
// drops the event reference before its owner closes the event; late callbacks do nothing.
class FenceWake {
public:
    explicit FenceWake(std::function<void()> signal_) : signal{std::move(signal_)} {}

    static std::function<void()> Callback(const std::shared_ptr<FenceWake>& wake) {
        return [weak = std::weak_ptr{wake}] {
            if (const auto current = weak.lock()) {
                current->Notify();
            }
        };
    }

    void Cancel() {
        std::scoped_lock lock{mutex};
        signal = {};
    }

private:
    void Notify() {
        std::scoped_lock lock{mutex};
        if (signal) {
            signal();
            signal = {};
        }
    }

    std::mutex mutex;
    std::function<void()> signal;
};

} // namespace Service::Nx2
