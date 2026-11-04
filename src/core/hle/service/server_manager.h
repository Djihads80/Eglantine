// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <vector>

#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/hle/result.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/os/multi_wait.h"
#include "core/hle/service/os/mutex.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
class KServerPort;
class KServerSession;
} // namespace Kernel

namespace Service {

class Port;
class Session;

class ServerManager {
public:
    /// Called on a server thread with the context of a deferred request that is retired because
    /// its client closed the session, before the context and session go. The request is never
    /// retried again.
    using DeferralCancelHandler = std::function<void(const HLERequestContext&)>;

    explicit ServerManager(Core::System& system);
    ~ServerManager();

    Result RegisterSession(Kernel::KServerSession* session,
                           std::shared_ptr<SessionRequestManager> manager);
    Result RegisterNamedService(const std::string& service_name,
                                SessionRequestHandlerFactory&& handler_factory,
                                u32 max_sessions = 64);
    Result RegisterNamedService(const std::string& service_name,
                                std::shared_ptr<SessionRequestHandler>&& handler,
                                u32 max_sessions = 64);
    Result ManageNamedPort(const std::string& service_name,
                           SessionRequestHandlerFactory&& handler_factory, u32 max_sessions = 64);
    Result ManageDeferral(Kernel::KEvent** out_event);
    /// Sets what has the handlers of deferred requests forget those retired. To be called before
    /// any thread processes requests, StartAdditionalHostThreads included.
    void SetDeferralCancelHandler(DeferralCancelHandler&& handler);

    Result LoopProcess();
    void StartAdditionalHostThreads(const char* name, size_t num_threads);

    static void RunServer(std::unique_ptr<ServerManager>&& server);

private:
    void LinkToDeferredList(MultiWaitHolder* holder);
    void LinkDeferred();
    MultiWaitHolder* WaitSignaled();
    Result Process(MultiWaitHolder* holder);
    bool WaitAndProcessImpl();
    Result LoopProcessImpl();

    Result OnPortEvent(Port* port);
    Result OnSessionEvent(Session* session);
    Result OnDeferralEvent();
    Result CompleteSyncRequest(Session* session);
    void RetireDeferredRequest(Session* session);

private:
    void DestroySession(Session* session);

private:
    Core::System& m_system;
    Mutex m_selection_mutex;

    // Events
    Kernel::KEvent* m_wakeup_event{};
    Kernel::KEvent* m_deferral_event{};
    DeferralCancelHandler m_deferral_cancel_handler{};

    // Deferred wait list
    std::mutex m_deferred_list_mutex{};
    MultiWait m_deferred_list{};

    // Guest state tracking
    MultiWait m_multi_wait{};
    Common::IntrusiveListBaseTraits<Port>::ListType m_servers{};
    Common::IntrusiveListBaseTraits<Session>::ListType m_sessions{};
    /// The sessions whose request waits, deferred, for a retry. They are not waited on meanwhile:
    /// each signals the deferral event once its client closes. Guarded by m_deferred_list_mutex.
    std::list<Session*> m_deferred_sessions{};
    /// Counts the deferral events handled, each of which takes the deferred session list.
    /// Guarded by m_deferred_list_mutex.
    u64 m_deferral_epoch{};
    std::optional<MultiWaitHolder> m_wakeup_holder{};
    std::optional<MultiWaitHolder> m_deferral_holder{};

    // Host state tracking
    Common::Event m_stopped{};
    std::vector<std::jthread> m_threads{};
    std::stop_source m_stop_source{};
};

} // namespace Service
