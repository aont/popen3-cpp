#ifndef TINYPROC_POPEN3_HPP
#define TINYPROC_POPEN3_HPP

#if !defined(_WIN32)
#  error "tinyproc::popen3 is Windows-only."
#endif

#define NOMINMAX
#include <windows.h>
#include <vector>
#include <string>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef TINYPROC_UNUSED
#  define TINYPROC_UNUSED(x) (void)(x)
#endif

#ifndef _SSIZE_T_DEFINED
#  include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#  define _SSIZE_T_DEFINED
#endif

namespace tinyproc {

class popen3 {
public:
    struct stream_spec {
        enum kind_t { INHERIT, PIPE, USE_HANDLE };
        kind_t kind;
        HANDLE user_handle; // USE_HANDLE のときのみ
        stream_spec() : kind(INHERIT), user_handle(NULL) {}
        static stream_spec inherit() { stream_spec s; s.kind = INHERIT; return s; }
        static stream_spec pipe()    { stream_spec s; s.kind = PIPE;    return s; }
        static stream_spec use_handle(HANDLE h) { stream_spec s; s.kind = USE_HANDLE; s.user_handle = h; return s; }
    };

    struct options {
        stream_spec in;   // child's stdin
        stream_spec out;  // child's stdout
        stream_spec err;  // child's stderr
        bool parent_nonblock; // 同期モード時: データ無しで 0 を返す（PeekNamedPipe）
        bool overlapped_io;   // true なら PIPE は NamedPipe + Overlapped で構築
        size_t io_buffer_size;// Overlapped 読み取りチャンクサイズ
        options() : in(stream_spec::inherit()),
                    out(stream_spec::inherit()),
                    err(stream_spec::inherit()),
                    parent_nonblock(false),
                    overlapped_io(false),
                    io_buffer_size(64*1024) {}
    };

    popen3()
        : proc_(NULL), th_(NULL), pid_(0),
          h_stdin_w_(NULL), h_stdout_r_(NULL), h_stderr_r_(NULL),
          parent_nonblock_(false), overlapped_(false),
          last_err_(0)
    {
        init_ov_read(out_rd_);
        init_ov_read(err_rd_);
        init_ov_write(in_wr_);
    }

    ~popen3() {
        // Overlapped の保留 I/O をキャンセルしてからクローズ
        cancel_all_io_();
        close_stdin();
        close_stdout();
        close_stderr();
        if (th_)   { CloseHandle(th_);   th_   = NULL; }
        if (proc_) { CloseHandle(proc_); proc_ = NULL; }
        if (out_rd_.evt) CloseHandle(out_rd_.evt);
        if (err_rd_.evt) CloseHandle(err_rd_.evt);
        if (in_wr_.evt)  CloseHandle(in_wr_.evt);
    }

    // argv: UTF-8。argv[0] はコマンド名/パス
    bool start(const std::vector<std::string>& argv, const options& opt) {
        clear_error();
        if (argv.empty()) return set_error("argv is empty", ERROR_INVALID_PARAMETER), false;

        overlapped_ = opt.overlapped_io;
        parent_nonblock_ = opt.parent_nonblock;
        io_buf_size_ = (opt.io_buffer_size == 0) ? (64*1024) : opt.io_buffer_size;

        std::wstring cmd = build_cmdline_utf16(argv);

        SECURITY_ATTRIBUTES sa_inh; ZeroMemory(&sa_inh, sizeof(sa_inh));
        sa_inh.nLength = sizeof(sa_inh);
        sa_inh.bInheritHandle = TRUE;
        sa_inh.lpSecurityDescriptor = NULL;

        // 子標準ハンドル（継承対象）を用意
        HANDLE ch_in  = NULL; // 子の stdin  (READ側)
        HANDLE ch_out = NULL; // 子の stdout (WRITE側)
        HANDLE ch_err = NULL; // 子の stderr (WRITE側)

        // 親が使うエンド
        HANDLE parent_in_w  = NULL; // 親->子
        HANDLE parent_out_r = NULL; // 子->親
        HANDLE parent_err_r = NULL; // 子->親

        // ---- stdin ----
        if (opt.in.kind == stream_spec::PIPE) {
            if (overlapped_) {
                if (!make_named_pipe_pair_(/*parent_reads=*/false, &parent_in_w, &ch_in, &sa_inh)) return false;
            } else {
                HANDLE r=NULL, w=NULL;
                if (!CreatePipe(&r, &w, &sa_inh, 0)) return fail_api("CreatePipe(stdin) (anon)"), false;
                // 親は write（w）を保持。子は read（r）を継承
                if (!SetHandleInformation(w, HANDLE_FLAG_INHERIT, 0)) { CloseHandle(r); CloseHandle(w); return fail_api("SetHandleInformation(stdin w)"); }
                parent_in_w = w; ch_in = r;
            }
        } else if (opt.in.kind == stream_spec::USE_HANDLE) {
            if (!dup_inheritable(opt.in.user_handle, &ch_in)) return fail_api("DuplicateHandle(stdin use_handle)"), false;
        } else { // INHERIT
            HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
            if (!dup_inheritable(h, &ch_in)) return fail_api("DuplicateHandle(stdin inherit)"), false;
        }

        // ---- stdout ----
        if (opt.out.kind == stream_spec::PIPE) {
            if (overlapped_) {
                if (!make_named_pipe_pair_(/*parent_reads=*/true, &parent_out_r, &ch_out, &sa_inh)) { cleanup_child_handles_(ch_in, NULL, NULL); return false; }
            } else {
                HANDLE r=NULL, w=NULL;
                if (!CreatePipe(&r, &w, &sa_inh, 0)) { cleanup_child_handles_(ch_in, NULL, NULL); return fail_api("CreatePipe(stdout) (anon)"), false; }
                if (!SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0)) { CloseHandle(r); CloseHandle(w); cleanup_child_handles_(ch_in, NULL, NULL); return fail_api("SetHandleInformation(stdout r)"); }
                parent_out_r = r; ch_out = w;
            }
        } else if (opt.out.kind == stream_spec::USE_HANDLE) {
            if (!dup_inheritable(opt.out.user_handle, &ch_out)) { cleanup_child_handles_(ch_in, NULL, NULL); return fail_api("DuplicateHandle(stdout use_handle)"), false; }
        } else { // INHERIT
            HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
            if (!dup_inheritable(h, &ch_out)) { cleanup_child_handles_(ch_in, NULL, NULL); return fail_api("DuplicateHandle(stdout inherit)"), false; }
        }

        // ---- stderr ----
        if (opt.err.kind == stream_spec::PIPE) {
            if (overlapped_) {
                if (!make_named_pipe_pair_(/*parent_reads=*/true, &parent_err_r, &ch_err, &sa_inh)) { cleanup_child_handles_(ch_in, ch_out, NULL); return false; }
            } else {
                HANDLE r=NULL, w=NULL;
                if (!CreatePipe(&r, &w, &sa_inh, 0)) { cleanup_child_handles_(ch_in, ch_out, NULL); return fail_api("CreatePipe(stderr) (anon)"), false; }
                if (!SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0)) { CloseHandle(r); CloseHandle(w); cleanup_child_handles_(ch_in, ch_out, NULL); return fail_api("SetHandleInformation(stderr r)"); }
                parent_err_r = r; ch_err = w;
            }
        } else if (opt.err.kind == stream_spec::USE_HANDLE) {
            if (!dup_inheritable(opt.err.user_handle, &ch_err)) { cleanup_child_handles_(ch_in, ch_out, NULL); return fail_api("DuplicateHandle(stderr use_handle)"), false; }
        } else { // INHERIT
            HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
            if (!dup_inheritable(h, &ch_err)) { cleanup_child_handles_(ch_in, ch_out, NULL); return fail_api("DuplicateHandle(stderr inherit)"), false; }
        }

        STARTUPINFOW si; ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput  = ch_in;
        si.hStdOutput = ch_out;
        si.hStdError  = ch_err;

        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
        std::wstring cmdw = cmd;
        std::vector<wchar_t> cmd_buf(cmdw.size() + 1, L'\0');
        if (!cmdw.empty()) std::memcpy(&cmd_buf[0], &cmdw[0], cmdw.size()*sizeof(wchar_t));

        BOOL ok = CreateProcessW(
            NULL,
            cmd_buf.empty()? NULL : &cmd_buf[0],
            NULL, NULL,
            TRUE,      // 子へ継承
            0,
            NULL, NULL,
            &si, &pi
        );

        // 親では子ハンドルをすぐ閉じる（継承済み）
        if (ch_in)  CloseHandle(ch_in);
        if (ch_out) CloseHandle(ch_out);
        if (ch_err) CloseHandle(ch_err);

        if (!ok) {
            if (parent_in_w)  CloseHandle(parent_in_w);
            if (parent_out_r) CloseHandle(parent_out_r);
            if (parent_err_r) CloseHandle(parent_err_r);
            return fail_api("CreateProcessW");
        }

        proc_ = pi.hProcess;
        th_   = pi.hThread;
        pid_  = (unsigned long)pi.dwProcessId;

        // 親側エンドを保持
        h_stdin_w_  = parent_in_w;
        h_stdout_r_ = parent_out_r;
        h_stderr_r_ = parent_err_r;

        // Overlapped 状態初期化＆最初の Read を投入
        if (overlapped_) {
            if (opt.out.kind == stream_spec::PIPE && h_stdout_r_) {
                setup_ov_read_(out_rd_, h_stdout_r_, io_buf_size_);
                post_read_(out_rd_); // 最初のリクエスト
            }
            if (opt.err.kind == stream_spec::PIPE && h_stderr_r_) {
                setup_ov_read_(err_rd_, h_stderr_r_, io_buf_size_);
                post_read_(err_rd_);
            }
            if (opt.in.kind == stream_spec::PIPE && h_stdin_w_) {
                setup_ov_write_(in_wr_, h_stdin_w_);
            }
        }

        return true;
    }

    // ---- 親→子 stdin (同期) ----
    // Overlapped 構成でも同期 WriteFile を許容
    ssize_t write_stdin(const void* buf, size_t len) {
        if (!h_stdin_w_) return set_error("stdin not available", ERROR_INVALID_HANDLE), (ssize_t)-1;
        DWORD n = 0;
        BOOL ok = WriteFile(h_stdin_w_, buf, (DWORD)len, &n, NULL);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE) return 0;
            return fail_api("WriteFile(stdin)"), (ssize_t)-1;
        }
        return (ssize_t)n;
    }

    // ---- 親→子 stdin (非同期 Overlapped) ----
    // 同時に 1 件のみ保留可能。完了は stdin_event() で待機し、try_finalize_stdin_write() で取り出す。
    bool write_stdin_async(const void* buf, size_t len) {
        if (!overlapped_ || !h_stdin_w_) return set_error("stdin async not available", ERROR_INVALID_HANDLE), false;
        if (in_wr_.pending) return set_error("stdin write already pending", ERROR_IO_PENDING), false;
        if (len == 0) return true;

        in_wr_.buf.resize(len);
        std::memcpy(&in_wr_.buf[0], buf, len);
        in_wr_.size = len;
        ResetEvent(in_wr_.evt);
        ZeroMemory(&in_wr_.ov, sizeof(in_wr_.ov));
        in_wr_.ov.hEvent = in_wr_.evt;

        DWORD n = 0;
        BOOL ok = WriteFile(in_wr_.h, &in_wr_.buf[0], (DWORD)in_wr_.size, &n, &in_wr_.ov);
        if (ok) {
            // 即時完了
            in_wr_.pending = false;
            in_wr_.last_n  = n;
            SetEvent(in_wr_.evt);
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            in_wr_.pending = true;
            return true;
        }
        return fail_api("WriteFile(stdin overlapped)"), false;
    }

    bool stdin_write_pending() const { return in_wr_.pending; }

    // 完了していれば true を返し、*nwritten に書き込まれたバイト数を返す
    bool try_finalize_stdin_write(size_t* nwritten) {
        if (!overlapped_ || !h_stdin_w_) return false;
        if (!in_wr_.pending) {
            if (nwritten) *nwritten = in_wr_.last_n;
            return true;
        }
        DWORD n=0;
        BOOL ok = GetOverlappedResult(in_wr_.h, &in_wr_.ov, &n, FALSE);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) return false; // まだ
            if (e == ERROR_BROKEN_PIPE) {
                in_wr_.pending = false;
                in_wr_.last_n = 0;
                SetEvent(in_wr_.evt);
                if (nwritten) *nwritten = 0;
                return true;
            }
            fail_api("GetOverlappedResult(stdin)");
            in_wr_.pending = false; // 以降の操作を可能にする
            in_wr_.last_n = 0;
            if (nwritten) *nwritten = 0;
            return true;
        }
        in_wr_.pending = false;
        in_wr_.last_n = n;
        if (nwritten) *nwritten = (size_t)n;
        return true;
    }

    // ---- 子→親 stdout/stderr 読み取り ----
    // Overlapped 構成：
    //   - イベントがシグナル後、read_* を呼ぶと内部バッファから返却し、消費し終わったら次の Read を投入。
    //   - データが無ければ 0（非エラー）。
    // 非 Overlapped 構成：
    //   - parent_nonblock==true なら PeekNamedPipe で 0/データ量ぶん返却。同期 read も可。
    ssize_t read_stdout(void* buf, size_t len) {
        if (!h_stdout_r_) return set_error("stdout not available", ERROR_INVALID_HANDLE), (ssize_t)-1;
        if (overlapped_ && out_rd_.evt) return read_from_ov_(out_rd_, buf, len, "stdout");
        return read_sync_(h_stdout_r_, buf, len, "stdout");
    }
    ssize_t read_stderr(void* buf, size_t len) {
        if (!h_stderr_r_) return set_error("stderr not available", ERROR_INVALID_HANDLE), (ssize_t)-1;
        if (overlapped_ && err_rd_.evt) return read_from_ov_(err_rd_, buf, len, "stderr");
        return read_sync_(h_stderr_r_, buf, len, "stderr");
    }

    // ストリームを閉じる
    void close_stdin()  { close_and_reset_write_(in_wr_, h_stdin_w_);  }
    void close_stdout() { close_and_reset_read_(out_rd_, h_stdout_r_); }
    void close_stderr() { close_and_reset_read_(err_rd_, h_stderr_r_); }

    // 親が保持するハンドル（便宜）
    HANDLE stdin_handle()  const { return h_stdin_w_;  } // 親 Write 側
    HANDLE stdout_handle() const { return h_stdout_r_; } // 親 Read 側
    HANDLE stderr_handle() const { return h_stderr_r_; } // 親 Read 側

    // ---- WaitForMultipleObjects 用 ----
    HANDLE process_handle() const { return proc_; }
    HANDLE stdout_event()  const { return out_rd_.evt; }
    HANDLE stderr_event()  const { return err_rd_.evt; }
    HANDLE stdin_event()   const { return in_wr_.evt; } // 非同期書き込み完了

    // 便利関数：待機対象を収集（proc + out + err [+stdin write]）
    void collect_wait_handles(std::vector<HANDLE>& out, bool include_stdin_evt=false) const {
        out.clear();
        if (proc_) out.push_back(proc_);
        if (out_rd_.evt) out.push_back(out_rd_.evt);
        if (err_rd_.evt) out.push_back(err_rd_.evt);
        if (include_stdin_evt && in_wr_.evt) out.push_back(in_wr_.evt);
    }

    // プロセス管理
    bool alive() const {
        if (!proc_) return false;
        DWORD r = WaitForSingleObject(proc_, 0);
        return r == WAIT_TIMEOUT;
    }

    // timeout_ms==0 で無限待ち（従来互換）
    bool wait(int* exit_status, unsigned timeout_ms) {
        if (!proc_) return set_error("process not started", ERROR_INVALID_HANDLE), false;
        DWORD tm = (timeout_ms == 0) ? INFINITE : (DWORD)timeout_ms;
        DWORD r = WaitForSingleObject(proc_, tm);
        if (r == WAIT_TIMEOUT) return false;
        if (r != WAIT_OBJECT_0) return fail_api("WaitForSingleObject(process)"), false;
        DWORD code = 0;
        if (!GetExitCodeProcess(proc_, &code)) return fail_api("GetExitCodeProcess"), false;
        if (exit_status) *exit_status = (int)code;
        return true;
    }

    // 情報
    const std::string& last_error() const { return last_msg_; }
    int  last_errno() const { return (int)last_err_; }
    unsigned long pid() const { return pid_; }

private:
    // -------- 内部状態 --------
    HANDLE proc_;
    HANDLE th_;
    unsigned long pid_;

    HANDLE h_stdin_w_;
    HANDLE h_stdout_r_;
    HANDLE h_stderr_r_;

    bool parent_nonblock_;
    bool overlapped_;
    size_t io_buf_size_;

    DWORD last_err_;
    std::string last_msg_;

    struct ov_read_t {
        HANDLE h;        // 親の Read ハンドル
        OVERLAPPED ov;
        HANDLE evt;      // 完了イベント
        std::vector<char> buf;
        size_t have;     // buf に格納済み（完了済み）バイト数
        size_t pos;      // 既に返却済み位置
        bool pending;    // ReadFile 投入中
        bool eof;        // EOF 到達（0 バイト完了）
        ov_read_t() : h(NULL), evt(NULL), have(0), pos(0), pending(false), eof(false) { ZeroMemory(&ov, sizeof(ov)); }
    } out_rd_, err_rd_;

    struct ov_write_t {
        HANDLE h;        // 親の Write ハンドル
        OVERLAPPED ov;
        HANDLE evt;      // 完了イベント
        std::vector<char> buf;
        size_t size;     // 投入サイズ
        bool pending;    // WriteFile 投入中
        DWORD last_n;    // 直近完了バイト
        ov_write_t() : h(NULL), evt(NULL), size(0), pending(false), last_n(0) { ZeroMemory(&ov, sizeof(ov)); }
    } in_wr_;

    void clear_error() { last_err_ = 0; last_msg_.clear(); }
    bool set_error(const char* msg, DWORD e) { last_err_ = e; last_msg_ = format_error_(msg, e); return false; }
    bool fail_api(const char* api) { return set_error(api, GetLastError()); }

    static std::string format_error_(const char* msg, DWORD e) {
        LPWSTR wbuf = 0;
        DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS;
        FormatMessageW(flags, 0, e, 0, (LPWSTR)&wbuf, 0, 0);
        std::string tail;
        if (wbuf) { tail = utf16_to_utf8_(wbuf); LocalFree(wbuf); }
        std::ostringstream oss; oss << msg << " failed: " << tail << " (GetLastError=" << e << ")";
        return oss.str();
    }

    static std::wstring utf8_to_utf16_(const std::string& s) {
        if (s.empty()) return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, &s[0], (int)s.size(), 0, 0);
        if (len <= 0) {
            len = MultiByteToWideChar(CP_ACP, 0, &s[0], (int)s.size(), 0, 0);
            if (len <= 0) return std::wstring();
            std::wstring w(len, 0);
            MultiByteToWideChar(CP_ACP, 0, &s[0], (int)s.size(), &w[0], len);
            return w;
        }
        std::wstring w(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, &s[0], (int)s.size(), &w[0], len);
        return w;
    }
    static std::string utf16_to_utf8_(const std::wstring& w) {
        if (w.empty()) return std::string();
        int len = WideCharToMultiByte(CP_UTF8, 0, &w[0], (int)w.size(), 0, 0, 0, 0);
        std::string s(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, &w[0], (int)w.size(), &s[0], len, 0, 0);
        return s;
    }

    static std::wstring quote_arg_(const std::wstring& a) {
        if (a.empty()) return L"\"\"";
        bool need=false;
        for (size_t i=0;i<a.size();++i){ wchar_t c=a[i]; if (c==L' '||c==L'\t'||c==L'"'){ need=true; break; } }
        if (!need) return a;
        std::wstring out; out.push_back(L'"');
        size_t bs=0;
        for (size_t i=0;i<a.size();++i){
            wchar_t c=a[i];
            if (c==L'\\') ++bs;
            else if (c==L'"'){ out.append(bs*2, L'\\'); bs=0; out.push_back(L'\\'); out.push_back(L'"'); }
            else { if (bs){ out.append(bs, L'\\'); bs=0; } out.push_back(c); }
        }
        if (bs) out.append(bs*2, L'\\');
        out.push_back(L'"');
        return out;
    }
    static std::wstring build_cmdline_utf16(const std::vector<std::string>& argv) {
        std::wstring out;
        for (size_t i=0;i<argv.size();++i){ if (i) out.push_back(L' '); out += quote_arg_(utf8_to_utf16_(argv[i])); }
        return out;
    }

    static BOOL dup_inheritable(HANDLE src, HANDLE* dst) {
        if (!src || src == INVALID_HANDLE_VALUE) { *dst = NULL; return TRUE; }
        HANDLE proc = GetCurrentProcess();
        return DuplicateHandle(proc, src, proc, dst, 0, TRUE, DUPLICATE_SAME_ACCESS);
    }

    // -------- Named Pipe (Overlapped) 構築 --------
    // parent_reads=true  : 親が READ（INBOUND）, 子が WRITE（GENERIC_WRITE）
    // parent_reads=false : 親が WRITE（OUTBOUND）, 子が READ（GENERIC_READ）
    bool make_named_pipe_pair_(bool parent_reads, HANDLE* parent_end, HANDLE* child_end_inheritable, SECURITY_ATTRIBUTES* sa_child) {
        std::wstring name = unique_pipe_name_();
        DWORD open_mode = parent_reads ? (PIPE_ACCESS_INBOUND  | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE)
                                       : (PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE);
        HANDLE hServer = CreateNamedPipeW(
            name.c_str(), open_mode,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,            // max instances
            64*1024,      // out buf
            64*1024,      // in buf
            0,            // default timeout
            NULL          // security attrs for server (非継承)
        );
        if (hServer == INVALID_HANDLE_VALUE) return fail_api("CreateNamedPipeW"), false;

        // Connect を Overlapped で開始（同一スレッドで CreateFile するため）
        OVERLAPPED ov; ZeroMemory(&ov, sizeof(ov));
        HANDLE conn_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!conn_evt) { CloseHandle(hServer); return fail_api("CreateEvent(connect)"); }
        ov.hEvent = conn_evt;

        BOOL c = ConnectNamedPipe(hServer, &ov);
        if (!c) {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) {
                // 既につながっているケース（まれ）
                SetEvent(conn_evt);
            } else if (e != ERROR_IO_PENDING) {
                CloseHandle(conn_evt);
                CloseHandle(hServer);
                return set_error("ConnectNamedPipe", e);
            }
        }

        // クライアント（子側）を親プロセスで開く（継承可能）
        DWORD desired = parent_reads ? GENERIC_WRITE : GENERIC_READ;
        HANDLE hClient = CreateFileW(
            name.c_str(), desired, 0, sa_child,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hClient == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            CloseHandle(conn_evt);
            CloseHandle(hServer);
            return set_error("CreateFileW(pipe client)", e);
        }

        // Connect 完了待ち（即完了のはずだが安全のため待つ）
        DWORD dummy=0;
        if (!GetOverlappedResult(hServer, &ov, &dummy, TRUE)) {
            DWORD e = GetLastError();
            if (e != ERROR_PIPE_CONNECTED) {
                CloseHandle(hClient);
                CloseHandle(conn_evt);
                CloseHandle(hServer);
                return set_error("GetOverlappedResult(connect)", e);
            }
        }
        CloseHandle(conn_evt);

        // 親サーバハンドルは非継承化（保険）
        SetHandleInformation(hServer, HANDLE_FLAG_INHERIT, 0);

        *parent_end = hServer;
        *child_end_inheritable = hClient;
        return true;
    }

    static std::wstring unique_pipe_name_() {
        static LONG s_count = 0;
        DWORD pid = GetCurrentProcessId();
        DWORD tid = GetCurrentThreadId();
        DWORD tick = GetTickCount();
        LONG  c = InterlockedIncrement(&s_count);
        wchar_t buf[256];
#if defined(_MSC_VER)
        _snwprintf(buf, 255, L"\\\\.\\pipe\\tinyproc_popen3_%lu_%lu_%lu_%ld",
                   (unsigned long)pid, (unsigned long)tid, (unsigned long)tick, (long)c);
#else
        swprintf(buf, 255, L"\\\\.\\pipe\\tinyproc_popen3_%lu_%lu_%lu_%ld",
                 (unsigned long)pid, (unsigned long)tid, (unsigned long)tick, (long)c);
#endif
        buf[255] = 0;
        return std::wstring(buf);
    }

    // -------- Overlapped 読み取りユーティリティ --------
    static void init_ov_read(ov_read_t& r) { r = ov_read_t(); }
    static void init_ov_write(ov_write_t& w) { w = ov_write_t(); }

    void setup_ov_read_(ov_read_t& r, HANDLE h, size_t bufsize) {
        r.h = h;
        if (!r.evt) r.evt = CreateEventW(NULL, TRUE, FALSE, NULL);
        r.buf.resize(bufsize);
        r.have = r.pos = 0;
        r.pending = false;
        r.eof = false;
        ZeroMemory(&r.ov, sizeof(r.ov));
        r.ov.hEvent = r.evt;
        ResetEvent(r.evt);
    }
    void setup_ov_write_(ov_write_t& w, HANDLE h) {
        w.h = h;
        if (!w.evt) w.evt = CreateEventW(NULL, TRUE, FALSE, NULL);
        w.buf.clear();
        w.size = 0;
        w.pending = false;
        w.last_n = 0;
        ZeroMemory(&w.ov, sizeof(w.ov));
        w.ov.hEvent = w.evt;
        ResetEvent(w.evt);
    }

    bool post_read_(ov_read_t& r) {
        if (!r.h || r.eof) return true;
        ResetEvent(r.evt);
        ZeroMemory(&r.ov, sizeof(r.ov));
        r.ov.hEvent = r.evt;
        DWORD n = 0;
        BOOL ok = ReadFile(r.h, r.buf.empty()? NULL : &r.buf[0], (DWORD)r.buf.size(), &n, &r.ov);
        if (ok) {
            // 即時完了
            r.have = n;
            r.pos  = 0;
            r.pending = false;
            if (n == 0) r.eof = true;
            SetEvent(r.evt);
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            r.pending = true;
            return true;
        }
        if (e == ERROR_BROKEN_PIPE) {
            r.have = 0; r.pos = 0; r.pending = false; r.eof = true;
            SetEvent(r.evt);
            return true;
        }
        return fail_api("ReadFile(overlapped)");
    }

    // イベントがシグナルなら結果を取得しキャッシュへ
    bool acquire_completed_read_(ov_read_t& r) {
        if (!r.pending) return (r.have > r.pos) || r.eof;
        DWORD n = 0;
        BOOL ok = GetOverlappedResult(r.h, &r.ov, &n, FALSE);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) return false;
            if (e == ERROR_BROKEN_PIPE) {
                r.have = 0; r.pos = 0; r.pending = false; r.eof = true;
                return true;
            }
            fail_api("GetOverlappedResult(read)");
            r.have = 0; r.pos = 0; r.pending = false; r.eof = true;
            return true;
        }
        r.have = n; r.pos = 0; r.pending = false;
        if (n == 0) r.eof = true;
        return true;
    }

    ssize_t read_from_ov_(ov_read_t& r, void* dst, size_t len, const char* tag) {
        TINYPROC_UNUSED(tag);
        if (!r.h) return set_error("overlapped pipe invalid", ERROR_INVALID_HANDLE), (ssize_t)-1;

        // 既にキャッシュがあればまず返却
        if (r.have > r.pos) {
            size_t avail = r.have - r.pos;
            size_t tocpy = (len < avail) ? len : avail;
            if (tocpy) std::memcpy(dst, &r.buf[r.pos], tocpy);
            r.pos += tocpy;
            if (r.pos == r.have) {
                // 読み切ったので次の Read を投入
                r.have = r.pos = 0;
                if (!r.eof) post_read_(r);
                else ResetEvent(r.evt); // EOF 到達後は消灯
            }
            return (ssize_t)tocpy;
        }

        if (r.eof) return 0;

        // 未完了なら完了確認
        if (!acquire_completed_read_(r)) {
            // まだ完了していない
            return 0;
        }

        // 完了済み（have/pos セット済み）
        if (r.have > r.pos) {
            size_t avail = r.have - r.pos;
            size_t tocpy = (len < avail) ? len : avail;
            if (tocpy) std::memcpy(dst, &r.buf[r.pos], tocpy);
            r.pos += tocpy;
            if (r.pos == r.have) {
                r.have = r.pos = 0;
                if (!r.eof) post_read_(r);
                else ResetEvent(r.evt);
            }
            return (ssize_t)tocpy;
        }

        // 完了したが 0（EOF）
        r.eof = true;
        ResetEvent(r.evt);
        return 0;
    }

    // -------- 同期読み取り（PeekNamedPipe を使ったノンブロッキング風） --------
    ssize_t read_sync_(HANDLE h, void* buf, size_t len, const char* tag) {
        if (!h) return set_error((std::string(tag) + " not available").c_str(), ERROR_INVALID_HANDLE), (ssize_t)-1;
        DWORD to_read = (DWORD)len;
        if (parent_nonblock_) {
            DWORD avail = 0;
            BOOL ok = PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL);
            if (!ok) {
                DWORD e = GetLastError();
                if (e == ERROR_BROKEN_PIPE) return 0;
                // PeekNamedPipe は匿名パイプでも使用可能
                return fail_api("PeekNamedPipe"), (ssize_t)-1;
            }
            if (avail == 0) return 0;
            if (avail < to_read) to_read = avail;
        }
        DWORD n=0;
        BOOL ok = ReadFile(h, buf, to_read, &n, NULL);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE) return 0;
            return fail_api((std::string("ReadFile(")+tag+")").c_str()), (ssize_t)-1;
        }
        return (ssize_t)n;
    }

    void close_and_reset_read_(ov_read_t& r, HANDLE& h) {
        if (h) {
            if (r.pending) CancelIo(h);
            CloseHandle(h); h = NULL;
        }
        r.h = NULL;
        r.have = r.pos = 0;
        r.pending = false;
        r.eof = true;
        if (r.evt) ResetEvent(r.evt);
    }
    void close_and_reset_write_(ov_write_t& w, HANDLE& h) {
        if (h) {
            if (w.pending) CancelIo(h);
            CloseHandle(h); h = NULL;
        }
        w.h = NULL;
        w.buf.clear();
        w.size = 0;
        w.pending = false;
        w.last_n = 0;
        if (w.evt) ResetEvent(w.evt);
    }

    void cancel_all_io_() {
        if (h_stdout_r_) CancelIo(h_stdout_r_);
        if (h_stderr_r_) CancelIo(h_stderr_r_);
        if (h_stdin_w_)  CancelIo(h_stdin_w_);
    }

    void cleanup_child_handles_(HANDLE in, HANDLE out, HANDLE err) {
        if (in)  CloseHandle(in);
        if (out) CloseHandle(out);
        if (err) CloseHandle(err);
    }
};

} // namespace tinyproc

#endif // TINYPROC_POPEN3_HPP
