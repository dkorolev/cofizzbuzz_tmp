// Introduced the "cache for the yet unseen value". Stops at 15 now.

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

struct FizzBuzzGenerator {
  int value = 0;
  queue<string> next_values;
  void Next(function<void(string)> cb) {
    if (next_values.empty()) {
      ++value;
      bool const d3 = (value % 3 == 0);
      bool const d5 = (value % 5 == 0);
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
