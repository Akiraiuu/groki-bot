// SPDX-License-Identifier: BSL-1.0
#include "conversation/session_operation_gate.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
using namespace stopwatch::candidate;
static unsigned checks=0;
static void check(bool value,const char* why) {++checks;if(!value) {std::cerr<<why<<'\n';std::abort();}}
int main() {
    SessionOperationGate gate;
    check(gate.begin(1,1,60'000'000),"new scope");
    check(!gate.acquire(ClientOperation,1,2),"no work before seal");
    auto b=gate.adopt_bootstrap(1);check(bool(b) && !gate.adopt_bootstrap(1),"bootstrap existing owners exactly once");
    check(gate.seal(1) && !gate.seal(1),"one seal");
    auto client=gate.acquire(ClientOperation,1,3);auto audio=gate.acquire(AudioOperation,1,4);
    check(client && audio,"independent domains admitted");
    check(!gate.acquire(16,1,4) && !gate.acquire(1,2,4),"unknown domain and stale scope denied");
    auto s=gate.observe(55'000'000);check(s.revoked && s.work_deadline_reached && !s.deadline_exceeded,"reserve uses original deadline");
    check(!gate.acquire(PreparationOperation,1,55'000'001),"no late operation");
    check(!gate.claim_client_close(1) && !gate.claim_audio_close(1),"live operations retain owners");
    client.reset();check(gate.claim_client_close(1) && !gate.claim_client_close(1),"client close exactly once after ack");
    audio.reset();check(!gate.claim_audio_close(1),"bootstrap wake still owns audio");
    b->audio.reset();check(gate.claim_audio_close(1),"wake ack permits serialized audio closer");
    check(!gate.release_scope(1),"live coordinator prevents owner release");
    b->coordinator.reset();check(gate.release_scope(1),"quiescent original owner may release");
    check(!gate.begin(1,2,60'000'000) && gate.begin(2,2,60'000'000),"generation never silently reused");
    check(gate.seal(2),"new scope seal");
    auto lease=gate.acquire(ClientOperation|AudioOperation,2,3);
    check(lease && gate.observe(3).client==1 && gate.observe(3).audio==1,"multi-domain lease");
    auto moved=std::move(*lease);lease.reset();check(gate.observe(3).client==1,"move has one release");
    moved.reset();check(gate.observe(3).client==0,"moved lease returned once");
    check(!gate.observe(2).invariant_failed,"out-of-order task time sample is not a reversed clock");
    check(gate.observe(1).invariant_failed && !gate.acquire(ClientOperation,2,4),"before-origin clock fails closed");
    SessionOperationGate concurrent;check(concurrent.begin(9,0,60'000'000) && concurrent.seal(9),"concurrent scope");
    std::atomic<unsigned> admitted{0};std::vector<std::thread> threads;
    for(unsigned t=0;t<4;++t) threads.emplace_back([&] {for(unsigned n=0;n<25000;++n) {
        auto work=concurrent.acquire(ClientOperation|AudioOperation,9,1);if(work) ++admitted;
    }});
    for(auto& t:threads) t.join();
    check(admitted==100000 && concurrent.observe(1).quiescent(),"100000 concurrent leases exactly released");
    concurrent.revoke(9);check(!concurrent.acquire(ClientOperation,9,2),"explicit revoke denies new work");
    SessionOperationGate expired;check(expired.begin(4,61'000'000,60'000'000),"expired original deadline retained");
    auto old=expired.adopt_bootstrap(4);check(old && expired.seal(4),"expired bootstrap tracks existing owner only");
    check(!expired.acquire(ClientOperation,4,61'000'001) && expired.observe(61'000'001).deadline_exceeded,"expired setup cannot extend time");
    std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<",\"concurrent_leases\":"<<admitted<<"}\n";
}
