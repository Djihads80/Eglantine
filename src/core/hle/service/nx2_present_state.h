// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Service::Nx2 {

struct NativePresentRequest {
    u64 layer{};
    u32 image{}, shown_size{}, timing{};
    u8 credit{};
    std::array<u32, 4> render_sync{};

    static std::optional<NativePresentRequest> Parse(std::span<const u32> words) {
        if (words.size() < 26 || words[0] != 245 || words[1] != 24) {
            return std::nullopt;
        }
        NativePresentRequest request{
            .layer = u64{words[2]} | (u64{words[3]} << 32),
            .image = words[5],
            .shown_size = words[7],
            .timing = words[12],
            .credit = static_cast<u8>(words[4]),
            .render_sync = {words[22], words[23], words[24], words[25]},
        };
        const bool empty = request.render_sync == std::array<u32, 4>{};
        if (!request.layer || request.credit >= 15 ||
            (!empty && (request.render_sync[1] != 0 || request.render_sync[3] != 0 ||
                        ~request.render_sync[0] >= 192))) {
            return std::nullopt;
        }
        return request;
    }
};

// Serialized by the session's shared mutex. GPU callbacks hold that same mutex while publishing
// the returned counter increments, so cancellation quiesces publication before closing resources.
class NativePresentState {
public:
    static constexpr u8 CreditCount = 2;
    static constexpr u64 InitialCredits = (u64{1} << CreditCount) - 1;
    static constexpr size_t NotificationWords = 32;
    using Notifications = std::array<u32, NotificationWords>;

    struct Ticket {
        u64 generation{};
        u64 ordering_key{};
        u32 sequence{}, counter{}, value{}, image{};
        u8 credit{};

        std::array<u32, 6> Reply(u64 available) const {
            return {static_cast<u32>(available), static_cast<u32>(available >> 32),
                    ~counter, 0, value, 0};
        }
    };

    struct Completion {
        bool accepted{};
        bool failed{};
        u32 counter{}, increments{};
    };

    bool Register(u64 token, u32 first_notification, u32 credit_notification,
                  u32 counter, u32 base) {
        if (!token || !first_notification || !credit_notification ||
            first_notification >= 1024 || credit_notification >= 1024 ||
            first_notification == credit_notification || counter == 0 || counter >= 192 ||
            active.contains(token) || generation == std::numeric_limits<u64>::max()) {
            return false;
        }
        for (const auto& [id, layer] : layers) {
            if (layer.counter == counter ||
                (layer.active &&
                 (layer.first_notification == first_notification ||
                  layer.credit_notification == first_notification ||
                  layer.first_notification == credit_notification ||
                  layer.credit_notification == credit_notification))) {
                return false;
            }
        }
        const auto order_key = NextOrderingKey();
        if (!order_key) return false;
        const u64 id = ++generation;
        layers.emplace(id, Layer{.token = token,
                                 .ordering_key = *order_key,
                                 .first_notification = first_notification,
                                 .credit_notification = credit_notification,
                                 .counter = counter,
                                 .base = base});
        active.emplace(token, id);
        return true;
    }

    bool Unregister(u64 token) {
        const auto it = active.find(token);
        if (it == active.end()) return false;
        auto& layer = layers.at(it->second);
        layer.active = false;
        ClearNotification(layer.first_notification);
        ClearNotification(layer.credit_notification);
        active.erase(it);
        return true;
    }

    std::optional<u64> Available(u64 token) const {
        const auto* layer = FindActive(token);
        if (!layer || layer->faulted) return std::nullopt;
        return layer->available;
    }

    std::optional<std::array<u32, 2>> LastRelease(u64 token) const {
        const auto* layer = FindActive(token);
        if (!layer || layer->faulted) return std::nullopt;
        return std::array<u32, 2>{layer->counter, layer->base + layer->issued};
    }

    std::optional<Ticket> Submit(const NativePresentRequest& request) {
        auto* layer = FindActive(request.layer);
        if (!layer || layer->faulted || request.credit >= CreditCount ||
            !(layer->available & (u64{1} << request.credit)) ||
            layer->issued >= std::numeric_limits<u32>::max() - layer->base) {
            return std::nullopt;
        }
        const u32 sequence = ++layer->issued;
        layer->available &= ~(u64{1} << request.credit);
        layer->pending.emplace(sequence, Pending{request.image, request.credit, false, false});
        return Ticket{active.at(request.layer), layer->ordering_key, sequence, layer->counter,
                      layer->base + sequence, request.image, request.credit};
    }

    // Only before a reply/queued callback exists: a rejected composition never hands out a fence.
    bool AbortUnpublished(const Ticket& ticket) {
        const auto it = layers.find(ticket.generation);
        if (it == layers.end()) return false;
        auto& layer = it->second;
        const auto pending = layer.pending.find(ticket.sequence);
        if (ticket.sequence != layer.issued || pending == layer.pending.end() ||
            pending->second.done || pending->second.failed || !Matches(layer, pending->second, ticket)) {
            return false;
        }
        layer.available |= u64{1} << ticket.credit;
        layer.pending.erase(pending);
        --layer.issued;
        return true;
    }

    bool CancelCredit(u64 token, u8 credit) const {
        const auto* layer = FindActive(token);
        // Acquire happens in the SDK. A credit is owned by the compositor only after Present.
        return layer && !layer->faulted && credit < CreditCount &&
               (layer->available & (u64{1} << credit)) != 0;
    }

    Completion Complete(const Ticket& ticket, bool completed) {
        const auto it = layers.find(ticket.generation);
        if (it == layers.end()) return {};
        auto& layer = it->second;
        const auto pending = layer.pending.find(ticket.sequence);
        if (pending == layer.pending.end() || pending->second.done || pending->second.failed ||
            !Matches(layer, pending->second, ticket)) return {};
        if (!completed) {
            pending->second.failed = true;
            layer.faulted = true;
            if (layer.active) Notify(layer.credit_notification);
            return {.accepted = true, .failed = true, .counter = layer.counter};
        }
        pending->second.done = true;
        layer.available |= u64{1} << ticket.credit;
        if (layer.active) Notify(layer.credit_notification);
        u32 increments{};
        for (;;) {
            const auto next = layer.pending.find(layer.retired + 1);
            if (next == layer.pending.end() || !next->second.done) break;
            layer.pending.erase(next);
            ++layer.retired;
            ++increments;
        }
        return {.accepted = true, .counter = layer.counter, .increments = increments};
    }

    bool HasNotifications() const {
        for (const auto word : notifications) if (word) return true;
        return false;
    }

    Notifications TakeNotifications() {
        const auto result = notifications;
        notifications.fill(0);
        return result;
    }

    void RestoreNotifications(const Notifications& failed_delivery) {
        for (size_t i = 0; i < notifications.size(); ++i) notifications[i] |= failed_delivery[i];
    }

    std::vector<u32> CollectRetiredCounters() {
        std::vector<u32> counters;
        for (auto it = layers.begin(); it != layers.end();) {
            if (!it->second.active && it->second.pending.empty()) {
                counters.push_back(it->second.counter);
                it = layers.erase(it);
            } else {
                ++it;
            }
        }
        return counters;
    }

    std::vector<u32> Counters() const {
        std::vector<u32> counters;
        for (const auto& [id, layer] : layers) counters.push_back(layer.counter);
        return counters;
    }

    // A session can close without unregistering. Unfinished guest waiters may outlive it;
    // their IDs must remain reserved until allocator shutdown, never reach a new layer's target.
    std::vector<u32> ReleasableCounters() const {
        std::vector<u32> counters;
        for (const auto& [id, layer] : layers) {
            if (layer.pending.empty()) counters.push_back(layer.counter);
        }
        return counters;
    }

private:
    struct Pending { u32 image; u8 credit; bool done; bool failed; };
    struct Layer {
        u64 token{}, ordering_key{};
        u32 first_notification{}, credit_notification{}, counter{}, base{}, issued{}, retired{};
        u64 available{InitialCredits};
        bool active{true}, faulted{};
        std::map<u32, Pending> pending;
    };

    Layer* FindActive(u64 token) {
        const auto it = active.find(token);
        return it == active.end() ? nullptr : &layers.at(it->second);
    }
    const Layer* FindActive(u64 token) const {
        const auto it = active.find(token);
        return it == active.end() ? nullptr : &layers.at(it->second);
    }
    static bool Matches(const Layer& layer, const Pending& pending, const Ticket& ticket) {
        return layer.ordering_key == ticket.ordering_key && layer.counter == ticket.counter &&
               layer.base + ticket.sequence == ticket.value &&
               pending.image == ticket.image && pending.credit == ticket.credit;
    }
    static std::optional<u64> NextOrderingKey() {
        static std::atomic<u64> next{1};
        auto current = next.load(std::memory_order_relaxed);
        while (current != std::numeric_limits<u64>::max()) {
            if (next.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                return current;
            }
        }
        return std::nullopt;
    }
    void Notify(u32 index) { notifications[index / 32] |= u32{1} << (index % 32); }
    void ClearNotification(u32 index) { notifications[index / 32] &= ~(u32{1} << (index % 32)); }

    u64 generation{};
    std::map<u64, u64> active;
    std::map<u64, Layer> layers;
    Notifications notifications{};
};

} // namespace Service::Nx2
