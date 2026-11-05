// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>

namespace Vulkan {

// Used only by the recording thread. Dispatching a command chunk to the worker does not
// submit its Vulkan command buffer, so only a submission may retire recorded work here.
class SubmissionWork {
public:
    void Record() noexcept {
        pending = true;
    }

    void Submitted() noexcept {
        pending = false;
    }

    // Only automatic draw-counter flush requests use this guard. Explicit fences and
    // Flush/Finish must still submit even an empty context to signal their timeline tick.
    // Attachment clears may be deferred before any command has been recorded.
    template <typename Flush>
    bool FlushIfPending(bool deferred_clear, Flush&& flush) const {
        if (!pending && !deferred_clear) {
            return false;
        }
        std::forward<Flush>(flush)();
        return true;
    }

private:
    bool pending{};
};

} // namespace Vulkan
