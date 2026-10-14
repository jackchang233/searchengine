#include "Configuration.h"
#include "SearchEngineServer.h"

#include <exception>
#include <iostream>
#include <map>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <thread>
#include <unistd.h>

namespace {

class ShutdownSignals
{
public:
    explicit ShutdownSignals(SearchEngineServer &server)
    : _signalFd(-1), _cancelFd(-1), _previous(), _watcher()
    {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &signals, &_previous) != 0) {
            throw std::runtime_error("could not block shutdown signals");
        }
        try {
            _signalFd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
            _cancelFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (_signalFd < 0 || _cancelFd < 0) {
                throw std::runtime_error("could not create shutdown notifier");
            }
            _watcher = std::thread([this, &server] {
                pollfd fds[2] = {{_signalFd, POLLIN, 0},
                                 {_cancelFd, POLLIN, 0}};
                while (true) {
                    const int ready = ::poll(fds, 2, -1);
                    if (ready < 0 && errno == EINTR) {
                        continue;
                    }
                    if (ready <= 0 || (fds[1].revents & POLLIN)) {
                        return;
                    }
                    if (fds[0].revents & POLLIN) {
                        signalfd_siginfo info;
                        if (::read(_signalFd, &info, sizeof(info)) ==
                            sizeof(info)) {
                            server.stop();
                        }
                        return;
                    }
                }
            });
        } catch (...) {
            if (_signalFd >= 0) ::close(_signalFd);
            if (_cancelFd >= 0) ::close(_cancelFd);
            pthread_sigmask(SIG_SETMASK, &_previous, nullptr);
            throw;
        }
    }

    ~ShutdownSignals()
    {
        const uint64_t one = 1;
        ::write(_cancelFd, &one, sizeof(one));
        _watcher.join();
        ::close(_signalFd);
        ::close(_cancelFd);
        pthread_sigmask(SIG_SETMASK, &_previous, nullptr);
    }

private:
    int _signalFd;
    int _cancelFd;
    sigset_t _previous;
    std::thread _watcher;
};

} // namespace

int main(int argc, char *argv[])
{
    try {
        std::string config = "conf/myconf.conf";
        std::map<std::string, std::string> overrides;
        bool hasConfig = false;
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "usage: " << argv[0]
                      << " [--config FILE] [--host IPv4] [--port NUMBER]\n";
            return 0;
        }
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing option value");
            }
            const std::string option = argv[index];
            const std::string value = argv[index + 1];
            if (option == "--config" && !hasConfig) {
                config = value;
                hasConfig = true;
            } else if (option == "--host" &&
                       overrides.count("server_ip") == 0) {
                overrides["server_ip"] = value;
            } else if (option == "--port" &&
                       overrides.count("server_port") == 0) {
                overrides["server_port"] = value;
            } else {
                throw std::invalid_argument("unknown or duplicate option: " +
                                            option);
            }
        }
        const ServerOptions options = Configuration::load(config, overrides);
        SearchEngineServer server(options);
        ShutdownSignals signals(server);
        server.start();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "searchengine: " << error.what() << '\n';
        return 1;
    }
}
