// SPDX-License-Identifier: BSL-1.0
#include "local_wake.hpp"
#include "sdkconfig.h"

#if defined(CONFIG_STACKCHAN_JINGTONG_WAKE)
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <model_path.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace stackchan::app {
struct LocalWake::Impl {
    srmodel_list_t* models{nullptr};
    const esp_mn_iface_t* api{nullptr};
    model_iface_data_t* decoder{nullptr};
    std::int16_t* pcm{nullptr};
    int samples{0}, slot{0};
    bool commands{false}, mic{false}, recording{false};
    std::int64_t warm_until{0}, record_until{0};
    ~Impl() {
        if (mic) { M5.Mic.end(); vTaskDelay(pdMS_TO_TICKS(20)); }
        if (pcm) { std::fill_n(pcm, 2 * samples, 0); heap_caps_free(pcm); }
        if (commands) esp_mn_commands_free();
        if (decoder && api) api->destroy(decoder);
        if (models) esp_srmodel_deinit(models);
    }
    bool begin() {
        models = esp_srmodel_init("model");
        if (!models) return false;
        char* name = esp_srmodel_filter(models, "mn5q8", "cn");
        if (!name || !(api = esp_mn_handle_from_name(name))) return false;
        decoder = api->create(name, 4000);
        if (!decoder || api->get_samp_rate(decoder) != 16000) return false;
        if (esp_mn_commands_alloc(api, decoder) != ESP_OK) return false;
        commands = true;
        // Full phrase only; ordinary greetings and partial names never count.
        if (esp_mn_commands_clear() != ESP_OK ||
            esp_mn_commands_add(1, "ni hao jing tong") != ESP_OK ||
            esp_mn_commands_add(2, "ni hao") != ESP_OK ||
            esp_mn_commands_add(2, "jing tong") != ESP_OK ||
            esp_mn_commands_add(2, "jie shao yi xia zi ji") != ESP_OK ||
            esp_mn_commands_update() != nullptr) return false;
        api->set_det_threshold(decoder, 0.90f);
        samples = api->get_samp_chunksize(decoder);
        if (samples <= 0 || samples > 2048) return false;
        pcm = static_cast<std::int16_t*>(heap_caps_calloc(2 * samples, sizeof(std::int16_t),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!pcm) return false;
        M5.Speaker.end();
        M5.Mic.end();
        vTaskDelay(pdMS_TO_TICKS(20));
        // Match the accepted microphone gain; recognition remains entirely local.
        auto cfg = M5.Mic.config();
        cfg.magnification = 4;
        M5.Mic.config(cfg);
        mic = M5.Mic.begin();
        warm_until = esp_timer_get_time() / 1000 + 1500;
        if (mic) ESP_LOGI("local-wake", "offline command detector ready; no token/socket/upload");
        return mic;
    }
    LocalWake::Result poll() {
        const auto now = esp_timer_get_time() / 1000;
        if (!recording) {
            if (!M5.Mic.record(pcm, samples, 16000, false)) return LocalWake::Result::Failed;
            recording = true;
            record_until = now + 1000;
            return LocalWake::Result::Waiting;
        }
        if (M5.Mic.isRecording())
            return now >= record_until ? LocalWake::Result::Failed : LocalWake::Result::Waiting;
        auto* completed = pcm + slot * samples;
        // Queue the next buffer BEFORE inference so CPU work does not leave
        // holes in the microphone stream. The driver never owns completed.
        slot = 1 - slot;
        if (!M5.Mic.record(pcm + slot * samples, samples, 16000, false)) return LocalWake::Result::Failed;
        record_until = now + 1000;
        if (now < warm_until) { api->clean(decoder); return LocalWake::Result::Waiting; }
        const auto state = api->detect(decoder, completed);
        if (state == ESP_MN_STATE_DETECTED) {
            const auto* r = api->get_results(decoder);
            const bool hit = r && r->num > 0 && r->command_id[0] == 1 &&
                             std::isfinite(r->prob[0]) && r->prob[0] >= 0.90f;
            api->clean(decoder);
            return hit ? LocalWake::Result::Detected : LocalWake::Result::Waiting;
        }
        if (state == ESP_MN_STATE_TIMEOUT) api->clean(decoder);
        return LocalWake::Result::Waiting;
    }
};
LocalWake::~LocalWake() { close(); }
void LocalWake::close() { delete impl_; impl_ = nullptr; }
LocalWake::Result LocalWake::poll() {
    if (!impl_) {
        impl_ = new (std::nothrow) Impl;
        if (!impl_ || !impl_->begin()) {
            close();
            ESP_LOGW("local-wake", "unavailable; disabled without touching paid permission");
            return Result::Failed;
        }
    }
    return impl_->poll();
}
} // namespace stackchan::app
#else
namespace stackchan::app {
LocalWake::~LocalWake() = default;
void LocalWake::close() {}
LocalWake::Result LocalWake::poll() { return Result::Failed; }
}
#endif
