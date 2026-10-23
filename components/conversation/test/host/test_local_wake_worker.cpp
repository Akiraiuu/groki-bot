// Exercise production local_wake.cpp with only hardware/RTOS calls substituted.
#include "fake_sdk.hpp"
#include "local_wake.hpp"
#include "conversation/local_wake_status.hpp"
#include "conversation/paid_voice_test.hpp"
#include <limits>
using stackchan::app::LocalWake;
using S=stackchan::conversation::LocalWakeStatus;
auto& state=stackchan::conversation::local_wake_status;
void begin(Fake::Fixture& f,bool check=false){
    Fake::active=&f;state.enabled=true;state.check_only=check;state.started_ms=0;
    state.microphone_active=false;state.worker_active=false;state.error=S::Error::None;
}
template<class F> void until(F test){
    for(int i=0;i<2000;i++){
        if(test())return;
        Fake::f().now+=10000;std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }assert(false&&"worker failed to finish");
}
void close(LocalWake& w){until([&]{return w.close();});assert(!state.microphone_active&&!state.worker_active);}
int main(){
    using R=LocalWake::Result;
    using stackchan::conversation::local_wake_remaining;
    assert(local_wake_remaining(0,40000,39999)==1);
    assert(local_wake_remaining(0,40000,40000)==0);
    assert(local_wake_remaining(0xffffff9c,40000,200)==39700);
    {
        Fake::Fixture f;begin(f,true);LocalWake w;
        until([&]{return w.poll()==R::Checked;});
        assert(f.mic_begins==0&&f.records==0&&f.speaker_ends==0);
        assert(f.maps==0&&f.decoders==0&&f.commands==0);
        assert(state.worker_active);close(w);
    }
    {
        Fake::Fixture f;begin(f);LocalWake w;
        until([&]{return w.poll()==R::Waiting;});
        assert(f.now>=1500000&&f.mic&&state.microphone_active);
        state.note_stop();close(w);assert(f.records>0);
        assert(!stackchan::conversation::paid_voice_test.permitted(1000));
    }
    {
        Fake::Fixture f;begin(f);LocalWake w;
        until([&]{return w.poll()==R::Waiting;});f.now=40000000;
        until([&]{return w.poll()==R::Expired;});
        assert(!f.mic&&state.error==S::Error::Timeout);close(w);
    }
    {
        Fake::Fixture f;begin(f);f.now=40000000;LocalWake w;
        until([&]{return w.poll()==R::Expired;});
        assert(f.mic_begins==0);close(w);
    }
    {
        Fake::Fixture f;begin(f);f.internal=1000;LocalWake w;
        until([&]{return w.poll()==R::Failed;});
        assert(state.error==S::Error::NoMemory&&f.mic_begins==0);close(w);
    }
    {
        Fake::Fixture f;begin(f);f.fail_model=true;LocalWake w;
        until([&]{return w.poll()==R::Failed;});
        assert(state.error==S::Error::NoModel&&f.mic_begins==0);close(w);
    }
    {
        Fake::Fixture f;begin(f);f.fail_task=true;LocalWake w;
        assert(w.poll()==R::Failed);assert(!state.worker_active&&f.mic_begins==0);close(w);
    }
    {
        Fake::Fixture f;begin(f);f.block_create=true;LocalWake w;
        until([&]{w.poll();return f.in_create.load();});
        assert(!w.close()); // no ownership transfer while create is still running
        assert(state.worker_active&&!state.microphone_active);
        f.release_create=true;close(w);assert(f.mic_begins==0);
    }
    {
        Fake::Fixture f;begin(f);LocalWake w;
        until([&]{return w.poll()==R::Waiting;});
        f.next_command=2;
        for(int i=0;i<15;i++){assert(w.poll()!=R::Detected);f.now+=20000;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        f.next_command=1;f.probability=std::numeric_limits<float>::quiet_NaN();
        for(int i=0;i<15;i++){assert(w.poll()!=R::Detected);f.now+=20000;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        f.probability=0.95f;
        until([&]{return w.poll()==R::Detected;});
        assert(!state.microphone_active&&f.maps==0&&f.decoders==0&&f.commands==0);
        close(w);
    }
}
