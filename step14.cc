// Leveraging the thread-local-singleton executor in the coroutine-handling code.

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
using std::vector;
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
struct Coroutine {
  struct promise_type : CoroutineLifetime {
    unique_ptr<ExecutorCoroutineScope> coroutine_executor_lifetime;

    Coroutine get_return_object() {
      if (coroutine_executor_lifetime) {
        // Internal error, should only have one `get_return_object` call per instance.
        terminate();
      }
      coroutine_executor_lifetime = make_unique<ExecutorCoroutineScope>(this);
      return Coroutine();
    }

    std::suspend_always initial_suspend() noexcept {
      // Should be `.resume()`-d via `.ResumeFromExecutorWorkerThread()` from the executor.
      return {};
    }

    std::suspend_never final_suspend() noexcept {
      coroutine_executor_lifetime = nullptr;
      return {};
    }

    void return_void() noexcept {}

    void unhandled_exception() noexcept { terminate(); }

    void ResumeFromExecutorWorkerThread() override {
      std::coroutine_handle<promise_type>::from_promise(*this).resume();
    }
  };
};

struct CoroutineBool {
  struct promise_type : CoroutineLifetime {
    unique_ptr<ExecutorCoroutineScope> coroutine_executor_lifetime;
    mutex mut;
    bool returned = false;
    bool value;
    vector<std::coroutine_handle<>> to_resume;  // Other coroutines waiting awaiting on this one returning.

    CoroutineBool get_return_object() {
      if (coroutine_executor_lifetime) {
        // Internal error, should only have one `get_return_object` call per instance.
        terminate();
      }
      coroutine_executor_lifetime = make_unique<ExecutorCoroutineScope>(this);
      return CoroutineBool(*this);
    }

    std::suspend_always initial_suspend() noexcept {
      // Should be `.resume()`-d via `.ResumeFromExecutorWorkerThread()` from the executor.
      return {};
    }

    std::suspend_never final_suspend() noexcept {
      coroutine_executor_lifetime = nullptr;
      return {};
    }

    void return_value(bool v) noexcept {
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

    void unhandled_exception() noexcept { terminate(); }

    void ResumeFromExecutorWorkerThread() override {
      std::coroutine_handle<promise_type>::from_promise(*this).resume();
    }
  };

  promise_type& self;
  explicit CoroutineBool(promise_type& self) : self(self) {}

  bool await_ready() noexcept {
    lock_guard<mutex> lock(self.mut);
    return self.returned;
  }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    lock_guard<mutex> lock(self.mut);
    if (self.returned) {
      h.resume();
    } else {
      self.to_resume.push_back(h);
    }
  }

  bool await_resume() noexcept {
    lock_guard<mutex> lock(self.mut);
    if (!self.returned) {
      // Internal error: `await_resume()` should only be called once the result is available.
      terminate();
    }
    return self.value;
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

inline CoroutineBool IsEven(int x) {
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

void RunExampleCoroutine() {
  function<Coroutine(string)> MultiStepFunction = [](string s) -> Coroutine {
    for (int i = 1; i <= 10; ++i) {
      co_await Sleep(100ms);
      cout << s << ", i=" << i << "/10, even=" << flush << ((co_await IsEven(i)) ? "true" : "false") << "." << endl;
    }
  };

  ExecutorScope executor;

  {
    // Call the coroutine. The return object, of type `Coroutine`, will go out of scope, which is normal.
    MultiStepFunction("The MultiStepFunction");
  }

  // The destructor of `ExecutorScope` will wait for the running coroutine(s) to complete.
  // To be more precise, it will wait until it is shut down gracefully, and it will be shut down gracefully
  // as soon as the last outstanding coroutine is done with its execution!
}

inline void IsDivisibleByThree(int value, function<void(bool)> cb) {
  Executor().Schedule(10ms, [=]() { cb((value % 3) == 0); });
}

inline void IsDivisibleByFive(int value, function<void(bool)> cb) {
  Executor().Schedule(10ms, [=]() { cb((value % 5) == 0); });
}

struct FizzBuzzGenerator {
  int value = 0;
  queue<string> next_values;
  void InvokeCbThenNext(function<void(string)> cb, function<void()> next) {
    cb(next_values.front());
    next_values.pop();
    next();
  }
  struct AsyncNextStepLogic {
    FizzBuzzGenerator* self;
    function<void(string)> cb;
    function<void()> next;
    AsyncNextStepLogic(FizzBuzzGenerator* self, function<void(string)> cb, function<void()> next)
        : self(self), cb(cb), next(next) {}
    mutex mut;
    bool has_d3 = false;
    bool has_d5 = false;
    bool d3;
    bool d5;
    void SetD3(bool d3_value) {
      lock_guard<mutex> lock(mut);
      d3 = d3_value;
      has_d3 = true;
      ActIfHasAllInputs();
    }
    void SetD5(bool d5_value) {
      lock_guard<mutex> lock(mut);
      d5 = d5_value;
      has_d5 = true;
      ActIfHasAllInputs();
    }
    void ActIfHasAllInputs() {
      if (has_d3 && has_d5) {
        if (d3) {
          self->next_values.push("Fizz");
        }
        if (d5) {
          self->next_values.push("Buzz");
        }
        if (!d3 && !d5) {
          self->next_values.push(to_string(self->value));
        }
        self->InvokeCbThenNext(cb, next);
      }
    }
  };
  void Next(function<void(string)> cb, function<void()> next) {
    if (!next_values.empty()) {
      InvokeCbThenNext(cb, next);
    } else {
      ++value;
      // Need a shared instance so that it outlives both the called and either of the async calls.
      auto shared_async_caller_instance = make_shared<AsyncNextStepLogic>(this, cb, next);
      IsDivisibleByThree(value, [shared_async_caller_instance](bool d3) { shared_async_caller_instance->SetD3(d3); });
      IsDivisibleByFive(value, [shared_async_caller_instance](bool d5) { shared_async_caller_instance->SetD5(d5); });
    }
  }
};

int main() {
#if defined(NDEBUG) && !defined(DEBUG)
  cout << "Running the NDEBUG build." << endl;
#elif defined(DEBUG) && !defined(NDEBUG)
  cout << "Running the DEBUG build." << endl;
#else
#error "Must have either `DEBUG` or `NDEBUG` `#define`-d."
#endif

  CurrentThreadName() = "main()";

  RunExampleCoroutine();
  return 0;

  FizzBuzzGenerator g;
  int total = 0;
  auto t0 = TimestampMS();

  function<void(string)> Print = [&total, &t0](string s) {
    auto t1 = TimestampMS();
    cout << ++total << " : " << s << ", in " << (t1 - t0) << "ms, from thread " << CurrentThreadName() << endl;
    t0 = t1;
  };

  function<void()> KeepGoing = [&]() {
    if (total < 15) {
      g.Next(Print, KeepGoing);
    } else {
      Executor().GracefulShutdown();
    }
  };

  // Create an executor for the scope of `main()`.
  // NOTE(dkorolev): Important that this line happens after `Print` and `KeepGoing` are declared!
  // Since otherwise they will be destructed before the instance of the `executor`, welcome to the horrors of C++.
  ExecutorScope executor;

  // Kick off the run.
  // It will initiate the series of "call back-s", via the executor, from its thread.
  KeepGoing();

  cout << "main() done, but will wait for the executor to complete its tasks." << endl;
}
