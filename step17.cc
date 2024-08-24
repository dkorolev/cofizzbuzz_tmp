// Using the `Coro<T>` type for real.

#include <iostream>
#include <string>
#include <functional>
#include <queue>
#include <thread>
#include <chrono>
#include <future>
#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <set>
#include <coroutine>
#include <vector>

using std::cout;
using std::endl;
using std::flush;
using std::function;
using std::queue;
using std::string;
using std::to_string;
using namespace std::chrono_literals;
using std::atomic_bool;
using std::atomic_int;
using std::future;
using std::lock_guard;
using std::make_shared;
using std::make_unique;
using std::multimap;
using std::mutex;
using std::promise;
using std::set;
using std::shared_ptr;
using std::terminate;
using std::thread;
using std::unique_ptr;
using std::vector;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
using std::this_thread::sleep_for;

inline string& CurrentThreadName() {
  static thread_local string current_thread_name = "<a yet unnamed thread>";
  return current_thread_name;
}

struct TimestampMS final {
  milliseconds time_point;
  explicit TimestampMS() : time_point(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()) {}
  int operator-(TimestampMS const& rhs) const { return int((time_point - rhs.time_point).count()); }
};

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

class ExecutorInstance {
 private:
  thread worker;
  TimestampMS const t0;

  bool done = false;

  mutex mut;

  // Store the jobs in a red-black tree, the `priority_queue` is not as clean syntax-wise in C++.
  multimap<int, function<void()>> jobs;

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
        lock_guard<mutex> lock(mut);
        if (done) {
          return nullptr;
        }
        if (!jobs.empty()) {
          auto it = jobs.begin();
          if ((TimestampMS() - t0) >= it->first) {
            function<void()> extracted = it->second;
            jobs.erase(it);
            return extracted;
          }
        }
      }
      // NOTE(dkorolev): This is a "busy wait" loop, but it does the job for illustrative purposes.
      //                 Should of course `wait` + `wait_for` on a `condition_variable` if this code goes to prod.
      sleep_for(100us);  // 0.1ms.
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
    }
  }

  friend class ExecutorCoroutineScope;
  void Register(CoroutineLifetime* coro) {
    Schedule(0ms, [coro]() { coro->ResumeFromExecutorWorkerThread(); });
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
      GracefulShutdown();
    }
  }

 public:
  ~ExecutorInstance() {
    worker.join();
    unlock_when_done.lock();
  }

  void GracefulShutdown() {
    {
      lock_guard<mutex> lock(mut);
      done = true;
    }
  }

  void Schedule(milliseconds delay, function<void()> code) {
    lock_guard<mutex> lock(mut);
    jobs.emplace((TimestampMS() - t0) + delay.count(), code);
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
  CoroutineRetvalHolder<RETVAL>& self;
  explicit CoroutineAwaitResume(CoroutineRetvalHolder<RETVAL>& self) : self(self) {}

  RETVAL await_resume() noexcept {
    lock_guard<mutex> lock(self.mut);
    if (!self.returned) {
      // Internal error: `await_resume()` should only be called once the result is available.
      terminate();
    }
    return self.value;
  }
};

template <>
struct CoroutineAwaitResume<void> {
  CoroutineRetvalHolder<void>& self;

  explicit CoroutineAwaitResume(CoroutineRetvalHolder<void>& self) : self(self) {}

  void await_resume() noexcept {
    lock_guard<mutex> lock(self.mut);
    if (!self.returned) {
      // Internal error: `await_resume()` should only be called once the result is available.
      terminate();
    }
  }
};

template <typename RETVAL = void>
struct Coro : CoroutineAwaitResume<RETVAL> {
  struct promise_type : CoroutineLifetime, CoroutineRetvalHolder<RETVAL> {
    unique_ptr<ExecutorCoroutineScope> coroutine_executor_lifetime;

    Coro get_return_object() {
      if (coroutine_executor_lifetime) {
        // Internal error, should only have one `get_return_object` call per instance.
        terminate();
      }
      coroutine_executor_lifetime = make_unique<ExecutorCoroutineScope>(this);
      return Coro(*this);
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
      std::coroutine_handle<promise_type>::from_promise(*this).resume();
    }
  };

  explicit Coro(promise_type& self) : CoroutineAwaitResume<RETVAL>(self) {}

  bool await_ready() noexcept {
    lock_guard<mutex> lock(CoroutineAwaitResume<RETVAL>::self.mut);
    return CoroutineAwaitResume<RETVAL>::self.returned;
  }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    lock_guard<mutex> lock(CoroutineAwaitResume<RETVAL>::self.mut);
    if (CoroutineAwaitResume<RETVAL>::self.returned) {
      h.resume();
    } else {
      CoroutineAwaitResume<RETVAL>::self.to_resume.push_back(h);
    }
  }
};

class Sleep final {
 private:
  milliseconds const ms;

 public:
  explicit Sleep(milliseconds ms) : ms(ms) {}

  constexpr bool await_ready() noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    Executor().Schedule(ms, [h]() { h.resume(); });
  }

  void await_resume() noexcept {}
};

inline Coro<bool> IsEven(int x) {
  // Confirm multiple suspend/resume steps work just fine.
  // Just `co_return ((x % 2) == 0);` works too, of course.
  if ((x % 2) == 0) {
    co_await Sleep(1ms);
    co_await Sleep(1ms);
    co_return true;
  } else {
    co_await Sleep(1ms);
    co_await Sleep(1ms);
    co_await Sleep(1ms);
    co_return false;
  }
}

inline Coro<void> CallSleep(milliseconds ms) {
  co_await Sleep(ms);
  co_return;
}

inline Coro<int> Square(int x) {
  co_await CallSleep(1ms);
  co_return (x * x);
}

void RunExampleCoroutine() {
  function<Coro<>(string)> MultiStepFunction = [](string s) -> Coro<> {
    for (int i = 1; i <= 10; ++i) {
      co_await Sleep(100ms);
      cout << s << ", i=" << i << "/10, even=" << flush << ((co_await IsEven(i)) ? "true" : "false")
           << ", square=" << flush << co_await Square(i) << endl;
    }
  };

  ExecutorScope executor;

  {
    // Call the coroutine. The return object, of type `Coro<void>`, will go out of scope, which is normal.
    MultiStepFunction("The MultiStepFunction");
  }

  // The destructor of `ExecutorScope` will wait for the running coroutine(s) to complete.
  // To be more precise, it will wait until it is shut down gracefully, and it will be shut down gracefully
  // as soon as the last outstanding coroutine is done with its execution!
}

inline Coro<bool> IsDivisibleByThree(int value) {
  co_await Sleep(10ms);
  co_return (value % 3) == 0;
}

inline Coro<bool> IsDivisibleByFive(int value) {
  co_await Sleep(10ms);
  co_return (value % 5) == 0;
}

inline Coro<> CoroFizzBuzz(function<Coro<bool>(string)> next) {
  int value = 0;
  while (true) {
    ++value;
    Coro<bool> awaitable_d3 = IsDivisibleByThree(value);
    Coro<bool> awaitable_d5 = IsDivisibleByFive(value);
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
  auto t0 = TimestampMS();

  ExecutorScope executor;

  CoroFizzBuzz([&total, &t0](string s) -> Coro<bool> {
    auto t1 = TimestampMS();
    cout << ++total << " : " << s << ", in " << (t1 - t0) << "ms, from thread " << CurrentThreadName() << endl;
    t0 = t1;
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
