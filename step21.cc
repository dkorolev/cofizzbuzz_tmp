// The time is now simulated.

#include <condition_variable>
#include <iostream>
#include <string>
#include <functional>
#include <queue>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <set>
#include <coroutine>
#include <vector>

#ifdef DEBUG
#define TELEMETRY
#define PRINT_SIMULATED_TIME
#endif

using std::atomic_bool;
using std::atomic_int;
using std::condition_variable;
using std::cout;
using std::deque;
using std::endl;
using std::flush;
using std::function;
using std::future;
using std::lock_guard;
using std::make_shared;
using std::make_unique;
using std::map;
using std::multimap;
using std::mutex;
using std::promise;
using std::queue;
using std::set;
using std::shared_ptr;
using std::string;
using std::terminate;
using std::thread;
using std::to_string;
using std::unique_lock;
using std::unique_ptr;
using std::vector;

inline string& CurrentThreadName() {
  static thread_local string current_thread_name = "<a yet unnamed thread>";
  return current_thread_name;
}

class ExecutorInstance;

struct ExecutorThreadLocalPlaceholder {
  mutable mutex mut;
  ExecutorInstance* ptr = nullptr;
  ExecutorInstance& Instance() {
    lock_guard<mutex> lock(mut);
    if (!ptr) {
      terminate();
    }
    return *ptr;
  }
  void Set(ExecutorInstance& ref) {
    lock_guard<mutex> lock(mut);
    if (ptr) {
      terminate();
    }
    ptr = &ref;
  }
  void Unset(ExecutorInstance& ref) {
    lock_guard<mutex> lock(mut);
    if (ptr != &ref) {
      terminate();
    }
    ptr = nullptr;
  }
  void FailIfNoExecutor() const {
    lock_guard<mutex> lock(mut);
    if (!ptr) {
      cout << "No executor, have one in scope before starting the coroutine." << endl;
      terminate();
    }
  }
};

ExecutorThreadLocalPlaceholder& ExecutorForThisThread() {
  static thread_local ExecutorThreadLocalPlaceholder p;
  return p;
}

struct CoroutineLifetime {
  virtual ~CoroutineLifetime() = default;
  virtual void ResumeFromExecutorWorkerThread() = 0;
};

#ifdef TELEMETRY
struct ExecutorStats {
  // NOTE(dkorolev): These should be `atomic`-s and/or mutex-locked in real, multithreaded code.
  int64_t total_worker_steps = 0;
  int64_t total_resume_steps = 0;
  int64_t total_awaitable_ready = 0;
  int64_t total_awaitable_need_to_wait = 0;
  int64_t total_immediate_ready = 0;
  int64_t total_sleep_resumes = 0;
  ~ExecutorStats() {
    cout << "Executor total worker steps: " << total_worker_steps << endl;
    cout << "Executor total resume steps: " << total_resume_steps << endl;
    cout << "Executor total immediate / ready / waiting: " << total_awaitable_ready << " / " << total_immediate_ready
         << " / " << total_awaitable_need_to_wait << endl;
    cout << "Executor total sleep resumes: " << total_sleep_resumes << endl;
  }
};
#endif

struct TimeUnits {
  uint64_t tu;
  static TimeUnits Zero() { return TimeUnits{0}; }
  operator bool() const { return tu != 0; }
  bool operator<(TimeUnits rhs) const { return tu < rhs.tu; }
  TimeUnits operator+(TimeUnits delta) const { return TimeUnits{tu + delta.tu}; }
  uint64_t AsNumber() const { return tu; }
};

inline std::ostream& operator<<(std::ostream& os, TimeUnits const& tu) {
  os << tu.tu;
  return os;
}

TimeUnits operator"" _tu(unsigned long long v) { return TimeUnits{v}; }

class ExecutorInstance {
 private:
  thread worker;
  TimeUnits time_now = TimeUnits::Zero();

  bool executor_time_to_terminate_thread = false;

  mutable mutex mut;
  condition_variable cv;

  // Store the jobs in a red-black tree, the `priority_queue` is not as clean syntax-wise in C++.
  map<TimeUnits, deque<function<void()>>> jobs;

  // A quick & hacky way to wait until everything is done, at scope destruction.
  mutex unlock_when_done;

  // The coroutines (coroutine frames, or fibers), that are currently running.
  // Will declare termination once the last one is done.
  // NOTE(dkorolev): Even a plain inc/dec counter would do. But this example is extra safe and extra illustative.
  set<CoroutineLifetime*> coroutines;

 protected:
  friend class ExecutorScope;
  ExecutorInstance() : worker([this]() { Thread(); }) { unlock_when_done.lock(); }

  function<void()> GetNextTask() {
    while (true) {
      {
        unique_lock<mutex> lock(mut);
        if (executor_time_to_terminate_thread) {
          return nullptr;
        }
        while (!jobs.empty() && jobs.begin()->second.empty()) {
          jobs.erase(jobs.begin());
        }
        if (!jobs.empty()) {
          auto it_time_moment = jobs.begin();
          if (time_now < it_time_moment->first) {
#ifdef PRINT_SIMULATED_TIME
            cout << "Advancing time from " << time_now << " to " << it_time_moment->first << endl;
#endif
            time_now = it_time_moment->first;
          }
          auto it_job = it_time_moment->second.begin();
          function<void()> extracted = *it_job;
          it_time_moment->second.erase(it_job);
          return extracted;
        } else {
          // Note that now this code is properly waiting on the condition variable!
          cv.wait(lock, [this]() { return !executor_time_to_terminate_thread && !jobs.empty(); });
        }
      }
    }
  }

  void Thread() {
    ExecutorForThisThread().Set(*this);
    auto& name = CurrentThreadName();
    string const goal = "ExecutorInstance";
    if (name == goal) {
      // Sanity check.
      cout << "There can only be one " + goal << endl;
      terminate();
    }
    name = goal;

    while (true) {
      // `GetNextTask()` a) is the blocking call, and b) returns `nullptr` once signaled termination.
      function<void()> next_task = GetNextTask();
      if (!next_task) {
        ExecutorForThisThread().Unset(*this);
        unlock_when_done.unlock();
        return;
      }
      next_task();
#ifdef TELEMETRY
      ++stats.total_worker_steps;
#endif
    }
  }

  friend class ExecutorCoroutineScope;
  void Register(CoroutineLifetime* coro) {
    ScheduleNext([coro]() { coro->ResumeFromExecutorWorkerThread(); });
    if (coroutines.count(coro)) {
      terminate();
    }
    coroutines.insert(coro);
  }
  void Unregister(CoroutineLifetime* coro) {
    auto it = coroutines.find(coro);
    if (it == coroutines.end()) {
      terminate();
    }
    coroutines.erase(it);
    if (coroutines.empty()) {
      {
        lock_guard<mutex> lock(mut);
        executor_time_to_terminate_thread = true;
      }
      cv.notify_one();
    }
  }

 public:
#ifdef TELEMETRY
  ExecutorStats stats;
#endif

  ~ExecutorInstance() {
    worker.join();
    unlock_when_done.lock();
  }

  void ScheduleNext(function<void()> code) {
    {
      lock_guard<mutex> lock(mut);
      jobs[time_now].push_front(code);
    }
    cv.notify_one();
  }

  void Schedule(TimeUnits delay, function<void()> code) {
    if (!delay) {
      cout << "`Schedule()` is for the future, use `ScheduleNext()` for the present." << endl;
      terminate();
    }
    {
      lock_guard<mutex> lock(mut);
      jobs[time_now + delay].push_back(code);
    }
    cv.notify_one();
  }

  // Return time in units, mostly for demo purposes.
  uint64_t Now() const {
    lock_guard<mutex> lock(mut);
    return time_now.AsNumber();
  }
};

// The instance of the executor is created and owned by `ExecutorScope`.
class ExecutorScope {
 private:
  ExecutorInstance executor;

 public:
  ExecutorScope() { ExecutorForThisThread().Set(executor); }
  ~ExecutorScope() { ExecutorForThisThread().Unset(executor); }
};

inline ExecutorInstance& Executor() { return ExecutorForThisThread().Instance(); }

class ExecutorCoroutineScope {
 private:
  CoroutineLifetime* coro;

 public:
  ExecutorCoroutineScope(CoroutineLifetime* coro) : coro(coro) { Executor().Register(coro); }
  ~ExecutorCoroutineScope() { Executor().Unregister(coro); }
};

// The "minimalistic" coroutine runner integrated with the executor.
// Instructs the executor to call `.resume()` once.
// Tracks its own lifetime with the executor, so that the executor knows when all the active coroutines are done.

struct CoroutineRetvalHolderBase {
  mutable mutex mut;
  bool returned = false;
  vector<std::coroutine_handle<>> to_resume;  // Other coroutines waiting awaiting on this one returning.
};

template <typename RETVAL>
struct CoroutineRetvalHolder : CoroutineRetvalHolderBase {
  RETVAL value;
  void return_value(RETVAL v) noexcept {
    {
      lock_guard<mutex> lock(mut);
      if (returned) {
        terminate();
      }
      returned = true;
      value = v;
    }
    for (auto& h : to_resume) {
      h.resume();
    }
  }
};

template <>
struct CoroutineRetvalHolder<void> : CoroutineRetvalHolderBase {
  void return_void() noexcept {
    {
      lock_guard<mutex> lock(mut);
      if (returned) {
        terminate();
      }
      returned = true;
    }
    for (auto& h : to_resume) {
      h.resume();
    }
  }
};

template <typename RETVAL>
struct CoroutineAwaitResume {
  CoroutineRetvalHolder<RETVAL>* pself;
  RETVAL immediate_value;

  CoroutineAwaitResume(RETVAL immediate) : pself(nullptr), immediate_value(immediate) {}

  explicit CoroutineAwaitResume(CoroutineRetvalHolder<RETVAL>& self) : pself(&self) {}

  RETVAL await_resume() noexcept {
    if (pself) {
      lock_guard<mutex> lock(pself->mut);
      if (!pself->returned) {
        // Internal error: `await_resume()` should only be called once the result is available.
        terminate();
      }
      return pself->value;
    } else {
      return immediate_value;
    }
  }
};

template <>
struct CoroutineAwaitResume<void> {
  CoroutineRetvalHolder<void>* pself;

  CoroutineAwaitResume() : pself(nullptr) {}

  explicit CoroutineAwaitResume(CoroutineRetvalHolder<void>& self) : pself(&self) {}

  void await_resume() noexcept {
    if (pself) {
      lock_guard<mutex> lock(pself->mut);
      if (!pself->returned) {
        // Internal error: `await_resume()` should only be called once the result is available.
        terminate();
      }
    }
  }
};

template <typename RETVAL = void>
struct Async : CoroutineAwaitResume<RETVAL> {
  struct promise_type : CoroutineLifetime, CoroutineRetvalHolder<RETVAL> {
    unique_ptr<ExecutorCoroutineScope> coroutine_executor_lifetime;

    Async get_return_object() {
      if (coroutine_executor_lifetime) {
        // Internal error, should only have one `get_return_object` call per instance.
        terminate();
      }
      coroutine_executor_lifetime = make_unique<ExecutorCoroutineScope>(this);
      return Async(*this);
    }

    std::suspend_always initial_suspend() noexcept {
      // Should be `.resume()`-d via `.ResumeFromExecutorWorkerThread()` from the executor.
      return {};
    }

    std::suspend_never final_suspend() noexcept {
      coroutine_executor_lifetime = nullptr;
      return {};
    }

    void unhandled_exception() noexcept { terminate(); }

    void ResumeFromExecutorWorkerThread() override {
#ifdef TELEMETRY
      ++Executor().stats.total_resume_steps;
#endif
      std::coroutine_handle<promise_type>::from_promise(*this).resume();
    }
  };

  explicit Async(promise_type& self) : CoroutineAwaitResume<RETVAL>(self) {}

  using CoroutineAwaitResume<RETVAL>::CoroutineAwaitResume;

  bool await_ready() noexcept {
    if (CoroutineAwaitResume<RETVAL>::pself) {
      lock_guard<mutex> lock(CoroutineAwaitResume<RETVAL>::pself->mut);
#ifndef TELEMETRY
      return CoroutineAwaitResume<RETVAL>::pself->returned;
#else
      if (CoroutineAwaitResume<RETVAL>::pself->returned) {
        ++Executor().stats.total_awaitable_ready;
        return true;
      } else {
        ++Executor().stats.total_awaitable_need_to_wait;
        return false;
      }
#endif
    } else {
#ifdef TELEMETRY
      ++Executor().stats.total_immediate_ready;
#endif
      return true;
    }
  }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    if (CoroutineAwaitResume<RETVAL>::pself) {
      lock_guard<mutex> lock(CoroutineAwaitResume<RETVAL>::pself->mut);
      if (CoroutineAwaitResume<RETVAL>::pself->returned) {
        h.resume();
      } else {
        CoroutineAwaitResume<RETVAL>::pself->to_resume.push_back(h);
      }
    } else {
      cout << "FATAL: Should never attempt to `await_suspend` an immediate value." << endl;
      terminate();
    }
  }
};

class Sleep final {
 private:
  TimeUnits const delay;

 public:
  explicit Sleep(TimeUnits delay) : delay(delay) {}

  constexpr bool await_ready() noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    Executor().Schedule(delay, [h]() {
#ifdef TELEMETRY
      ++Executor().stats.total_sleep_resumes;
#endif
      h.resume();
    });
  }

  void await_resume() noexcept {}
};

inline Async<bool> IsEven(int x) {
  // Confirm multiple suspend/resume steps work just fine.
  // Just `co_return ((x % 2) == 0);` works too, of course.
  if ((x % 2) == 0) {
    co_await Sleep(1_tu);
    co_await Sleep(1_tu);
    co_return true;
  } else {
    co_await Sleep(1_tu);
    co_await Sleep(1_tu);
    co_await Sleep(1_tu);
    co_return false;
  }
}

inline Async<void> CallSleep(TimeUnits delay) {
  co_await Sleep(delay);
  co_return;
}

inline Async<int> Square(int x) {
  co_await CallSleep(10_tu);
  co_return (x * x);
}

void RunExampleCoroutine() {
  function<Async<>(string)> MultiStepFunction = [](string s) -> Async<> {
    for (int i = 1; i <= 10; ++i) {
      co_await Sleep(100_tu);
      cout << s << ", i=" << i << "/10, even=..." << endl;
      bool even = co_await IsEven(i);
      auto s_even = even ? "true" : "false";
      cout << s << ", i=" << i << "/10, even=" << s_even << ", square=..." << endl;
      auto square = co_await Square(i);
      cout << s << ", i=" << i << "/10, even=" << s_even << ", square=" << square << endl;
    }
  };

  ExecutorScope executor;

  {
    // Call the coroutine. The return object, of type `Async<void>`, will go out of scope, which is normal.
    MultiStepFunction("The MultiStepFunction");
  }

  // The destructor of `ExecutorScope` will wait for the running coroutine(s) to complete.
  // To be more precise, it will wait until it is shut down gracefully, and it will be shut down gracefully
  // as soon as the last outstanding coroutine is done with its execution!
}

inline bool SyncIsDivisibleByThree(int value) { return ((value % 3) == 0); }

inline Async<bool> IsDivisibleByThree(int value) {
  co_await Sleep(10_tu);
  co_return (value % 3) == 0;
}

inline Async<bool> IsDivisibleByFive(int value) {
  co_await Sleep(5_tu);
  co_await Sleep(5_tu);
  co_return (value % 5) == 0;
}

inline Async<> CoroFizzBuzz(function<Async<bool>(string)> next) {
  int value = 0;
  while (true) {
    ++value;
#if 1
    // This commit makes "casting" `bool` into `Async<bool>` perfectly legal.
    // Note that this change also breaks `g++`, and this is why this demo is in `clang++`.
    Async<bool> awaitable_d3 = SyncIsDivisibleByThree(value);
#else
    Async<bool> awaitable_d3 = IsDivisibleByThree(value);
#endif
    Async<bool> awaitable_d5 = IsDivisibleByFive(value);
    bool const d3 = co_await awaitable_d3;
    bool const d5 = co_await awaitable_d5;
    if (d3) {
      if (!(co_await next("Fizz"))) {
        co_return;
      }
    }
    if (d5) {
      if (!(co_await next("Buzz"))) {
        co_return;
      }
    }
    if (!d3 && !d5) {
      if (!(co_await next(to_string(value)))) {
        co_return;
      }
    }
  }
}

void RunCoroFizzBuzz() {
  int total = 0;

  ExecutorScope executor;

  CoroFizzBuzz([&total](string s) -> Async<bool> {
    cout << ++total << " : " << s << ", at " << Executor().Now() << " time units, from thread " << CurrentThreadName()
         << endl;
    co_return (total < 15);
  });

  cout << "main() done, but will wait for the executor to complete its tasks." << endl;
}

int main(int argc, char** argv) {
#if defined(NDEBUG) && !defined(DEBUG)
  cout << "Running the NDEBUG build." << endl;
#elif defined(DEBUG) && !defined(NDEBUG)
  cout << "Running the DEBUG build." << endl;
#else
#error "Must have either `DEBUG` or `NDEBUG` `#define`-d."
#endif

  CurrentThreadName() = "main()";

  if (string("--example") == argv[argc - 1]) {
    RunExampleCoroutine();
  } else {
    RunCoroFizzBuzz();
  }
}
