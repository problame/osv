/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/sched.hh>
#include <list>
#include <osv/mutex.h>
#include <osv/rwlock.h>
#include <mutex>
#include <osv/debug.hh>
#include <osv/irqlock.hh>
#include <osv/align.hh>
#include <osv/interrupt.hh>
#include <smp.hh>
#include "osv/trace.hh"
#include <osv/percpu.hh>
#include <osv/prio.hh>
#include <osv/elf.hh>
#include <stdlib.h>
#include <math.h>
#include <unordered_map>
#include <osv/wait_record.hh>
#include <osv/preempt-lock.hh>
#include <osv/app.hh>
#include <osv/symbols.hh>
#include <atomic>
#include <algorithm>
#include <iterator>
#include <osv/spinlock.h>

MAKE_SYMBOL(sched::thread::current);
MAKE_SYMBOL(sched::cpu::current);
MAKE_SYMBOL(sched::get_preempt_counter);
MAKE_SYMBOL(sched::preemptable);
MAKE_SYMBOL(sched::preempt);
MAKE_SYMBOL(sched::preempt_disable);
MAKE_SYMBOL(sched::preempt_enable);

__thread char* percpu_base;

extern char _percpu_start[], _percpu_end[];

using namespace osv;
using namespace osv::clock::literals;

namespace sched {

TRACEPOINT(trace_sched_idle, "");
TRACEPOINT(trace_sched_idle_ret, "");
TRACEPOINT(trace_sched_switch, "to %p vold=%g vnew=%g", thread*, float, float);
TRACEPOINT(trace_sched_wait, "");
TRACEPOINT(trace_sched_wait_ret, "");
TRACEPOINT(trace_sched_wake, "wake %p", thread*);
TRACEPOINT(trace_sched_migrate, "thread=%p cpu=%d", thread*, unsigned);
TRACEPOINT(trace_sched_queue, "thread=%p", thread*);
TRACEPOINT(trace_sched_load, "load=%d", size_t);
TRACEPOINT(trace_sched_preempt, "");
TRACEPOINT(trace_sched_ipi, "cpu %d", unsigned);
TRACEPOINT(trace_sched_yield, "");
TRACEPOINT(trace_sched_yield_switch, "");
TRACEPOINT(trace_sched_sched, "");
TRACEPOINT(trace_timer_set, "timer=%p time=%d", timer_base*, s64);
TRACEPOINT(trace_timer_reset, "timer=%p time=%d", timer_base*, s64);
TRACEPOINT(trace_timer_cancel, "timer=%p", timer_base*);
TRACEPOINT(trace_timer_fired, "timer=%p", timer_base*);
TRACEPOINT(trace_thread_create, "thread=%p", thread*);
TRACEPOINT(trace_sched_stage_enqueue, "stage=%p scpu=%d tcpu=%d thread=%p", stage*, unsigned, unsigned, thread*);
TRACEPOINT(trace_sched_stage_dequeue, "dcpu=%d thread=%p", unsigned, thread*);
TRACEPOINT(trace_sched_stage_dequeue_stagemig, "dcpu=%d thread=%p", unsigned, thread*);
// TODO more elegant way to support sched::stage::max_stages and sched::max_cpus
TRACEPOINT(trace_sched_stage_update_assignment, "cpu=%d ns=%d c0=%d c1=%d c2=%d c3=%d c4=%d c5=%d c6=%d c7=%d s0=%lx s1=%lx s2=%lx s3=%lx", unsigned, int, int, int, int, int, int, int, int, int, unsigned long, unsigned long, unsigned long, unsigned long);

std::vector<cpu*> cpus __attribute__((init_priority((int)init_prio::cpus)));

thread __thread * s_current;
cpu __thread * current_cpu;

unsigned __thread preempt_counter = 1;
bool __thread need_reschedule = false;

elf::tls_data tls;

constexpr float cmax = 0x1P63;
constexpr float cinitial = 0x1P-63;

static inline float exp_tau(thread_runtime::duration t) {
    // return expf((float)t/(float)tau);
    // Approximate e^x as much faster 1+x for x<0.001 (the error is O(x^2)).
    // Further speed up by comparing and adding integers as much as we can:
    static constexpr int m = tau.count() / 1000;
    static constexpr float invtau = 1.0f / tau.count();
    if (t.count() < m && t.count() > -m)
        return (tau.count() + t.count()) * invtau;
    else
        return expf(t.count() * invtau);
}

// fastlog2() is an approximation of log2, designed for speed over accuracy
// (it is accurate to roughly 5 digits).
// The function is copyright (C) 2012 Paul Mineiro, released under the
// BSD license. See https://code.google.com/p/fastapprox/.
static inline float
fastlog2 (float x)
{
    union { float f; u32 i; } vx = { x };
    union { u32 i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };
    float y = vx.i;
    y *= 1.1920928955078125e-7f;
    return y - 124.22551499f - 1.498030302f * mx.f
            - 1.72587999f / (0.3520887068f + mx.f);
}

static inline float taulog(float f) {
    //return tau * logf(f);
    // We don't need the full accuracy of logf - we use this in time_until(),
    // where it's fine to overshoot, even significantly, the correct time
    // because a thread running a bit too much will "pay" in runtime.
    // We multiply by 1.01 to ensure overshoot, not undershoot.
    static constexpr float tau2 = tau.count() * 0.69314718f * 1.01;
    return tau2 * fastlog2(f);
}

static constexpr runtime_t inf = std::numeric_limits<runtime_t>::infinity();

mutex cpu::notifier::_mtx;
std::list<cpu::notifier*> cpu::notifier::_notifiers __attribute__((init_priority((int)init_prio::notifiers)));

}

#include "arch-switch.hh"

namespace sched {

class thread::reaper {
public:
    reaper();
    void reap();
    void add_zombie(thread* z);
private:
    mutex _mtx;
    std::list<thread*> _zombies;
    std::unique_ptr<thread> _thread;
};

rspinlock::holder::holder(cpu *c, thread *t)
{
    static_assert(sizeof(c->id) == 4);
    static_assert(sizeof(t->id()) == 4);
    uint32_t cpuid = (uint32_t)c->id;
    uint32_t tid = (uint32_t)t->id();
    assert(cpuid != (uint32_t)-1);
    assert(tid != (uint32_t)-1);
    v = (uint64_t)cpuid << 32 | (uint64_t)tid;
}

rspinlock::holder rspinlock::holder::current()
{
    return holder(cpu::current(), thread::current());
}

void rspinlock::lock()
{
    sched::preempt_disable();
    holder caller = holder::current();
    if (_holder.load() == caller)
        goto out;
    while (true) {
        holder before;
        if (_holder.compare_exchange_strong(before, caller))
            break;
        while(_holder.load()) {
            barrier();
        }
    }
out:
    lock_count++;
}

void rspinlock::unlock()
{
    assert(_holder.load() == holder::current());
    if (--lock_count == 0) {
        _holder.store(holder::empty());
    }
    sched::preempt_enable();
}

cpu::cpu(unsigned _id)
    : id(_id)
    , idle_thread()
    , terminating_thread(nullptr)
    , c(cinitial)
    , renormalize_count(0)
{
    auto pcpu_size = _percpu_end - _percpu_start;
    // We want the want the per-cpu area to be aligned as the most strictly
    // aligned per-cpu variable. This is probably CACHELINE_ALIGNED (64 bytes)
    // but we'll be even stricter, and go for page (4096 bytes) alignment.
    percpu_base = (char *) aligned_alloc(4096, pcpu_size);
    memcpy(percpu_base, _percpu_start, pcpu_size);
    percpu_base -= reinterpret_cast<size_t>(_percpu_start);
    if (id == 0) {
        ::percpu_base = percpu_base;
    }
}

void cpu::init_idle_thread()
{
    running_since = osv::clock::uptime::now();
    std::string name = osv::sprintf("idle%d", id);
    idle_thread = thread::make([this] { idle(); }, thread::attr().pin(this).name(name));
    idle_thread->set_priority(thread::priority_idle);
}

// Estimating a *running* thread's total cpu usage (in thread::thread_clock())
// requires knowing a pair [running_since, cpu_time_at_running_since].
// Since we can't read a pair of u64 values atomically, nor want to slow down
// context switches by additional memory fences, our solution is to write
// a single 64 bit "_cputime_estimator" which is atomically written with
// 32 bits from each of the above values. We arrive at 32 bits by dropping
// the cputime_shift=10 lowest bits (so we get microsec accuracy instead of ns)
// and the 22 highest bits (so our range is reduced to about 2000 seconds, but
// since context switches occur much more frequently than that, we're ok).
constexpr unsigned cputime_shift = 10;
void thread::cputime_estimator_set(
        osv::clock::uptime::time_point running_since,
        osv::clock::uptime::duration total_cpu_time)
{
    u32 rs = running_since.time_since_epoch().count() >> cputime_shift;
    u32 tc = total_cpu_time.count() >> cputime_shift;
    _cputime_estimator.store(rs | ((u64)tc << 32), std::memory_order_relaxed);
}
void thread::cputime_estimator_get(
        osv::clock::uptime::time_point &running_since,
        osv::clock::uptime::duration &total_cpu_time)
{
    u64 e = _cputime_estimator.load(std::memory_order_relaxed);
    u64 rs = ((u64)(u32) e) << cputime_shift;
    u64 tc = (e >> 32) << cputime_shift;
    // Recover the (64-32-cputime_shift) high-order bits of rs and tc that we
    // didn't save in _cputime_estimator, by taking the current values of the
    // bits in the current time and _total_cpu_time, respectively.
    // These high bits usually remain the same if little time has passed, but
    // there's also the chance that the old value was close to the cutoff, and
    // just a short passing time caused the high-order part to increase by one
    // since we saved _cputime_estimator. We recognize this case, and
    // decrement the high-order part when recovering the saved value. To do
    // this correctly, we need to assume that less than 2^(32+cputime_shift-1)
    // ns have passed since the estimator was saved. This is 2200 seconds for
    // cputime_shift=10, way longer than our typical context switches.
    constexpr u64 ho = (std::numeric_limits<u64>::max() &
            ~(std::numeric_limits<u64>::max() >> (64 - 32 - cputime_shift)));
    u64 rs_ref = osv::clock::uptime::now().time_since_epoch().count();
    u64 tc_ref = _total_cpu_time.count();
    u64 rs_ho = rs_ref & ho;
    u64 tc_ho = tc_ref & ho;
    if ((rs_ref & ~ho) < rs) {
        rs_ho -= (1ULL << (32 + cputime_shift));
    }
    if ((tc_ref & ~ho) < tc) {
        tc_ho -= (1ULL << (32 + cputime_shift));
    }
    running_since = osv::clock::uptime::time_point(
            osv::clock::uptime::duration(rs_ho | rs));
    total_cpu_time = osv::clock::uptime::duration(tc_ho | tc);
}

// Note that this is a static (class) function, which can only reschedule
// on the current CPU, not on an arbitrary CPU. Allowing to run one CPU's
// scheduler on a different CPU would be disastrous.
void cpu::schedule()
{
    WITH_LOCK(irq_lock) {
        current()->reschedule_from_interrupt();
    }
}

void cpu::reschedule_from_interrupt()
{
    trace_sched_sched();
    assert(sched::exception_depth <= 1);
    need_reschedule = false;
    handle_incoming_wakeups();

    auto now = osv::clock::uptime::now();
    auto interval = now - running_since;
    running_since = now;
    if (interval <= 0) {
        // During startup, the clock may be stuck and we get zero intervals.
        // To avoid scheduler loops, let's make it non-zero.
        // Also ignore backward jumps in the clock.
        interval = context_switch_penalty;
    }
    thread* p = thread::current();

    const auto p_status = p->_detached_state->st.load();
    assert(p_status != thread::status::queued);

    if (p_status != thread::status::stagemig_run) { // see stage::dequeue() assertion
        stage::dequeue();
    }

    p->_total_cpu_time += interval;

    if (p_status == thread::status::running) {
        if (p == idle_thread && runqueue.empty()) {
            /* we are the idle thread, let it run */
            return;
        }
        if (p != idle_thread && runqueue.size() == 1) {
            /* we are the only thread other than the idle thread */
            return;
        }
        /* TODO work-conservation for running threads
         *
         * we should give global stage scheduling an opportunity to balance load
         * between CPUs by enqueing this thread into it's _detached_state->_stage
         */
        p->_detached_state->st.store(thread::status::queued);
        trace_sched_preempt();
        p->stat_preemptions.incr();
        enqueue(*p);
    } else if (p->_detached_state->_stage) {
        // thread is not runnable
        p->_detached_state->_stage->_c_in--;
    }

    /* Find a new thread from CPU-local runqueue */
    auto ni = runqueue.begin();
    auto n = &*ni;
    runqueue.erase(ni);
    assert(n->_detached_state->st.load() == thread::status::queued);

    n->cputime_estimator_set(now, n->_total_cpu_time);

    if (n == idle_thread) {
        trace_sched_idle();
    } else if (p == idle_thread) {
        trace_sched_idle_ret();
    }
    n->stat_switches.incr();

    trace_sched_load(runqueue.size());

    n->_detached_state->st.store(thread::status::running);

    if (app_thread.load(std::memory_order_relaxed) != n->_app) { // don't write into a cache line if it can be avoided
        app_thread.store(n->_app, std::memory_order_relaxed);
    }
    if (lazy_flush_tlb.exchange(false, std::memory_order_seq_cst)) {
        mmu::flush_tlb_local();
    }

    n->switch_to();

    // Note: after the call to n->switch_to(), we should no longer use any of
    // the local variables, nor "this" object, because we just switched to n's
    // stack and the values we can access now are those that existed in the
    // reschedule call which scheduled n out, and will now be returning.
    // So to get the current cpu, we must use cpu::current(), not "this".
    if (cpu::current()->terminating_thread) {
        cpu::current()->terminating_thread->destroy();
        cpu::current()->terminating_thread = nullptr;
    }
}

void cpu::timer_fired()
{
    // nothing to do, preemption will happen if needed
}

struct idle_poll_lock_type {
    explicit idle_poll_lock_type(cpu& c) : _c(c) {}
    void lock() { _c.idle_poll_start(); }
    void unlock() { _c.idle_poll_end(); }
    cpu& _c;
};

void cpu::idle_poll_start()
{
    idle_poll.store(true, std::memory_order_relaxed);
}

void cpu::idle_poll_end()
{
    idle_poll.store(false, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

bool cpu::idle_mwait = true;

void cpu::do_idle()
{
    do {
        handle_incoming_wakeups();
        stage::dequeue();
        if (!runqueue.empty()) {
            break;
        }
        if (cpu::idle_mwait) {
            static_assert(sizeof(cpu::current()->incoming_wakeups_mask) == 8);
            arch::monitor(&cpu::current()->incoming_wakeups_mask, 0, 0);
            arch::mwait(0, 0);
        }
    } while (runqueue.empty());
}

void start_early_threads();

void cpu::idle()
{
    // The idle thread must not sleep, because the whole point is that the
    // scheduler can always find at least one runnable thread.
    // We set preempt_disable just to help us verify this.
    preempt_disable();

    if (id == 0) {
        start_early_threads();
    }

    while (true) {
        do_idle();
        // We have idle priority, so this runs the thread on the runqueue:
        schedule();
    }
}

void cpu::handle_incoming_wakeups()
{
    cpu_set queues_with_wakes{incoming_wakeups_mask.fetch_clear()};
    if (!queues_with_wakes) {
        return;
    }
    for (auto i : queues_with_wakes) {
        irq_save_lock_type irq_lock;
        WITH_LOCK(irq_lock) {
            auto& q = incoming_wakeups[i];
            while (!q.empty()) {
                auto& t = q.front();
                auto& st = t._detached_state->st;
                q.pop_front();
                assert(t.tcpu() == this);
                if (&t == thread::current()) {
                    // Special case of current thread being woken before
                    // having a chance to be scheduled out.
                    // No need to resume timers because migration only happens
                    // if thread was not running.
                    auto st_before = thread::status::waking_run;
                    auto cmpxchg_res = st.compare_exchange_strong(st_before, thread::status::running);
                    assert(cmpxchg_res);
                } else  {
                    while (true) {
                        // TODO spin, not sure if we are allowed to write to incoming_wakups
                        //
                        // FIXME spinning here delays dequeuing from all other incoming_wakups queues
                        // FIXME since we dequeued t, we could have a queue local to this function
                        // FIXME that accumulates all still-running threads and checks on them after
                        // FIXME handling all the other ones
                        // FIXME use the boost::intrusive links for that
                        auto st_before = thread::status::waking_sto;
                        if (st.compare_exchange_strong(st_before,thread::status::queued))
                            break;
                        assert(st_before == thread::status::waking_run);
                    }
                    enqueue(t);
                    if (t._detached_state->_stage) {
                        t._detached_state->_stage->_c_in++;
                    }
                    assert(t._detached_state->_cpu == this); // can't do that in resume_timers
                    t.resume_timers(this);
                }
            }
        }
    }

    trace_sched_load(runqueue.size());
}

void cpu::enqueue(thread& t)
{
    trace_sched_queue(&t);
    runqueue.push_back(t);
}

void cpu::init_on_cpu()
{
    arch.init_on_cpu();
    clock_event->setup_on_cpu();
}

unsigned cpu::load()
{
    return runqueue.size();
}


// The assignment of stages to CPUs based on #cores requirements.
class assignment {
public:
    using requirements = std::array<int, stage::max_stages>;
private:
    requirements reqs;
    std::array<bitset_cpu_set, stage::max_stages> cpus_per_stage;
    int cpus;
    int stages;
public:

    // Used to construct the initial assignment.
    // The given cpus and stages cannot be changed afterwards.
    assignment(int cpus, int stages) : cpus(cpus), stages(stages) {
        assert(stages <= cpus);
        std::fill(reqs.begin(), reqs.begin() + stages, 0);
        for (int si = 0; si < stages; si++) {
            cpus_per_stage[si].reset();
        }
        for (int c = 0; c < cpus; c++) {
            reqs[c%stages] += 1;
            cpus_per_stage[c%stages].set(c);
        }
        validate_reqs(reqs);
    }

    inline bitset_cpu_set stage_cpus(int stageno) {
        return cpus_per_stage[stageno];
    }

private:

    // Assert that reqs requires exactly as many cores as we have available
    // FIXME: replace with consistency check that also checks cpus_per_stage
    inline void validate_reqs(const requirements& reqs) {
        int core_sum = 0;
        for (int si = 0; si < stages; si++) {
            assert(reqs[si] >= 0);
            core_sum += reqs[si];
        }
        assert(core_sum == cpus);
    }

public:

    // Transition this assignment to an assignment that fulfills
    // the given new_reqs requirements.
    // As many CPUs as possible are left untouched.
    inline void transition_to(const requirements& new_reqs) {

        validate_reqs(new_reqs);

        std::array<int, stage::max_stages> req_delta;
        int delta_total = 0;
        for (int si = 0; si < stages; si++) {
            req_delta[si] = new_reqs[si] - reqs[si];
            delta_total += req_delta[si];
        }
        assert(delta_total == 0); // otherwise, phase 1 did bad assignment or we can't use the algorithm below

        // req_delta[i] > 0: stage i needs cpus
        // req_delta[i] < 0: stage i gives cpus
        for (int si = 0; si < stages; si++) {
            if (req_delta[si] == 0) continue;
            for (int isi = si; isi < stages; isi++) {
                using namespace std;
                auto txcpu_c = min(abs(req_delta[isi]),abs(req_delta[si]));
                if (req_delta[isi] < 0 && req_delta[si] > 0) {
                    req_delta[si] -= txcpu_c;
                    req_delta[isi] += txcpu_c;
                    transfer_cpus(isi, si, txcpu_c);
                    assert(req_delta[isi] <= 0);
                    assert(req_delta[si] >= 0);
                } else if (req_delta[isi] > 0 && req_delta[si] < 0) {
                    req_delta[si] += txcpu_c;
                    req_delta[isi] -= txcpu_c;
                    transfer_cpus(si, isi, txcpu_c);
                    assert(req_delta[isi] >= 0);
                    assert(req_delta[si] <= 0);
                }
            }
            assert(req_delta[si] == 0);
        }
        reqs = new_reqs;
    }

private:
    inline void transfer_cpus(int from_stage, int to_stage, unsigned amount) {
        // FIXME clever bit counting operations on x86
        auto& from_set = cpus_per_stage[from_stage];
        auto& to_set = cpus_per_stage[to_stage];
        for (auto f : from_set) {
            if (amount == 0) break;
            if (!to_set.test_and_set(f)) {
                from_set.reset(f);
            }
            amount--;
        }
        assert(amount == 0);
    }
};

// we don't want to spill the details of _assignment into
// stagesched.h since it is an implementation detail and the
// header is directly used by applications
// FIXME: better encapsulation of policy code
static osv::rcu_ptr<assignment> _assignment;
std::chrono::nanoseconds stage::max_assignment_age(20*1000000); // 20ms
static std::atomic<bool> _assignment_updating;
static std::chrono::time_point<std::chrono::steady_clock> _assignment_creation;
static constexpr float _stage_sizes_expavg_factor = 0.1;
static std::array<float, stage::max_stages> _stage_sizes_expavg;
/**
 * Compute stages' CPU-requirements and update the current
 * CPU assignment, _assignment.
 *
 * Callers must assert that
 * - update_assignment() runs exclusively (for RCU)
 * - context is preemptible (memory allocation)
 *
 **/
void stage::update_assignment()
{

    assert(preemptable()); // we use 'new'

    auto begin = osv::clock::uptime::now();

    auto& a = *_assignment.read_by_owner();
    constexpr float eps = 0.003;

    //
    // PHASE 1: DISTRIBUTE CPUS AMONG STAGES
    //
    // Note: It is acceptable that a stage gets assigned no CPU
    //

    // Fetch all stages' _c_in and cache it locally
    std::array<int, max_stages> stage_sizes;
    std::fill(stage_sizes.begin(), stage_sizes.end(), 0);
    for (int si = 0; si < stages_next; si++) {
        stage_sizes[si] = stages[si]._c_in;
    }

    std::array<float, max_stages> stage_sizes_f;
    float total_stage_load = 0.0;
    for (int si = 0; si < stages_next; si++) {
        static_assert(_stage_sizes_expavg_factor < 1.0);
        stage_sizes_f[si] = _stage_sizes_expavg_factor * (float)stage_sizes[si]
                            + (1.0f - _stage_sizes_expavg_factor) * _stage_sizes_expavg[si];
        _stage_sizes_expavg[si] = stage_sizes_f[si];
        total_stage_load += stage_sizes_f[si];
    }
    if (total_stage_load <= 0.0) {
        return;
    }

    static_assert(max_stages >= 8);

    // Record CPU distribution in reqs (see assignment::validate_reqs)
    // TODO: encapsulate requirements into opaque type
    std::array<int, max_stages> reqs;
    std::fill(reqs.begin(), reqs.begin() + stages_next, 0);

    // Distribute CPUs using stage_priorities
    std::array<float, max_stages> sp;
    // First round of priorities is proportional to _c_in
    float sp_total = 0.0;
    for (int si = 0; si < stages_next; si++) {
        sp[si] = stage_sizes_f[si] / total_stage_load;
        sp_total += sp[si];
    }
    assert(sp_total <= 1.0 + eps);

    int cpus_left = cpus.size();
    while (cpus_left > 0) {

        // Try to use sp as is or drive priorities toward a winner
        std::array<float, max_stages> remainders;
        int cpus_assigned;
        float total_remainders;
        int number_of_priority_redistrs = 0;
        while (true) {
            cpus_assigned = 0;
            total_remainders = 0.0;
            for (int si = 0; si < stages_next; si++) {
                auto cpus_fp = cpus_left * sp[si];
                int cpus = std::floor(cpus_fp);
                assert(cpus >= 0);
                remainders[si] = cpus_fp - cpus;
                assert(remainders[si] >= 0.0);
                total_remainders += remainders[si];
                reqs[si] += cpus;
                cpus_assigned += cpus;
            }
            assert(cpus_assigned >= 0);
            if (cpus_assigned > 0) {
                break;
            }
            // At this point, no single stage has sufficiently more priority over the others
            // to win at least one CPU.
            // => Perform rebalancing by giving the lowest-priority stage's priority to the
            // hightest-priority stage. This drives us toward a winner.
            // NOTE: Refrain from the optimization to pick a single winner and give it all
            //       other stages priorities directly: while this makes sense if cpus_left == 1,
            //       all other situations (cpus_left > 1) may be resolved more fairly by doing
            //       the rebalancing iteratively.
            //       Example: cpus_left = 2, sp = {1/4, 1/4, 1/4, 1/4}
            //                => We could rebalance sp' = {1/2, 1/2, 0, 0} and have a fairer
            //                   outcome than sp' = {1, 0, 0, 0}
            // TODO: validate this code does the above

            // TODO actually necessary?
            // think it's a leftover of max_idx == min_idx, but we handle that now
            assert(stages_next >= 2);
            // Find leftmost max
            int max_idx = 0;
            for (int si = max_idx+1; si < stages_next; si++) {
                max_idx = sp[si] > sp[max_idx] ? si : max_idx;
            }
            // Find rightmost non-0 min
            int min_idx = stages_next-1;
            for (int si = min_idx-1; si >= 0; si--) {
                min_idx = sp[min_idx] == 0.0 || (sp[si] != 0.0 && sp[si] < sp[min_idx])
                    ? si : min_idx;
            }
            if (min_idx == max_idx) {
                // The aforementioned iterative redistribution failed.
                // assert: all other elements in sp in stages_next range are 0
                assert(cpus_left == 1);
                assert(sp[max_idx] + eps > 1.0);
                reqs[max_idx] += 1;
                cpus_assigned++;
                break;
            }
            sp[max_idx] += sp[min_idx];
            sp[min_idx] = 0.0;
            number_of_priority_redistrs++;
        }
        // Loop invariant:
        assert(cpus_assigned > 0);
        assert(cpus_assigned <= cpus_left);

        // Because we can't split CPUs, the remainders are the priority
        // when distributing the remaining CPUs
        for (int si = 0; si < stages_next; si++) {
            sp[si] = remainders[si] / total_remainders;
        }

        cpus_left -= cpus_assigned;
        assert(cpus_left >= 0);
    }
    // Loop invariant:
    assert(cpus_left == 0);

    //
    // PHASE 2: FIND NEW ASSIGNMENT WITH MINIMAL TRANSITION COST
    //

    auto na = new assignment(a);
    na->transition_to(reqs);

    auto updater_time = osv::clock::uptime::now() - begin;

    trace_sched_stage_update_assignment(cpu::current()->id, updater_time.count(),
            stage_sizes[0],
            stage_sizes[1],
            stage_sizes[2],
            stage_sizes[3],
            stage_sizes[4],
            stage_sizes[5],
            stage_sizes[6],
            stage_sizes[7],
            na->stage_cpus(0).to_ulong(),
            na->stage_cpus(1).to_ulong(),
            na->stage_cpus(2).to_ulong(),
            na->stage_cpus(3).to_ulong()
            );

    //
    // PHASE 3: USE NEW ASSIGNMENT
    //

    _assignment.assign(na);
    rcu_dispose(&a);
}

stage stage::stages[stage::max_stages];
mutex stage::stages_mtx;
int stage::stages_next = 0;

stage* stage::define(const std::string name) {

    std::lock_guard<mutex> guard(stages_mtx);

    if (stages_next == max_stages)
        return nullptr;

    auto& next = stages[stages_next];
    next._id = stages_next;
    stages_next++;
    next._name = name;

    // Must not create stages after using stage::enqueue because
    // - update_assignment() does not lock stages_mtx before accessing stages_next
    // - class assignment can't handle changing stage count
    // FIXME above

    // FIXME: technically, above assertion does not protect us from another
    // FIXME: thread starting to use _assignment via stage::enqueue, so there's
    // FIXME: a race ... however, all apps converted to stagesched define
    // FIXME: all stages before they use them, so this will only be a problem
    // FIXME: if multiple apps use stages.
    auto ca = _assignment.read_by_owner();
    auto da = new assignment(cpus.size(), stages_next);
    _assignment.assign(da);
    _assignment_creation = std::chrono::steady_clock::now();
    rcu_dispose(ca);

    return &next;
}

int stage::fixed_cpus_per_stage = 0;

cpu *stage::enqueue_policy() {

    // Fixed assignment?
    if (fixed_cpus_per_stage) {
        bitset_cpu_set acpus;
        acpus.reset();
        acpus.set(fixed_cpus_per_stage * _id + 0);
        acpus.set(fixed_cpus_per_stage * _id + 1);
        auto least_busy = *std::min_element(acpus.begin(), acpus.end(),
                [](unsigned a, unsigned b){
            return sched::cpus[a]->runqueue.size() < sched::cpus[b]->runqueue.size();
        });
        return sched::cpus[least_busy];
    }

    // Use existing assignment for ca. max_assignment_age enqueue operations
    // RCU ensures other CPUs can use the old assignment while we compute the update
    std::chrono::nanoseconds assignment_age = std::chrono::steady_clock::now() - _assignment_creation;
    auto can_updater = preemptable() && // preemptable required by update_assignment()
         assignment_age > max_assignment_age;
    auto already_updating = false;
    auto is_updater = can_updater && _assignment_updating.compare_exchange_strong(already_updating, true);
    if (is_updater) {
        // no need for mutex, we are the only updater (see above)
        update_assignment();
        // make sure updated assignment is propagated before we reset the counter
        barrier(); // TODO unsure if necessary, mutex has a barrier() in unlock()
        // restart aging after we collected the statistics
        _assignment_creation = std::chrono::steady_clock::now();
        _assignment_updating.store(false);
    }

    bitset_cpu_set acpus;
    WITH_LOCK(rcu_read_lock) {
        auto ap =_assignment.read();
        assert(ap != nullptr);
        acpus = ap->stage_cpus(this->_id);
    }

    if (!acpus) {
        // This should be a rare case: this stage is so irrelevant that
        // it has not been assigned any dedicated CPU.
        // => Use CPUs round-robin.
        // TODO: evaluate against alternative of using CPU with shortest runqueue, see below
        //static std::atomic<int> victimcpu;
        //return sched::cpus[(victimcpu++)%sched::cpus.size()];
        return sched::cpus[cpus.size()-1];
    }
    auto least_busy = *std::min_element(acpus.begin(), acpus.end(),
            [](unsigned a, unsigned b){
        return sched::cpus[a]->runqueue.size() < sched::cpus[b]->runqueue.size();
    });
    return sched::cpus[least_busy];
}


void stage::enqueue()
{
    cpu *target_cpu = enqueue_policy();
    assert(target_cpu);

    /* prohibit migration of this thread off this cpu */
    irq_save_lock_type irq_lock;
    std::lock_guard<irq_save_lock_type> guard(irq_lock);

    cpu *source_cpu = cpu::current();
    thread *t = thread::current();

    trace_sched_stage_enqueue(this, source_cpu->id, target_cpu->id, t);

    // must be called from migratable context)
    assert(t->migratable());
    // must be called from a thread executing on a CPU
    assert(t->_runqueue_link.is_linked() == false);
    //must be called from a runnable thread
    auto& st = t->_detached_state->st;
    auto st_before = thread::status::running;
    auto st_cmpxchg_success = st.compare_exchange_strong(st_before, thread::status::stagemig_run);
    assert(st_cmpxchg_success);

    auto& ds_stage = t->_detached_state->_stage;
    if (ds_stage) {
        ds_stage->_c_in--;
    }
    ds_stage = this;
    // to reschedule_from_interrupt, this operation will look like we are scheduling out,
    // hence it will decrement the _c_in of the target stage (this) instead of the previous stage
    // (which we did above).
    this->_c_in++;

    if (target_cpu->id == source_cpu->id) {
        st.store(thread::status::running);
        source_cpu->reschedule_from_interrupt(); // releases guard
        return;
    }

    /* status::stagemig_run prohibits target_cpu from executing current thread
       which is critical because we are still executing it right now on this cpu */

    /* thread migration code adopted + extended from thread::pin */
    t->stat_migrations.incr();
    t->suspend_timers();
    t->_detached_state->_cpu = target_cpu;
    percpu_base = target_cpu->percpu_base;
    current_cpu = target_cpu;

    // enqueue as late as possible to minimize the time c is in status::stagemig
    // but target_cpu->stagesched_incoming avoid target_cpu
    target_cpu->stagesched_incoming.push(t);
    target_cpu->incoming_wakeups_mask.set(source_cpu->id);

    /* find another thread to run on source_cpu and make sure that c is marked
     * runnable once source_cpu doesn't execute it anymore so that target_cpu
     * stops re-enqueuing it to its stagesched_incoming
     */
    source_cpu->reschedule_from_interrupt(); // releases guard

    /* from here on, the calling thread is in target_cpu->stagesched_incoming
       or already in target_cpu->runqueue */
}

void stage::dequeue()
{
    /* cannot dequeue during stage migration because current_cpu has already been changed in stage::enqueue */
    assert(thread::current()->_detached_state->st.load() != thread::status::stagemig_run);

    /* prohibit migration of this thread off this cpu while dequeuing */
    irq_save_lock_type irq_lock;
    std::lock_guard<irq_save_lock_type> guard(irq_lock);

    auto inq = &cpu::current()->stagesched_incoming;

    /* fully drain inq
     * FIXME the runtime of the loop is unbounded.
     *       can only fix this once do_idle uses mwait */

    // FIXME (1) the runtime of this loop is unbounded
    //
    // FIXME (2) busy-waiting costs time which could be used to perform the
    // FIXME     remaining dequeues
    // FIXME since we dequeued t, we could have a queue local to this function
    // FIXME that accumulates all still-running threads and checks on them after
    // FIXME handling all the other ones
    // FIXME use the boost::intrusive links for that
    thread *t;
    while ((t = inq->pop()) != nullptr) {
        auto& st = t->_detached_state->st;
        while(true) {
            /* This situation is unlikely:
             * t's source CPU has not completed the context switch yet.
             * The source_cpu is likely somewhere between stagesched_incoming.push() and
             * thread::switch_to's */
            auto st_before = thread::status::stagemig_sto;
            if (st.compare_exchange_strong(st_before, thread::status::queued)) {
                break;
            }
            trace_sched_stage_dequeue_stagemig(cpu::current()->id, t);
            assert(st_before == thread::status::stagemig_run);
        }
        assert(t->_detached_state->_cpu == cpu::current());
        trace_sched_stage_dequeue(cpu::current()->id, t);
        cpu::current()->enqueue(*t);
        if (t->_detached_state->_stage) {
            t->_detached_state->_stage->_c_in++;
        }
        t->resume_timers(cpu::current());
    }

}


// function to pin the *current* thread:
void thread::pin(cpu *target_cpu)
{
    // Note that this code may proceed to migrate the current thread even if
    // it was protected by a migrate_disable(). It is the thread's own fault
    // for doing this to itself...
    thread &t = *current();
    if (!t._pinned) {
        // _pinned comes with a +1 increase to _migration_counter.
        migrate_disable();
        t._pinned = true;
    }
    cpu *source_cpu = cpu::current();
    if (source_cpu == target_cpu) {
        return;
    }
    // We want to wake this thread on the target CPU, but can't do this while
    // it is still running on this CPU. So we need a different thread to
    // complete the wakeup. We could re-used an existing thread (e.g., the
    // load balancer thread) but a "good-enough" dirty solution is to
    // temporarily create a new ad-hoc thread, "wakeme".
    bool do_wakeme = false;
    std::unique_ptr<thread> wakeme(thread::make([&] () {
        wait_until([&] { return do_wakeme; });
        t.wake();
    }, sched::thread::attr().pin(source_cpu)));
    wakeme->start();
    WITH_LOCK(irq_lock) {
        trace_sched_migrate(&t, target_cpu->id);
        t.stat_migrations.incr();
        t.suspend_timers();
        t._detached_state->_cpu = target_cpu;
        percpu_base = target_cpu->percpu_base;
        current_cpu = target_cpu;
        t._detached_state->st.store(thread::status::waiting_run);
        // Note that wakeme is on the same CPU, and irq is disabled,
        // so it will not actually run until we stop running.
        wakeme->wake_with([&] { do_wakeme = true; });
        source_cpu->reschedule_from_interrupt();
    }
    // wakeme will be implicitly join()ed here.
}

void thread::unpin()
{
    // Unpinning the current thread is straightforward. But to work on a
    // different thread safely, without risking races with concurrent attempts
    // to pin, unpin, or migrate the same thread, we need to run the actual
    // unpinning code on the same CPU as the target thread.
    if (this == current()) {
        WITH_LOCK(preempt_lock) {
            if (_pinned) {
                _pinned = false;
                 std::atomic_signal_fence(std::memory_order_release);
                _migration_lock_counter--;
            }
        }
        return;
    }
    std::unique_ptr<thread> helper(thread::make([this] {
        WITH_LOCK(preempt_lock) {
            // helper thread started on the same CPU as "this", but by now
            // "this" might migrated. If that happened helper need to migrate.
            while (sched::cpu::current() != this->tcpu()) {
                DROP_LOCK(preempt_lock) {
                    thread::pin(this->tcpu());
                }
            }
            if (_pinned) {
                _pinned = false;
                 std::atomic_signal_fence(std::memory_order_release);
                _migration_lock_counter--;
            }
        }
    }, sched::thread::attr().pin(tcpu())));
    helper->start();
    helper->join();
}

void cpu::bring_up() {
    notifier::fire();
}

cpu::notifier::notifier(std::function<void ()> cpu_up)
    : _cpu_up(cpu_up)
{
    WITH_LOCK(_mtx) {
        _notifiers.push_back(this);
    }
}

cpu::notifier::~notifier()
{
    WITH_LOCK(_mtx) {
        _notifiers.remove(this);
    }
}

void cpu::notifier::fire()
{
    WITH_LOCK(_mtx) {
        for (auto n : _notifiers) {
            n->_cpu_up();
        }
    }
}

void thread::yield(thread_runtime::duration preempt_after)
{
    trace_sched_yield();
    auto t = current();
    std::lock_guard<irq_lock_type> guard(irq_lock);
    // FIXME: drive by IPI
    cpu::current()->handle_incoming_wakeups();
    // FIXME: what about other cpus?
    if (cpu::current()->runqueue.empty()) {
        return;
    }
    assert(t->_detached_state->st.load() == status::running);
    // Do not yield to a thread with idle priority
    thread &tnext = *(cpu::current()->runqueue.begin());
    if (tnext.priority() == thread::priority_idle) {
        return;
    }
    trace_sched_yield_switch();

    cpu::current()->reschedule_from_interrupt();
}

void thread::set_priority(float priority)
{
    // NOOP
}

float thread::priority() const
{
    return priority_default;
}

sched::thread::status thread::get_status() const
{
    return _detached_state->st.load(std::memory_order_relaxed);
}

thread::stack_info::stack_info()
    : begin(nullptr), size(0), deleter(nullptr)
{
}

thread::stack_info::stack_info(void* _begin, size_t _size)
    : begin(_begin), size(_size), deleter(nullptr)
{
    auto end = align_down(begin + size, 16);
    size = static_cast<char*>(end) - static_cast<char*>(begin);
}

void thread::stack_info::default_deleter(thread::stack_info si)
{
    free(si.begin);
}

// thread_map is used for a list of all threads, but also as a map from
// numeric (4-byte) threads ids to the thread object, to support Linux
// functions which take numeric thread ids.
static mutex thread_map_mutex;
using id_type = std::result_of<decltype(&thread::id)(thread)>::type;
std::unordered_map<id_type, thread *> thread_map
    __attribute__((init_priority((int)init_prio::threadlist)));

static thread_runtime::duration total_app_time_exited(0);

thread_runtime::duration thread::thread_clock() {
    if (this == current()) {
        WITH_LOCK (preempt_lock) {
            // Inside preempt_lock, we are running and the scheduler can't
            // intervene and change _total_cpu_time or _running_since
            return _total_cpu_time +
                    (osv::clock::uptime::now() - tcpu()->running_since);
        }
    } else {
        auto status = _detached_state->st.load(std::memory_order_acquire);
        if (status == thread::status::running) {
            // The cputime_estimator set before the status is already visible.
            // Even if the thread stops running now, cputime_estimator will
            // remain; Our max overshoot will be the duration of this code.
            osv::clock::uptime::time_point running_since;
            osv::clock::uptime::duration total_cpu_time;
            cputime_estimator_get(running_since, total_cpu_time);
            return total_cpu_time +
                    (osv::clock::uptime::now() - running_since);
        } else {
            // _total_cpu_time is set before setting status, so it is already
            // visible. During this code, the thread might start running, but
            // it doesn't matter, total_cpu_time will remain. Our maximum
            // undershoot will be the duration that this code runs.
            // FIXME: we assume reads/writes to _total_cpu_time are atomic.
            // They are, but we should use std::atomic to guarantee that.
            return _total_cpu_time;
        }
    }
}

// Return the total amount of cpu time used by the process. This is the amount
// of time that passed since boot multiplied by the number of CPUs, from which
// we subtract the time spent in the idle threads.
// Besides the idle thread, we do not currently account for "steal time",
// i.e., time in which the hypervisor preempted us and ran other things.
// In other words, when a hypervisor gives us only a part of a CPU, we pretend
// it is still a full CPU, just a slower one. Ordinary CPUs behave similarly
// when faced with variable-speed CPUs.
osv::clock::uptime::duration process_cputime()
{
    // FIXME: This code does not handle the possibility of CPU hot-plugging.
    // See issue #152 for a suggested solution.
    auto ret = osv::clock::uptime::now().time_since_epoch();
    ret *= sched::cpus.size();
    for (sched::cpu *cpu : sched::cpus) {
        ret -= cpu->idle_thread->thread_clock();
    }
    // idle_thread->thread_clock() may make tiny (<microsecond) temporary
    // mistakes when racing with the idle thread's starting or stopping.
    // To ensure that process_cputime() remains monotonous, we monotonize it.
    static std::atomic<osv::clock::uptime::duration> lastret;
    auto l = lastret.load(std::memory_order_relaxed);
    while (ret > l &&
           !lastret.compare_exchange_weak(l, ret, std::memory_order_relaxed));
    if (ret < l) {
        ret = l;
    }
    return ret;
}

std::chrono::nanoseconds osv_run_stats()
{
    thread_runtime::duration total_app_time;

    WITH_LOCK(thread_map_mutex) {
        total_app_time = total_app_time_exited;
        for (auto th : thread_map) {
            thread *t = th.second;
            total_app_time += t->thread_clock();
        }
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(total_app_time);
}

int thread::numthreads()
{
    SCOPE_LOCK(thread_map_mutex);
    return thread_map.size();
}

// We reserve a space in the end of the PID space, so we can reuse those
// special purpose ids for other things. 4096 positions is arbitrary, but
// <<should be enough for anybody>> (tm)
constexpr unsigned int tid_max = UINT_MAX - 4096;
unsigned long thread::_s_idgen = 0;

thread *thread::find_by_id(unsigned int id)
{
    auto th = thread_map.find(id);
    if (th == thread_map.end())
        return NULL;
    return (*th).second;
}

void* thread::do_remote_thread_local_var(void* var)
{
    auto tls_cur = static_cast<char*>(current()->_tcb->tls_base);
    auto tls_this = static_cast<char*>(this->_tcb->tls_base);
    auto offset = static_cast<char*>(var) - tls_cur;
    return tls_this + offset;
}

thread::thread(std::function<void ()> func, attr attr, bool main, bool app)
    : _func(func)
    , _detached_state(new detached_state(this))
    , _attr(attr)
    , _migration_lock_counter(0)
    , _pinned(false)
    , _id(0)
    , _cleanup([this] { delete this; })
    , _app(app)
    , _joiner(nullptr)
{
    trace_thread_create(this);

    if (!main && sched::s_current) {
        auto app = application::get_current().get();
        if (override_current_app) {
            app = override_current_app;
        }
        if (_app && app) {
            _app_runtime = app->runtime();
        }
    }
    setup_tcb();
    // module 0 is always the core:
    assert(_tls.size() == elf::program::core_module_index);
    _tls.push_back((char *)_tcb->tls_base);
    if (_app_runtime) {
        auto& offsets = _app_runtime->app.lib()->initial_tls_offsets();
        for (unsigned i = 1; i < offsets.size(); i++) {
            if (!offsets[i]) {
                _tls.push_back(nullptr);
            } else {
                _tls.push_back(reinterpret_cast<char*>(_tcb) + offsets[i]);
            }
        }
    }

    WITH_LOCK(thread_map_mutex) {
        if (!main) {
            auto ttid = _s_idgen;
            auto tid = ttid;
            do {
                tid++;
                if (tid > tid_max) { // wrap around
                    tid = 1;
                }
                if (!find_by_id(tid)) {
                    _s_idgen = _id = tid;
                    thread_map.insert(std::make_pair(_id, this));
                    break;
                }
            } while (tid != ttid); // One full round trip is enough
            if (tid == ttid) {
                abort("Can't allocate a Thread ID");
            }
        }
    }
    // setup s_current before switching to the thread, so interrupts
    // can call thread::current()
    // remote_thread_local_var() doesn't work when there is no current
    // thread, so don't do this for main threads (switch_to_first will
    // do that for us instead)
    if (!main && sched::s_current) {
        remote_thread_local_var(s_current) = this;
    }
    init_stack();

    if (_attr._detached) {
        _detach_state.store(detach_state::detached);
    }

    if (_attr._pinned_cpu) {
        ++_migration_lock_counter;
        _pinned = true;
    }

    if (main) {
        _detached_state->_cpu = attr._pinned_cpu;
        _detached_state->st.store(status::running);
        if (_detached_state->_cpu == sched::cpus[0]) {
            s_current = this;
        }
        remote_thread_local_var(current_cpu) = _detached_state->_cpu;
    }

    // For debugging purposes, it is useful for threads to have names. If no
    // name was set for this one, set one by prepending ">" to parent's name.
    if (!_attr._name[0] && s_current) {
        _attr._name[0] = '>';
        strncpy(_attr._name.data()+1, s_current->_attr._name.data(),
                sizeof(_attr._name) - 2);
    }
}

static std::list<std::function<void ()>> exit_notifiers
        __attribute__((init_priority((int)init_prio::threadlist)));
static rwlock exit_notifiers_lock
        __attribute__((init_priority((int)init_prio::threadlist)));
void thread::register_exit_notifier(std::function<void ()> &&n)
{
    WITH_LOCK(exit_notifiers_lock.for_write()) {
        exit_notifiers.push_front(std::move(n));
    }
}
static void run_exit_notifiers()
{
    WITH_LOCK(exit_notifiers_lock.for_read()) {
        for (auto& notifier : exit_notifiers) {
            notifier();
        }
    }
}

// not in the header to avoid double inclusion between osv/app.hh and
// osv/sched.hh
osv::application *thread::current_app() {
    auto cur = current();

    if (!cur->_app_runtime) {
        return nullptr;
    }

    return &(cur->_app_runtime->app);
}

thread::~thread()
{
    cancel_this_thread_alarm();

    if (!_attr._detached) {
        join();
    }
    WITH_LOCK(thread_map_mutex) {
        thread_map.erase(_id);
        total_app_time_exited += _total_cpu_time;
    }
    if (_attr._stack.deleter) {
        _attr._stack.deleter(_attr._stack);
    }
    for (unsigned i = 1; i < _tls.size(); i++) {
        if (_app_runtime) {
            auto& offsets = _app_runtime->app.lib()->initial_tls_offsets();
            if (i < offsets.size() && offsets[i]) {
                continue;
            }
        }
        delete[] _tls[i];
    }
    free_tcb();
    rcu_dispose(_detached_state.release());
}

void thread::start()
{
    assert(_detached_state->st == status::unstarted);

    if (!sched::s_current) {
        _detached_state->st.store(status::prestarted);
        return;
    }

    _detached_state->_cpu = _attr._pinned_cpu ? _attr._pinned_cpu : current()->tcpu();
    remote_thread_local_var(percpu_base) = _detached_state->_cpu->percpu_base;
    remote_thread_local_var(current_cpu) = _detached_state->_cpu;
    _detached_state->st.store(status::waiting_sto);
    wake();
}

void thread::prepare_wait()
{
    // After setting the thread's status to "waiting_run", we must not preempt it,
    // as it is no longer in "running" state and therefore will not return.
    preempt_disable();
    assert(_detached_state->st.load() == status::running);
    _detached_state->st.store(status::waiting_run);
}

// This function is responsible for changing a thread's state from
// "terminating" to "terminated", while also waking a thread sleeping on
// join(), if any.
// This function cannot be called by the dying thread, because waking its
// joiner usually triggers deletion of the thread and its stack, and it
// must not be running at the same time.
// TODO: rename this function, perhaps to wake_joiner()?
void thread::destroy()
{
    // thread can't destroy() itself, because if it decides to wake joiner,
    // it will delete the stack it is currently running on.
    assert(thread::current() != this);

    assert(_detached_state->st.load(std::memory_order_relaxed) == status::terminating);
    // Solve a race between join() and the thread's completion. If join()
    // manages to set _joiner first, it will sleep and we need to wake it.
    // But if we set _joiner first, join() will never wait.
    sched::thread *joiner = nullptr;
    WITH_LOCK(rcu_read_lock_in_preempt_disabled) {
        auto ds = _detached_state.get();
        // Note we can't set status to "terminated" before the CAS on _joiner:
        // As soon as we set status to terminated, a concurrent join might
        // return and delete the thread, and _joiner will become invalid.
        if (_joiner.compare_exchange_strong(joiner, this)) {
            // In this case, the concurrent join() may have already noticed it
            // lost the race, returned, and the thread "this" may have been
            // deleted. But ds is still valid because of RCU lock.
            ds->st.store(status::terminated);
        } else {
            // The joiner won the race, and will wait. We need to wake it.
            joiner->wake_with([&] { ds->st.store(status::terminated); });
        }
    }
}

// Must be called under rcu_read_lock
//
// allowed_initial_states_mask
//  *must* contain status::waiting_*
//  *may* contain status::sending_lock* (for waitqueue wait morphing)
// It will transition from one of the allowed initial states to the waking state.
void thread::wake_impl(detached_state* st, unsigned allowed_initial_states_mask)
{
    /* Codify the docs above */
    constexpr unsigned possible_states_mask = (1 << unsigned(status::waiting_run))
                                              | (1 << unsigned(status::waiting_sto))
                                              | (1 << unsigned(status::sending_lock_run))
                                              | (1 << unsigned(status::sending_lock_sto));
    assert(allowed_initial_states_mask & (1 << unsigned(status::waiting_run)));
    assert(allowed_initial_states_mask & (1 << unsigned(status::waiting_sto)));
    assert(!(allowed_initial_states_mask & ~possible_states_mask));

    trace_sched_wake(st->t);

    /* Try to catch st->t while it is still going to sleep (not in status::waiting_sto yet)
       LOGICAL ASSERTION: allowed initial states always transition directly to status::waking_run,
                          not to one another */
    status s;
    bool stopped;
    s = status::waiting_run;
    if ((1 << unsigned(s) & allowed_initial_states_mask) &&
            st->st.compare_exchange_strong(s, status::waking_run)) {
        stopped = false;
        goto wakeup;
    }
    barrier(); // TODO necessary? Idea: need ordered check on states because it's their temporal ordering
    s = status::waiting_sto;
    if ((1 << unsigned(s) & allowed_initial_states_mask) &&
            st->st.compare_exchange_strong(s, status::waking_sto)) {
        stopped = true;
        goto wakeup;
    }
    barrier();
    s = status::sending_lock_run;
    if ((1 << unsigned(s) & allowed_initial_states_mask) &&
            st->st.compare_exchange_strong(s, status::waking_run)) {
        stopped = false;
        goto wakeup;
    }
    barrier();
    s = status::sending_lock_sto;
    if ((1 << unsigned(s) & allowed_initial_states_mask) &&
            st->st.compare_exchange_strong(s, status::waking_sto)) {
        stopped = true;
        goto wakeup;
    }

    /* st->t was either status::waking_sto or it was already woken up by another CPU */
    return;
wakeup:
    /* we are responsible for migrating st-> to its target CPU */
    WITH_LOCK(preempt_lock_in_rcu) {
        // we can now use st->t here, since the thread cannot terminate while
        // it's waking, but not afterwards, when it may be running

        cpu *tcpu = st->_cpu;
        if (stopped && st->_stage && st->t->migratable()) {
            assert(st->t != thread::current());
            assert(st->t->_runqueue_link.is_linked() == false);
            tcpu = st->_stage->enqueue_policy();
            if (tcpu != st->_cpu) {
                irq_save_lock_type irq_lock;
                WITH_LOCK(irq_lock) {
                    // This is remote thread migration, i.e. we are CPU A
                    // and migrate previously waiting st->t on CPU B to CPU C
                    trace_sched_migrate(st->t, tcpu->id);
                    st->t->stat_migrations.incr();
                    st->t->suspend_timers();
                    st->_cpu = tcpu;
                    st->t->remote_thread_local_var(::percpu_base) = tcpu->percpu_base;
                    st->t->remote_thread_local_var(current_cpu) = tcpu;
                }
            }
        }

        unsigned c = cpu::current()->id;
        irq_save_lock_type irq_lock;
        WITH_LOCK(irq_lock) {
            tcpu->incoming_wakeups[c].push_back(*st->t);
        }
        // Notify tcpu of the wakeup
        if (!tcpu->incoming_wakeups_mask.test_all_and_set(c)) {
            if (tcpu == current()->tcpu()) {
                need_reschedule = true;
            } else {
                // No need for IPIs, handle_incoming_wakups pools incoming_wakups_mask
            }
        }
    }
}

void thread::wake()
{
    WITH_LOCK(rcu_read_lock) {
        wake_impl(_detached_state.get());
    }
}

void thread::wake_lock(mutex* mtx, wait_record* wr)
{
    // must be called with mtx held
    WITH_LOCK(rcu_read_lock) {
        auto st = _detached_state.get();
        // We want to send_lock() to this thread, but we want to be sure we're the only
        // ones doing it, and that it doesn't wake up while we do
        auto expected = status::waiting_run;
        auto from_pre = false;
        if (st->st.compare_exchange_strong(expected, status::sending_lock_run)) {
            from_pre = true;
            goto cont;
        }
        barrier();
        expected = status::waiting_sto;
        if (st->st.compare_exchange_strong(expected, status::sending_lock_sto)) {
            goto cont;
        }
        // make sure the thread can see wr->woken() == true.  We're still protected by
        // the mutex, so so need for extra protection
        wr->clear();
        // let the thread acquire the lock itself
        return;

cont:

        // Send the lock to the thread, unless someone else already woke the us up,
        // and we're sleeping in mutex::lock().
        if (mtx->send_lock_unless_already_waiting(wr)) {
            st->lock_sent = true;
        } else {
            // revert to previous state
            status expected;
            if (from_pre) {
                expected = status::sending_lock_run;
                if (st->st.compare_exchange_strong(expected, status::waiting_run))
                    goto clear;
            }
            barrier();
            // must have scheduled out in the meantime
            assert(st->st.load() == status::sending_lock_sto);
            expected = status::sending_lock_sto;
            st->st.compare_exchange_strong(expected, status::waiting_sto); // load should suffice?
clear:
            wr->clear();
        }
        // since we're in status::sending_lock_run, no one can wake us except mutex::unlock
    }
}

bool thread::unsafe_stop()
{
    WITH_LOCK(rcu_read_lock) {
        auto st = _detached_state.get();
        auto expected = status::waiting_sto;
        return st->st.compare_exchange_strong(expected,
                status::terminated, std::memory_order_relaxed)
                || expected == status::terminated;
    }
}


void thread::main()
{
    _func();
}

void thread::wait()
{
    trace_sched_wait();
    cpu::schedule();
    trace_sched_wait_ret();
}

void thread::stop_wait()
{
    // General Note:
    //
    // We can only re-enable preemption of this thread after it is no longer
    // in "waiting_*" state (otherwise if preempted, it will not be scheduled
    // in again - this is why we disabled preemption in prepare_wait.
    //
    // A post-condition of this function must thus be that we are status::running

    // Check if we are just going to sleep and a predicate became true before we scheduled out
    auto& st = _detached_state->st;
    status old_status = status::waiting_run;
    if (st.compare_exchange_strong(old_status, status::running)) {
        preempt_enable();
        return;
    }

    // An asynchronous event must have occurred and changed our st->st to a state of their own.
    // Now we wait until it completes whatever it is doing and makes us run again.

    preempt_enable();

    // Were we terminated?
    if (old_status == status::terminated) {
        // We raced with thread::unsafe_stop() and lost
        cpu::schedule();
        assert(false); // will not return from here
    }

    while (true) {
        auto status = st.load();
        switch (status) {
            /* We ruled that out at the beginning of the function */
            case status::waiting_run:       while(true); // for debugging...

            /* Rule out all the states we can't be in while we execute stop_wait() */
            case status::waiting_sto:       assert(false);
            case status::waking_sto:        assert(false);
            case status::sending_lock_sto:  assert(false);
            case status::stagemig_sto:      assert(false);
            case status::terminating:       assert(false);
            case status::terminated:        assert(false);
            case status::queued:            assert(false);
            case status::unstarted:         assert(false);
            case status::prestarted:        assert(false);
            case status::invalid:           assert(false);
                while(true); // for debugging...

            /* Wait for the async event to complete what it is doing. */
            case status::sending_lock_run:
            case status::stagemig_run:
            // waking_run is completed by cpu::schedule and subsequent
            // cpu::handle_incoming_wakeups without ever going to sleep
            case status::waking_run:
                cpu::schedule();
                break;

            /* Only leave when we are running */
            case status::running:
                goto out;
        }
    }
out:
    assert(st.load() == status::running);
}

void thread::complete()
{
    run_exit_notifiers();

    auto value = detach_state::attached;
    _detach_state.compare_exchange_strong(value, detach_state::attached_complete);
    if (value == detach_state::detached) {
        _s_reaper->add_zombie(this);
    }
    // If this thread gets preempted after changing status it will never be
    // scheduled again to set terminating_thread. So must disable preemption.
    preempt_disable();
    _detached_state->st.store(status::terminating);
    // We want to run destroy() here, but can't because it cause the stack we're
    // running on to be deleted. Instead, set a _cpu field telling the next
    // thread running on this cpu to do the unref() for us.
    if (_detached_state->_cpu->terminating_thread) {
        assert(_detached_state->_cpu->terminating_thread != this);
        _detached_state->_cpu->terminating_thread->destroy();
    }
    _detached_state->_cpu->terminating_thread = this;
    // The thread is now in the "terminating" state, so on call to schedule()
    // it will never get to run again.
    while (true) {
        cpu::schedule();
    }
}

/*
 * Exit a thread.  Doesn't unwind any C++ ressources, and should
 * only be used to implement higher level threading abstractions.
 */
void thread::exit()
{
    thread* t = current();

    t->complete();
}

void thread::suspend_timers()
{
    std::lock_guard<rspinlock> lg_t(_timer_client_lock);
    if (_timers_need_reload) {
        return;
    }
    _timers_need_reload = true;

    cpu *c = _detached_state->_cpu;
    assert(c);
    assert(cpu::current() == c || _detached_state->st.load() == status::waking_sto);
    std::lock_guard<rspinlock> lg_c(c->_timer_client_lock);
    c->timers.suspend(_active_timers);
}

// call with IRQs disabled
void timer_base::client::suspend_timers()
{
    std::lock_guard<rspinlock> lg_x(_timer_client_lock);
    if (_timers_need_reload) {
        return;
    }
    _timers_need_reload = true;
    std::lock_guard<rspinlock> lg_c(cpu::current()->_timer_client_lock);
    cpu::current()->timers.suspend(_active_timers);
}

void timer_base::client::resume_timers(cpu *oncpu)
{
    std::lock_guard<rspinlock> lg(_timer_client_lock);
    if (!_timers_need_reload) {
        return;
    }
    _timers_need_reload = false;
    std::lock_guard<rspinlock> lg_c(oncpu->_timer_client_lock);
    oncpu->timers.resume(_active_timers);
}

void thread::join()
{
    auto& st = _detached_state->st;
    if (st.load() == status::unstarted) {
        // To allow destruction of a thread object before start().
        return;
    }
    sched::thread *old_joiner = nullptr;
    if (!_joiner.compare_exchange_strong(old_joiner, current())) {
        // The thread is concurrently completing and took _joiner in destroy().
        // At this point we know that destroy() will no longer use 'this', so
        // it's fine to return and for our caller to delete the thread.
        return;
    }
    wait_until([&] { return st.load() == status::terminated; });
}

void thread::detach()
{
    _attr._detached = true;
    auto value = detach_state::attached;
    _detach_state.compare_exchange_strong(value, detach_state::detached);
    if (value == detach_state::attached_complete) {
        // Complete was called prior to our call to detach. If we
        // don't add ourselves to the reaper now, nobody will.
        _s_reaper->add_zombie(this);
    }
}

thread::stack_info thread::get_stack_info()
{
    return _attr._stack;
}

void thread::set_cleanup(std::function<void ()> cleanup)
{
    assert(_detached_state->st == status::unstarted);
    _cleanup = cleanup;
}

void thread::timer_fired()
{
    wake();
}

unsigned int thread::id() const
{
    return _id;
}

void thread::set_name(std::string name)
{
    _attr.name(name);
}

std::string thread::name() const
{
    return _attr._name.data();
}

void* thread::setup_tls(ulong module, const void* tls_template,
        size_t init_size, size_t uninit_size)
{
    _tls.resize(std::max(module + 1, _tls.size()));
    _tls[module]  = new char[init_size + uninit_size];
    auto p = _tls[module];
    memcpy(p, tls_template, init_size);
    memset(p + init_size, 0, uninit_size);
    return p;
}

void thread::sleep_impl(timer &t)
{
    wait_until([&] { return t.expired(); });
}

void thread_handle::wake()
{
    WITH_LOCK(rcu_read_lock) {
        thread::detached_state* ds = _t.read();
        if (ds) {
            thread::wake_impl(ds);
        }
    }
}

timer_list::callback_dispatch::callback_dispatch()
{
    clock_event->set_callback(this);
}

void timer_list::fired()
{
    auto now = osv::clock::uptime::now();
 again:
    _last = osv::clock::uptime::time_point::max();
    _list.expire(now);
    timer_base* timer;
    while ((timer = _list.pop_expired())) {
        assert(timer->_state == timer_base::state::armed);
        timer->expire();
    }
    if (!_list.empty()) {
        // We could have simply called rearm() here, but this would lead to
        // recursion if the next timer has already expired in the time that
        // passed above. Better iterate in that case, instead.
        now = osv::clock::uptime::now();
        auto t = _list.get_next_timeout();
        if (t <= now) {
            goto again;
        } else {
            _last = t;
            clock_event->set(t - now);
        }
    }
}

void timer_list::rearm()
{
    auto t = _list.get_next_timeout();
    if (t < _last) {
        _last = t;
        clock_event->set(t - osv::clock::uptime::now());
    }
}

// call with irq disabled
void timer_list::suspend(timer_base::client_list_t& timers)
{
    for (auto& t : timers) {
        assert(t._state == timer::state::armed);
        _list.remove(t);
    }
}

// call with irq disabled
void timer_list::resume(timer_base::client_list_t& timers)
{
    bool do_rearm = false;
    for (auto& t : timers) {
        assert(t._state == timer::state::armed);
        do_rearm |= _list.insert(t);
    }
    if (do_rearm) {
        rearm();
    }
}

void timer_list::callback_dispatch::fired()
{
    std::lock_guard<rspinlock> lg(cpu::current()->_timer_client_lock);
    cpu::current()->timers.fired();
}

timer_list::callback_dispatch timer_list::_dispatch;

timer_base::timer_base(timer_base::client& t)
    : _t(t)
{
}

timer_base::~timer_base()
{
    cancel();
}

void timer_base::expire()
{
    trace_timer_fired(this);
    _state = state::expired;
    std::lock_guard<rspinlock> lg(_t._timer_client_lock);
    _t._active_timers.erase(_t._active_timers.iterator_to(*this));
    _t.timer_fired();
}

void timer_base::set(osv::clock::uptime::time_point time)
{
    trace_timer_set(this, time.time_since_epoch().count());
    irq_save_lock_type irq_lock;
    WITH_LOCK(irq_lock) {
        _state = state::armed;
        _time = time;

        std::lock_guard<rspinlock> lg_t(_t._timer_client_lock);
        std::lock_guard<rspinlock> lg_c(cpu::current()->_timer_client_lock);
        auto& timers = cpu::current()->timers;
        _t._active_timers.push_back(*this);
        if (timers._list.insert(*this)) {
            timers.rearm();
        }
    }
};

void timer_base::cancel()
{
    if (_state == state::free) {
        return;
    }
    trace_timer_cancel(this);
    irq_save_lock_type irq_lock;
    WITH_LOCK(irq_lock) {
        if (_state == state::armed) {
            std::lock_guard<rspinlock> lg_t(_t._timer_client_lock);
            _t._active_timers.erase(_t._active_timers.iterator_to(*this));
            std::lock_guard<rspinlock> lg_c(cpu::current()->_timer_client_lock);
            cpu::current()->timers._list.remove(*this);
        }
        _state = state::free;
    }
    // even if we remove the first timer, allow it to expire rather than
    // reprogramming the timer
}

void timer_base::reset(osv::clock::uptime::time_point time)
{
    trace_timer_reset(this, time.time_since_epoch().count());


    irq_save_lock_type irq_lock;
    WITH_LOCK(irq_lock) {

        std::lock_guard<rspinlock> lg_c(cpu::current()->_timer_client_lock);
        auto& timers = cpu::current()->timers;

        if (_state == state::armed) {
            timers._list.remove(*this);
        } else {
            std::lock_guard<rspinlock> lg_t(_t._timer_client_lock);
            _t._active_timers.push_back(*this);
            _state = state::armed;
        }

        _time = time;

        if (timers._list.insert(*this)) {
            timers.rearm();
        }
    }
};

bool timer_base::expired() const
{
    return _state == state::expired;
}

bool operator<(const timer_base& t1, const timer_base& t2)
{
    if (t1._time < t2._time) {
        return true;
    } else if (t1._time == t2._time) {
        return &t1 < &t2;
    } else {
        return false;
    }
}

thread::reaper::reaper()
    : _mtx{}, _zombies{}, _thread(thread::make([=] { reap(); }))
{
    _thread->start();
}

void thread::reaper::reap()
{
    while (true) {
        WITH_LOCK(_mtx) {
            wait_until(_mtx, [=] { return !_zombies.empty(); });
            while (!_zombies.empty()) {
                auto z = _zombies.front();
                _zombies.pop_front();
                z->join();
                z->_cleanup();
            }
        }
    }
}

void thread::reaper::add_zombie(thread* z)
{
    assert(z->_attr._detached);
    WITH_LOCK(_mtx) {
        _zombies.push_back(z);
        _thread->wake();
    }
}

thread::reaper *thread::_s_reaper;

void init_detached_threads_reaper()
{
    thread::_s_reaper = new thread::reaper;
}

void start_early_threads()
{
    // We're called from the idle thread, which must not sleep, hence this
    // strange try_lock() loop instead of just a lock().
    while (!thread_map_mutex.try_lock()) {
        cpu::schedule();
    }
    SCOPE_ADOPT_LOCK(thread_map_mutex);
    for (auto th : thread_map) {
        thread *t = th.second;
        if (t == sched::thread::current()) {
            continue;
        }
        t->remote_thread_local_var(s_current) = t;
        thread::status expected = thread::status::prestarted;
        if (t->_detached_state->st.compare_exchange_strong(expected,
                thread::status::unstarted, std::memory_order_relaxed)) {
            t->start();
        }
    }
}

void init(std::function<void ()> cont)
{
    thread::attr attr;
    attr.stack(4096*10).pin(smp_initial_find_current_cpu());
    attr.name("init");
    thread t{cont, attr, true};
    t.switch_to_first();
}

void init_tls(elf::tls_data tls_data)
{
    tls = tls_data;
}

size_t kernel_tls_size()
{
    return tls.size;
}

void with_all_threads(std::function<void(thread &)> f) {
    WITH_LOCK(thread_map_mutex) {
        for (auto th : thread_map) {
            f(*th.second);
        }
    }
}

void with_thread_by_id(unsigned id, std::function<void(thread *)> f) {
    WITH_LOCK(thread_map_mutex) {
        f(thread::find_by_id(id));
    }
}


}

irq_lock_type irq_lock;
