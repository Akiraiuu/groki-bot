// SPDX-License-Identifier: BSL-1.0
#include "local_wake.hpp"
#include "sdkconfig.h"
#include "conversation/local_wake_status.hpp"

#if defined(CONFIG_STACKCHAN_JINGTONG_WAKE)
#include <M5Unified.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <model_path.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>

namespace {
using Wake = stackchan::conversation::LocalWakeStatus;
auto& diag = stackchan::conversation::local_wake_status;
constexpr std::uint32_t kMagic = 0x4a545732;
RTC_NOINIT_ATTR std::uint32_t progress_magic;
RTC_NOINIT_ATTR std::uint32_t progress_stage;
RTC_NOINIT_ATTR std::uint32_t progress_check;
void stage(Wake::Stage value) {
    const auto n = static_cast<std::uint32_t>(value);
    progress_stage = n; progress_check = ~n; progress_magic = kMagic;
    diag.stage.store(value);
    diag.stack_free_bytes.store(uxTaskGetStackHighWaterMark(nullptr));
    ESP_LOGI("local-wake", "stage=%u internal=%u psram=%u stack_free=%u", static_cast<unsigned>(n),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(diag.stack_free_bytes.load()));
}
}
void local_wake_boot_diagnostics() {
    const bool valid = progress_magic == kMagic && progress_check == ~progress_stage &&
                       progress_stage <= static_cast<std::uint32_t>(Wake::Stage::Closed);
    diag.previous_stage.store(valid ? progress_stage : 0);
    ESP_LOGI("local-wake", "boot reset=%d previous_stage=%u; capture disabled",
             static_cast<int>(esp_reset_reason()), static_cast<unsigned>(diag.previous_stage.load()));
}
namespace stackchan::app {
struct LocalWake::Impl {
    const bool check_only = diag.check_only.load();
    const std::uint32_t started = diag.started_ms.load();
    const std::uint32_t limit = check_only ? Wake::check_limit_ms : Wake::listen_limit_ms;
    std::atomic<bool> stop{false};
    std::atomic<Result> current{Result::Preparing};
    StaticSemaphore_t done_storage{};
    SemaphoreHandle_t done{xSemaphoreCreateBinaryStatic(&done_storage)};
    TaskHandle_t task{nullptr};
    ~Impl() { if (done) vSemaphoreDelete(done); }
    bool permitted() const {
        return !stop.load() && diag.enabled.load() &&
            conversation::local_wake_remaining(started, limit,
                static_cast<std::uint32_t>(esp_timer_get_time() / 1000)) != 0;
    }
    Result cancelled_result() const {
        const bool expired = conversation::local_wake_remaining(started, limit,
            static_cast<std::uint32_t>(esp_timer_get_time() / 1000)) == 0;
        diag.error.store(expired ? Wake::Error::Timeout : Wake::Error::Cancelled);
        return expired ? Result::Expired : Result::Failed;
    }
    struct Decoder {
        Impl& owner;
        srmodel_list_t* models{nullptr};
        const esp_mn_iface_t* api{nullptr};
        model_iface_data_t* decoder{nullptr};
        std::int16_t* pcm{nullptr};
        int samples{0}, slot{0};
        bool commands{false}, mic{false}, recording{false}, ready{false};
        std::int64_t warm_until{0}, record_until{0};
        ~Decoder() {
            stage(Wake::Stage::Cleanup);
            if (mic) { M5.Mic.end(); vTaskDelay(pdMS_TO_TICKS(20)); }
            diag.microphone_active.store(false);
            if (pcm) { std::fill_n(pcm, 2 * samples, 0); heap_caps_free(pcm); }
            if (decoder && api) api->destroy(decoder);
            if (commands) esp_mn_commands_free();
            if (models) esp_srmodel_deinit(models);
            stage(Wake::Stage::Closed);
        }
        bool fail(Wake::Error error) { diag.error.store(error); return false; }
        bool begin() {
            if (!owner.permitted()) return false;
            stage(Wake::Stage::MapModel);
            // Refuse before entering the library's assert-based allocations.
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2'700'000 ||
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 32'768)
                return fail(Wake::Error::NoMemory);
            models = esp_srmodel_init("model");
            if (!models || models->num <= 0) return fail(Wake::Error::NoModel);
            char* name = esp_srmodel_filter(models, "mn5q8", "cn");
            if (!name || !(api = esp_mn_handle_from_name(name))) return fail(Wake::Error::NoModel);
            if (!owner.permitted()) return false;
            stage(Wake::Stage::CreateDecoder);
            decoder = api->create(name, 4000);
            if (!decoder || api->get_samp_rate(decoder) != 16000) return fail(Wake::Error::Decoder);
            if (!owner.permitted()) return false;
            stage(Wake::Stage::Commands);
            if (esp_mn_commands_alloc(api, decoder) != ESP_OK) return fail(Wake::Error::Commands);
            commands = true;
            if (esp_mn_commands_clear() != ESP_OK ||
                esp_mn_commands_add(1, "ni hao jing tong") != ESP_OK ||
                esp_mn_commands_add(2, "ni hao") != ESP_OK ||
                esp_mn_commands_add(2, "jing tong") != ESP_OK ||
                esp_mn_commands_add(2, "jie shao yi xia zi ji") != ESP_OK ||
                esp_mn_commands_update() != nullptr) return fail(Wake::Error::Commands);
            api->set_det_threshold(decoder, 0.90f);
            samples = api->get_samp_chunksize(decoder);
            if (samples <= 0 || samples > 2048) return fail(Wake::Error::Decoder);
            if (!owner.permitted()) return false;
            if (owner.check_only) return true; // NO mic/speaker action in self-check
            pcm = static_cast<std::int16_t*>(heap_caps_aligned_alloc(16, 2 * samples * sizeof(std::int16_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (!pcm) return fail(Wake::Error::NoMemory);
            std::fill_n(pcm, 2 * samples, 0);
            M5.Speaker.end(); M5.Mic.end();
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!owner.permitted()) return false;
            stage(Wake::Stage::Warmup);
            auto cfg = M5.Mic.config(); cfg.magnification = 4; M5.Mic.config(cfg);
            mic = M5.Mic.begin();
            diag.microphone_active.store(mic);
            warm_until = esp_timer_get_time() / 1000 + 1500;
            return mic || fail(Wake::Error::Microphone);
        }
        Result poll() {
            const auto now = esp_timer_get_time() / 1000;
            if (!recording) {
                if (!M5.Mic.record(pcm, samples, 16000, false)) { fail(Wake::Error::Microphone); return Result::Failed; }
                recording = true; record_until = now + 1000; return Result::Preparing;
            }
            if (M5.Mic.isRecording()) {
                if (now >= record_until) { fail(Wake::Error::Microphone); return Result::Failed; }
                return ready ? Result::Waiting : Result::Preparing;
            }
            auto* completed = pcm + slot * samples;
            slot = 1 - slot;
            if (!M5.Mic.record(pcm + slot * samples, samples, 16000, false)) { fail(Wake::Error::Microphone); return Result::Failed; }
            record_until = now + 1000;
            if (now < warm_until) { api->clean(decoder); return Result::Preparing; }
            if (!ready) {
                api->clean(decoder); ready = true; stage(Wake::Stage::Detect);
                ESP_LOGI("local-wake", "offline detector ready; no token/socket/upload");
                return Result::Waiting;
            }
            const auto s = api->detect(decoder, completed);
            if (s == ESP_MN_STATE_DETECTED) {
                const auto* r = api->get_results(decoder);
                const bool hit = r && r->num > 0 && r->command_id[0] == 1 &&
                    std::isfinite(r->prob[0]) && r->prob[0] >= 0.90f;
                api->clean(decoder);
                return hit ? Result::Detected : Result::Waiting;
            }
            if (s == ESP_MN_STATE_TIMEOUT) api->clean(decoder);
            return Result::Waiting;
        }
    };
    static void entry(void* arg) {
        auto& self = *static_cast<Impl*>(arg);
        Result result = Result::Failed;
        {
            Decoder decoder{self};
            if (decoder.begin()) {
                if (self.check_only) result = Result::Checked;
                else {
                    while (self.permitted()) {
                        result = decoder.poll();
                        if (result == Result::Detected || result == Result::Failed) break;
                        self.current.store(result);
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                }
            }
            if (!self.permitted() && result != Result::Detected) result = self.cancelled_result();
        } // I2S + model destroy/unmap complete on this SAME internal stack
        self.current.store(result);
        xSemaphoreGive(self.done);
        vTaskSuspend(nullptr); // same-core coordinator deletes this task WithCaps
        for (;;) vTaskDelay(portMAX_DELAY);
    }
};
LocalWake::~LocalWake() { while (!close()) vTaskDelay(pdMS_TO_TICKS(10)); }
bool LocalWake::close() {
    if (!impl_) return true;
    impl_->stop.store(true);
    if (impl_->task && xSemaphoreTake(impl_->done, 0) != pdTRUE) return false;
    if (impl_->task) vTaskDeleteWithCaps(impl_->task);
    impl_->~Impl(); heap_caps_free(impl_); impl_ = nullptr;
    diag.worker_active.store(false);
    return true;
}
LocalWake::Result LocalWake::poll() {
    if (!impl_) {
        diag.error.store(Wake::Error::None);
        diag.stage.store(Wake::Stage::Allocate);
        void* memory = heap_caps_malloc(sizeof(Impl), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        impl_ = memory ? new (memory) Impl : nullptr;
        if (!impl_ || !impl_->done) {
            if (impl_) { impl_->~Impl(); heap_caps_free(impl_); impl_ = nullptr; }
            diag.error.store(Wake::Error::NoMemory); return Result::Failed;
        }
        // Same core as coordinator: the acknowledged worker cannot still be
        // executing on a remote CPU when its task/stack is reclaimed.
        diag.worker_active.store(true);
        const auto ok = xTaskCreatePinnedToCoreWithCaps(Impl::entry, "local-wake", 16384, impl_,
            3, &impl_->task, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            impl_->~Impl(); heap_caps_free(impl_); impl_ = nullptr; diag.worker_active.store(false);
            diag.error.store(Wake::Error::NoMemory); return Result::Failed;
        }
    }
    return impl_->current.load();
}
} // namespace stackchan::app
#else
void local_wake_boot_diagnostics() {}
namespace stackchan::app {
LocalWake::~LocalWake() = default;
bool LocalWake::close() { return true; }
LocalWake::Result LocalWake::poll() { return Result::Failed; }
}
#endif
