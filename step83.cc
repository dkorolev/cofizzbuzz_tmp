// Introduced (synchronous) `IsDivisibleBy{Three,Five}`.

#include <iostream>
#include <string>
#include <functional>
#include <queue>

using std::cout;
using std::endl;
using std::function;
using std::queue;
using std::string;
using std::to_string;

inline bool IsDivisibleByThree(int value) { return (value % 3) == 0; }

inline bool IsDivisibleByFive(int value) { return (value % 5) == 0; }

struct FizzBuzzGenerator {
  int value = 0;
  queue<string> next_values;
  void Next(function<void(string)> cb) {
    if (next_values.empty()) {
      ++value;
      bool const d3 = IsDivisibleByThree(value);
      bool const d5 = IsDivisibleByFive(value);
      if (d3) {
        next_values.push("Fizz");
      }
      if (d5) {
        next_values.push("Buzz");
      }
      if (!d3 && !d5) {
        next_values.push(to_string(value));
      }
    }
    cb(next_values.front());
    next_values.pop();
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
  while (total < 15) {
    g.Next([&total](string s) { cout << ++total << " : " << s << endl; });
  }
}
