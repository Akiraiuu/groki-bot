// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstdint>
namespace stackchan::conversation {
enum class CloseFailure : std::uint8_t { None, WorkerStart, StopFailed, DestroyFailed,
    AudioFailed, Deadline, Unsupported, Invariant };
inline const char* close_failure_name(CloseFailure value) {
    switch (value) {
    case CloseFailure::None: return "none";
    case CloseFailure::WorkerStart: return "worker_start";
    case CloseFailure::StopFailed: return "stop_failed";
    case CloseFailure::DestroyFailed: return "destroy_failed";
    case CloseFailure::AudioFailed: return "audio_failed";
    case CloseFailure::Deadline: return "deadline";
    case CloseFailure::Unsupported: return "unsupported";
    case CloseFailure::Invariant: return "invariant";
    }
    return "invariant";
}
enum class TransportCloseStage : std::uint8_t { Inactive, AwaitSender, Stop, Destroy, ReleaseBuffers, Finished, Failed };
inline const char* transport_close_stage_name(TransportCloseStage stage) {
    switch(stage) {
    case TransportCloseStage::Inactive:return "inactive";case TransportCloseStage::AwaitSender:return "await_sender";
    case TransportCloseStage::Stop:return "stop";case TransportCloseStage::Destroy:return "destroy";
    case TransportCloseStage::ReleaseBuffers:return "release_buffers";case TransportCloseStage::Finished:return "finished";
    case TransportCloseStage::Failed:return "failed";
    }return "failed";
}
enum class SupervisionStage : std::uint8_t { Inactive, Active, AwaitPreparation, AwaitClient,
    AwaitAudio, AwaitCoordinator, Transport, Audio, Drain, Finalizing, Closed, Failed };
inline const char* supervision_stage_name(SupervisionStage stage) {
    switch(stage) {
    case SupervisionStage::Inactive:return "inactive";case SupervisionStage::Active:return "active";
    case SupervisionStage::AwaitPreparation:return "await_preparation";case SupervisionStage::AwaitClient:return "await_client";
    case SupervisionStage::AwaitAudio:return "await_audio";case SupervisionStage::AwaitCoordinator:return "await_coordinator";
    case SupervisionStage::Transport:return "transport";case SupervisionStage::Audio:return "audio";
    case SupervisionStage::Drain:return "drain";case SupervisionStage::Finalizing:return "finalizing";
    case SupervisionStage::Closed:return "closed";case SupervisionStage::Failed:return "failed";
    }return "failed";
}
struct SupervisionProgress {
    SupervisionStage stage{SupervisionStage::Inactive};
    TransportCloseStage transport_stage{TransportCloseStage::Inactive};
    std::uint32_t coordinator{},preparation{},client{},audio{},audio_facts{},audio_workers{};
    bool supervisor_finished{false};
};
struct ControlledCloseSnapshot {
    bool supported{false}, requested{false}, sender_finished{false};
    bool transport_closed{false}, callbacks_quiesced{false}, worker_finished{false};
    CloseFailure failure{CloseFailure::None};
    TransportCloseStage stage{TransportCloseStage::Inactive};
};
enum CloseFact : std::uint32_t { PermissionRevoked=1, AudioStopped=2, SenderFinished=4,
    TransportClosed=8, CallbacksQuiesced=16, WorkerFinished=32, QueueDrained=64,
    ApplicationQuiesced=128, SupervisorFinished=256 };
inline constexpr std::uint32_t all_close_facts=511;
} // namespace stackchan::conversation
