#include "popen3.hpp"
#include <vector>
#include <string>
#include <windows.h>

int main() {
    using namespace tinyproc;

    // Create out.txt (use CreateFileW if a UTF-16 path is required)
    HANDLE hFile = CreateFileA(
        "out.txt", GENERIC_WRITE, FILE_SHARE_READ,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) return 1;

    popen3::options opt;
    opt.out = popen3::stream_spec::use_handle(hFile); // Route the child's stdout to the file
    // The library does not close the parent's hFile (caller responsibility)

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
    CloseHandle(hFile); // The parent is responsible for closing the handle
    return 0;
}
