// Introducing the executor: the thread that runs all the async stuff.

// TODO(dkorolev): Illustrate the purpose of `[std::]move` everywhere.
// TODO(dkorolev): Remove `std::` everywhere in all files!

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

using std::cout;
using std::endl;
using std::function;
using std::move;
using std::queue;
using std::string;
using std::to_string;
using namespace std::chrono_literals;
using std::atomic_bool;
using std::atomic_int;
using std::future;
using std::lock_guard;
using std::make_shared;
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

struct ExecutorInstance {
  thread worker;
  TimestampMS const t0;

  bool done = false;

  mutex mut;

  // Store the jobs in a red-black tree, the `priority_queue` is not as clean syntax-wise in C++.
  multimap<int, function<void()>> jobs;

  ExecutorInstance() : worker([this]() { Thread(); }) {
  }

  void Schedule(milliseconds delay, function<void()> code) {
    lock_guard<mutex> lock(mut);
    jobs.emplace((TimestampMS() - t0) + delay.count(), move(code));
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
            function<void()> extracted = move(it->second);
            jobs.erase(it);
            return extracted;
          }
        }
      }
      // NOTE(dkorolev): This is a "busy wait" loop, but it does the job for illustrative purposes.
      //                 Can of course `wait` + `wait_for` on a `condition_variable` if this code goes to prod.
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

    function<void()> next_task;
    while (next_task = GetNextTask()) {
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
  void Next(function<void(string)> cb, function<void()> next) {
    struct AsyncCaller {
      FizzBuzzGenerator* self;
      function<void(string)> cb;
      function<void()> next;
      AsyncCaller(FizzBuzzGenerator* self, function<void(string)> cb, function<void()> next)
          : self(self), cb(move(cb)), next(move(next)) {
      }
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
          self->InvokeCbThenNext(move(cb), move(next));
        }
      }
    };
    if (!next_values.empty()) {
      InvokeCbThenNext(cb, next);
    } else {
      ++value;
      auto shared_async_caller_instance = make_shared<AsyncCaller>(this, move(cb), move(next));
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
  KeepGoing();
  // NOTE(dkorolev): Now we must wait, otherwise the destroyed instance of `g` will be used from other threads.
  unlocked_when_done.lock();
  Executor().GracefulShutdown();
}
