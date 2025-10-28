#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  //#define NOMINMAX TODO: Already defined error. See if I can mitigate this at a later time.... (do I even need this?)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  using SOCKET = int;
#endif

/**
 * 
 */
static void set_nonblocking(SOCKET s) 
{
#ifdef _WIN32
  u_long mode = 1;
  ioctlsocket(s, FIONBIO, &mode);
#else
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

/**
 * 
 */
static void close_socket(SOCKET s) 
{
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

/**
 * 
 */
static long recv_n(SOCKET s, char* buf, size_t n) 
{
    size_t total = 0;
    while (total < n) 
    {
        long r = recv(s, buf + total, (int)(n - total), 0);
        if (r <= 0)
        { 
            return r; // 0=closed, -1=err or would-block
        }
        total += r;
    }

    return (long)total;
}

/**
 * 
 */
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
        if (sent <= 0) return false;
        left -= sent;
        ptr += sent;
    }

    return true;
}

/**
 * 
 */
struct Vec2 
{ 
    float x = 0, y = 0; 
};

/**
 * 
 */
struct Player 
{
    int id;
    SOCKET sock;
    std::string name;
    Vec2 pos;
    std::string recv_buffer; // accumulate incoming bytes
    std::mutex mtx;

    Player(int id_, SOCKET s) : id(id_), sock(s), name("Player" + std::to_string(id_)) { }
};

/**
 * 
 */
class World 
{
    public:
        World() { }

        void add_player(std::shared_ptr<Player> p) 
        {
            std::lock_guard<std::mutex> lg(mtx_);
            players_[p->id] = p;
        }

        void remove_player(int id) 
        {
            std::lock_guard<std::mutex> lg(mtx_);
            players_.erase(id);
        }

        std::vector<std::shared_ptr<Player>> snapshot_players() 
        {
            std::lock_guard<std::mutex> lg(mtx_);
            std::vector<std::shared_ptr<Player>> out;
            for (auto &kv : players_)
            { 
                out.push_back(kv.second);
            }

            return out;
        }

        std::shared_ptr<Player> find_by_sock(SOCKET s) 
        {
            std::lock_guard<std::mutex> lg(mtx_);
            for (auto &kv : players_) 
            {
                if (kv.second->sock == s)
                { 
                    return kv.second;
                }
            }

            return nullptr;
        }

    private:
        std::map<int, std::shared_ptr<Player>> players_;
        std::mutex mtx_;
};

/**
 * 
 */
class Server 
{
public:
    Server(uint16_t port, int tick_hz = 20) : port_(port), tick_ms_(1000 / tick_hz), next_player_id_(1), running_(false) { }

    bool start() 
    {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) 
        {
            std::cerr << "WSAStartup failed\n";
            return false;
        }
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) 
        { 
            perror("socket"); return false; 
        }

        int on = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listener_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) 
        {
            perror("bind");
            close_socket(listener_);
            return false;
        }

        if (listen(listener_, SOMAXCONN) == SOCKET_ERROR) 
        {
            perror("listen");
            close_socket(listener_);
            return false;
        }

        set_nonblocking(listener_);

        running_ = true;
        server_thread_ = std::thread(&Server::run_loop, this);
        std::cout << "Server started on port " << port_ << "\n";

        return true;
    }

    void stop() 
    {
        running_ = false;
        if (server_thread_.joinable()) 
        {
            server_thread_.join();
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    ~Server()
    { 
        stop(); 
    }

private:
    uint16_t port_;
    int tick_ms_;
    SOCKET listener_{INVALID_SOCKET};
    std::atomic<int> next_player_id_;
    std::map<SOCKET, std::shared_ptr<Player>> sock_to_player_;
    std::mutex sock_mtx_;
    World world_;
    std::atomic<bool> running_;
    std::thread server_thread_;

    void run_loop() 
    {
        using clock = std::chrono::steady_clock;
        auto next_tick = clock::now();
        while (running_) 
        {
            // handle incoming connections and I/O
            do_network_io();

            // tick world if it's time
            auto now = clock::now();
            if (now >= next_tick) 
            {
                tick();
                next_tick += std::chrono::milliseconds(tick_ms_);
            } 
            else 
            {
                // sleep a little to avoid busy loop
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // cleanup
        cleanup_sockets();
    }

    void do_network_io() 
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listener_, &readfds);
        SOCKET maxfd = listener_;

        // build fd set
        {
            std::lock_guard<std::mutex> lg(sock_mtx_);
            for (auto &kv : sock_to_player_) 
            {
                SOCKET s = kv.first;
                FD_SET(s, &readfds);
                if (s > maxfd) maxfd = s;
            }
        }

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 0; // immediate return (we'll sleep in loop)

        int ready = select((int)(maxfd + 1), &readfds, nullptr, nullptr, &tv);
        if (ready < 0) 
        {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) 
            {
                return;
            }
#else
            if (errno == EINTR) 
            {
                return;
            }
#endif
            perror("select");
            running_ = false;
            return;
        }

        // new connection?
        if (FD_ISSET(listener_, &readfds)) 
        {
            accept_new();
        }

        // existing sockets
        std::vector<SOCKET> to_remove;
        {
            std::lock_guard<std::mutex> lg(sock_mtx_);
            for (auto &kv : sock_to_player_) 
            {
                SOCKET s = kv.first;
                auto player = kv.second;
                if (FD_ISSET(s, &readfds)) 
                {
                    if (!recv_and_process(player)) 
                    {
                        to_remove.push_back(s);
                    }
                }
            }
        }

        for (SOCKET s : to_remove) 
        {
            drop_connection(s);
        }
    }

    void accept_new() 
    {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        SOCKET cs = accept(listener_, (sockaddr*)&client, &len);
        if (cs == INVALID_SOCKET) 
        {
            return;
        }

        set_nonblocking(cs);
        int pid = next_player_id_++;
        auto p = std::make_shared<Player>(pid, cs);
        {
            std::lock_guard<std::mutex> lg(sock_mtx_);
            sock_to_player_[cs] = p;
        }

        world_.add_player(p);
        std::cout << "Player " << pid << " connected (sock=" << cs << ")\n";
        // greet
        send_packet(cs, "WELCOME " + p->name + "\n");
        broadcast_except(cs, p->name + " has joined the world.\n");
    }

    bool recv_and_process(std::shared_ptr<Player> p) 
    {
        // We will attempt to read available bytes. With non-blocking socket,
        // recv may return -1 with EWOULDBLOCK; treat as no data.
        char buf[4096];
        long r = recv(p->sock, buf, sizeof(buf), 0);
        if (r == 0) 
        {
            // closed
            return false;
        }
#ifdef _WIN32
        if (r == SOCKET_ERROR) 
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) return true;
            return false;
        }
#else
        if (r < 0) 
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN) return true;
            return false;
        }
#endif
        if (r > 0) 
        {
            // append to buffer; then parse length-prefixed messages
            p->recv_buffer.append(buf, buf + r);
            // Try parse as many complete packets as possible
            while (true) 
            {
                if (p->recv_buffer.size() < sizeof(uint32_t)) break;
                uint32_t netlen;
                memcpy(&netlen, p->recv_buffer.data(), sizeof(netlen));
                uint32_t len = ntohl(netlen);

                // sanity limit 10MB
                if (len > 10 * 1024 * 1024) 
                {
                    std::cerr << "Received too-large packet, dropping player\n";
                    return false;
                }

                if (p->recv_buffer.size() < sizeof(netlen) + len) 
                {
                    // incomplete
                    break;
                }

                std::string payload = p->recv_buffer.substr(sizeof(netlen), len);
                p->recv_buffer.erase(0, sizeof(netlen) + len);
                handle_message(p, payload);
            }
        }

        return true;
    }

    void handle_message(std::shared_ptr<Player> p, const std::string &msg) 
    {
        // for now payloads are text commands
        std::string trimmed = msg;
        if (!trimmed.empty() && trimmed.back() == '\n') 
        {
            trimmed.pop_back();
        }
        if (!trimmed.empty() && trimmed.back() == '\r') 
        {
            trimmed.pop_back();
        }

        if (trimmed.empty()) 
        {
            return;
        }

        if (trimmed.rfind("/move ", 0) == 0) 
        {
            // format: /move x y
            std::istringstream iss(trimmed.substr(6));
            float x, y;
            if (iss >> x >> y) 
            {
                std::lock_guard<std::mutex> lg(p->mtx);
                p->pos.x = x; p->pos.y = y;
                // inform others (we'll also send periodic updates in tick)
                broadcast_except(p->sock, p->name + " moved to " + std::to_string(x) + "," + std::to_string(y) + "\n");
            } 
            else 
            {
                send_packet(p->sock, "ERROR bad /move format\n");
            }
        } 
        else if (trimmed.rfind("/name ", 0) == 0) 
        {
            std::string newname = trimmed.substr(6);
            if (!newname.empty()) 
            {
                std::string old = p->name;
                p->name = newname;
                broadcast_except(p->sock, old + " is now " + newname + "\n");
            }
        } 
        else if (trimmed.rfind("/say ", 0) == 0) 
        {
            std::string body = trimmed.substr(5);
            broadcast(p->name + ": " + body + "\n");
        } 
        else 
        {
            send_packet(p->sock, "UNKNOWN_CMD\n");
        }
    }

    /**
     * This is the authoritative tick where server updates world and sends snapshots.
     * For now: send a simple snapshot every tick with every player's pos.
     */
    void tick() 
    {
        auto players = world_.snapshot_players();
        std::ostringstream oss;
        oss << "SNAP\n";
        for (auto &p : players) 
        {
            std::lock_guard<std::mutex> lg(p->mtx);
            oss << p->id << " " << p->name << " " << p->pos.x << " " << p->pos.y << "\n";
        }

        std::string snap = oss.str();

        // Broadcast to everyone
        broadcast_raw(snap);
    }

    void broadcast(const std::string &msg) 
    {
        std::lock_guard<std::mutex> lg(sock_mtx_);
        for (auto &kv : sock_to_player_) 
        {
            send_packet(kv.first, msg);
        }
    }

    void broadcast_except(SOCKET except_sock, const std::string &msg) 
    {
        std::lock_guard<std::mutex> lg(sock_mtx_);
        for (auto &kv : sock_to_player_) 
        {
            if (kv.first == except_sock) continue;
            send_packet(kv.first, msg);
        }
    }

    /**
     * Send a raw text snapshot without length prefix helper (we still call send_packet)
    */
    void broadcast_raw(const std::string &raw) 
    {
        std::lock_guard<std::mutex> lg(sock_mtx_);
        for (auto &kv : sock_to_player_) 
        {
            send_packet(kv.first, raw);
        }
    }

    void drop_connection(SOCKET s) 
    {
        std::shared_ptr<Player> p;
        {
            std::lock_guard<std::mutex> lg(sock_mtx_);
            auto it = sock_to_player_.find(s);
            if (it == sock_to_player_.end()) return;
            p = it->second;
            sock_to_player_.erase(it);
        }
        if (p) 
        {
            world_.remove_player(p->id);
            std::cout << "Player " << p->id << " disconnected\n";
            broadcast(p->name + " has left the world.\n");
            close_socket(s);
        }
    }

    void cleanup_sockets() 
    {
        std::lock_guard<std::mutex> lg(sock_mtx_);
        for (auto &kv : sock_to_player_) 
        {
            close_socket(kv.first);
        }
        sock_to_player_.clear();
        if (listener_ != INVALID_SOCKET) 
        {
            close_socket(listener_);
            listener_ = INVALID_SOCKET;
        }
    }
};

/**
 * Main
 */
int main(int argc, char** argv) 
{
    uint16_t port = 5182;
    if (argc > 1) 
    {
        port = (uint16_t)atoi(argv[1]);
    }
    Server s(port, 20); // 20 ticks/sec

    if (!s.start()) 
    {
        std::cerr << "Failed to start server\n";
        return 1;
    }

    std::cout << "Press ENTER to stop the server...\n";
    std::string line;
    std::getline(std::cin, line);
    s.stop();
    std::cout << "Server stopped.\n";

    return 0;
}