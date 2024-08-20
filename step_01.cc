// Initial FizzBuzz.
// Note how is prints eight, not seven rows.

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
  void Next(function<void(string)> cb) {
    ++value;
    bool const d3 = (value % 3 == 0);
    bool const d5 = (value % 3 == 0);
    if (d3) {
      cb("Fizz");
    }
    if (d5) {
      cb("Buzz");
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
  while (total < 7) {
    g.Next([&total](string s) {
      cout << ++total << " : " << s << endl;
    });
  }
}
