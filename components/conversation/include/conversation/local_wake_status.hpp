// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <atomic>
#include <cstdint>

namespace stackchan::conversation {
// Runtime diagnostics contain counters and resource states, never audio or keys.
// This clock comparison also works across the 32-bit millisecond wrap.
inline std::uint32_t local_wake_remaining(std::uint32_t start, std::uint32_t limit,
                                         std::uint32_t now) {
    const auto elapsed = now - start;
    return elapsed >= limit ? 0 : limit - elapsed;
}
struct LocalWakeStatus {
    enum Status { Off, Loading, Listening, Confirming, Unavailable, Paused, Stopping, Checked };
    enum class Stage { Idle, Allocate, MapModel, CreateDecoder, Commands, Warmup,
                       Detect, Cleanup, Closed };
    enum class Error { None, NoMemory, NoModel, Decoder, Commands, Microphone,
                       Timeout, Cancelled };
    static constexpr std::uint32_t listen_limit_ms = 40'000;
    static constexpr std::uint32_t check_limit_ms = 20'000;
    std::atomic<bool> available{false}, enabled{false}, check_only{false};
    std::atomic<std::uint32_t> approval_id{0}, started_ms{0};
    std::atomic<int> status{Off};
    std::atomic<Stage> stage{Stage::Idle};
    std::atomic<Error> error{Error::None};
    std::atomic<bool> microphone_active{false}, worker_active{false};
    std::atomic<std::uint32_t> stack_free_bytes{0}, previous_stage{0};
    std::atomic<std::uint32_t> stop_request{0}, stop_ack{0};
    std::atomic<bool> stop_notice{false};
    // Aggregate signal/decoder counters only. Never retain PCM or transcripts.
    std::atomic<std::uint32_t> frames{0}, samples_per_frame{0}, peak{0}, mean_abs_peak{0};
    std::atomic<std::uint32_t> clipped_samples{0}, timeouts{0}, rejected{0}, detect_max_us{0};
    std::atomic<std::uint32_t> best_target_per_mille{0}, best_other_per_mille{0};
    std::atomic<int> last_command{0};
    void reset_observations() {
        frames=0; samples_per_frame=0; peak=0; mean_abs_peak=0;
        clipped_samples=0; timeouts=0; rejected=0; detect_max_us=0;
        best_target_per_mille=0; best_other_per_mille=0; last_command=0;
    }
    std::uint32_t remaining(std::uint32_t now) const {
        if (!enabled.load()) return 0;
        return local_wake_remaining(started_ms.load(),
            check_only.load() ? check_limit_ms : listen_limit_ms, now);
    }
    void note_stop() {
        enabled.store(false);
        stop_notice.store(true);
        stop_request.fetch_add(1);
    }
};
inline LocalWakeStatus local_wake_status;
} // namespace stackchan::conversation
