
#include <iostream>
#include <memory>

#include <proxc/config.hpp>

#include <proxc/scheduler.hpp>
#include <proxc/context.hpp>

#include <proxc/scheduling_policy/policy_base.hpp>
#include <proxc/scheduling_policy/round_robin.hpp>

#include <boost/assert.hpp>
#include <boost/bind.hpp>
#include <boost/intrusive_ptr.hpp>

PROXC_NAMESPACE_BEGIN

namespace hook = detail::hook;

namespace detail {

struct SchedulerInitializer
{
    thread_local static Scheduler * self_;
    thread_local static std::size_t counter_;

    SchedulerInitializer()
    {
        if (counter_++ != 0) { return; }

        auto scheduler = new Scheduler{};
        self_ = scheduler;
    }

    ~SchedulerInitializer()
    {
        if (--counter_ != 0) { return; }

        BOOST_ASSERT( Scheduler::running()->is_type( Context::Type::Main ) );
        auto scheduler = self_;
        delete scheduler;
    }
};

thread_local Scheduler * SchedulerInitializer::self_{ nullptr };
thread_local std::size_t SchedulerInitializer::counter_{ 0 };

} // namespace detail

Scheduler * Scheduler::self() noexcept
{
    //thread_local static boost::context::detail::activation_record_initializer ac_rec_init;
    thread_local static detail::SchedulerInitializer sched_init;
    return detail::SchedulerInitializer::self_;
}

Context * Scheduler::running() noexcept
{
    BOOST_ASSERT(Scheduler::self() != nullptr);
    return Scheduler::self()->running_;
}

Scheduler::Scheduler()
    : policy_{ new scheduling_policy::RoundRobin{} }
    , main_ctx_{ new Context{ context::main_type } }
    , scheduler_ctx_{ new Context{ context::scheduler_type,
        [this](void * vp) { run_(vp); } } }
{
    running_ = main_ctx_.get();
    schedule(scheduler_ctx_.get());
}

Scheduler::~Scheduler()
{
    BOOST_ASSERT(main_ctx_.get() != nullptr);
    BOOST_ASSERT(scheduler_ctx_.get() != nullptr);
    BOOST_ASSERT(running_ == main_ctx_.get());

    exit_ = true;
    join( scheduler_ctx_.get() );

    BOOST_ASSERT( main_ctx_->wait_queue_.empty() );
    BOOST_ASSERT( scheduler_ctx_->wait_queue_.empty() );

    scheduler_ctx_.reset();
    main_ctx_.reset();


    for (auto et = work_queue_.begin(); et != work_queue_.end(); ) {
        auto ctx = &( *et );
        et = work_queue_.erase(et);
        delete ctx;
    }
    running_ = nullptr;

    BOOST_ASSERT( work_queue_.empty() );
    BOOST_ASSERT( sleep_queue_.empty() );
    BOOST_ASSERT( terminated_queue_.empty() );
}

void Scheduler::resume_(Context * to_ctx, void * vp) noexcept
{
    BOOST_ASSERT(   to_ctx != nullptr );
    BOOST_ASSERT(   running_ != nullptr );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! to_ctx->is_linked< hook::Terminated >() );

    std::swap( to_ctx, running_ );

    // context switch
    running_->resume( vp );
}

void Scheduler::resume() noexcept
{
    resume( policy_->pick_next() );
}

void Scheduler::resume(Context * to_ctx) noexcept
{
    resume_( to_ctx );
}

void Scheduler::terminate(Context * ctx) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    BOOST_ASSERT( running_ == ctx );

    // FIXME: is_type(Dynamic) instead?
    BOOST_ASSERT(   ctx->is_type( Context::Type::Work ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    ctx->has_terminated_ = true;
    ctx->link( terminated_queue_ );
    ctx->unlink< hook::Work >();

    wakeup_waiting_on( ctx );

    resume();
    BOOST_ASSERT_MSG(false, "unreachable: should not return from scheduler terminate().");
}

void Scheduler::schedule(Context * ctx) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    policy_->enqueue( ctx );
}

void Scheduler::attach(Context * ctx) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT(   ctx->is_type( Context::Type::Work ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Work >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    ctx->link( work_queue_ );
}

void Scheduler::detach(Context * ctx) noexcept
{
    BOOST_ASSERT(ctx != nullptr);
    BOOST_ASSERT(   ctx->is_type( Context::Type::Work ) );
    BOOST_ASSERT(   ctx->is_linked< hook::Work >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    ctx->unlink< hook::Work >();
}

void Scheduler::commit(Context * ctx) noexcept
{
    BOOST_ASSERT( ctx != nullptr );
    attach( ctx );
    schedule( ctx );
}

void Scheduler::yield() noexcept
{
    auto ctx = running_;
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT(   ctx->is_type( Context::Type::Process ) );
    BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

    auto next = policy_->pick_next();
    if (next != nullptr) {
        schedule( ctx );
        resume( next );
    }
}

void Scheduler::join(Context * ctx) noexcept
{
    BOOST_ASSERT(ctx != nullptr);
    Context * running_ctx = running_;
    // FIXME: this might need rework
    if ( ! ctx->has_terminated() ) {
        running_ctx->link( ctx->wait_queue_ );
        resume();
        BOOST_ASSERT( running_ == running_ctx );
    }
}

void Scheduler::sleep_until(TimePointType const & time_point) noexcept
{
    BOOST_ASSERT(   running_ != nullptr );
    BOOST_ASSERT(   running_->is_type( Context::Type::Process ) );
    BOOST_ASSERT( ! running_->is_linked< hook::Ready >() );
    BOOST_ASSERT( ! running_->is_linked< hook::Wait >() );
    BOOST_ASSERT( ! running_->is_linked< hook::Sleep >() );
    BOOST_ASSERT( ! running_->is_linked< hook::Terminated >() );

    running_->time_point_ = time_point;
    running_->link( sleep_queue_ );
    resume();
    // FIXME: is this check guaranteed?
    BOOST_ASSERT( ClockType::now() > time_point );
}

void Scheduler::wakeup_sleep() noexcept
{
    auto now = ClockType::now();
    auto end = sleep_queue_.end();
    for (auto it  = sleep_queue_.begin(); it != end; ) {
        auto ctx = &( *it );

        BOOST_ASSERT(   ctx->is_type( Context::Type::Process ) );
        BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Terminated >() );

        // Keep advancing the queue if deadline is reached,
        // break if not.
        if (ctx->time_point_ > now) {
            break;
        }
        it = sleep_queue_.erase( it );
        ctx->time_point_ = (TimePointType::max)();
        schedule( ctx );
    }
}

void Scheduler::wakeup_waiting_on(Context * ctx) noexcept
{
    BOOST_ASSERT(   ctx != nullptr );
    BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );

    while ( ! ctx->wait_queue_.empty() ) {
        auto waiting_ctx = & ctx->wait_queue_.front();
        ctx->wait_queue_.pop_front();
        schedule( waiting_ctx );
    }
}

void Scheduler::cleanup_terminated() noexcept
{
    while ( ! terminated_queue_.empty() ) {
        auto ctx = & terminated_queue_.front();
        terminated_queue_.pop_front();

        // FIXME: is_type(Dynamic) instead?
        BOOST_ASSERT(   ctx->is_type(Context::Type::Work) );
        BOOST_ASSERT( ! ctx->is_type(Context::Type::Static) );
        BOOST_ASSERT( ! ctx->is_linked< hook::Ready >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Work >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Wait >() );
        BOOST_ASSERT( ! ctx->is_linked< hook::Sleep >() );

        // FIXME: might do a more reliable cleanup here.
        // work contexts might not necessarily be new-allocated,
        // allthough it is now, eg. intrusive_ptr/shared_ptr
        intrusive_ptr_release( ctx );
    }
}

void Scheduler::print_debug() noexcept
{
    std::cout << "Scheduler: " << std::endl;
    std::cout << "  Scheduler Ctx: " << std::endl;
    scheduler_ctx_->print_debug();
    std::cout << "  Main Ctx: " << std::endl;
    main_ctx_->print_debug();
    std::cout << "  Running: " << std::endl;
    running_->print_debug();
    std::cout << "  Work Queue:" << std::endl;
    for (auto& ctx : work_queue_) {
        ctx.print_debug();
    }
    std::cout << "  Terminated Queue:" << std::endl;
    for (auto& ctx : terminated_queue_) {
        ctx.print_debug();
    }
}

// Scheduler context loop
void Scheduler::run_(void *) noexcept
{
    BOOST_ASSERT(Scheduler::running() == scheduler_ctx_.get());
    for (;;) {
        if (exit_) {
            policy_->notify();
            if (work_queue_.empty()) {
                break;
            }
        }

        cleanup_terminated();
        wakeup_sleep();

        auto ctx = policy_->pick_next();
        if (ctx != nullptr) {
            schedule( scheduler_ctx_.get() );
            resume( ctx );
            BOOST_ASSERT( running_ == scheduler_ctx_.get() );
        } else {
            auto sleep_it = sleep_queue_.begin();
            auto suspend_time = (sleep_it != sleep_queue_.end())
                ? sleep_it->time_point_
                : (PolicyType::TimePointType::max)();
            policy_->suspend_until( suspend_time );
        }
    }

    cleanup_terminated();
    wakeup_waiting_on( scheduler_ctx_.get() );
    main_ctx_->unlink< hook::Ready >();
    resume( main_ctx_.get() );
    BOOST_ASSERT_MSG(false, "unreachable: should not return after scheduler run.");
}

PROXC_NAMESPACE_END
