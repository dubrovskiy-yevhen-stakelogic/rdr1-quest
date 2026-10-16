// Host-only: compile with C++20 and native/src on the include path, then run.
#include "vr/VrFireDiagnostics.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <thread>
using namespace rdrvr::vr::firediag;
struct Shot {std::uint64_t ns{};std::uintptr_t weapon{};bool open{};};
int main() {
    Pending<Shot,3> pending;unsigned overflows=0,expired=0;
    auto overflow=[&](const Shot& s){assert(s.ns==10);++overflows;};
    pending.add({10,1,true},overflow);pending.add({20,1,true},overflow);pending.add({30,2,true},overflow);
    assert(!pending.match(3,40,700)); // a different weapon cannot confirm these presses
    assert(!pending.match(1,5,700)); // clock precedes press
    auto* s=pending.match(2,40,700);assert(s&&s->ns==30);
    s=pending.match(1,40,700);assert(s&&s->ns==10); // oldest matching press, not newest
    s=pending.match(1,40,700);assert(s&&s->ns==20);
    assert(!pending.match(1,40,700)); // one projectile only closes one press
    pending.add({10,1,true},overflow);pending.add({20,1,true},overflow);pending.add({30,2,true},overflow);
    pending.add({40,2,true},overflow);assert(overflows==1); // bounded overflow is explicit
    pending.expire(720,700,[&](const Shot&){++expired;});assert(expired==0);
    pending.expire(721,700,[&](const Shot& shot){assert(shot.ns==20);++expired;});assert(expired==1);
    assert(!pending.match(1,721,700));
    s=pending.match(2,730,700);assert(s&&s->ns==30); // inclusive deadline
    pending.expire(741,700,[&](const Shot& shot){assert(shot.ns==40);++expired;});assert(expired==2);
    pending.expire(999,700,[&](const Shot&){++expired;});assert(expired==2);

    // Ring remains coherent across input/engine/report threads and wraps without growing.
    std::array<std::thread,4> workers;
    for(auto& w:workers)w=std::thread([]{for(unsigned i=0;i<100;++i){Event e;e.ns=i;e.reason="TEST";record(e);}});
    for(auto& w:workers)w.join();assert(count==400);
    Event e;e.ns=1000;e.attemptNs=900;e.stage="BLOCKED";e.reason="NOT_REARMED";
    e.trigger=std::numeric_limits<float>::quiet_NaN();e.clip=std::numeric_limits<float>::infinity();
    std::snprintf(e.name.data(),e.name.size(),"gun\"\\\n");record(e);
    // Python can parse this stdout to verify escaping, numeric sentinels and chronological ring order.
    std::cout<<report()<<'\n';
}
