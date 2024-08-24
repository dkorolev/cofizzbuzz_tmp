// Measuring the time each step takes, confirming it's 20ms == 10ms + 10ms.

#include <iostream>
#include <string>
#include <functional>
#include <queue>
#include <thread>
#include <chrono>

using std::cout;
using std::endl;
using std::function;
using std::queue;
using std::string;
using std::to_string;
using namespace std::chrono_literals;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
using std::this_thread::sleep_for;

struct TimestampMS final {
  milliseconds time_point;
  explicit TimestampMS() : time_point(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()) {}
  int operator-(TimestampMS const& rhs) const { return int((time_point - rhs.time_point).count()); }
};

inline void IsDivisibleByThree(int value, function<void(bool)> cb) {
  sleep_for(10ms);
  cb((value % 3) == 0);
}

inline void IsDivisibleByFive(int value, function<void(bool)> cb) {
  sleep_for(10ms);
  cb((value % 5) == 0);
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
      IsDivisibleByThree(value, [this, InvokeCbThenNext](bool d3) {
        IsDivisibleByFive(value, [this, InvokeCbThenNext, d3](bool d5) {
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
        });
      });
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

  FizzBuzzGenerator g;
  int total = 0;
  auto t0 = TimestampMS();

  function<void(string)> Print = [&total, &t0](string s) {
    auto t1 = TimestampMS();
    cout << ++total << " : " << s << " (in " << (t1 - t0) << "ms)" << endl;
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
