#include "popen3.hpp"
#include <vector>
#include <string>
#include <cstdio>

int main() {
    using namespace tinyproc;

    popen3::options opt;
    opt.in  = popen3::stream_spec::pipe();
    opt.out = popen3::stream_spec::pipe();
    opt.err = popen3::stream_spec::pipe();
    // 非ブロッキングにして select/poll が使いやすいように
    opt.parent_nonblock = true;

    popen3 proc;
    std::vector<std::string> argv;
    argv.push_back("bash");
    argv.push_back("-c");
    argv.push_back("while read L; do echo OUT:$L; echo ERR:$L 1>&2; done");

    if (!proc.start(argv, opt)) { /* error handling */ }

    // 変更点：write の直後に close_stdin() を追加
    const char* line = "hello\n";
    ssize_t wn = proc.write_stdin(line, std::strlen(line));
    if (wn < 0) {
        std::perror("write_stdin");
    } 
    // ここで EOF を伝えるために親側の書き込み端を閉じる
    proc.close_stdin();

    // 以降は stdout/stderr を読み尽くして子の終了を待つループ
    fd_set rfds;
    for (;;) {
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (proc.stdout_fd() != -1) { FD_SET(proc.stdout_fd(), &rfds); if (proc.stdout_fd() > maxfd) maxfd = proc.stdout_fd(); }
        if (proc.stderr_fd() != -1) { FD_SET(proc.stderr_fd(), &rfds); if (proc.stderr_fd() > maxfd) maxfd = proc.stderr_fd(); }
        if (maxfd < 0) break; // もう読むものがない

        int r = ::select(maxfd + 1, &rfds, 0, 0, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { std::perror("select"); break; }

        char buf[4096];
        if (proc.stdout_fd() != -1 && FD_ISSET(proc.stdout_fd(), &rfds)) {
            ssize_t n = proc.read_stdout(buf, sizeof(buf));
            if (n > 0) ::write(1, buf, n);
            else proc.close_stdout();
        }
        if (proc.stderr_fd() != -1 && FD_ISSET(proc.stderr_fd(), &rfds)) {
            ssize_t n = proc.read_stderr(buf, sizeof(buf));
            if (n > 0) ::write(2, buf, n);
            else proc.close_stderr();
        }

        // 子プロセスが終了していて、読み取り用 FD も閉じられたらループ終了
        if (!proc.alive() && proc.stdout_fd() == -1 && proc.stderr_fd() == -1) break;
    }

    int st = 0;
    proc.wait(&st, 0);

    return 0;
}
