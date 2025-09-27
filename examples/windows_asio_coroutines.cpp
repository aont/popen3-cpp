// ensure <winsock2.h> is included before <windows.h>
#include <winsock2.h>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define ASIO_STANDALONE
#include <asio.hpp>


#include "popen3.hpp"


struct coroutine_process : std::enable_shared_from_this<coroutine_process> {
    std::string name;
    tinyproc::popen3 proc;
    asio::windows::stream_handle stdout_stream;

    explicit coroutine_process(asio::io_context& ctx, std::string tag)
        : name(std::move(tag)), stdout_stream(ctx) {}
};

asio::awaitable<void> forward_stdout(std::shared_ptr<coroutine_process> ctx) {
    auto& stream = ctx->stdout_stream;
    std::array<char, 512> buffer{};

    for (;;) {
        asio::error_code ec;
        std::size_t n = co_await stream.async_read_some(
            asio::buffer(buffer),
            asio::redirect_error(asio::use_awaitable, ec));

        if (ec == asio::error::eof) {
            break;
        }
        if (ec) {
            std::cerr << '[' << ctx->name << "] read error: " << ec.message() << "\n";
            break;
        }

        std::cout << '[' << ctx->name << "] "
                  << std::string_view(buffer.data(), n) << std::flush;
    }

    co_return;
}

std::shared_ptr<coroutine_process> launch(asio::io_context& ctx,
                                          std::string name,
                                          std::vector<std::string> argv) {
    auto proc_ctx = std::make_shared<coroutine_process>(ctx, std::move(name));

    tinyproc::popen3::options opt;
    opt.out = tinyproc::popen3::stream_spec::pipe();
    opt.overlapped_io = true;    // Required for async I/O on Windows handles
    opt.parent_nonblock = true;  // Avoid blocking reads in the parent

    if (!proc_ctx->proc.start(argv, opt)) {
        throw std::runtime_error("start failed: " + proc_ctx->proc.last_error());
    }

    HANDLE duplicated = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), proc_ctx->proc.stdout_handle(),
                         GetCurrentProcess(), &duplicated, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        proc_ctx->proc.close_stdout();
        throw std::runtime_error("DuplicateHandle(stdout) failed");
    }

    proc_ctx->stdout_stream.assign(duplicated);
    proc_ctx->proc.close_stdout();

    return proc_ctx;
}

int main() {
    try {
        asio::io_context io_ctx;
        std::vector<std::shared_ptr<coroutine_process>> children;

        children.push_back(launch(io_ctx, "slow",
                                  {"cmd.exe", "/C",
                                   "for /L %i in (1,1,5) do (echo slow-%i & timeout /T 1 >NUL)"}));
        children.push_back(launch(io_ctx, "fast",
                                  {"cmd.exe", "/C",
                                   "for /L %i in (1,1,8) do (echo fast-%i & ping -n 1 -w 200 127.0.0.1 >NUL)"}));

        for (auto& child : children) {
            asio::co_spawn(io_ctx, forward_stdout(child), asio::detached);
        }

        io_ctx.run();

        for (auto& child : children) {
            int status = 0;
            if (!child->proc.wait(&status, 0)) {
                std::cerr << '[' << child->name << "] wait failed: "
                          << child->proc.last_error() << "\n";
                continue;
            }
            std::cout << '[' << child->name << "] exited with code " << status << "\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "fatal error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
