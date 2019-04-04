// Copyright 2018, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include <boost/asio/steady_timer.hpp>
#include <boost/fiber/mutex.hpp>
#include <boost/fiber/operations.hpp>
#include <boost/fiber/scheduler.hpp>

#include "base/logging.h"
#include "util/asio/io_context.h"

namespace util {

using fibers_ext::BlockingCounter;
using namespace boost;
using namespace std;

namespace {
constexpr unsigned MAIN_NICE_LEVEL = 0;

// Amount of fiber switches we make before bringing back the main IO loop.
constexpr unsigned MAIN_SWITCH_LIMIT = 4;

thread_local unsigned main_resumes = 0;

class AsioScheduler final : public fibers::algo::algorithm_with_properties<IoFiberProperties> {
 private:
  using ready_queue_type = fibers::scheduler::ready_queue_type;
  std::shared_ptr<asio::io_context> io_context_;
  std::unique_ptr<asio::steady_timer> suspend_timer_;
  //]
  ready_queue_type rqueue_arr_[IoFiberProperties::NUM_NICE_LEVELS + 1];
  fibers::mutex mtx_;

  // it's single threaded and so https://github.com/boostorg/fiber/issues/194 is unlikely to affect
  // here.
  fibers::condition_variable_any cnd_;
  uint32_t last_nice_level_ = 0;
  std::size_t ready_cnt_{0};  // ready *worker* fibers count, i.e. not including dispatcher.
  std::size_t switch_cnt_{0};
  fibers::context* dispatch_ctx_ = nullptr;

  enum : uint8_t { LOOP_RUN_ONE = 1, LOOP_SUSPEND = 2 };
  uint8_t mask_ = 0;

 public:
  //[asio_rr_ctor
  AsioScheduler(const std::shared_ptr<asio::io_context>& io_svc)
      : io_context_(io_svc), suspend_timer_(new asio::steady_timer(*io_svc)) {}

  ~AsioScheduler();

  void awakened(fibers::context* ctx, IoFiberProperties& props) noexcept override;

  fibers::context* pick_next() noexcept override;

  void property_change(boost::fibers::context* ctx, IoFiberProperties& props) noexcept final {
    // Although our priority_props class defines multiple properties, only
    // one of them (priority) actually calls notify() when changed. The
    // point of a property_change() override is to reshuffle the ready
    // queue according to the updated priority value.

    // 'ctx' might not be in our queue at all, if caller is changing the
    // priority of (say) the running fiber. If it's not there, no need to
    // move it: we'll handle it next time it hits awakened().
    if (!ctx->ready_is_linked()) {
      return;
    }

    // Found ctx: unlink it
    ctx->ready_unlink();
    if (!ctx->is_context(fibers::type::dispatcher_context)) {
      DCHECK_GT(ready_cnt_, 0);
      --ready_cnt_;
    }

    // Here we know that ctx was in our ready queue, but we've unlinked
    // it. We happen to have a method that will (re-)add a context* to the
    // right place in the ready queue.
    awakened(ctx, props);
  }

  bool has_ready_fibers() const noexcept override { return 0 < ready_cnt_; }

  size_t active_fiber_count() const { return ready_cnt_; }

  // suspend_until halts the thread in case there are no active fibers to run on it.
  // This is done by dispatcher fiber.
  void suspend_until(std::chrono::steady_clock::time_point const& abs_time) noexcept override {
    DVLOG(2) << "suspend_until " << abs_time.time_since_epoch().count();

    // Only dispatcher context stops the thread.
    DCHECK(fibers::context::active()->is_context(fibers::type::dispatcher_context));

    // Set a timer so at least one handler will eventually fire, causing
    // run_one() to eventually return.
    if ((std::chrono::steady_clock::time_point::max)() != abs_time) {
      // Each expires_at(time_point) call cancels any previous pending
      // call. We could inadvertently spin like this:
      // dispatcher calls suspend_until() with earliest wake time
      // suspend_until() sets suspend_timer_,
      // loop() calls run_one()
      // some other asio handler runs before timer expires
      // run_one() returns to loop()
      // loop() yields to dispatcher
      // dispatcher finds no ready fibers
      // dispatcher calls suspend_until() with SAME wake time
      // suspend_until() sets suspend_timer_ to same time, canceling
      // previous async_wait()
      // loop() calls run_one()
      // asio calls suspend_timer_ handler with operation_aborted
      // run_one() returns to loop()... etc. etc.
      // So only actually set the timer when we're passed a DIFFERENT
      // abs_time value.
      suspend_timer_->expires_at(abs_time);
      suspend_timer_->async_wait([](system::error_code const&) { this_fiber::yield(); });
    }
    CHECK_EQ(0, mask_ & LOOP_RUN_ONE) << "Deadlock detected";

    // We do not need a mutex here.
    cnd_.notify_one();
  }
  //]

  void notify() noexcept override {
    if (!suspend_timer_) {
      VLOG(1) << "Called during shutdown phase";
      return;
    }

    // Something has happened that should wake one or more fibers BEFORE
    // suspend_timer_ expires. Reset the timer to cause it to fire
    // immediately, causing the run_one() call to return. In theory we
    // could use cancel() because we don't care whether suspend_timer_'s
    // handler is called with operation_aborted or success. However --
    // cancel() doesn't change the expiration time, and we use
    // suspend_timer_'s expiration time to decide whether it's already
    // set. If suspend_until() set some specific wake time, then notify()
    // canceled it, then suspend_until() was called again with the same
    // wake time, it would match suspend_timer_'s expiration time and we'd
    // refrain from setting the timer. So instead of simply calling
    // cancel(), reset the timer, which cancels the pending sleep AND sets
    // a new expiration time. This will cause us to spin the loop twice --
    // once for the operation_aborted handler, once for timer expiration
    // -- but that shouldn't be a big problem.
    suspend_timer_->async_wait([](system::error_code const&) { this_fiber::yield(); });
    suspend_timer_->expires_at(std::chrono::steady_clock::now());
  }

  void MainLoop();

 private:
  void WaitTillFibersSuspend();
};

AsioScheduler::~AsioScheduler() {}

void AsioScheduler::MainLoop() {
  asio::io_context* io_cntx = io_context_.get();

  while (!io_cntx->stopped()) {
    if (has_ready_fibers()) {
      while (io_cntx->poll())
        ;

      // Gives up control to allow other fibers to run in the thread.
      WaitTillFibersSuspend();
    } else {
      // run one handler inside io_context
      // if no handler available, blocks this thread
      DVLOG(2) << "MainLoop::RunOneStart";
      mask_ |= LOOP_RUN_ONE;
      if (!io_cntx->run_one()) {
        mask_ &= ~LOOP_RUN_ONE;
        break;
      }
      DVLOG(2) << "MainLoop::RunOneEnd";
      mask_ &= ~LOOP_RUN_ONE;
    }
  }

  VLOG(1) << "MainLoop exited";
  suspend_timer_.reset();
}

void AsioScheduler::WaitTillFibersSuspend() {
  // block this fiber till all pending (ready) fibers are processed
  // == AsioScheduler::suspend_until() has been called.
  mask_ |= LOOP_SUSPEND;

  switch_cnt_ = 0;

  std::unique_lock<fibers::mutex> lk(mtx_);
  DVLOG(2) << "WaitTillFibersSuspend:Start";
  cnd_.wait(lk);
  mask_ &= ~LOOP_SUSPEND;
  DVLOG(2) << "WaitTillFibersSuspend:End";
}

void AsioScheduler::awakened(fibers::context* ctx, IoFiberProperties& props) noexcept {
  DCHECK(!ctx->ready_is_linked());

  ready_queue_type* rq;

  // Dispatcher fiber has lowest priority. Is it ok?
  if (ctx->is_context(fibers::type::dispatcher_context)) {
    rq = rqueue_arr_ + IoFiberProperties::MAX_NICE_LEVEL + 1;
    DVLOG(1) << "ReadyLink: " << ctx->get_id() << " dispatch";
  } else {
    unsigned nice = props.nice_level();
    DCHECK_LT(nice, IoFiberProperties::NUM_NICE_LEVELS);
    rq = rqueue_arr_ + nice;
    ++ready_cnt_;
    if (last_nice_level_ > nice)
      last_nice_level_ = nice;
    DVLOG(1) << "ReadyLink: " << ctx->get_id() << " " << nice;
  }

  ctx->ready_link(*rq); /*< fiber, enqueue on ready queue >*/
}

fibers::context* AsioScheduler::pick_next() noexcept {
  fibers::context* ctx(nullptr);
  DVLOG(3) << "pick_next: ReadyCnt " << ready_cnt_;

  // NUM_NICE_LEVELS is for dispatcher.
  for (; last_nice_level_ < IoFiberProperties::NUM_NICE_LEVELS; ++last_nice_level_) {
    auto& q = rqueue_arr_[last_nice_level_];
    if (q.empty())
      continue;

    // pop an item from the ready queue
    ctx = &q.front();
    q.pop_front();

    DCHECK(ctx && fibers::context::active() != ctx);
    DCHECK(!ctx->is_context(fibers::type::dispatcher_context));
    DCHECK_GT(ready_cnt_, 0);
    --ready_cnt_;
    if (mask_ & LOOP_SUSPEND) {
      if (++switch_cnt_ > MAIN_SWITCH_LIMIT) {
        cnd_.notify_one();
        ++main_resumes;
      }
    }

    /* We check for ready_cnt_ > K for 2 reasons:
       1. To allow dispatcher_context to run. Otherwise if ready_cnt_ == 0 and
       dispatcher_context is active and we switch to the main loop it will never switch back
       to dispatcher_context because has_ready_fibers() will return false.
       2. To switch to main loop only if the active_cnt is large enough, i.e. it might take
          a lot of time to switch back to main so resuming main fiber is worth it.

       In addition we switch only priorities higher than MAIN_NICE_LEVEL, which also implies
       that MAIN is suspended (otherwise pick_next would choose it with i == MAIN_NICE_LEVEL).
    */
#if 0
TBD
      if (i > MAIN_NICE_LEVEL && ready_cnt_ > 1) {
        if (++switch_cnt_ > MAIN_SWITCH_LIMIT) {
          DVLOG(1) << "SwitchToMain on " << i << " " << switch_cnt_ << " " << ready_cnt_;
          ++main_resumes;
          cnd_.notify_one();  // no need for mutex.
        }
      }
#endif

    DVLOG(3) << "pick_next: " << ctx->get_id();
    return ctx;
  }

  DCHECK_EQ(0, ready_cnt_);

  auto& dispatch_q = rqueue_arr_[IoFiberProperties::MAX_NICE_LEVEL + 1];
  if (!dispatch_q.empty()) {
    fibers::context* ctx = &dispatch_q.front();
    dispatch_q.pop_back();

    DVLOG(2) << "switching to dispatch from " << fibers::context::active()->get_id()
             << ", mask: " << unsigned(mask_);

    return ctx;
  }

  DVLOG(2) << "pick_next: null";

  return nullptr;
}

}  // namespace

constexpr unsigned IoFiberProperties::MAX_NICE_LEVEL;
constexpr unsigned IoFiberProperties::NUM_NICE_LEVELS;

void IoFiberProperties::SetNiceLevel(unsigned p) {
  // Of course, it's only worth reshuffling the queue and all if we're
  // actually changing the nice.
  p = std::min(p, MAX_NICE_LEVEL);
  if (p != nice_) {
    nice_ = p;
    notify();
  }
}

void IoContext::StartLoop(BlockingCounter* bc) {
  // I do not use use_scheduling_algorithm because I want to retain access to the scheduler.
  // fibers::use_scheduling_algorithm<AsioScheduler>(io_ptr);
  AsioScheduler* scheduler = new AsioScheduler(context_ptr_);
  fibers::context::active()->get_scheduler()->set_algo(scheduler);
  this_fiber::properties<IoFiberProperties>().set_name("io_loop");
  this_fiber::properties<IoFiberProperties>().SetNiceLevel(MAIN_NICE_LEVEL);
  CHECK(fibers::context::active()->is_context(fibers::type::main_context));

  thread_id_ = this_thread::get_id();

  io_context& io_cntx = *context_ptr_;

  // We run the main loop inside the callback of io_context, blocking it until the loop exits.
  // The reason for this is that io_context::running_in_this_thread() is deduced based on the
  // call-stack. GAIA code should use InContextThread() to check whether the code runs in the
  // context's thread.
  Async([scheduler, bc] {
    bc->Dec();
    scheduler->MainLoop();
  });

  // Bootstrap - launch the callback handler above.
  // It will block until MainLoop exits. See comment above.
  io_cntx.run_one();

  for (unsigned i = 0; i < 2; ++i) {
    DVLOG(1) << "Cleanup Loop " << i;
    while (io_cntx.poll() || scheduler->has_ready_fibers()) {
      this_fiber::yield();  // while something happens, pass the ownership to other fiber.
    }
    io_cntx.restart();
  }

  VLOG(1) << "MainSwitch Resumes :" << main_resumes;
}

void IoContext::Stop() {
  if (cancellable_arr_.size() > 0) {
    fibers_ext::BlockingCounter cancel_bc(cancellable_arr_.size());

    VLOG(1) << "Cancelling " << cancellable_arr_.size() << " cancellables";
    // Shutdown sequence and cleanup.
    for (auto& k_v : cancellable_arr_) {
      AsyncFiber([&] {
        k_v.first->Cancel();
        cancel_bc.Dec();
      });
    }
    cancel_bc.Wait();
    for (auto& k_v : cancellable_arr_) {
      k_v.second.join();
    }
    cancellable_arr_.clear();
  }

  context_ptr_->stop();
  VLOG(1) << "AsioIoContext stopped";
}

}  // namespace util
