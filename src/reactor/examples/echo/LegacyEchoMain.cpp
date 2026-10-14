#include "EchoServer.h"
#include <cstdlib>
#include <iostream>

int main(int argc, char *argv[])
{
    unsigned short port = 8888;
    if (argc >= 2) {
        port = static_cast<unsigned short>(std::atoi(argv[1]));
        if (port == 0) {
            std::cerr << "invalid port: " << argv[1] << std::endl;
            return 1;
        }
    }

    try {
        EchoServer server(4, 1024, "0.0.0.0", port);
        server.start();
    } catch (const std::exception &ex) {
        std::cerr << "fatal: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
