#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#define CONFIG_STACKCHAN_JINGTONG_WAKE 1
#define RTC_NOINIT_ATTR
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr int ESP_OK=0, pdTRUE=1, pdPASS=1;
constexpr unsigned MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_SPIRAM=4;
constexpr unsigned portMAX_DELAY=0xffffffff;
using TickType_t=unsigned;
inline unsigned pdMS_TO_TICKS(unsigned n){return n;}
namespace Fake {
struct Fixture {
    std::atomic<std::int64_t> now{0};
    std::atomic<int> maps{0}, decoders{0}, commands{0}, allocations{0}, tasks{0};
    std::atomic<int> mic_begins{0}, records{0}, speaker_ends{0}, detects{0};
    std::atomic<bool> mic{false}, fail_task{false}, fail_model{false};
    std::atomic<bool> block_create{false}, in_create{false}, release_create{false};
    std::atomic<int> next_command{0};
    std::atomic<float> probability{0.95f};
    std::atomic<std::size_t> internal{131072}, psram{6*1024*1024};
    std::atomic<std::int64_t> record_end{0};
    ~Fixture(){assert(maps==0&&decoders==0&&commands==0&&allocations==0&&tasks==0&&!mic);}
};
inline Fixture* active=nullptr;
inline Fixture& f(){assert(active);return *active;}
struct Task {std::thread thread; std::mutex mutex; std::condition_variable cv; bool deleted=false;};
inline thread_local Task* current=nullptr;
struct TaskExit {};
inline void model_thread(){assert(current);}
}
using TaskHandle_t=Fake::Task*;
inline int xTaskCreatePinnedToCoreWithCaps(void(*entry)(void*),const char*,unsigned bytes,void* arg,
                                         unsigned priority,TaskHandle_t* out,int core,unsigned caps){
    assert(bytes==16384&&priority==3&&core==0);
    assert((caps&MALLOC_CAP_INTERNAL)&&!(caps&MALLOC_CAP_SPIRAM));
    auto& f=Fake::f(); if(f.fail_task)return 0;
    auto* t=new Fake::Task;*out=t;f.tasks++;
    t->thread=std::thread([=]{Fake::current=t;try{entry(arg);}catch(Fake::TaskExit&){}Fake::current=nullptr;});
    return pdPASS;
}
inline void vTaskDelay(unsigned){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
inline void vTaskSuspend(TaskHandle_t){
    auto* t=Fake::current;assert(t);std::unique_lock l(t->mutex);
    t->cv.wait(l,[&]{return t->deleted;});throw Fake::TaskExit{};
}
inline void vTaskDeleteWithCaps(TaskHandle_t t){
    assert(t&&t!=Fake::current);
    auto& f=Fake::f();assert(!f.mic&&f.maps==0&&f.decoders==0&&f.commands==0);
    {std::lock_guard l(t->mutex);t->deleted=true;}t->cv.notify_all();t->thread.join();delete t;f.tasks--;
}
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t){Fake::model_thread();return 12000;}
struct StaticSemaphore_t{std::mutex mutex;int count=0;};
using SemaphoreHandle_t=StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* s){return s;}
inline int xSemaphoreTake(SemaphoreHandle_t s,unsigned){std::lock_guard l(s->mutex);if(!s->count)return 0;s->count--;return pdTRUE;}
inline int xSemaphoreGive(SemaphoreHandle_t s){std::lock_guard l(s->mutex);s->count++;return pdTRUE;}
inline void vSemaphoreDelete(SemaphoreHandle_t){}
inline std::int64_t esp_timer_get_time(){return Fake::f().now.load();}
inline int esp_reset_reason(){return 3;}
inline std::size_t heap_caps_get_free_size(unsigned cap){return cap==MALLOC_CAP_INTERNAL?Fake::f().internal.load():Fake::f().psram.load();}
inline void* heap_caps_malloc(std::size_t size,unsigned){auto* p=std::malloc(size);if(p)Fake::f().allocations++;return p;}
inline void* heap_caps_aligned_alloc(std::size_t align,std::size_t size,unsigned caps){
    assert(caps&MALLOC_CAP_INTERNAL);auto* p=std::aligned_alloc(align,(size+align-1)/align*align);
    if(p)Fake::f().allocations++;return p;
}
inline void heap_caps_free(void* p){if(p){Fake::f().allocations--;std::free(p);}}
struct FakeMic {
    struct Config{int magnification=4;};Config config(){return {};}
    void config(Config){}
    bool begin(){Fake::model_thread();Fake::f().mic_begins++;Fake::f().mic=true;return true;}
    void end(){Fake::model_thread();Fake::f().mic=false;}
    bool record(std::int16_t* p,std::size_t n,unsigned rate,bool stereo){
        Fake::model_thread();auto& f=Fake::f();assert(f.mic&&rate==16000&&!stereo);
        std::memset(p,0,n*sizeof(*p));f.records++;f.record_end=f.now+n*1000000/rate;return true;
    }
    bool isRecording(){return Fake::f().now<Fake::f().record_end;}
};
struct FakeSpeaker{void end(){Fake::model_thread();Fake::f().speaker_ends++;}};
struct FakeM5{FakeMic Mic;FakeSpeaker Speaker;};
inline FakeM5 M5;
struct srmodel_list_t{int num=1;};
struct model_iface_data_t{};
struct esp_mn_error_t{};
enum esp_mn_state_t{ESP_MN_STATE_DETECTING,ESP_MN_STATE_DETECTED,ESP_MN_STATE_TIMEOUT};
struct esp_mn_results_t{int num=1;int command_id[5]{};float prob[5]{};};
struct esp_mn_iface_t {
    model_iface_data_t*(*create)(const char*,int);
    int(*get_samp_rate)(model_iface_data_t*);
    int(*get_samp_chunksize)(model_iface_data_t*);
    int(*set_det_threshold)(model_iface_data_t*,float);
    void(*destroy)(model_iface_data_t*);
    void(*clean)(model_iface_data_t*);
    esp_mn_state_t(*detect)(model_iface_data_t*,std::int16_t*);
    esp_mn_results_t*(*get_results)(model_iface_data_t*);
};
inline srmodel_list_t* esp_srmodel_init(const char*){Fake::model_thread();if(Fake::f().fail_model)return nullptr;Fake::f().maps++;static srmodel_list_t m;return &m;}
inline void esp_srmodel_deinit(srmodel_list_t*){Fake::model_thread();Fake::f().maps--;}
inline char* esp_srmodel_filter(srmodel_list_t*,const char*,const char*){static char n[]="mn5q8_cn";return n;}
inline esp_mn_iface_t* esp_mn_handle_from_name(char*){
    static esp_mn_iface_t api{
        [](const char*,int duration)->model_iface_data_t*{
            Fake::model_thread();assert(duration==4000);auto& f=Fake::f();f.in_create=true;
            while(f.block_create&&!f.release_create)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            f.decoders++;return new model_iface_data_t;
        },
        [](model_iface_data_t*){return 16000;},
        [](model_iface_data_t*){return 480;},
        [](model_iface_data_t*,float t){assert(t==0.90f);return 0;},
        [](model_iface_data_t* p){Fake::model_thread();Fake::f().decoders--;delete p;},
        [](model_iface_data_t*){},
        [](model_iface_data_t*,std::int16_t*){Fake::model_thread();Fake::f().detects++;return Fake::f().next_command?ESP_MN_STATE_DETECTED:ESP_MN_STATE_DETECTING;},
        [](model_iface_data_t*)->esp_mn_results_t*{static esp_mn_results_t r;r.command_id[0]=Fake::f().next_command;r.prob[0]=Fake::f().probability;return &r;}
    };return &api;
}
inline int esp_mn_commands_alloc(const esp_mn_iface_t*,model_iface_data_t*){Fake::model_thread();Fake::f().commands++;return ESP_OK;}
inline int esp_mn_commands_clear(){return ESP_OK;}
inline int esp_mn_commands_add(int,const char*){return ESP_OK;}
inline esp_mn_error_t* esp_mn_commands_update(){return nullptr;}
inline int esp_mn_commands_free(){Fake::model_thread();Fake::f().commands--;return ESP_OK;}
