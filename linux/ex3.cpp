#include "popen3.hpp"
#include <vector>
#include <string>
#include <cstdio>
#include <fcntl.h>

int main() {
    using namespace tinyproc;

    int fd = ::open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    popen3::options opt;
    opt.out = popen3::stream_spec::use_fd(fd); // 子の stdout を fd に差し替え
    // （子では dup2 の後、fd をクローズ。親の fd はそのまま）

    popen3 proc;
    std::vector<std::string> argv; argv.push_back("echo"); argv.push_back("hello");
    proc.start(argv, opt);
    proc.wait(0, 0);
    ::close(fd); // 親の責務でクローズ
    return 0;
}
