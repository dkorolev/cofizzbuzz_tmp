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
#include <type_traits>

using std::cout;
using std::endl;
// using std::function;
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

// TODO(dkorolev): For production-ready code:
// 1) put `std::` back everywhere
// 2) wait on the condition variable, not in the busy-wait loop
// 3) use `std::priority_queue`
// 4) put `final` everywhere
// 5) mark unused constructors and assignment operators `=delete`-d everywhere.

template <class F>
using function_t = decltype(std::function(std::declval<F>()));

template <class CTX, class F>
struct MovableFunctionInnerImpl;

template <class CTX, class F>
struct MovableFunctionInnerImpl<CTX, std::function<F>> {
  using ctx_t = CTX;
  using f_t = std::function<F>;

  ctx_t ctx;
  f_t f;

  MovableFunctionInnerImpl(std::nullptr_t) : f(nullptr) {}
  MovableFunctionInnerImpl(ctx_t ctx, f_t f) : ctx(std::move(ctx)), f(f) {}
  
  MovableFunctionInnerImpl(MovableFunctionInnerImpl&&) = default;
  MovableFunctionInnerImpl& operator=(MovableFunctionInnerImpl&&) = default;
  
  MovableFunctionInnerImpl(MovableFunctionInnerImpl const&) = delete;
  MovableFunctionInnerImpl& operator=(MovableFunctionInnerImpl const&) = delete;

  operator bool const() { return f != nullptr; }

  template <typename... ARGS>
  std::invoke_result_t<F, ARGS...> operator()(ARGS&&... args) const {
    return f(ctx, std::forward<ARGS>(args)...);
  }
};

template <class CTX, class F>
using MovableFunctionImpl = MovableFunctionInnerImpl<CTX, std::function<F>>;

template <class CTX, class F>
MovableFunctionInnerImpl<CTX, function_t<F>> ConstructF(CTX ctx, F&& f) {
  return MovableFunctionInnerImpl<CTX, function_t<F>>(std::move(ctx), std::forward<F>(f));
}

template <class CTX, class F>
MovableFunctionInnerImpl<CTX, F> MoveF(MovableFunctionInnerImpl<CTX, F>& from) {
  return std::move(from);
}

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

struct ExecutorInstance {
  thread worker;
  TimestampMS const t0;

  bool done = false;

  mutex mut;

  // Store the jobs in a red-black tree, the `priority_queue` is not as clean syntax-wise in C++.
  multimap<int, MovableFunctionImpl<void()>> jobs;

  ExecutorInstance() : worker([this]() { Thread(); }) {
  }

  void Schedule(milliseconds delay, MovableFunctionImpl<void()> code) {
    lock_guard<mutex> lock(mut);
    jobs.emplace((TimestampMS() - t0) + delay.count(), code);
  }

  MovableFunctionImpl<void()> GetNextTask() {
    while (true) {
      {
        lock_guard<mutex> lock(mut);
        if (done) {
          return nullptr;
        }
        if (!jobs.empty()) {
          auto it = jobs.begin();
          if ((TimestampMS() - t0) >= it->first) {
            MovableFunctionImpl<void()> extracted = MoveF(it->second);
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
      MovableFunctionImpl<void()> next_task = GetNextTask();
      if (!next_task) {
        return;
      }
      next_task();
    }
  }

  void GracefulShutdown() {
    {
      lock_guard<mutex> lock(mut);
      done = true;
    }
    worker.join();
  }
};

inline ExecutorInstance& Executor() {
  static ExecutorInstance singleton_executor;
  return singleton_executor;
}

inline void IsDivisibleByThree(int value, MovableFunctionImpl<void(bool)> cb) {
  MovableFunctionImpl<MovableFunctionImpl<void(bool)>, void()> f = ConstructF(MoveF(cb), [value](
  MovableFunctionImpl<void(bool)>& cb
  ) { 
    cb((value % 3) == 0);
  });
  Executor().Schedule(10ms, MoveF(f)); // MoveF([value, cb2 = MoveF(cb)]() { cb2((value % 3) == 0); }));
}

inline void IsDivisibleByFive(int value, MovableFunctionImpl<void(bool)> cb) {
  MovableFunctionImpl<MovableFunctionImpl<void(bool)>, void()> f = ConstructF(MoveF(cb), [value](MovableFunctionImpl<void(bool)>& cb) {
    cb((value % 5) == 0);
  });
  Executor().Schedule(10ms, MoveF(f)); // [=]() { cb((value % 5) == 0); });
}

struct FizzBuzzGenerator {
  int value = 0;
  bool done = false;
  queue<string> next_values;
  void InvokeCbThenNext(MovableFunctionImpl<void(string)> const& cb, MovableFunctionImpl<void()> const& next) {
    cb(next_values.front());
    next_values.pop();
    next();
  }
  struct AsyncNextStepLogic {
    FizzBuzzGenerator* self;
    MovableFunctionImpl<void(string)> cb;
    MovableFunctionImpl<void()> next;
    AsyncNextStepLogic(FizzBuzzGenerator* self, MovableFunctionImpl<void(string)> cb, MovableFunctionImpl<void()> next)
        : self(self), cb(MoveF(cb)), next(MoveF(next)) {
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
  void Next(MovableFunctionImpl<void(string)> const& cb, MovableFunctionImpl<void()> const& next) {
    if (!next_values.empty()) {
      InvokeCbThenNext(cb, next);
    } else {
      ++value;
      // Need a shared instance so that it outlives both the called and either of the async calls.
      auto shared_async_caller_instance = make_shared<AsyncNextStepLogic>(this, cb, next);
      IsDivisibleByThree(value, ConstructF(shared_async_caller_instance, [](shared_ptr<AsyncNextStepLogic>& ctx, bool d3) {
        ctx->SetD3(d3);
      }));
      IsDivisibleByFive(value, ConstructF(shared_async_caller_instance, [](shared_ptr<AsyncNextStepLogic>& ctx, bool d5) {
        ctx->SetD5(d5);
      }));
    }
  }
};

template <typename T1, typename T2>
struct is_same_or_compile_error {
  enum { value = std::is_same_v<T1, T2> };
  char is_same_static_assert_failed[value ? 1 : -1];
};
#define CURRENT_FAIL_IF_NOT_SAME_TYPE(A, B) static_assert(sizeof(is_same_or_compile_error<A, B>), "")

int main() {
#if defined(NDEBUG) && !defined(DEBUG)
  cout << "Running the NDEBUG build." << endl;
#elif defined(DEBUG) && !defined(NDEBUG)
  cout << "Running the DEBUG build." << endl;
#else
#error "Must have either `DEBUG` or `NDEBUG` `#define`-d."
#endif

  CurrentThreadName() = "main()";

  FizzBuzzGenerator g;
  int total = 0;
  auto t0 = TimestampMS();
  // A quick & hacky way to wait until everything is done.
  mutex unlocked_when_done;
  unlocked_when_done.lock();

  auto tmp = [&total, &t0](string s) {
    auto t1 = TimestampMS();
    cout << ++total << " : " << s << ", in " << (t1 - t0) << "ms, from thread " << CurrentThreadName() << endl;
    t0 = t1;
  };

  auto Print = ConstructF([&total, &t0](string s) {
    auto t1 = TimestampMS();
    cout << ++total << " : " << s << ", in " << (t1 - t0) << "ms, from thread " << CurrentThreadName() << endl;
    t0 = t1;
  });

  MovableFunctionImpl<void()> KeepGoing = ConstructF([&]() {
    if (total < 15) {
      g.Next(Print, KeepGoing);
    } else {
      unlocked_when_done.unlock();
    }
  });
  KeepGoing();

  // NOTE(dkorolev): Now we must wait, otherwise the destroyed instance of `g` will be used from other threads.
  unlocked_when_done.lock();
  Executor().GracefulShutdown();
}
