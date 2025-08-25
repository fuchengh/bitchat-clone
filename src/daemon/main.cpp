#include <iostream>
#include "app/chat_service.hpp"
#include "crypto/psk_aead.hpp"
#include "transport/itransport.hpp"
#include "util/constants.hpp"

// NOTE: This is a stub main that doesn't wire real logic yet.

int main(int argc, char **argv)
{
    std::cout << "bitchatd (stub) — TODO: implement serve loop, transport, IPC\n";
    (void)argc;
    (void)argv;
    return 0;
}
