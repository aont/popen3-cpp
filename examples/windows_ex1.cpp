#include "popen3.hpp"
#include <vector>
#include <string>
#include <cstdio>

int main() {
    using namespace tinyproc;

    popen3::options opt; // Defaults to INHERIT for every stream
    popen3 proc;

    std::vector<std::string> argv;
    // Equivalent to the Unix "ls -la"
    argv.push_back("cmd");
    argv.push_back("/c");
    argv.push_back("dir");

    if (!proc.start(argv, opt)) {
        std::fprintf(stderr, "start failed: %s (errno=%d)\n",
                     proc.last_error().c_str(), proc.last_errno());
        return 1;
    }
    int st = 0;
    proc.wait(&st, 0); // Wait indefinitely
    return (st == 0) ? 0 : 1;
}
