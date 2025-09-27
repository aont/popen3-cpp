#include "popen3.hpp"
#include <vector>
#include <string>
#include <cstdio>

int main() {
    using namespace tinyproc;

    popen3::options opt; // 既定はすべて INHERIT
    popen3 proc;

    std::vector<std::string> argv;
    // Unix の "ls -la" 相当
    argv.push_back("cmd");
    argv.push_back("/c");
    argv.push_back("dir");

    if (!proc.start(argv, opt)) {
        std::fprintf(stderr, "start failed: %s (errno=%d)\n",
                     proc.last_error().c_str(), proc.last_errno());
        return 1;
    }
    int st = 0;
    proc.wait(&st, 0); // 無限待ち
    return (st == 0) ? 0 : 1;
}
