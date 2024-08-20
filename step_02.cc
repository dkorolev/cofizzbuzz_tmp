// Introduced `bool done = false`.
// The ugly way to stop at seven.

#include <iostream>
#include <string>
#include <functional>

using std::cout;
using std::endl;
using std::string;
using std::to_string;
using std::function;

struct FizzBuzzGenerator {
  int value = 0;
  bool done = false;
  void Next(function<void(string)> cb) {
    ++value;
    bool const d3 = (value % 3 == 0);
    bool const d5 = (value % 3 == 0);
    if (d3) {
      cb("Fizz");
      if (done) {
        return;
      }
    }
    if (d5) {
      cb("Buzz");
      if (done) {
        return;
      }
    }
    if (!d3 && !d5) {
      cb(to_string(value));
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
  while (!g.done) {
    g.Next([&total, &g](string s) {
      cout << ++total << " : " << s << endl;
      if (total >= 7) {
        g.done = true;
      }
    });
  }
}
