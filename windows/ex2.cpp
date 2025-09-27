#include "popen3.hpp"
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <windows.h>

int main() {
    using namespace tinyproc;

    popen3::options opt;
    opt.in  = popen3::stream_spec::pipe();
    opt.out = popen3::stream_spec::pipe();
    opt.err = popen3::stream_spec::pipe();
    opt.parent_nonblock = true; // データが無い場合は read_* が 0 を返す

    popen3 proc;
    std::vector<std::string> argv;
    argv.push_back("cmd");
    argv.push_back("/c");
    argv.push_back("for /f usebackq^ delims^= %L in (`more`) do (echo OUT:%L & echo ERR:%L 1>&2)");

    if (!proc.start(argv, opt)) {
        std::fprintf(stderr, "start failed: %s (errno=%d)\n",
                     proc.last_error().c_str(), proc.last_errno());
        return 1;
    }

    // 親 -> 子 stdin
    const char* line = "hello\r\n";
    proc.write_stdin(line, std::strlen(line));
    // EOF 通知
    proc.close_stdin();

    // 同時読み取り（ポーリング）
    char buf[4096];
    for (;;) {
        ssize_t n1 = proc.read_stdout(buf, sizeof(buf));
        if (n1 > 0) std::fwrite(buf, 1, (size_t)n1, stdout);

        ssize_t n2 = proc.read_stderr(buf, sizeof(buf));
        if (n2 > 0) std::fwrite(buf, 1, (size_t)n2, stderr);

        if (!proc.alive()) {
            // 残りを吸いきる
            for (;;) {
                ssize_t m1 = proc.read_stdout(buf, sizeof(buf));
                ssize_t m2 = proc.read_stderr(buf, sizeof(buf));
                if (m1 <= 0 && m2 <= 0) break;
                if (m1 > 0) std::fwrite(buf, 1, (size_t)m1, stdout);
                if (m2 > 0) std::fwrite(buf, 1, (size_t)m2, stderr);
            }
            break;
        }

        // CPU を荒らさないよう少し待つ
        Sleep(10);
    }

    int st = 0;
    proc.wait(&st, 0);
    return 0;
}
