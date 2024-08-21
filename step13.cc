// Introducing the executor: the thread that runs all the async stuff.

#include <iostream>
#include <string>
#include <functional>
#include <queue>
#include <thread>
#include <chrono>
#include <future>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <coroutine>

using std::cout;
using std::endl;
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
using std::shared_ptr;
using std::terminate;
using std::thread;
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
  explicit TimestampMS() : time_point(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()) {
  }
  int operator-(TimestampMS const& rhs) const {
    return int((time_point - rhs.time_point).count());
  }
};

class ExecutorInstance;

struct ExecutorThreadLocalPlaceholder {
  mutex mut;
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
};

ExecutorThreadLocalPlaceholder& ExecutorForThisThread() {
  static thread_local ExecutorThreadLocalPlaceholder p;
  return p;
}

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

 protected:
  friend class ExecutorScope;
  ExecutorInstance() : worker([this]() { Thread(); }) {
    unlock_when_done.lock();
  }

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

class ExecutorScope {
 private:
  // The instance of the executor is created and owned by `ExecutorScope`.
  ExecutorInstance executor;

 public:
  ExecutorScope() {
    ExecutorForThisThread().Set(executor);
  }

  ~ExecutorScope() {
    ExecutorForThisThread().Unset(executor);
  }
};

inline ExecutorInstance& Executor() {
  return ExecutorForThisThread().Instance();
}

inline void IsDivisibleByThree(int value, function<void(bool)> cb) {
  Executor().Schedule(10ms, [=]() { cb((value % 3) == 0); });
}

inline void IsDivisibleByFive(int value, function<void(bool)> cb) {
  Executor().Schedule(10ms, [=]() { cb((value % 5) == 0); });
}

struct FizzBuzzGenerator {
  int value = 0;
  bool done = false;
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
        : self(self), cb(cb), next(next) {
    }
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

// The "minimalistic" coroutine runner.
// It does `.resume()` once, and then waits until `final_suspend` was invoked on its `promise_type`.
struct ResumeOnceTask {
  struct promise_type {
    mutex unlocked_when_coro_done;

    using handle_type = std::coroutine_handle<promise_type>;

    ResumeOnceTask get_return_object() {
      return ResumeOnceTask(handle_type::from_promise(*this), unlocked_when_coro_done);
    }

    std::suspend_always initial_suspend() noexcept {
      return {};
    }

    std::suspend_never final_suspend() noexcept {
      unlocked_when_coro_done.unlock();
      return {};
    }

    void return_void() noexcept {
    }

    void unhandled_exception() noexcept {
    }
  };

  explicit ResumeOnceTask(promise_type::handle_type coro, mutex& unlocked_when_coro_done)
      : coro(coro), unlocked_when_coro_done(unlocked_when_coro_done) {
    unlocked_when_coro_done.lock();
  }

  void RunToCompletion() noexcept {
    // Resume once.
    coro.resume();
    // Wait until the coroutine completes.
    unlocked_when_coro_done.lock();
  }

 private:
  promise_type::handle_type coro;
  mutex& unlocked_when_coro_done;
};

class Sleep final {
 private:
  milliseconds const ms;

 public:
  explicit Sleep(milliseconds ms) : ms(ms) {
  }

  constexpr bool await_ready() noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> h) {
    thread([ms = this->ms, h]() {
      sleep_for(ms);
      h.resume();
    }).detach();
  }

  void await_resume() {
  }
};

void RunExampleCoroutine() {
  function<ResumeOnceTask(string)> MultiStepFunction = [](string s) -> ResumeOnceTask {
    for (int i = 1; i <= 10; ++i) {
      co_await Sleep(100ms);
      cout << s << ", i=" << i << "/10." << endl;
    }
  };

  ResumeOnceTask task = MultiStepFunction("The MultiStepFunction");
  task.RunToCompletion();
}

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
