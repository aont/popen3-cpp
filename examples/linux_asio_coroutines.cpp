#include "popen3.hpp"

#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::literals;

struct coroutine_process : std::enable_shared_from_this<coroutine_process> {
    std::string name;
    tinyproc::popen3 proc;
    asio::posix::stream_descriptor stdout_stream;

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
            break; // Child closed stdout
        }
        if (ec) {
            std::cerr << "[" << ctx->name << "] read error: " << ec.message() << "\n";
            break;
        }

        std::cout << "[" << ctx->name << "] "
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
    opt.parent_nonblock = true; // Required for integration with async descriptors

    if (!proc_ctx->proc.start(argv, opt)) {
        throw std::runtime_error("start failed: " + proc_ctx->proc.last_error());
    }

    int fd = ::dup(proc_ctx->proc.stdout_fd());
    if (fd == -1) {
        proc_ctx->proc.close_stdout();
        throw std::runtime_error("dup(stdout) failed");
    }

    proc_ctx->stdout_stream.assign(fd);
    proc_ctx->proc.close_stdout(); // stream_descriptor now owns the fd

    return proc_ctx;
}

int main() {
    try {
        asio::io_context io_ctx;
        std::vector<std::shared_ptr<coroutine_process>> children;

        children.push_back(launch(io_ctx, "slow",
                                  {"/bin/bash", "-lc",
                                   "for i in {1..5}; do echo slow-$i; sleep 1; done"}));
        children.push_back(launch(io_ctx, "fast",
                                  {"/bin/bash", "-lc",
                                   "for i in {1..8}; do echo fast-$i; sleep 0.2; done"}));

        for (auto& child : children) {
            asio::co_spawn(io_ctx, forward_stdout(child), asio::detached);
        }

        io_ctx.run();

        for (auto& child : children) {
            int status = 0;
            if (child->proc.wait(&status, 0) < 0) {
                std::cerr << "[" << child->name << "] wait failed: "
                          << child->proc.last_error() << "\n";
                continue;
            }
            if (WIFEXITED(status)) {
                std::cout << "[" << child->name << "] exited with code "
                          << WEXITSTATUS(status) << "\n";
            } else if (WIFSIGNALED(status)) {
                std::cout << "[" << child->name << "] terminated by signal "
                          << WTERMSIG(status) << "\n";
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "fatal error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
