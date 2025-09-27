#include "popen3.hpp"
#include <vector>
#include <string>
#include <windows.h>

int main() {
    using namespace tinyproc;

    // out.txt を作成（UTF-16 のパスが必要なら CreateFileW を使用）
    HANDLE hFile = CreateFileA(
        "out.txt", GENERIC_WRITE, FILE_SHARE_READ,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) return 1;

    popen3::options opt;
    opt.out = popen3::stream_spec::use_handle(hFile); // 子の stdout をファイルへ
    // 親側の hFile はライブラリでは閉じません（呼び出し側の責務）

    popen3 proc;
    std::vector<std::string> argv;
    argv.push_back("cmd");
    argv.push_back("/c");
    argv.push_back("echo hello");

    if (!proc.start(argv, opt)) {
        CloseHandle(hFile);
        return 1;
    }
    proc.wait(0, 0);
    CloseHandle(hFile); // 親の責務でクローズ
    return 0;
}
