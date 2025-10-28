#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <mutex>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  using SOCKET = int;
#endif

static void close_socket(SOCKET s) 
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static bool send_packet(SOCKET s, const std::string &payload) 
{
    uint32_t len = (uint32_t)payload.size();
    uint32_t netlen = htonl(len);
    std::string out;
    out.resize(sizeof(netlen) + payload.size());
    memcpy(&out[0], &netlen, sizeof(netlen));
    if (!payload.empty()) 
    {
        memcpy(&out[sizeof(netlen)], payload.data(), payload.size());
    }

    const char* ptr = out.data();
    size_t left = out.size();
    while (left > 0) 
    {
        long sent = send(s, ptr, (int)left, 0);
        if (sent <= 0) 
        {
            return false;
        }
        left -= sent;
        ptr += sent;
    }

    return true;
}

static bool recv_packet(SOCKET s, std::string &out) 
{
    uint32_t netlen;
    long r = recv(s, (char*)&netlen, sizeof(netlen), MSG_WAITALL);
    if (r == 0) 
    {
        return false;
    }
    if (r != sizeof(netlen)) 
    {
        return false;
    }
    uint32_t len = ntohl(netlen);
    if (len > 10 * 1024 * 1024) 
    {
        return false;
    }
    out.resize(len);
    r = recv(s, &out[0], len, MSG_WAITALL);
    if (r <= 0) 
    {
        return false;
    }

    return true;
}

int main(int argc, char** argv) 
{
    std::string host = "127.0.0.1";
    uint16_t port = 5182;
    if (argc > 1) 
    {
        host = argv[1];
    }
    if (argc > 2) 
    {
        port = (uint16_t)atoi(argv[2]);
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) 
    {
        std::cerr << "socket() failed\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) 
    {
        std::cerr << "connect() failed\n";
        close_socket(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "Connected to server " << host << ":" << port << "\n";
    std::atomic<bool> running{true};

    // Reader thread: continuously receive packets
    std::thread reader([&]() 
    {
        while (running) 
        {
            std::string msg;
            if (!recv_packet(s, msg)) 
            {
                std::cout << "Server closed connection or error.\n";
                running = false;
                break;
            }

            std::cout << "[SERVER] " << msg;
        }
    });

    // Main input loop: read from stdin, send to server
    std::cout << "Type commands (/name, /say, /move x y). Type /quit to exit.\n";
    std::string line;
    while (running && std::getline(std::cin, line)) 
    {
        if (line == "/quit") 
        {
            running = false;
            break;
        }
        if (!send_packet(s, line + "\n")) 
        {
            std::cout << "Send failed.\n";
            running = false;
            break;
        }
    }

    close_socket(s);
    running = false;
    if (reader.joinable()) 
    {
        reader.join();
    }

#ifdef _WIN32
    WSACleanup();
#endif

    std::cout << "Disconnected.\n";
    return 0;
}
