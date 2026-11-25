// SPDX-License-Identifier: GPL-2.0-or-later
#include "iss_session.h"
#include <cassert>
int main() {
    TlgQuality::Governor gov;
    for(int i=0;i<60;i++) assert(gov.Update(30,12,50)==0);
    for(int i=0;i<60;i++) gov.Update(30,15,400);
    assert(gov.Level()==14);
    for(int i=0;i<12;i++) gov.Update(30,30,400);
    assert(gov.Level()==13);
    assert(gov.Update(0,30,400)==0);
    IssSession::Window window;
    int32_t level=0; IssSession::guest_level.store(&level);
    uint64_t time=1,draws=0; window.Flip(time,draws,30,30);
    for(int i=0;i<150;i++) {
        time+=100000000; draws+=500;
        IssSession::gpu_timing.store(true);
        IssSession::gpu_busy_ns.fetch_add(50000000);
        window.Flip(time,draws,30,30);
    }
    assert(level>0 && level<=14);
}
