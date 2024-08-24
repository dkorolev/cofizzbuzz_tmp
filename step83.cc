// Logging thread names, to illustrate the "work" is done by a large number of "dedicated" threads.

#include <iostream>
#include <string>
#include <functional>
#include <queue>
#include <thread>
#include <chrono>
#include <future>
#include <thread>
#include <atomic>

using std::cout;
using std::endl;
using std::function;
using std::queue;
using std::string;
using std::to_string;
using namespace std::chrono_literals;
using std::atomic_int;
using std::future;
using std::promise;
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
  void Next(function<void(string)> cb, function<void()> next) {
    auto const InvokeCbThenNext = [this, cb, next]() {
      cb(next_values.front());
      next_values.pop();
      next();
    };
    if (next_values.empty()) {
      ++value;
      promise<bool> pd3;
      promise<bool> pd5;
      IsDivisibleByThree(value, [&pd3](bool d3) { pd3.set_value(d3); });
      IsDivisibleByFive(value, [&pd5](bool d5) { pd5.set_value(d5); });
      bool const d3 = pd3.get_future().get();
      bool const d5 = pd5.get_future().get();
      if (d3) {
        next_values.push("Fizz");
      }
      if (d5) {
        next_values.push("Buzz");
      }
      if (!d3 && !d5) {
        next_values.push(to_string(value));
      }
      InvokeCbThenNext();
    } else {
      InvokeCbThenNext();
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

  function<void(string)> Print = [&total, &t0](string s) {
    auto t1 = TimestampMS();
    cout << ++total << " : " << s << ", in " << (t1 - t0) << "ms, from thread " << CurrentThreadName() << endl;
    t0 = t1;
  };

  function<void()> KeepGoing = [&]() {
    if (total < 15) {
      g.Next(Print, KeepGoing);
    }
  };

  // Kick off the run.
  // It will initiate the series of "call back-s", via the executor, from its thread.
  KeepGoing();
}
