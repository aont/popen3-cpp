#include "popen3.hpp"
#include <vector>
#include <string>
#include <cstdio>
#include <fcntl.h>

int main() {
    using namespace tinyproc;

    int fd = ::open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    popen3::options opt;
    opt.out = popen3::stream_spec::use_fd(fd); // Route the child's stdout to fd
    // (Child closes the fd after dup2; the parent keeps its descriptor)

    popen3 proc;
    std::vector<std::string> argv; argv.push_back("echo"); argv.push_back("hello");
    proc.start(argv, opt);
    proc.wait(0, 0);
    ::close(fd); // The parent is responsible for closing the fd
    return 0;
}
