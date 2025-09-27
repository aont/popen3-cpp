# popen3-cpp

`popen3-cpp` provides a small, header-only C++ helper that launches a child
process with three dedicated pipes (stdin, stdout, stderr) on both Windows and
POSIX platforms. It is designed to be lightweight, dependency-free, and easy to
integrate into existing projects that need fine-grained control over child
process I/O without pulling in a larger process-management framework. The
library focuses on a minimal API surface while still offering escape hatches
for advanced use cases such as integrating with an event loop or wiring the
child process to pre-existing file descriptors.

## Repository layout

```
.
├── include/
│   └── popen3.hpp           # Cross-platform implementation
└── examples/
    ├── linux_ex?.cpp        # POSIX examples (g++/clang)
    ├── linux_asio_*.cpp     # Advanced POSIX samples
    ├── windows_ex?.cpp      # Windows examples (MSVC/MinGW)
    └── windows_asio_*.cpp   # Advanced Windows samples
```

The header automatically selects the appropriate implementation based on the
platform macros. Both implementations expose the same `tinyproc::popen3`
interface, so code written against the POSIX API should compile on Windows and
vice-versa.

## Quick start

The project is intentionally header-only. You can either add `include/` to your
include path or copy `popen3.hpp` into your project tree. A minimal program
looks like this:

```cpp
#include "popen3.hpp"
#include <iostream>
#include <string>

int main() {
    tinyproc::popen3 proc{"/usr/bin/env", {"python3", "-c", "print('hello')"}};

    // Write to the child's stdin if necessary.
    proc.stdin_stream() << "input" << std::flush;

    // Read a line of output and wait for the process to exit.
    std::string line;
    std::getline(proc.stdout_stream(), line);
    proc.wait();

    return 0;
}
```

`popen3` closes the parent's copy of unused pipe ends automatically. You can
explicitly close or duplicate handles/descriptors using the helper functions if
you need additional control.

## Building the examples

### Linux / POSIX

```bash
g++ -std=c++11 -Wall -Wextra -pedantic linux/ex1.cpp -o ex1
```

`ex1.cpp`, `ex2.cpp`, and `ex3.cpp` illustrate the basic API, non-blocking
interaction with child output, and redirecting the child's output to a file
descriptor. `linux_asio_coroutines.cpp` shows how to integrate two child
processes with [Asio standalone](https://think-async.com/) and C++20 coroutines
so their stdout streams are consumed concurrently:

```bash
g++ -std=c++20 -Wall -Wextra -pedantic -pthread \
    -I../include linux_asio_coroutines.cpp -o linux_asio_coroutines
```

### Windows

```powershell
cl /EHsc /std:c++17 windows\ex1.cpp
```

The Windows examples mirror the POSIX ones and demonstrate how to integrate the
class with Windows HANDLEs, overlapped I/O, and file redirection. Adjust the
compiler options to match your toolchain (MSVC, MinGW, etc.).

`windows_asio_coroutines.cpp` complements the Linux coroutine sample with a
version that uses `asio::windows::stream_handle` and overlapped pipes. Build it
with a C++20-capable MSVC or MinGW configuration, for example:

```powershell
cl /EHsc /std:c++20 /I ..\include windows_asio_coroutines.cpp
```

If you prefer a single command to build all samples for your platform, change
into the `examples/` directory and run `make`, `make linux`, or `make windows`.
The makefile detects the host platform and selects the appropriate set of
targets.

## Usage highlights

* Configure each of the child's standard streams independently via
  `popen3::options`.
* Choose anonymous pipes or reuse existing descriptors/handles.
* Access helper methods to read, write, close, and wait on child processes.
* Optional overlapped I/O support on Windows for non-blocking reads and writes.

See the example programs for end-to-end demonstrations of synchronous and
non-blocking workflows.

## License

This repository does not currently include an explicit license. If you plan to
use the code in your own project, please consult the repository owner or add an
appropriate license file.

## Contributing

Bug reports, questions, or suggestions can be opened as GitHub issues. Pull
requests are welcome—please include platform details and a brief description of
how you tested your changes so the maintainers can reproduce the setup easily.
