#pragma once
#include <atomic>

// HTTP requests maintenance; only the cabinet owner can grant it.
class OtaGate {
public:
    enum State { IDLE, REQUESTED, GRANTED, REJECTED, REBOOT };
    std::atomic<State> state{IDLE};
    bool request() { State expected=IDLE; return state.compare_exchange_strong(expected, REQUESTED); }
    void decide(bool safe) {
        State expected=REQUESTED;
        state.compare_exchange_strong(expected, safe ? GRANTED : REJECTED);
    }
    bool blocked() const { return state.load() != IDLE; }
    void release() { state=IDLE; }
};
