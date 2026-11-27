// SPDX-License-Identifier: GPL-2.0-or-later
#include "iss_session.h"
#include <cassert>
#include <thread>
#include <vector>
int main() {
    TlgQuality::Governor gov;
    for(int i=0;i<60;i++) assert(gov.Update(30,12,50)==0);
    for(int i=0;i<60;i++) gov.Update(30,15,400);
    assert(gov.Level()==14);
    for(int i=0;i<12;i++) gov.Update(30,30,400);
    assert(gov.Level()==13);
    assert(gov.Update(0,30,400)==0);
    {
        IssSession::ReadFaultSample fault(true);
        auto* sample = IssReadCost::active;
        assert(sample);
        std::thread owner([sample] {
            IssReadCost::Bind bind{sample};
            assert(IssReadCost::active == sample);
            IssReadCost::Add(sample, IssReadCost::Queue,
                             IssReadCost::Clock::now() - std::chrono::milliseconds(2));
            ++sample->requests;
            ++sample->ready;
        });
        owner.join();
    }
    assert(!IssReadCost::active && IssReadCost::window.samples == 1);
    assert(IssReadCost::window.ns[IssReadCost::Queue] >= 2000000);
    assert(IssReadCost::window.requests == 1 && IssReadCost::window.ready == 1);
    using namespace IssReadCost;
    SubmissionHistory history;
    history.Record(3, 12, 2);
    history.Record(4, 9, 3);
    const auto stamp = history.Find(4, 2);
    assert(stamp.prior_batches == 1 && stamp.prior_work == 12 && stamp.prior_dispatches == 2);
    assert(Classify(true,4,4)==Origin::Forced && Classify(true,4,5)==Origin::Submitted);
    assert(Classify(false,4,4)==Origin::Staging);
    struct Timeline {
        unsigned long long complete{};
        std::vector<unsigned long long> waits;
        bool IsFree(unsigned long long tick) { return complete >= tick; }
        void Wait(unsigned long long tick) { waits.push_back(tick); complete=tick; }
    } timeline;
    Totals measurement;
    measurement.fault_start=Clock::now();
    MeasureWait(measurement,timeline,4,Origin::Forced,stamp);
    assert((timeline.waits==std::vector<unsigned long long>{3,4}));
    assert(measurement.producers[0].waits==1 && measurement.producers[0].work==9);
    timeline.waits.clear();
    MeasureWait(measurement,timeline,5,Origin::Submitted,{});
    assert((timeline.waits==std::vector<unsigned long long>{5}));
    history.Record(68, 7, 0);
    assert(history.Find(4,2).tick==0);
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
