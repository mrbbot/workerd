// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/util/wait-list.h>
#include <kj/debug.h>

namespace workerd {

namespace {
thread_local CrossThreadWaitList::WaiterMap threadLocalWaiters;
// Optimization: If the same wait list is waited multiple times in the same thread, we want to
// share the signal rather than send two cross-thread signals.
}  // namespace

void END_WAIT_LIST_CANCELER_STACK_START_CANCELEE_STACK() {}

CrossThreadWaitList::CrossThreadWaitList(Options options)
    : state(kj::atomicRefcounted<State>(options)) {}

void CrossThreadWaitList::destroyed() {
  if (!createdFulfiller) state->lostFulfiller();
}

CrossThreadWaitList::Waiter::Waiter(const State& state,
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfillerArg)
    : state(kj::atomicAddRef(state)), fulfiller(kj::mv(fulfillerArg)) {
  auto lock = state.waiters.lockExclusive();
#ifdef _WIN32
  if (state.done.load(std::memory_order_acquire)) {
    KJ_IF_MAYBE(e, state.exception) {
      fulfiller->reject(kj::cp(*e));
    } else {
      fulfiller->fulfill();
    }
  } else {
    lock->add(*this);
  }
#else
  if (__atomic_load_n(&state.done, __ATOMIC_ACQUIRE)) {
    KJ_IF_MAYBE(e, state.exception) {
      fulfiller->reject(kj::cp(*e));
    } else {
      fulfiller->fulfill();
    }
  } else {
    lock->add(*this);
  }
#endif

}
CrossThreadWaitList::Waiter::~Waiter() noexcept(false) {
#ifdef _WIN32
  if (unlinked.load(std::memory_order_acquire)) {
    // No need to take a lock, already unlinked.
    KJ_ASSERT(!link.isLinked());
  } else {
    auto lock = state->waiters.lockExclusive();
    if (link.isLinked()) {
      lock->remove(*this);
    }
  }
#else
  if (__atomic_load_n(&unlinked, __ATOMIC_ACQUIRE)) {
    // No need to take a lock, already unlinked.
    KJ_ASSERT(!link.isLinked());
  } else {
    auto lock = state->waiters.lockExclusive();
    if (link.isLinked()) {
      lock->remove(*this);
    }
  }
#endif

  if (state->useThreadLocalOptimization) {
    auto& entry = KJ_ASSERT_NONNULL(threadLocalWaiters.findEntry(state.get()));
    KJ_ASSERT(entry.value == this);
    threadLocalWaiters.erase(entry);
  }
}

kj::Promise<void> CrossThreadWaitList::addWaiter() const {
#ifdef _WIN32
  if (state->done.load(std::memory_order_acquire)) {
    KJ_IF_MAYBE(e, state->exception) {
      return kj::cp(*e);
    } else {
      return kj::READY_NOW;
    }
  }
#else
  if (__atomic_load_n(&state.done, __ATOMIC_ACQUIRE)) {
    KJ_IF_MAYBE(e, state->exception) {
      return kj::cp(*e);
    } else {
      return kj::READY_NOW;
    }
  }
#endif

  if (state->useThreadLocalOptimization) {
    kj::Own<Waiter> ownWaiter;

    auto& waiter = threadLocalWaiters.findOrCreate(state.get(),
        [&]() -> decltype(threadLocalWaiters)::Entry {
      auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
      ownWaiter = kj::refcounted<Waiter>(*state, kj::mv(paf.fulfiller));
      ownWaiter->forkedPromise = paf.promise.fork();
      return { state.get(), ownWaiter.get() };
    });

    if (ownWaiter.get() == nullptr) {
      ownWaiter = kj::addRef(*waiter);
    }

    return waiter->forkedPromise.addBranch().attach(kj::mv(ownWaiter));
  } else {
    // No refcounting, no forked promise.
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    auto waiter = kj::heap<Waiter>(*state, kj::mv(paf.fulfiller));
    return paf.promise.attach(kj::mv(waiter));
  }
}

kj::Own<kj::CrossThreadPromiseFulfiller<void>> CrossThreadWaitList::makeSeparateFulfiller() {
  class FulfillerImpl final: public kj::CrossThreadPromiseFulfiller<void> {
  public:
    FulfillerImpl(kj::Own<const State> state): state(kj::mv(state)) {}
    ~FulfillerImpl() noexcept(false) {
      state->lostFulfiller();
    }
    void fulfill(kj::_::Void&&) const override {
      state->fulfill();
    }
    void reject(kj::Exception&& exception) const override {
      state->reject(kj::mv(exception));
    }
    bool isWaiting() const override {
      // Note that it would be incorrect for isWaiting() to return false when `done` is false
      // even if the waiter list is empty, because the waiter list could become non-empty later.
      // In theory if we could determine that there will never be a waiter, then isWaiting()
      // could return false.
#ifdef _WIN32
      return !state->done.load(std::memory_order_acquire);
#else
      return !__atomic_load_n(&state->done, __ATOMIC_ACQUIRE);
#endif
    }

  private:
    kj::Own<const State> state;
  };

  KJ_REQUIRE(!createdFulfiller, "makeSeparateFulfiller() can only be called once");
  createdFulfiller = true;
  return kj::heap<FulfillerImpl>(kj::atomicAddRef(*state));
}

#ifdef _WIN32

void CrossThreadWaitList::State::fulfill() const {
  if (done.load(std::memory_order_acquire)) return;
  auto lock = waiters.lockExclusive();
  if (done) return;
  done.store(true, std::memory_order_release);

  for (auto& waiter: *lock) {
    lock->remove(waiter);
    waiter.fulfiller->fulfill();
    waiter.unlinked.store(true, std::memory_order_release);
  }
}

void CrossThreadWaitList::State::reject(kj::Exception&& e) const {
  if (done.load(std::memory_order_acquire)) return;
  auto lock = waiters.lockExclusive();
  if (done) return;
  auto& exceptionRef = exception.emplace(kj::mv(e));
  done.store(true, std::memory_order_release);

  for (auto& waiter: *lock) {
    lock->remove(waiter);
    waiter.fulfiller->reject(kj::cp(exceptionRef));
    waiter.unlinked.store(true, std::memory_order_release);
  }
}

void CrossThreadWaitList::State::lostFulfiller() const {
  if (done.load(std::memory_order_acquire)) return;
  auto lock = waiters.lockExclusive();
  if (done) return;
  auto& exceptionRef = exception.emplace(kj::getDestructionReason(
        reinterpret_cast<void*>(&END_WAIT_LIST_CANCELER_STACK_START_CANCELEE_STACK),
        kj::Exception::Type::FAILED, __FILE__, __LINE__, "wait list was never fulfilled"_kj));
  done.store(true, std::memory_order_release);

  if (!lock->empty()) {
    for (auto& waiter: *lock) {
      lock->remove(waiter);
      waiter.fulfiller->reject(kj::cp(exceptionRef));
      waiter.unlinked.store(true, std::memory_order_release);
    }
  }
}

#else // #ifdef _WIN32

void CrossThreadWaitList::State::fulfill() const {
  if (__atomic_load_n(&done, __ATOMIC_ACQUIRE)) return;
  auto lock = waiters.lockExclusive();
  if (done) return;
  __atomic_store_n(&done, true, __ATOMIC_RELEASE);

  for (auto& waiter: *lock) {
    lock->remove(waiter);
    waiter.fulfiller->fulfill();
    __atomic_store_n(&waiter.unlinked, true, __ATOMIC_RELEASE);
  }
}

void CrossThreadWaitList::State::reject(kj::Exception&& e) const {
  if (__atomic_load_n(&done, __ATOMIC_ACQUIRE)) return;
  auto lock = waiters.lockExclusive();
  if (done) return;
  auto& exceptionRef = exception.emplace(kj::mv(e));
  __atomic_store_n(&done, true, __ATOMIC_RELEASE);

  for (auto& waiter: *lock) {
    lock->remove(waiter);
    waiter.fulfiller->reject(kj::cp(exceptionRef));
    __atomic_store_n(&waiter.unlinked, true, __ATOMIC_RELEASE);
  }
}

void CrossThreadWaitList::State::lostFulfiller() const {
  if (__atomic_load_n(&done, __ATOMIC_ACQUIRE)) return;
  auto lock = waiters.lockExclusive();
  if (done) return;
  auto& exceptionRef = exception.emplace(kj::getDestructionReason(
        reinterpret_cast<void*>(&END_WAIT_LIST_CANCELER_STACK_START_CANCELEE_STACK),
        kj::Exception::Type::FAILED, __FILE__, __LINE__, "wait list was never fulfilled"_kj));
  __atomic_store_n(&done, true, __ATOMIC_RELEASE);

  if (!lock->empty()) {
    for (auto& waiter: *lock) {
      lock->remove(waiter);
      waiter.fulfiller->reject(kj::cp(exceptionRef));
      __atomic_store_n(&waiter.unlinked, true, __ATOMIC_RELEASE);
    }
  }
}

#endif // #else, #ifdef _WIN32

}  // namespace workerd
