#include "popen3.hpp"
#include <vector>
#include <string>
#include <cstdio>

int main() {
    using namespace tinyproc;

    popen3::options opt; // Defaults to INHERIT for every stream
    popen3 proc;

    std::vector<std::string> argv;
    argv.push_back("ls");
    argv.push_back("-la");

    if (!proc.start(argv, opt)) {
        std::fprintf(stderr, "start failed: %s (errno=%d)\n",
                     proc.last_error().c_str(), proc.last_errno());
        return 1;
    }
    int st;
    proc.wait(&st, 0);
    return 0;
}
