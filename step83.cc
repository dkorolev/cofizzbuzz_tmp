// Explicitly verbose, ugly, event-driven logic. Work still initiated and executed by dedicated threads.

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
using std::mutex;
using std::promise;
using std::shared_ptr;
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
  explicit TimestampMS() : time_point(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()) {}
  int operator-(TimestampMS const& rhs) const { return int((time_point - rhs.time_point).count()); }
};

static atomic_int idx_d3 = 0;
inline void IsDivisibleByThree(int value, function<void(bool)> cb) {
  // NOTE(dkorolev): Could use `[=]`, but want to keep it readable.
  thread(
      [](int value, function<void(bool)> cb) {
        CurrentThreadName() = "IsDivisibleByThree[" + to_string(++idx_d3) + ']';
        sleep_for(10ms);
        cb((value % 3) == 0);
      },
      value,
      cb)
      .detach();
}

static atomic_int idx_d5 = 0;
inline void IsDivisibleByFive(int value, function<void(bool)> cb) {
  thread(
      [](int value, function<void(bool)> cb) {
        CurrentThreadName() = "IsDivisibleByFive[" + to_string(++idx_d5) + ']';
        sleep_for(10ms);
        cb((value % 5) == 0);
      },
      value,
      cb)
      .detach();
}

struct FizzBuzzGenerator {
  int value = 0;
  queue<string> next_values;
  void InvokeCbThenNext(function<void(string)> cb, function<void()> next) {
    cb(next_values.front());
    next_values.pop();
    next();
  }
  void Next(function<void(string)> cb, function<void()> next) {
    struct AsyncCaller {
      FizzBuzzGenerator* self;
      function<void(string)> cb;
      function<void()> next;
      AsyncCaller(FizzBuzzGenerator* self, function<void(string)> cb, function<void()> next)
          : self(self), cb(std::move(cb)), next(std::move(next)) {}
      mutex mut;
      bool has_d3 = false;
      bool has_d5 = false;
      bool d3;
      bool d5;
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
          self->InvokeCbThenNext(std::move(cb), std::move(next));
        }
      }
    };
    if (!next_values.empty()) {
      InvokeCbThenNext(cb, next);
    } else {
      ++value;
      auto shared_async_caller_instance = make_shared<AsyncCaller>(this, std::move(cb), std::move(next));
      IsDivisibleByThree(value, [shared_async_caller_instance](bool d3) {
        lock_guard<mutex> lock(shared_async_caller_instance->mut);
        shared_async_caller_instance->d3 = d3;
        shared_async_caller_instance->has_d3 = true;
        shared_async_caller_instance->ActIfHasAllInputs();
      });
      IsDivisibleByFive(value, [shared_async_caller_instance](bool d5) {
        lock_guard<mutex> lock(shared_async_caller_instance->mut);
        shared_async_caller_instance->d5 = d5;
        shared_async_caller_instance->has_d5 = true;
        shared_async_caller_instance->ActIfHasAllInputs();
      });
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

  FizzBuzzGenerator g;
  int total = 0;
  auto t0 = TimestampMS();

  // A quick & hacky way to wait until everything is done.
  mutex unlocked_when_done;
  unlocked_when_done.lock();

  function<void(string)> Print = [&total, &t0](string s) {
    auto t1 = TimestampMS();
    cout << ++total << " : " << s << ", in " << (t1 - t0) << "ms, from thread " << CurrentThreadName() << endl;
    t0 = t1;
  };

  function<void()> KeepGoing = [&]() {
    if (total < 15) {
      g.Next(Print, KeepGoing);
    } else {
      unlocked_when_done.unlock();
    }
  };

  // Kick off the run.
  // It will initiate the series of "call back-s", via the executor, from its thread.
  KeepGoing();

  // NOTE(dkorolev): Now we must wait, otherwise the destroyed instance of `g` will be used from other threads.
  unlocked_when_done.lock();
}
