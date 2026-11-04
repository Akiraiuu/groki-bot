// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "event_policy_lock.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
namespace stopwatch::candidate {
enum OperationDomain : std::uint32_t { CoordinatorOperation=1, PreparationOperation=2,
    ClientOperation=4, AudioOperation=8 };
struct OperationSnapshot {
    bool owned{}, revoked{}, work_deadline_reached{}, deadline_exceeded{}, invariant_failed{};
    std::uint32_t generation{}, coordinator{}, preparation{}, client{}, audio{};
    bool quiescent() const { return coordinator==0 && preparation==0 && client==0 && audio==0; }
};
// The owner and immutable deadline outlive every lease, SDK closer and supervisor.
// Only fixed scalar state is accessed while holding the platform state lock.
class SessionOperationGate {
public:
    class Lease {
        friend class SessionOperationGate;
        SessionOperationGate* owner_{};
        std::uint32_t generation_{},domains_{};
        Lease(SessionOperationGate& o,std::uint32_t g,std::uint32_t d):owner_(&o),generation_(g),domains_(d) {}
    public:
        Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
        Lease(Lease&& other) noexcept:owner_(std::exchange(other.owner_,nullptr)),generation_(other.generation_),domains_(other.domains_) {}
        Lease& operator=(Lease&& other) noexcept {
            if(this!=&other) { reset();owner_=std::exchange(other.owner_,nullptr);generation_=other.generation_;domains_=other.domains_; }return *this;
        }
        ~Lease(){reset();}
        void reset() noexcept { if(auto* o=std::exchange(owner_,nullptr)) o->release(generation_,domains_); }
    };
    struct Bootstrap { Lease coordinator;Lease audio; };
    bool begin(std::uint32_t generation,std::uint64_t now,std::uint64_t deadline,
               std::uint64_t reserve_us=5'000'000) {
        const EventPolicyGuard lock{lock_};
        if(owned_ || !generation || generation==generation_ || !deadline) return false;
        for(auto n:active_) if(n) return false;
        generation_=generation;origin_=last_=now;deadline_=deadline;
        work_deadline_=deadline>reserve_us?deadline-reserve_us:0;
        owned_=true;revoked_=now>=work_deadline_;fault_=false;
        client_close_claimed_=audio_close_claimed_=false;
        bootstrap_taken_=sealed_=false;
        return true;
    }
    std::optional<Bootstrap> adopt_bootstrap(std::uint32_t generation) {
        const EventPolicyGuard lock{lock_};
        if(!owned_ || generation!=generation_ || bootstrap_taken_ || sealed_) return std::nullopt;
        for(auto n:active_) if(n) return std::nullopt;
        bootstrap_taken_=true;active_[0]=active_[3]=1;
        return Bootstrap{Lease{*this,generation,CoordinatorOperation},Lease{*this,generation,AudioOperation}};
    }
    bool seal(std::uint32_t generation) {
        const EventPolicyGuard lock{lock_};
        if(!owned_ || generation!=generation_ || sealed_) return false;
        sealed_=true;return true;
    }
    std::optional<Lease> acquire(std::uint32_t domains,std::uint32_t generation,std::uint64_t now) {
        const EventPolicyGuard lock{lock_};tick_locked(now);
        if(!owned_ || !sealed_ || revoked_ || generation!=generation_ || !domains || (domains&~15u)) return std::nullopt;
        for(unsigned i=0;i<4;++i) if((domains&(1u<<i)) && active_[i]>=64) {fault_=revoked_=true;return std::nullopt;}
        for(unsigned i=0;i<4;++i) if(domains&(1u<<i)) ++active_[i];
        return Lease{*this,generation,domains};
    }
    void revoke(std::uint32_t generation) {
        const EventPolicyGuard lock{lock_};if(owned_ && generation==generation_) revoked_=true;
    }
    OperationSnapshot observe(std::uint64_t now) {
        const EventPolicyGuard lock{lock_};tick_locked(now);
        return {owned_,revoked_,last_>=work_deadline_,last_>=deadline_,fault_,generation_,active_[0],active_[1],active_[2],active_[3]};
    }
    bool claim_client_close(std::uint32_t generation) {
        const EventPolicyGuard lock{lock_};
        if(!owned_ || !revoked_ || generation!=generation_ || active_[2] || client_close_claimed_) return false;
        client_close_claimed_=true;return true;
    }
    bool claim_audio_close(std::uint32_t generation) {
        const EventPolicyGuard lock{lock_};
        if(!owned_ || !revoked_ || generation!=generation_ || active_[3] || audio_close_claimed_) return false;
        audio_close_claimed_=true;return true;
    }
    bool release_scope(std::uint32_t generation) {
        const EventPolicyGuard lock{lock_};
        if(!owned_ || generation!=generation_ || !revoked_ || !client_close_claimed_ || !audio_close_claimed_) return false;
        for(auto n:active_) if(n) return false;
        owned_=false;return true; // Caller separately proved every close/supervisor acknowledgement.
    }
private:
    void tick_locked(std::uint64_t now) {
        if(!owned_) return;
        if(now<origin_) { fault_=revoked_=true;return; }
        // Different tasks sample time before taking this state lock. Their
        // samples may arrive out of order without the monotonic clock reversing.
        if(now>last_) last_=now;
        if(last_>=work_deadline_) revoked_=true;
    }
    void release(std::uint32_t generation,std::uint32_t domains) noexcept {
        const EventPolicyGuard lock{lock_};
        if(!owned_ || generation!=generation_) {fault_=revoked_=true;return;}
        for(unsigned i=0;i<4;++i) if(domains&(1u<<i)) {
            if(!active_[i]) {fault_=revoked_=true;return;}
            --active_[i];
        }
    }
    mutable EventPolicyLock lock_;
    std::array<std::uint32_t,4> active_{};
    std::uint32_t generation_{};
    std::uint64_t origin_{},last_{},deadline_{},work_deadline_{};
    bool owned_{false},revoked_{false},fault_{false},client_close_claimed_{false},audio_close_claimed_{false};
    bool bootstrap_taken_{false},sealed_{false};
};
} // namespace stopwatch::candidate
