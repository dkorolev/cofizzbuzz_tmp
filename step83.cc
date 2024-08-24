// Test setup, dummy code so far.

#include <iostream>

using std::cout;
using std::endl;

int main() {
#if defined(NDEBUG) && !defined(DEBUG)
  cout << "Running the NDEBUG build." << endl;
#elif defined(DEBUG) && !defined(NDEBUG)
  cout << "Running the DEBUG build." << endl;
#else
#error "Must have either `DEBUG` or `NDEBUG` `#define`-d."
#endif

  cout << "Hello, Coroutines!" << endl;
}
