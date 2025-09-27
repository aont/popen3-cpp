# popen3-cpp

`popen3-cpp` provides a small, header-only C++ helper that launches a child
process with three dedicated pipes (stdin, stdout, stderr) on both Windows and
POSIX platforms. It is designed to be lightweight, dependency-free, and easy to
integrate into existing projects that need fine-grained control over child
process I/O without pulling in a larger process-management framework.

## Repository layout

```
.
├── popen3.hpp      # Cross-platform implementation
├── linux/          # POSIX examples (g++/clang)
└── windows/        # Windows examples (MSVC/MinGW)
```

The header automatically selects the appropriate implementation based on the
platform macros. Both implementations expose the same `tinyproc::popen3`
interface.

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
