#ifndef TINYPROC_POPEN3_HPP
#define TINYPROC_POPEN3_HPP

// C++03 / Linux-only
#include <vector>
#include <string>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>

namespace tinyproc {

class popen3 {
public:
    struct stream_spec {
        enum mode_t { INHERIT, PIPE, USE_FD } mode;
        int fd; // only for USE_FD
        stream_spec() : mode(INHERIT), fd(-1) {}
        static stream_spec inherit() { stream_spec s; s.mode = INHERIT; return s; }
        static stream_spec pipe()    { stream_spec s; s.mode = PIPE;    return s; }
        static stream_spec use_fd(int child_fd_source) {
            stream_spec s; s.mode = USE_FD; s.fd = child_fd_source; return s;
        }
    };

    struct options {
        stream_spec in;   // child's stdin  (0)
        stream_spec out;  // child's stdout (1)
        stream_spec err;  // child's stderr (2)

        // 親側(パイプ終端)を非ブロッキングにする
        bool parent_nonblock;

        // 子プロセスのカレントディレクトリを変更（空文字なら変更なし）
        std::string chdir_to;

        // 子プロセスの環境変数の調整
        // clear_env が true の場合、いったん環境を空にしてから env_kv を適用
        bool clear_env;
        std::vector<std::string> env_kv; // 各要素は "KEY=VALUE" 形式

        // setpgid を行う（プロセスグループ分離などに）
        bool setpgid;
        pid_t pgid; // 0 なら自分をリーダに

        options()
        : parent_nonblock(false), clear_env(false),
          setpgid(false), pgid(0) {}
    };

public:
    popen3()
    : pid_(-1),
      in_w_(-1), out_r_(-1), err_r_(-1),
      own_in_w_(false), own_out_r_(false), own_err_r_(false),
      last_errno_(0) {}

    ~popen3() {
        // FD を閉じる
        close_stdin();
        close_stdout();
        close_stderr();
        // ゾンビ対策：非同期に waitpid(WNOHANG)
        if (pid_ > 0) {
            int status;
            ::waitpid(pid_, &status, WNOHANG);
        }
    }

    // 起動：argv は ["prog", "arg1", ...]、空でないこと
    // 戻り値：成功 true / 失敗 false（詳細は last_error() / last_errno()）
    bool start(const std::vector<std::string>& argv, const options& opt = options()) {
        clear_last_error_();

        if (argv.empty()) {
            set_last_error_("argv is empty", EINVAL);
            return false;
        }

        // ---- 準備：必要なパイプの作成 ----
        int in_pipe[2]  = { -1, -1 }; // parent writes -> child reads (stdin)
        int out_pipe[2] = { -1, -1 }; // child writes  -> parent reads (stdout)
        int err_pipe[2] = { -1, -1 }; // child writes  -> parent reads (stderr)

        if (opt.in.mode  == stream_spec::PIPE && ::pipe(in_pipe)  != 0)  return fail_perror_("pipe(stdin)");
        if (opt.out.mode == stream_spec::PIPE && ::pipe(out_pipe) != 0)  { safe_close_pair_(in_pipe);  return fail_perror_("pipe(stdout)"); }
        if (opt.err.mode == stream_spec::PIPE && ::pipe(err_pipe) != 0)  { safe_close_pair_(in_pipe); safe_close_pair_(out_pipe); return fail_perror_("pipe(stderr)"); }

        // exec 失敗通知用パイプ（child->parent に errno を送る）
        int exerr[2] = { -1, -1 };
        if (::pipe(exerr) != 0) {
            safe_close_pair_(in_pipe); safe_close_pair_(out_pipe); safe_close_pair_(err_pipe);
            return fail_perror_("pipe(exec_err)");
        }
        // 親子とも CLOEXEC 付与（成功した exec 後に自動的に閉じる）
        set_cloexec_(exerr[0]);
        set_cloexec_(exerr[1]);

        // 親側に残るパイプ終端は CLOEXEC を付与（FD リーク抑止）
        if (opt.in.mode  == stream_spec::PIPE) set_cloexec_(in_pipe[1]);
        if (opt.out.mode == stream_spec::PIPE) set_cloexec_(out_pipe[0]);
        if (opt.err.mode == stream_spec::PIPE) set_cloexec_(err_pipe[0]);

        // ---- fork ----
        pid_t p = ::fork();
        if (p < 0) {
            safe_close_pair_(in_pipe); safe_close_pair_(out_pipe); safe_close_pair_(err_pipe);
            safe_close_pair_(exerr);
            return fail_perror_("fork");
        }

        if (p == 0) {
            // -------- child --------
            // exerr[0] は子では使わない
            ::close(exerr[0]);

            // 子側パイプ終端の CLOEXEC：exec 成功時に自動的に閉じる
            if (opt.in.mode  == stream_spec::PIPE) set_cloexec_(in_pipe[0]);
            if (opt.out.mode == stream_spec::PIPE) set_cloexec_(out_pipe[1]);
            if (opt.err.mode == stream_spec::PIPE) set_cloexec_(err_pipe[1]);

            // 標準入出力の付け替え
            if (!setup_child_stdio_(opt, in_pipe, out_pipe, err_pipe, exerr[1])) _exit(127);

            // 環境変数の調整
            if (!apply_child_env_(opt, exerr[1])) _exit(127);

            // chdir
            if (!opt.chdir_to.empty()) {
                if (::chdir(opt.chdir_to.c_str()) != 0) {
                    write_errno_and_exit_(exerr[1], "chdir");
                }
            }

            // setpgid
            if (opt.setpgid) {
                pid_t target_pgid = opt.pgid ? opt.pgid : 0; // 0 は自分の PID
                if (::setpgid(0, target_pgid) != 0) {
                    write_errno_and_exit_(exerr[1], "setpgid");
                }
            }

            // argv 準備
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for (size_t i = 0; i < argv.size(); ++i)
                cargv.push_back(const_cast<char*>(argv[i].c_str()));
            cargv.push_back(0);

            // execvp（PATH 解決）を利用
            ::execvp(cargv[0], &cargv[0]);

            // ここに来たら失敗
            write_errno_and_exit_(exerr[1], "execvp");
            // _exit しているので到達しない
        }

        // -------- parent --------
        pid_ = p;

        // exerr: 親は書き端を閉じてから errno 受信を試みる
        ::close(exerr[1]);

        // 親子で不要なパイプ端を閉じる
        if (opt.in.mode  == stream_spec::PIPE)  ::close(in_pipe[0]);   // child-read end
        if (opt.out.mode == stream_spec::PIPE)  ::close(out_pipe[1]);  // child-write end
        if (opt.err.mode == stream_spec::PIPE)  ::close(err_pipe[1]);  // child-write end

        // 親側保持 FD を保存
        in_w_  = (opt.in.mode  == stream_spec::PIPE) ? in_pipe[1]  : -1;
        out_r_ = (opt.out.mode == stream_spec::PIPE) ? out_pipe[0] : -1;
        err_r_ = (opt.err.mode == stream_spec::PIPE) ? err_pipe[0] : -1;
        own_in_w_  = (in_w_  != -1);
        own_out_r_ = (out_r_ != -1);
        own_err_r_ = (err_r_ != -1);

        if (opt.parent_nonblock) {
            if (in_w_  != -1) set_nonblock_(in_w_,  true);
            if (out_r_ != -1) set_nonblock_(out_r_, true);
            if (err_r_ != -1) set_nonblock_(err_r_, true);
        }

        // exec 成否を確認：子が exerr に errno(int) を書く
        int child_exec_errno = 0;
        ssize_t n = read_full_errno_(exerr[0], &child_exec_errno, sizeof(child_exec_errno));
        ::close(exerr[0]);

        if (n > 0) {
            // exec 構築中に失敗
            int st;
            ::waitpid(pid_, &st, 0); // 確実に回収
            cleanup_parent_fds_();
            char buf[128]; std::snprintf(buf, sizeof(buf), "exec failed (errno=%d)", child_exec_errno);
            set_last_error_(buf, child_exec_errno);
            pid_ = -1;
            return false;
        }
        // n == 0 の場合は EOF（= exec 成功で CLOEXEC により閉じられた）
        return true;
    }

    // 書き込み（子の stdin へ）。EINTR は内部でリトライし、他はそのまま返す。
    ssize_t write_stdin(const void* data, size_t len) {
        if (in_w_ == -1) { set_last_error_("stdin is not a pipe", EBADF); return -1; }
        return retry_eintr_write_(in_w_, data, len);
    }

    // 読み込み（子の stdout / stderr から）
    ssize_t read_stdout(void* buf, size_t len) {
        if (out_r_ == -1) { set_last_error_("stdout is not a pipe", EBADF); return -1; }
        return retry_eintr_read_(out_r_, buf, len);
    }
    ssize_t read_stderr(void* buf, size_t len) {
        if (err_r_ == -1) { set_last_error_("stderr is not a pipe", EBADF); return -1; }
        return retry_eintr_read_(err_r_, buf, len);
    }

    // 親側パイプ終端を明示的に閉じる（EPIPE を発生させたい場合など）
    void close_stdin()  { safe_close_(in_w_,  own_in_w_);  own_in_w_  = false; in_w_  = -1; }
    void close_stdout() { safe_close_(out_r_, own_out_r_); own_out_r_ = false; out_r_ = -1; }
    void close_stderr() { safe_close_(err_r_, own_err_r_); own_err_r_ = false; err_r_ = -1; }

    // 子プロセス制御
    pid_t pid() const { return pid_; }

    // 子が生存しているか（非ブロッキング）
    bool alive() const {
        if (pid_ <= 0) return false;
        int st;
        pid_t r = ::waitpid(pid_, &st, WNOHANG);
        return (r == 0);
    }

    // wait：options は 0 または WNOHANG 等
    int wait(int* status, int options) {
        if (pid_ <= 0) { set_last_error_("no child", ECHILD); return -1; }
        int st;
        int r;
        do {
            r = ::waitpid(pid_, &st, options);
        } while (r == -1 && errno == EINTR);
        if (r > 0) {
            if (status) *status = st;
            pid_ = -1;
            cleanup_parent_fds_();
        } else if (r == 0) {
            // まだ終了していない
        } else {
            set_last_error_("waitpid", errno);
        }
        return r;
    }

    // kill(シグナル送信)
    int kill(int sig) {
        if (pid_ <= 0) { set_last_error_("no child", ECHILD); return -1; }
        int r = ::kill(pid_, sig);
        if (r != 0) set_last_error_("kill", errno);
        return r;
    }

    // FD 取得（-1 の場合あり）
    int stdin_fd()  const { return in_w_;  } // 親が書く
    int stdout_fd() const { return out_r_; } // 親が読む
    int stderr_fd() const { return err_r_; } // 親が読む

    // 直近のエラー
    const std::string& last_error() const { return last_error_msg_; }
    int last_errno() const { return last_errno_; }

private:
    pid_t pid_;
    int in_w_, out_r_, err_r_;
    bool own_in_w_, own_out_r_, own_err_r_;
    std::string last_error_msg_;
    int last_errno_;

    // ---- child 構築補助 ----
    bool setup_child_stdio_(const options& opt, int in_pipe[2], int out_pipe[2], int err_pipe[2], int exerr_w) {
        // stdin
        if (opt.in.mode == stream_spec::PIPE) {
            ::close(in_pipe[1]); // 親書き端は閉じる
            if (::dup2(in_pipe[0], 0) == -1) { write_errno_and_exit_(exerr_w, "dup2(stdin)"); }
            if (in_pipe[0] != 0) ::close(in_pipe[0]);
        } else if (opt.in.mode == stream_spec::USE_FD) {
            if (opt.in.fd != 0) {
                if (::dup2(opt.in.fd, 0) == -1) { write_errno_and_exit_(exerr_w, "dup2(stdin FD)"); }
            }
            // 子では元 FD を閉じてリーク抑止（親側には影響なし）
            if (opt.in.fd > 2) ::close(opt.in.fd);
        }
        // stdout
        if (opt.out.mode == stream_spec::PIPE) {
            ::close(out_pipe[0]);
            if (::dup2(out_pipe[1], 1) == -1) { write_errno_and_exit_(exerr_w, "dup2(stdout)"); }
            if (out_pipe[1] != 1) ::close(out_pipe[1]);
        } else if (opt.out.mode == stream_spec::USE_FD) {
            if (opt.out.fd != 1) {
                if (::dup2(opt.out.fd, 1) == -1) { write_errno_and_exit_(exerr_w, "dup2(stdout FD)"); }
            }
            if (opt.out.fd > 2) ::close(opt.out.fd);
        }
        // stderr
        if (opt.err.mode == stream_spec::PIPE) {
            ::close(err_pipe[0]);
            if (::dup2(err_pipe[1], 2) == -1) { write_errno_and_exit_(exerr_w, "dup2(stderr)"); }
            if (err_pipe[1] != 2) ::close(err_pipe[1]);
        } else if (opt.err.mode == stream_spec::USE_FD) {
            if (opt.err.fd != 2) {
                if (::dup2(opt.err.fd, 2) == -1) { write_errno_and_exit_(exerr_w, "dup2(stderr FD)"); }
            }
            if (opt.err.fd > 2) ::close(opt.err.fd);
        }
        return true;
    }

    bool apply_child_env_(const options& opt, int exerr_w) {
        if (opt.clear_env) {
#if defined(__GLIBC__)
            // clearenv は glibc で使用可能
            if (::clearenv() != 0) {
                write_errno_and_exit_(exerr_w, "clearenv");
            }
#else
            // 非 glibc の場合は何もしない（互換性優先）
#endif
        }
        for (size_t i = 0; i < opt.env_kv.size(); ++i) {
            const std::string& kv = opt.env_kv[i];
            std::string k, v;
            split_kv_(kv, k, v);
            if (k.empty()) continue;
            if (::setenv(k.c_str(), v.c_str(), 1) != 0) {
                write_errno_and_exit_(exerr_w, "setenv");
            }
        }
        return true;
    }

    // ---- util ----
    void cleanup_parent_fds_() {
        close_stdin();
        close_stdout();
        close_stderr();
    }

    void set_last_error_(const char* msg, int err) {
        last_error_msg_ = msg ? msg : "";
        last_errno_ = err;
    }
    void clear_last_error_() {
        last_error_msg_.clear();
        last_errno_ = 0;
    }
    bool fail_perror_(const char* where) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: %s", where, std::strerror(errno));
        set_last_error_(buf, errno);
        return false;
    }

    static void safe_close_(int fd, bool do_close) {
        if (do_close && fd >= 0) { ::close(fd); }
    }
    static void safe_close_pair_(int p[2]) {
        if (p[0] != -1) ::close(p[0]);
        if (p[1] != -1) ::close(p[1]);
        p[0] = p[1] = -1;
    }
    static int set_cloexec_(int fd) {
        if (fd < 0) return -1;
        int flags = ::fcntl(fd, F_GETFD);
        if (flags == -1) return -1;
        return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    static int set_nonblock_(int fd, bool on) {
        if (fd < 0) return -1;
        int flags = ::fcntl(fd, F_GETFL);
        if (flags == -1) return -1;
        if (on) flags |= O_NONBLOCK;
        else    flags &= ~O_NONBLOCK;
        return ::fcntl(fd, F_SETFL, flags);
    }
    static ssize_t retry_eintr_read_(int fd, void* buf, size_t len) {
        for (;;) {
            ssize_t n = ::read(fd, buf, len);
            if (n < 0 && errno == EINTR) continue;
            return n;
        }
    }
    static ssize_t retry_eintr_write_(int fd, const void* buf, size_t len) {
        const char* p = static_cast<const char*>(buf);
        size_t left = len;
        while (left) {
            ssize_t n = ::write(fd, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                return n; // エラー
            }
            left -= (size_t)n;
            p += n;
        }
        return (ssize_t)len;
    }
    static ssize_t read_full_errno_(int fd, void* buf, size_t len) {
        // exec 成功時は CLOEXEC により EOF(0) を即座に受ける想定。
        // 失敗時は errno(int) が書かれる。
        char* p = static_cast<char*>(buf);
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::read(fd, p + got, len - got);
            if (n == 0) break;           // EOF
            if (n < 0) {
                if (errno == EINTR) continue;
                return n;
            }
            got += (size_t)n;
        }
        return (ssize_t)got;
    }
    static void write_errno_and_exit_(int fd, const char* where) {
        (void)where; // where はデバッグ用途（必要なら書き込む）
        int e = errno;
        // best-effort: errno を親へ
        (void)::write(fd, &e, sizeof(e));
        _exit(127);
    }
    static void split_kv_(const std::string& kv, std::string& k, std::string& v) {
        std::string::size_type pos = kv.find('=');
        if (pos == std::string::npos) {
            k = kv; v = "";
        } else {
            k = kv.substr(0, pos);
            v = kv.substr(pos + 1);
        }
    }
};

} // namespace tinyproc

#endif // TINYPROC_POPEN3_HPP
