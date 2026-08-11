/*
 * 斗地主 - 局域网联机版 v2.0
 * 参考狼人杀架构重写：多线程 + UDP发现 + TCP转发
 * Dev-C++ (MinGW64 g++) 可编译
 *
 * 编译命令: g++ -std=c++11 -o doudizhu.exe doudizhu.cpp -lws2_32 -static-libgcc -static-libstdc++
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
# endif
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h>
# include <windows.h>
# include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
# include <arpa/inet.h>
# include <netdb.h>
# include <netinet/in.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

static constexpr uint16_t kDiscoveryPort = 37020;
static constexpr uint16_t kGamePort = 9527;
static constexpr int kMaxPlayers = 3;
static constexpr int kMaxLine = 4096;

// ==================== 工具函数 ====================
static std::mutex g_print_mu;
static void safePrintln(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_print_mu);
    std::cout << s << std::endl;
}
static void safePrint(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_print_mu);
    std::cout << s << std::flush;
}
static void msleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void closeSocket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static bool setReuseAddr(socket_t s) {
    int yes = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes)) == 0;
}

static bool setBroadcast(socket_t s) {
    int yes = 1;
    return setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes)) == 0;
}

static std::string sockaddrToIp(const sockaddr_in& addr) {
#ifdef _WIN32
    const char* p = inet_ntoa(addr.sin_addr);
    return p ? std::string(p) : std::string("0.0.0.0");
#else
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, (void*)&addr.sin_addr, buf, sizeof(buf));
    return std::string(buf);
#endif
}

static std::string trimCRLF(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream iss(s);
    while (std::getline(iss, cur, delim)) out.push_back(cur);
    return out;
}

static std::string join(const std::vector<std::string>& v, char delim) {
    std::ostringstream oss;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) oss << delim;
        oss << v[i];
    }
    return oss.str();
}

static bool recvLine(socket_t s, std::string& out_line) {
    out_line.clear();
    char ch;
    while (true) {
#ifdef _WIN32
        int n = recv(s, &ch, 1, 0);
#else
        ssize_t n = recv(s, &ch, 1, 0);
#endif
        if (n <= 0) return false;
        out_line.push_back(ch);
        if ((int)out_line.size() > kMaxLine) return false;
        if (ch == '\n') break;
    }
    out_line = trimCRLF(out_line);
    return true;
}

static bool sendAll(socket_t s, const std::string& data) {
    const char* p = data.data();
    int left = (int)data.size();
    while (left > 0) {
#ifdef _WIN32
        int n = send(s, p, left, 0);
#else
        ssize_t n = send(s, p, left, 0);
#endif
        if (n <= 0) return false;
        p += n;
        left -= (int)n;
    }
    return true;
}

static bool sendLine(socket_t s, const std::string& line) {
    return sendAll(s, line + "\n");
}

static std::string getLocalIP() {
    char hn[256];
    gethostname(hn, sizeof(hn));
    hostent* he = gethostbyname(hn);
    if (he) {
        for (int i = 0; he->h_addr_list[i]; i++) {
            std::string ip = inet_ntoa(*(in_addr*)he->h_addr_list[i]);
            if (ip != "127.0.0.1") return ip;
        }
    }
    return "127.0.0.1";
}

// ==================== SocketInit ====================
struct SocketInit {
    SocketInit() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif
    }
    ~SocketInit() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

// ==================== Card & Hand ====================
struct Card {
    int suit; // 0=黑桃 1=红桃 2=梅花 3=方块 -1=王
    int rank; // 3-15(A), 16(2), 17(小王), 18(大王)
    int id;   // 0-53

    std::string toString() const {
        if (rank == 17) return "SJ";
        if (rank == 18) return "BJ";
        if (rank >= 3 && rank <= 15) {
            const char* rr[] = {"3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"};
            return rr[rank - 3];
        }
        if (rank == 16) return "2";
        return "?";
    }
    bool operator<(const Card& c) const { return rank < c.rank || (rank == c.rank && suit < c.suit); }
};

enum HandType {
    HT_INVALID = 0, HT_SINGLE, HT_PAIR, HT_TRIPLE, HT_TRIPLE_ONE, HT_TRIPLE_TWO,
    HT_STRAIGHT, HT_STRAIGHT_PAIR, HT_PLANE, HT_PLANE_SINGLE, HT_PLANE_PAIR,
    HT_FOUR_TWO, HT_FOUR_TWO_PAIR, HT_BOMB, HT_ROCKET
};

struct Hand {
    HandType type;
    int mainRank;
    int length;
    std::vector<Card> cards;
    bool canBeat(const Hand& o) const {
        if (type == HT_ROCKET) return true;
        if (o.type == HT_ROCKET) return false;
        if (type == HT_BOMB && o.type != HT_BOMB) return true;
        if (type != HT_BOMB && o.type == HT_BOMB) return false;
        if (type != o.type || length != o.length) return false;
        return mainRank > o.mainRank;
    }
};

static std::string handTypeName(HandType t) {
    const char* names[] = {"无效", "单张", "对子", "三张", "三带一", "三带二", "顺子", "连对", "飞机", "飞机带单", "飞机带对", "四带二", "四带二对", "炸弹", "火箭"};
    const size_t count = sizeof(names) / sizeof(names[0]);
    if (t < 0 || t >= (int)count) return "未知";
    return names[t];
}

// ==================== GameLogic ====================
class GameLogic {
public:
    static std::vector<Card> createDeck() {
        std::vector<Card> deck;
        for (int s = 0; s < 4; s++)
            for (int r = 3; r <= 15; r++)
                deck.push_back({s, r, (int)deck.size()});
        deck.push_back({-1, 17, 52});
        deck.push_back({-1, 18, 53});
        return deck;
    }

    static void shuffle(std::vector<Card>& deck) {
        static unsigned seedCount = 0;
        unsigned seed = (unsigned)time(nullptr) + seedCount++;
        std::mt19937 rng(seed);
        std::shuffle(deck.begin(), deck.end(), rng);
    }

    static Hand analyzeHand(const std::vector<Card>& cards) {
        Hand h = {HT_INVALID, 0, 0, cards};
        int n = (int)cards.size();
        if (n == 0) return h;

        int cnt[19] = {0};
        for (auto& c : cards) cnt[c.rank]++;
        std::vector<int> ranks;
        for (int r = 3; r <= 18; r++) if (cnt[r] > 0) ranks.push_back(r);

        if (n == 2 && cnt[17] && cnt[18]) { h.type = HT_ROCKET; h.mainRank = 18; return h; }
        if (n == 1) { h.type = HT_SINGLE; h.mainRank = cards[0].rank; return h; }
        if (n == 2 && ranks.size() == 1 && cnt[ranks[0]] == 2) { h.type = HT_PAIR; h.mainRank = ranks[0]; return h; }
        if (n == 3 && ranks.size() == 1 && cnt[ranks[0]] == 3) { h.type = HT_TRIPLE; h.mainRank = ranks[0]; return h; }
        if (n == 4 && ranks.size() == 1 && cnt[ranks[0]] == 4) { h.type = HT_BOMB; h.mainRank = ranks[0]; return h; }

        if (n == 4) { for (int r : ranks) if (cnt[r] == 3) { h.type = HT_TRIPLE_ONE; h.mainRank = r; return h; } }
        if (n == 5) {
            int tri = -1, pair = -1;
            for (int r : ranks) { if (cnt[r] == 3) tri = r; if (cnt[r] == 2) pair = r; }
            if (tri >= 0 && pair >= 0) { h.type = HT_TRIPLE_TWO; h.mainRank = tri; return h; }
        }

        // 顺子: 5张以上连续单牌, A(15)可以参与, 2(16)和王(17,18)不能
        if (n >= 5 && (int)ranks.size() == n) {
            bool ok = true;
            for (int r : ranks) if (r > 15 || cnt[r] != 1) { ok = false; break; }
            if (ok) {
                sort(ranks.begin(), ranks.end());
                for (int i = 1; i < (int)ranks.size(); i++) if (ranks[i] != ranks[i - 1] + 1) { ok = false; break; }
                if (ok) { h.type = HT_STRAIGHT; h.mainRank = ranks.back(); h.length = n; return h; }
            }
        }

        // 连对: 3对以上连续对子, A(15)可以参与, 2(16)和王不能
        if (n >= 6 && n % 2 == 0) {
            int pairs = n / 2;
            bool ok = true;
            for (int r : ranks) if (r > 15 || cnt[r] != 2) { ok = false; break; }
            if (ok && (int)ranks.size() == pairs) {
                sort(ranks.begin(), ranks.end());
                for (int i = 1; i < (int)ranks.size(); i++) if (ranks[i] != ranks[i - 1] + 1) { ok = false; break; }
                if (ok) { h.type = HT_STRAIGHT_PAIR; h.mainRank = ranks.back(); h.length = pairs; return h; }
            }
        }

        // 飞机: A(15)可以参与, 2(16)和王不能
        {
            std::vector<int> triRanks;
            for (int r : ranks) if (cnt[r] >= 3 && r <= 15) triRanks.push_back(r);
            sort(triRanks.begin(), triRanks.end());
            for (int len = (int)triRanks.size(); len >= 2; len--) {
                for (int start = 0; start + len - 1 < (int)triRanks.size(); start++) {
                    bool ok = true;
                    for (int i = 1; i < len; i++) if (triRanks[start + i] != triRanks[start] + i) { ok = false; break; }
                    if (ok) {
                        int remain = n - len * 3;
                        if (remain == 0) { h.type = HT_PLANE; h.mainRank = triRanks[start + len - 1]; h.length = len; return h; }
                        if (remain == len) {
                            // 验证剩余牌是否全为单张
                            int rc[19]; memcpy(rc, cnt, sizeof(rc));
                            for (int i = 0; i < len; i++) rc[triRanks[start + i]] -= 3;
                            bool allSingle = true;
                            for (int r = 3; r <= 18; r++) {
                                if (rc[r] != 0 && rc[r] != 1) { allSingle = false; break; }
                            }
                            if (allSingle) { h.type = HT_PLANE_SINGLE; h.mainRank = triRanks[start + len - 1]; h.length = len; return h; }
                        }
                        if (remain == len * 2) {
                            int rc[19]; memcpy(rc, cnt, sizeof(rc));
                            for (int i = 0; i < len; i++) rc[triRanks[start + i]] -= 3;
                            bool allPairs = true; int pc = 0;
                            for (int r = 3; r <= 18; r++) {
                                if (rc[r] == 1 || rc[r] == 3) { allPairs = false; break; }
                                if (rc[r] == 2) pc++;
                                if (rc[r] == 4) pc += 2;
                            }
                            if (allPairs && pc == len) { h.type = HT_PLANE_PAIR; h.mainRank = triRanks[start + len - 1]; h.length = len; return h; }
                        }
                    }
                }
            }
        }

        if (n == 6) {
            for (int r : ranks) if (cnt[r] == 4) {
                // 确保剩余两张是单张
                bool valid = true;
                for (int rr : ranks) if (rr != r && cnt[rr] != 1) { valid = false; break; }
                if (valid) { h.type = HT_FOUR_TWO; h.mainRank = r; return h; }
            }
        }
        if (n == 8) {
            int four = -1, pc = 0;
            for (int r : ranks) { if (cnt[r] == 4) four = r; if (cnt[r] == 2) pc++; }
            if (four >= 0 && pc == 2) { h.type = HT_FOUR_TWO_PAIR; h.mainRank = four; return h; }
        }
        return h;
    }
};

// ==================== 序列化 ====================
static std::string cardsToStr(const std::vector<Card>& cards) {
    std::string s;
    for (size_t i = 0; i < cards.size(); i++) { if (i) s += ","; s += std::to_string(cards[i].id); }
    return s;
}

static std::vector<Card> strToCards(const std::string& s) {
    std::vector<Card> cards;
    std::istringstream iss(s); std::string tok;
    while (std::getline(iss, tok, ',')) {
        if (tok.empty()) continue;
        try {
            int id = std::stoi(tok);
            if (id == 52) cards.push_back({-1, 17, 52});
            else if (id == 53) cards.push_back({-1, 18, 53});
            else if (id >= 0 && id < 52) cards.push_back({id / 13, id % 13 + 3, id});
        } catch (...) {
            // 忽略无效id
        }
    }
    return cards;
}

static std::string cardsDisplayStr(const std::vector<Card>& cards) {
    std::string s;
    for (size_t i = 0; i < cards.size(); i++) { if (i) s += " "; s += cards[i].toString(); }
    return s;
}

// ==================== UDP 房间发现 ====================
struct RoomInfo {
    std::string id;
    std::string roomName;
    std::string hostIp;
    uint16_t tcpPort = 0;
    int playerCount = 0;
    int maxPlayers = 0;
};

class DiscoveryResponder {
public:
    DiscoveryResponder() = default;
    ~DiscoveryResponder() { stop(); }

    void start(std::function<std::string()> provider) {
        stop();
        running_.store(true);
        provider_ = std::move(provider);
        th_ = std::thread([this] { loop(); });
    }

    void stop() {
        running_.store(false);
        if (sock_ != kInvalidSocket) { closeSocket(sock_); sock_ = kInvalidSocket; }
        if (th_.joinable()) th_.join();
    }

private:
    void loop() {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == kInvalidSocket) { safePrintln("[ERR] UDP socket创建失败"); return; }
        setReuseAddr(sock_);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(kDiscoveryPort);
        if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
            safePrintln("[ERR] UDP bind失败");
            closeSocket(sock_);
            sock_ = kInvalidSocket;
            return;
        }

        char buf[1024];
        while (running_.load()) {
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);
#ifdef _WIN32
            int n = recvfrom(sock_, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromlen);
#else
            ssize_t n = recvfrom(sock_, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromlen);
#endif
            if (n <= 0) { msleep(10); continue; }
            buf[n] = 0;
            std::string msg = trimCRLF(std::string(buf));
            if (msg == "DD_DISCOVER") {
                std::string resp = provider_ ? provider_() : "";
                if (!resp.empty()) {
                    std::string out = resp + "\n";
                    sendto(sock_, out.c_str(), (int)out.size(), 0, (sockaddr*)&from, fromlen);
                }
            }
        }
    }

    std::atomic<bool> running_{false};
    socket_t sock_{kInvalidSocket};
    std::thread th_;
    std::function<std::string()> provider_;
};

static std::vector<RoomInfo> discoverRooms(int waitMs) {
    std::vector<RoomInfo> rooms;
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidSocket) return rooms;
    setBroadcast(s);
    setReuseAddr(s);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kDiscoveryPort);
    to.sin_addr.s_addr = INADDR_BROADCAST;
    const char* msg = "DD_DISCOVER\n";
    sendto(s, msg, (int)strlen(msg), 0, (sockaddr*)&to, sizeof(to));

    auto start = std::chrono::steady_clock::now();
    char buf[1024];
    while (true) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        int left = waitMs - elapsed;
        if (left <= 0) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        timeval tv{};
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;
        int ret = select((int)(s + 1), &rfds, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
#ifdef _WIN32
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromlen);
#else
        ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromlen);
#endif
        if (n <= 0) continue;
        buf[n] = 0;
        std::string line = trimCRLF(std::string(buf));
        auto parts = split(line, '|');
        if (parts.size() == 6 && parts[0] == "DD_ROOM") {
            try {
                RoomInfo ri;
                ri.id = parts[1];
                ri.roomName = parts[2];
                ri.hostIp = sockaddrToIp(from);
                ri.tcpPort = (uint16_t)std::stoi(parts[3]);
                ri.playerCount = std::stoi(parts[4]);
                ri.maxPlayers = std::stoi(parts[5]);
                bool dup = false;
                for (auto& r : rooms) if (r.id == ri.id && r.hostIp == ri.hostIp) { dup = true; break; }
                if (!dup) rooms.push_back(std::move(ri));
            } catch (...) {
                // 忽略格式错误的响应
            }
        }
    }
    closeSocket(s);
    return rooms;
}

// ==================== 游戏状态 ====================
enum class Phase { Lobby, CallLandlord, Play, GameOver };

struct Player {
    socket_t sock = kInvalidSocket;
    std::string name;
    std::vector<Card> hand;
    int handCount = 0; // 客户端用
    bool isLandlord = false;
    bool called = false;
    int callScore = 0;
};

// ==================== 主机 ====================
class GameHost {
public:
    ~GameHost() { stop(); }

    bool start(const std::string& roomName, const std::string& hostName) {
        stop();
        roomName_ = roomName;
        hostName_ = hostName;
        hostPlayer_.name = hostName;
        hostPlayer_.sock = kInvalidSocket; // 房主没有socket
        roomId_ = []() {
            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> dist(0, 15);
            std::string s; for (int i = 0; i < 8; i++) s.push_back("0123456789abcdef"[dist(rng)]);
            return s;
        }();

        listenSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock_ == kInvalidSocket) {
#ifdef _WIN32
            safePrintln("[ERR] socket创建失败, WSA=" + std::to_string(WSAGetLastError()));
#else
            safePrintln("[ERR] socket创建失败");
#endif
            return false;
        }
        setReuseAddr(listenSock_);

        bool bound = false;
        for (int i = 0; i < 50; i++) {
            uint16_t p = (uint16_t)(kGamePort + i);
            sockaddr_in addr{};
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(p);
            if (bind(listenSock_, (sockaddr*)&addr, sizeof(addr)) == 0) { tcpPort_ = p; bound = true; break; }
        }
        if (!bound) {
#ifdef _WIN32
            safePrintln("[ERR] bind失败, 端口" + std::to_string(kGamePort) + "~" + std::to_string(kGamePort+49) + "均被占用, WSA=" + std::to_string(WSAGetLastError()));
#else
            safePrintln("[ERR] bind失败, 端口" + std::to_string(kGamePort) + "~" + std::to_string(kGamePort+49) + "均被占用");
#endif
            closeSocket(listenSock_); listenSock_ = kInvalidSocket; return false;
        }
        if (listen(listenSock_, 16) != 0) {
#ifdef _WIN32
            safePrintln("[ERR] listen失败, WSA=" + std::to_string(WSAGetLastError()));
#else
            safePrintln("[ERR] listen失败");
#endif
            closeSocket(listenSock_); listenSock_ = kInvalidSocket; return false;
        }

        running_.store(true);
        acceptTh_ = std::thread([this] { acceptLoop(); });
        responder_.start([this] { return discoveryResponse(); });
        safePrintln("[SYS] 房间已创建: \"" + roomName_ + "\" 端口=" + std::to_string(tcpPort_));
        return true;
    }

    void stop() {
        running_.store(false);
        responder_.stop();
        if (listenSock_ != kInvalidSocket) { closeSocket(listenSock_); listenSock_ = kInvalidSocket; }
        if (acceptTh_.joinable()) acceptTh_.join();

        // 先关闭所有socket，再收集线程
        std::vector<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& kv : players_) closeSocket(kv.first);
            for (auto& kv : threads_) {
                if (kv.second.joinable()) toJoin.push_back(std::move(kv.second));
            }
            players_.clear();
            threads_.clear();
        }
        for (auto& t : toJoin) {
            if (t.joinable()) t.join();
        }

        // 重置所有游戏状态
        phase_ = Phase::Lobby;
        hostPlayer_.hand.clear();
        hostPlayer_.isLandlord = false;
        hostPlayer_.called = false;
        hostPlayer_.callScore = 0;
        bottomCards_.clear();
        currentTurn_ = 0;
        callScore_ = 0;
        landlord_ = -1;
        callCount_ = 0;
        lastPlay_ = {HT_INVALID, 0, 0, {}};
        lastPlayer_ = -1;
        passCount_ = 0;
        {
            std::lock_guard<std::mutex> lk(msgMu_);
            hostMsgQueue_.clear();
        }
    }

    void hostLoop() {
        while (running_.load()) {
            if (phase_ == Phase::Lobby) {
                safePrintln("\n[指令] /start=开始游戏 /players=查看玩家 /help /quit=退出");
                safePrint("> ");
                std::string cmd;
                std::getline(std::cin, cmd);
                if (cmd == "/start") {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (phase_ == Phase::Lobby) {
                        if (1 + (int)players_.size() < kMaxPlayers) {
                            safePrintln("[ERR] 人数不足，无法开始");
                        } else {
                            startGame();
                        }
                    } else {
                        safePrintln("[ERR] 游戏已经开始");
                    }
                } else if (cmd == "/players") {
                    safePrintln(playersString());
                } else if (cmd == "/help") {
                    safePrintln("指令: /start /players /help /quit");
                } else if (cmd == "/quit") {
                    running_.store(false);
                    break;
                } else if (!cmd.empty()) {
                    broadcastLine("CHAT|" + hostName_ + "|" + cmd);
                }
            } else {
                // 游戏中：处理消息队列 + 等待输入
                std::string line;
                {
                    std::unique_lock<std::mutex> lk(msgMu_);
                    msgCv_.wait(lk, [this] { return !hostMsgQueue_.empty() || !running_.load(); });
                    while (!hostMsgQueue_.empty()) {
                        line = hostMsgQueue_.front();
                        hostMsgQueue_.pop_front();
                        lk.unlock();
                        handleHostMsg(line);
                        lk.lock();
                    }
                }

                // 检查是否轮到房主
                if (phase_ == Phase::CallLandlord && currentTurn_ == 0) {
                    while (true) {
                        safePrintln("\n[叫地主] 请选择: 1/2/3=叫地主 0=不叫");
                        safePrint("> ");
                        std::string input;
                        std::getline(std::cin, input);
                        int score = 0;
                        try { score = std::stoi(input); } catch (...) {
                            safePrintln("[ERR] 请输入数字 0~3");
                            continue;
                        }
                        if (score < 0 || score > 3) {
                            safePrintln("[ERR] 请输入数字 0~3");
                            continue;
                        }
                        handleAction(0, "CALL", std::to_string(score));
                        break;
                    }
                } else if (phase_ == Phase::Play && currentTurn_ == 0) {
                    while (true) {
                        bool canPass = (lastPlay_.type != HT_INVALID && lastPlayer_ != 0);
                        safePrintln("\n[出牌] 输入牌编号(逗号分隔)" + std::string(canPass ? ", 或 pass" : ""));
                        safePrint("> ");
                        std::string input;
                        std::getline(std::cin, input);
                        if (input.empty()) {
                            safePrintln("[ERR] 输入不能为空");
                            continue;
                        }
                        if (input == "pass") {
                            if (!canPass) {
                                safePrintln("[ERR] 自由出牌时不能跳过");
                                continue;
                            }
                            handleAction(0, "PASS", "");
                            break;
                        }
                        std::vector<Card> playCards;
                        std::istringstream iss(input); std::string tok;
                        bool hasInvalid = false;
                        while (std::getline(iss, tok, ',')) {
                            try {
                                int idx = std::stoi(tok);
                                if (idx >= 0 && idx < (int)hostPlayer_.hand.size())
                                    playCards.push_back(hostPlayer_.hand[idx]);
                                else { hasInvalid = true; }
                            } catch (...) { hasInvalid = true; }
                        }
                        if (hasInvalid || playCards.empty()) {
                            safePrintln("[ERR] 无效的牌编号");
                            continue;
                        }
                        handleAction(0, "CARDS", cardsToStr(playCards));
                        break;
                    }
                } else if (phase_ == Phase::GameOver) {
                    safePrintln("\n按回车键退出...");
                    std::string dummy;
                    std::getline(std::cin, dummy);
                    running_.store(false);
                    break;
                }
            }
        }
    }

    void handleHostMsg(const std::string& msg) {
        auto parts = split(msg, '|');
        if (parts.empty()) return;

        if (parts[0] == "SYS") {
            safePrintln("[系统] " + (parts.size() > 1 ? parts[1] : ""));
        } else if (parts[0] == "CHAT") {
            if (parts.size() >= 3) safePrintln("[" + parts[1] + "] " + parts[2]);
        } else if (parts[0] == "START") {
            safePrintln("[系统] 游戏开始！你是玩家1(房主)");
        } else if (parts[0] == "CARDS") {
            if (parts.size() > 1) {
                std::lock_guard<std::mutex> lk(mu_);
                hostPlayer_.hand = strToCards(parts[1]);
                sort(hostPlayer_.hand.begin(), hostPlayer_.hand.end());
                safePrintln("\n[你的手牌] " + cardsDisplayStr(hostPlayer_.hand));
            }
        } else if (parts[0] == "BOTTOM") {
            // 叫地主阶段只显示数量，确定地主后才显示内容
            if (parts.size() > 1 && parts[1] == "3") {
                safePrintln("[底牌] ??? ??? ??? (3张)");
            } else if (parts.size() > 1) {
                std::lock_guard<std::mutex> lk(mu_);
                bottomCards_ = strToCards(parts[1]);
                safePrintln("[底牌] " + cardsDisplayStr(bottomCards_));
            }
        } else if (parts[0] == "CALL_TURN") {
            if (parts.size() > 1) {
                try { currentTurn_ = std::stoi(parts[1]); } catch (...) { return; }
                safePrintln("当前叫地主: " + getPlayerName(currentTurn_));
            }
        } else if (parts[0] == "CALL_RESULT") {
            if (parts.size() >= 3) {
                try {
                    int player = std::stoi(parts[1]);
                    int score = std::stoi(parts[2]);
                    safePrintln(getPlayerName(player) + (score > 0 ? " 叫了" + std::to_string(score) + "分" : " 不叫"));
                } catch (...) {}
            }
        } else if (parts[0] == "LANDLORD") {
            if (parts.size() > 1) {
                try {
                    int ll;
                    ll = std::stoi(parts[1]);
                    std::lock_guard<std::mutex> lk(mu_);
                    landlord_ = ll;
                } catch (...) { return; }
                safePrintln("[系统] 地主是" + getPlayerName(landlord_));
            }
        } else if (parts[0] == "MY_CARDS") {
            if (parts.size() > 1) {
                std::lock_guard<std::mutex> lk(mu_);
                hostPlayer_.hand = strToCards(parts[1]);
                sort(hostPlayer_.hand.begin(), hostPlayer_.hand.end());
                safePrintln("\n[你的手牌(地主)] " + cardsDisplayStr(hostPlayer_.hand));
            }
        } else if (parts[0] == "TURN") {
            if (parts.size() > 1) {
                try { currentTurn_ = std::stoi(parts[1]); } catch (...) { return; }
                safePrintln("轮到: " + getPlayerName(currentTurn_));
            }
        } else if (parts[0] == "PLAY") {
            if (parts.size() >= 3 && parts[1] == "PASS") {
                try {
                    int player = std::stoi(parts[2]);
                    safePrintln(getPlayerName(player) + " 不出");
                } catch (...) {}
            } else if (parts.size() >= 4 && parts[1] == "CARDS") {
                try {
                    int player = std::stoi(parts[2]);
                    std::vector<Card> played = strToCards(parts[3]);
                    Hand h = GameLogic::analyzeHand(played);
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        lastPlay_ = h;
                        lastPlayer_ = player;
                    }
                    safePrintln(getPlayerName(player) + ": " + cardsDisplayStr(played) + " [" + handTypeName(h.type) + "]");
                } catch (...) {}
            }
        } else if (parts[0] == "WIN") {
            if (parts.size() > 1) {
                try {
                    int winner = std::stoi(parts[1]);
                    std::lock_guard<std::mutex> lk(mu_);
                    phase_ = Phase::GameOver;
                    safePrintln("[系统] 游戏结束！" + getPlayerName(winner) + " 获胜");
                } catch (...) {}
            }
        }
    }

private:
    std::string discoveryResponse() const {
        std::lock_guard<std::mutex> lk(mu_);
        int pc = 1 + (int)players_.size();
        std::string name = roomName_;
        if (phase_ != Phase::Lobby) name += " [对局中]";
        std::ostringstream oss;
        oss << "DD_ROOM|" << roomId_ << "|" << name << "|" << tcpPort_ << "|" << pc << "|" << kMaxPlayers;
        return oss.str();
    }

    void acceptLoop() {
        while (running_.load()) {
            sockaddr_in cli{};
            socklen_t len = sizeof(cli);
            socket_t cs = accept(listenSock_, (sockaddr*)&cli, &len);
            if (cs == kInvalidSocket) { msleep(10); continue; }
            setReuseAddr(cs);

            std::string line;
            if (!recvLine(cs, line)) { closeSocket(cs); continue; }
            auto parts = split(line, '|');
            if (parts.size() != 2 || parts[0] != "JOIN") {
                sendLine(cs, "SYS|握手失败");
                closeSocket(cs);
                continue;
            }

            std::string name = parts[1];
            if (name.empty()) name = "匿名";
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (phase_ != Phase::Lobby) {
                    sendLine(cs, "SYS|游戏已开始，无法加入");
                    closeSocket(cs);
                    continue;
                }
                if (1 + (int)players_.size() >= kMaxPlayers) {
                    sendLine(cs, "SYS|房间已满");
                    closeSocket(cs);
                    continue;
                }
                std::string base = name;
                int suffix = 2;
                while (nameExistsUnlocked(name)) { name = base + std::to_string(suffix++); }
                Player p;
                p.sock = cs;
                p.name = name;
                players_[cs] = p;
            }
            sendLine(cs, "SYS|加入成功，昵称=" + name);
            sendLine(cs, "SYS|等待房主开始游戏...");
            broadcastLine("SYS|" + name + " 加入了房间");
            sendPlayers();
            safePrintln("\n[系统] " + name + " 加入了房间");
            safePrintln(playersString());
            safePrint("> ");
            {
                std::lock_guard<std::mutex> lk(mu_);
                threads_[cs] = std::thread([this, cs] { clientLoop(cs); });
            }
        }
    }

    bool nameExistsUnlocked(const std::string& name) const {
        for (auto& kv : players_) if (kv.second.name == name) return true;
        return false;
    }

    void clientLoop(socket_t cs) {
        while (running_.load()) {
            std::string line;
            if (!recvLine(cs, line)) break;
            auto parts = split(line, '|');
            if (parts.empty()) continue;

            if (parts[0] == "CHAT" && parts.size() >= 2) {
                std::string text = line.substr(5);
                std::string name = getName(cs);
                broadcastLine("CHAT|" + name + "|" + text);
            } else if (parts[0] == "ACT" && parts.size() >= 2) {
                std::string type = parts[1];
                std::string payload;
                if (parts.size() >= 3) {
                    size_t pos = line.find('|', 5);
                    if (pos != std::string::npos && pos + 1 < line.size()) {
                        payload = line.substr(pos + 1);
                    }
                }
                handleActionBySocket(cs, type, payload);
            } else if (parts[0] == "LEAVE") {
                break;
            }
        }
        std::string leftName;
        bool wasInGame = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = players_.find(cs);
            if (it != players_.end()) {
                leftName = it->second.name;
                wasInGame = (phase_ != Phase::Lobby);
            }
        }
        closeSocket(cs);
        {
            std::lock_guard<std::mutex> lk(mu_);
            players_.erase(cs);
            // 从threads_中移除自己，避免stop()尝试join已退出的线程
            auto it = threads_.find(cs);
            if (it != threads_.end()) {
                // 将线程对象转移出来，稍后join
                if (it->second.joinable()) {
                    it->second.detach(); // 线程已经在退出，detach即可
                }
                threads_.erase(it);
            }
        }
        if (!leftName.empty()) {
            broadcastLine("SYS|" + leftName + " 离开了房间");
            sendPlayers();
            safePrintln("\n[系统] " + leftName + " 离开了房间");
            safePrintln(playersString());
            safePrint("> ");
            if (wasInGame) {
                broadcastLine("SYS|游戏中玩家断开，游戏结束");
                safePrintln("[系统] 游戏中玩家断开，游戏结束");
                std::lock_guard<std::mutex> lk(mu_);
                phase_ = Phase::Lobby;
                // 重置游戏状态以便重新开始
                hostPlayer_.hand.clear();
                hostPlayer_.isLandlord = false;
                hostPlayer_.called = false;
                hostPlayer_.callScore = 0;
                bottomCards_.clear();
                currentTurn_ = 0;
                callScore_ = 0;
                landlord_ = -1;
                callCount_ = 0;
                lastPlay_ = {HT_INVALID, 0, 0, {}};
                lastPlayer_ = -1;
                passCount_ = 0;
            }
        }
    }

    std::string getName(socket_t cs) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = players_.find(cs);
        return (it != players_.end()) ? it->second.name : "?";
    }

    std::string playersString() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> names = allPlayerNames();
        std::string s = "当前玩家(" + std::to_string(names.size()) + "/" + std::to_string(kMaxPlayers) + "): ";
        for (size_t i = 0; i < names.size(); i++) {
            if (i) s += ", ";
            s += names[i];
        }
        return s;
    }

    void sendPlayers() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> names = allPlayerNames();
        std::string msg = "PLAYERS|" + join(names, ',');
        for (auto& kv : players_) sendLine(kv.first, msg);
    }

    // ==================== 游戏流程 ====================
    void startGame() {
        std::lock_guard<std::mutex> lk(mu_);
        if (playerCount() < kMaxPlayers) {
            safePrintln("[ERR] 人数不足，需要" + std::to_string(kMaxPlayers) + "人(当前" + std::to_string(playerCount()) + "人)");
            return;
        }
        if (phase_ != Phase::Lobby) {
            safePrintln("[ERR] 游戏已开始");
            return;
        }

        phase_ = Phase::CallLandlord;
        // 重置所有玩家状态
        hostPlayer_.isLandlord = false;
        hostPlayer_.called = false;
        hostPlayer_.callScore = 0;
        for (auto& kv : players_) {
            kv.second.isLandlord = false;
            kv.second.called = false;
            kv.second.callScore = 0;
        }
        // 发牌
        std::vector<Card> deck = GameLogic::createDeck();
        GameLogic::shuffle(deck);

        // 房主(玩家0)的手牌
        hostPlayer_.hand.clear();
        for (int i = 0; i < 17; i++) hostPlayer_.hand.push_back(deck[i]);
        sort(hostPlayer_.hand.begin(), hostPlayer_.hand.end());

        // 远程玩家(1,2)的手牌
        int idx = 17;
        for (auto& kv : players_) {
            kv.second.hand.clear();
            for (int i = 0; i < 17; i++) kv.second.hand.push_back(deck[idx++]);
            sort(kv.second.hand.begin(), kv.second.hand.end());
        }
        bottomCards_.clear();
        for (int i = 51; i < 54; i++) bottomCards_.push_back(deck[i]);

        // 构建玩家名字列表消息
        std::vector<std::string> nameList;
        nameList.push_back(hostName_);
        for (auto& kv : players_) nameList.push_back(kv.second.name);
        std::string namesMsg = "NAMES|" + join(nameList, ',');

        // 通知远程玩家（底牌背面，只显示数量）
        int pi = 1;
        for (auto& kv : players_) {
            sendLine(kv.first, "START|" + std::to_string(pi));
            sendLine(kv.first, namesMsg);
            sendLine(kv.first, "CARDS|" + cardsToStr(kv.second.hand));
            sendLine(kv.first, "BOTTOM|3"); // 只告诉有3张底牌，不显示内容
            pi++;
        }

        // 通知房主自己（底牌背面）
        pushHostMsg("START|0");
        pushHostMsg(namesMsg);
        pushHostMsg("CARDS|" + cardsToStr(hostPlayer_.hand));
        pushHostMsg("BOTTOM|3"); // 只告诉有3张底牌

        // 叫地主
        currentTurn_ = 0;
        callScore_ = 0;
        landlord_ = -1;
        callCount_ = 0;
        hostPlayer_.called = false;
        hostPlayer_.callScore = 0;
        for (auto& kv : players_) { kv.second.called = false; kv.second.callScore = 0; }
        broadcastAllUnlocked("CALL_TURN|" + std::to_string(currentTurn_));
        msgCv_.notify_all();
        safePrintln("[SYS] 游戏开始！叫地主阶段");
    }

    // 远程客户端调用的handleAction
    void handleActionBySocket(socket_t cs, const std::string& type, const std::string& payload) {
        std::lock_guard<std::mutex> lk(mu_);
        // 在锁内查找playerIdx并直接处理
        int playerIdx = -1;
        int idx = 0;
        for (auto& kv : players_) {
            if (kv.first == cs) { playerIdx = idx + 1; break; }
            idx++;
        }
        if (playerIdx < 0) return;
        // 直接在锁内执行handleAction的逻辑（避免重复加锁）
        doHandleAction(playerIdx, type, payload);
    }

    void handleAction(int playerIdx, const std::string& type, const std::string& payload) {
        std::lock_guard<std::mutex> lk(mu_);
        doHandleAction(playerIdx, type, payload);
    }

    void doHandleAction(int playerIdx, const std::string& type, const std::string& payload) {
        // 调用者必须已持有 mu_ 锁
        Player& player = getPlayer(playerIdx);

        if (phase_ == Phase::CallLandlord) {
            if (playerIdx != currentTurn_) return;
            if (type == "CALL") {
                if (player.called) return; // 已叫过，拒绝重复叫分
                int score = 0;
                try { score = std::stoi(payload); } catch (...) { return; }
                if (score < 0 || score > 3) return;
                player.called = true;
                player.callScore = score;
                if (score > callScore_) { callScore_ = score; landlord_ = playerIdx; }
                callCount_++;
                broadcastAllUnlocked("CALL_RESULT|" + std::to_string(playerIdx) + "|" + std::to_string(score));

                if (callScore_ == 3 || callCount_ >= kMaxPlayers) {
                    finishCallLandlord();
                } else {
                    currentTurn_ = (currentTurn_ + 1) % kMaxPlayers;
                    broadcastAllUnlocked("CALL_TURN|" + std::to_string(currentTurn_));
                    msgCv_.notify_all();
                }
            }
        } else if (phase_ == Phase::Play) {
            if (playerIdx != currentTurn_) return;
            if (type == "PASS") {
                bool canPass = (lastPlay_.type != HT_INVALID && lastPlayer_ != playerIdx);
                if (!canPass) {
                    if (playerIdx != 0) sendLine(player.sock, "SYS|自由出牌时不能跳过");
                    else pushHostMsg("SYS|自由出牌时不能跳过");
                    return;
                }
                passCount_++;
                broadcastAllUnlocked("PLAY|PASS|" + std::to_string(playerIdx));
                if (passCount_ >= 2) {
                    lastPlay_ = {HT_INVALID, 0, 0, {}};
                    lastPlayer_ = -1;
                    passCount_ = 0;
                }
                currentTurn_ = (currentTurn_ + 1) % kMaxPlayers;
                broadcastAllUnlocked("TURN|" + std::to_string(currentTurn_));
                msgCv_.notify_all();
            } else if (type == "CARDS") {
                std::vector<Card> played = strToCards(payload);
                if (played.empty()) {
                    if (playerIdx != 0) sendLine(player.sock, "SYS|出牌不能为空");
                    else pushHostMsg("SYS|出牌不能为空");
                    return;
                }
                Hand h = GameLogic::analyzeHand(played);
                if (h.type == HT_INVALID) {
                    if (playerIdx != 0) sendLine(player.sock, "SYS|无效牌型");
                    else pushHostMsg("SYS|无效牌型");
                    return;
                }
                bool canPass = (lastPlay_.type != HT_INVALID && lastPlayer_ != playerIdx);
                if (canPass && !h.canBeat(lastPlay_)) {
                    if (playerIdx != 0) sendLine(player.sock, "SYS|管不上");
                    else pushHostMsg("SYS|管不上");
                    return;
                }

                // 从手牌移除，并校验玩家确实拥有这些牌
                std::vector<Card> newHand;
                std::vector<bool> used(player.hand.size(), false);
                for (auto& pc : played) {
                    bool found = false;
                    for (size_t i = 0; i < player.hand.size(); i++) {
                        if (!used[i] && player.hand[i].id == pc.id) { used[i] = true; found = true; break; }
                    }
                    if (!found) {
                        if (playerIdx != 0) sendLine(player.sock, "SYS|你没有这些牌");
                        else pushHostMsg("SYS|你没有这些牌");
                        return;
                    }
                }
                for (size_t i = 0; i < player.hand.size(); i++) if (!used[i]) newHand.push_back(player.hand[i]);
                player.hand = newHand;

                lastPlay_ = h;
                lastPlayer_ = playerIdx;
                passCount_ = 0;
                broadcastAllUnlocked("PLAY|CARDS|" + std::to_string(playerIdx) + "|" + payload);

                if (player.hand.empty()) {
                    phase_ = Phase::GameOver;
                    broadcastAllUnlocked("WIN|" + std::to_string(playerIdx));
                    safePrintln("[SYS] 游戏结束！玩家" + std::to_string(playerIdx + 1) + " 获胜");
                    return;
                }

                currentTurn_ = (currentTurn_ + 1) % kMaxPlayers;
                broadcastAllUnlocked("TURN|" + std::to_string(currentTurn_));
                msgCv_.notify_all();
            }
        }
    }

    void finishCallLandlord() {
        if (landlord_ < 0) {
            broadcastAllUnlocked("SYS|没人叫地主，重新发牌");
            phase_ = Phase::Lobby;
            return;
        }

        // 校验地主玩家是否仍然存在
        bool landlordExists = false;
        if (landlord_ == 0) {
            landlordExists = true;
        } else {
            int pi = 1;
            for (auto& kv : players_) {
                if (pi == landlord_) { landlordExists = true; break; }
                pi++;
            }
        }
        if (!landlordExists) {
            broadcastAllUnlocked("SYS|地主玩家已断开，重新发牌");
            phase_ = Phase::Lobby;
            return;
        }

        Player& ll = getPlayer(landlord_);
        ll.isLandlord = true;
        for (auto& c : bottomCards_) ll.hand.push_back(c);
        sort(ll.hand.begin(), ll.hand.end());

        // 通知所有人地主是谁（不包含底牌内容）
        broadcastAllUnlocked("LANDLORD|" + std::to_string(landlord_));
        msgCv_.notify_all();

        // 只通知地主本人底牌和更新后的手牌
        if (landlord_ == 0) {
            // 房主是地主
            pushHostMsg("BOTTOM|" + cardsToStr(bottomCards_));
            pushHostMsg("MY_CARDS|" + cardsToStr(ll.hand));
            msgCv_.notify_all();
        } else {
            // 远程玩家是地主
            int pi = 1;
            for (auto& kv : players_) {
                if (pi == landlord_) {
                    sendLine(kv.first, "BOTTOM|" + cardsToStr(bottomCards_));
                    sendLine(kv.first, "MY_CARDS|" + cardsToStr(kv.second.hand));
                    break;
                }
                pi++;
            }
        }

        phase_ = Phase::Play;
        currentTurn_ = landlord_;
        lastPlay_ = {HT_INVALID, 0, 0, {}};
        lastPlayer_ = -1;
        passCount_ = 0;
        broadcastAllUnlocked("TURN|" + std::to_string(currentTurn_));
        msgCv_.notify_all();
        safePrintln("[SYS] 地主是玩家" + std::to_string(landlord_ + 1) + "，开始出牌");
    }

    std::atomic<bool> running_{false};
    socket_t listenSock_ = kInvalidSocket;
    std::thread acceptTh_;
    DiscoveryResponder responder_;
    mutable std::mutex mu_;
    std::map<socket_t, Player> players_; // 远程客户端
    std::map<socket_t, std::thread> threads_;

    // 房主作为玩家0
    Player hostPlayer_;
    std::string hostName_;

    // 消息队列（房主自己看游戏状态用）
    std::mutex msgMu_;
    std::condition_variable msgCv_;
    std::deque<std::string> hostMsgQueue_;

    std::string roomName_;
    std::string roomId_;
    uint16_t tcpPort_ = 0;

    Phase phase_ = Phase::Lobby;
    std::vector<Card> bottomCards_;
    int currentTurn_ = 0;
    int callScore_ = 0;
    int landlord_ = -1;
    int callCount_ = 0;
    Hand lastPlay_ = {HT_INVALID, 0, 0, {}};
    int lastPlayer_ = -1;
    int passCount_ = 0;

    // 获取第idx个玩家的信息（0=房主，1,2=远程）
    // 调用者必须已持有 mu_ 锁
    Player& getPlayer(int idx) {
        if (idx == 0) return hostPlayer_;
        int ri = 0;
        for (auto& kv : players_) {
            ri++;
            if (ri == idx) return kv.second;
        }
        // 不应该到达这里，返回房主作为安全fallback
        safePrintln("[WARN] getPlayer(" + std::to_string(idx) + ") 未找到，返回房主");
        return hostPlayer_;
    }

    // 获取第idx个玩家的名字（调用者必须已持有 mu_ 锁）
    std::string getPlayerName(int idx) const {
        if (idx == 0) return hostName_;
        int ri = 0;
        for (auto& kv : players_) {
            ri++;
            if (ri == idx) return kv.second.name;
        }
        return "???";
    }

    // 给所有远程客户端发消息（调用者必须已持有 mu_ 锁）
    void broadcastLineUnlocked(const std::string& line) {
        for (auto& kv : players_) sendLine(kv.first, line);
    }

    // 给所有远程客户端发消息（自动加锁）
    void broadcastLine(const std::string& line) {
        std::lock_guard<std::mutex> lk(mu_);
        broadcastLineUnlocked(line);
    }

    // 给房主消息队列推送
    void pushHostMsg(const std::string& line) {
        {
            std::lock_guard<std::mutex> lk(msgMu_);
            hostMsgQueue_.push_back(line);
        }
        msgCv_.notify_all();
    }

    // 同时给房主和远程发消息（调用者必须已持有 mu_ 锁）
    void broadcastAllUnlocked(const std::string& line) {
        broadcastLineUnlocked(line);
        pushHostMsg(line);
    }

    // 同时给房主和远程发消息（自动加锁）
    void broadcastAll(const std::string& line) {
        std::lock_guard<std::mutex> lk(mu_);
        broadcastLineUnlocked(line);
        pushHostMsg(line);
    }

    // 获取玩家总数
    int playerCount() const { return 1 + (int)players_.size(); }

    // 获取所有玩家名
    std::vector<std::string> allPlayerNames() const {
        std::vector<std::string> names;
        names.push_back(hostName_);
        for (auto& kv : players_) names.push_back(kv.second.name);
        return names;
    }
};

// ==================== 客户端 ====================
class GameClient {
public:
    ~GameClient() { disconnect(); }

    bool connectTo(const std::string& ip, uint16_t port, const std::string& name) {
        disconnect();
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == kInvalidSocket) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        unsigned long ipAddr = inet_addr(ip.c_str());
        if (ipAddr == INADDR_NONE) {
            closeSocket(sock_);
            sock_ = kInvalidSocket;
            return false;
        }
        addr.sin_addr.s_addr = ipAddr;
        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closeSocket(sock_);
            sock_ = kInvalidSocket;
            return false;
        }

        if (!sendLine(sock_, "JOIN|" + name)) {
            closeSocket(sock_);
            sock_ = kInvalidSocket;
            return false;
        }

        running_.store(true);
        recvTh_ = std::thread([this] { recvLoop(); });
        return true;
    }

    void disconnect() {
        running_.store(false);
        if (sock_ != kInvalidSocket) { closeSocket(sock_); sock_ = kInvalidSocket; }
        if (recvTh_.joinable()) recvTh_.join();
    }

    void run() {
        while (running_.load()) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !msgQueue_.empty() || !running_.load(); });
            while (!msgQueue_.empty()) {
                std::string msg = msgQueue_.front();
                msgQueue_.pop_front();
                lk.unlock();
                handleMsg(msg);
                lk.lock();
            }
        }
    }

    void sendChat(const std::string& text) {
        if (sock_ != kInvalidSocket) sendLine(sock_, "CHAT|" + text);
    }

    void sendAction(const std::string& type, const std::string& payload) {
        if (sock_ != kInvalidSocket) sendLine(sock_, "ACT|" + type + "|" + payload);
    }

private:
    void recvLoop() {
        while (running_.load()) {
            std::string line;
            if (!recvLine(sock_, line)) {
                running_.store(false);
                cv_.notify_all();
                break;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                msgQueue_.push_back(line);
            }
            cv_.notify_all();
        }
    }

    void handleMsg(const std::string& msg) {
        auto parts = split(msg, '|');
        if (parts.empty()) return;

        if (parts[0] == "SYS") {
            safePrintln("[系统] " + (parts.size() > 1 ? parts[1] : ""));
        } else if (parts[0] == "CHAT") {
            if (parts.size() >= 3) safePrintln("[" + parts[1] + "] " + parts[2]);
        } else if (parts[0] == "PLAYERS") {
            if (parts.size() > 1) safePrintln("[玩家列表] " + parts[1]);
        } else if (parts[0] == "START") {
            if (parts.size() > 1) {
                try { myIndex_ = std::stoi(parts[1]); } catch (...) { return; }
                phase_ = Phase::CallLandlord;
                safePrintln("[系统] 游戏开始！你是玩家" + std::to_string(myIndex_ + 1));
            }
        } else if (parts[0] == "NAMES") {
            if (parts.size() > 1) {
                playerNames_.clear();
                auto names = split(parts[1], ',');
                for (size_t i = 0; i < names.size(); i++) playerNames_[(int)i] = names[i];
            }
        } else if (parts[0] == "CARDS") {
            if (parts.size() > 1) {
                myHand_ = strToCards(parts[1]);
                sort(myHand_.begin(), myHand_.end());
                showHand();
            }
        } else if (parts[0] == "BOTTOM") {
            // 叫地主阶段只显示数量，确定地主后才显示内容
            if (parts.size() > 1 && parts[1] == "3") {
                safePrintln("[底牌] ??? ??? ??? (3张)");
            } else if (parts.size() > 1) {
                bottomCards_ = strToCards(parts[1]);
                safePrintln("[底牌] " + cardsDisplayStr(bottomCards_));
            }
        } else if (parts[0] == "CALL_TURN") {
            if (parts.size() > 1) {
                try { currentTurn_ = std::stoi(parts[1]); } catch (...) { return; }
            }
            showStatus();
            if (currentTurn_ == myIndex_) {
                while (true) {
                    safePrintln("\n[叫地主] 请选择: 1/2/3=叫地主 0=不叫");
                    safePrint("> ");
                    std::string input;
                    std::cin >> input;
                    int score = 0;
                    try { score = std::stoi(input); } catch (...) {
                        safePrintln("[ERR] 请输入数字 0~3");
                        continue;
                    }
                    if (score < 0 || score > 3) {
                        safePrintln("[ERR] 请输入数字 0~3");
                        continue;
                    }
                    sendAction("CALL", std::to_string(score));
                    break;
                }
            }
        } else if (parts[0] == "CALL_RESULT") {
            if (parts.size() >= 3) {
                try {
                    int player = std::stoi(parts[1]);
                    int score = std::stoi(parts[2]);
                    safePrintln(getPlayerName(player) + (score > 0 ? " 叫了" + std::to_string(score) + "分" : " 不叫"));
                } catch (...) {}
            }
        } else if (parts[0] == "LANDLORD") {
            if (parts.size() > 1) {
                try { landlord_ = std::stoi(parts[1]); } catch (...) { return; }
                safePrintln("[系统] 地主是" + getPlayerName(landlord_));
            }
        } else if (parts[0] == "MY_CARDS") {
            if (parts.size() > 1) {
                myHand_ = strToCards(parts[1]);
                sort(myHand_.begin(), myHand_.end());
                showHand();
                safePrintln("[系统] 你获得了底牌！");
            }
        } else if (parts[0] == "TURN") {
            if (parts.size() > 1) {
                try { currentTurn_ = std::stoi(parts[1]); } catch (...) { return; }
            }
            showStatus();
            if (currentTurn_ == myIndex_) {
                while (true) {
                    bool canPass = (lastPlay_.type != HT_INVALID && lastPlayer_ != myIndex_);
                    safePrintln("\n[出牌] 输入牌编号(逗号分隔)" + std::string(canPass ? ", 或 pass" : ""));
                    safePrint("> ");
                    std::string input;
                    std::cin >> input;
                    if (input == "pass") {
                        if (!canPass) {
                            safePrintln("[ERR] 自由出牌时不能跳过");
                            continue;
                        }
                        sendAction("PASS", "");
                        break;
                    }
                    std::vector<Card> playCards;
                    std::istringstream iss(input); std::string tok;
                    bool hasInvalid = false;
                    while (std::getline(iss, tok, ',')) {
                        try {
                            int idx = std::stoi(tok);
                            if (idx >= 0 && idx < (int)myHand_.size()) playCards.push_back(myHand_[idx]);
                            else { hasInvalid = true; }
                        } catch (...) { hasInvalid = true; }
                    }
                    if (hasInvalid || playCards.empty()) {
                        safePrintln("[ERR] 无效的牌编号");
                        continue;
                    }
                    // 发送出牌请求，等服务器确认后再移除手牌
                    pendingPlay_ = playCards;
                    sendAction("CARDS", cardsToStr(playCards));
                    break;
                }
            }
        } else if (parts[0] == "PLAY") {
            if (parts.size() >= 3 && parts[1] == "PASS") {
                try {
                    int player = std::stoi(parts[2]);
                    safePrintln(getPlayerName(player) + " 不出");
                } catch (...) {}
            } else if (parts.size() >= 4 && parts[1] == "CARDS") {
                try {
                    int player = std::stoi(parts[2]);
                    std::vector<Card> played = strToCards(parts[3]);
                    Hand h = GameLogic::analyzeHand(played);
                    lastPlay_ = h;
                    lastPlayer_ = player;
                    safePrintln(getPlayerName(player) + ": " + cardsDisplayStr(played) + " [" + handTypeName(h.type) + "]");
                    // 如果是自己出的牌且服务器确认，从手牌移除
                    if (player == myIndex_ && !pendingPlay_.empty()) {
                        std::vector<Card> newHand;
                        std::vector<bool> used(myHand_.size(), false);
                        for (auto& pc : pendingPlay_) {
                            for (size_t i = 0; i < myHand_.size(); i++) {
                                if (!used[i] && myHand_[i].id == pc.id) { used[i] = true; break; }
                            }
                        }
                        for (size_t i = 0; i < myHand_.size(); i++) if (!used[i]) newHand.push_back(myHand_[i]);
                        myHand_ = newHand;
                        pendingPlay_.clear();
                    }
                } catch (...) {}
            }
        } else if (parts[0] == "WIN") {
            if (parts.size() > 1) {
                try {
                    int winner = std::stoi(parts[1]);
                    phase_ = Phase::GameOver;
                    safePrintln("[系统] 游戏结束！" + getPlayerName(winner) + " 获胜");
                    safePrintln("\n按回车键退出...");
                    std::string dummy;
                    std::getline(std::cin, dummy);
                    std::getline(std::cin, dummy);
                    running_.store(false);
                } catch (...) {}
            }
        }
    }

    std::string getPlayerName(int idx) const {
        auto it = playerNames_.find(idx);
        if (it != playerNames_.end()) return it->second;
        return "玩家" + std::to_string(idx + 1);
    }

    void showHand() {
        safePrintln("\n[你的手牌] " + cardsDisplayStr(myHand_));
    }

    void showStatus() {
        safePrintln("\n========== 状态 ==========");
        if (landlord_ >= 0) {
            safePrintln("地主: " + getPlayerName(landlord_));
            safePrintln("底牌: " + cardsDisplayStr(bottomCards_));
        }
        if (lastPlay_.type != HT_INVALID) {
            safePrintln("上一手: " + cardsDisplayStr(lastPlay_.cards) + " [" + handTypeName(lastPlay_.type) + "] (" + getPlayerName(lastPlayer_) + ")");
        }
        safePrintln("当前轮到: " + getPlayerName(currentTurn_));
        showHand();
    }

    socket_t sock_ = kInvalidSocket;
    std::atomic<bool> running_{false};
    std::thread recvTh_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> msgQueue_;

    int myIndex_ = -1;
    Phase phase_ = Phase::Lobby;
    std::vector<Card> myHand_;
    std::vector<Card> bottomCards_;
    int currentTurn_ = 0;
    int landlord_ = -1;
    Hand lastPlay_ = {HT_INVALID, 0, 0, {}};
    int lastPlayer_ = -1;
    std::vector<Card> pendingPlay_; // 等待服务器确认的出牌
    std::map<int, std::string> playerNames_; // 玩家索引->名字
};

// ==================== 主函数 ====================
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(936);
    SetConsoleCP(936);
#endif
    SocketInit* sockInit = nullptr;
    try {
        sockInit = new SocketInit();
    } catch (...) {
        safePrintln("网络初始化失败");
        return 1;
    }

    safePrintln("====================================");
    safePrintln("    斗地主 - 局域网联机版 v2.0");
    safePrintln("====================================");
    safePrint("  请输入你的名字: ");
    std::string myName;
    std::getline(std::cin, myName);
    if (myName.empty()) myName = "玩家";

    safePrintln("  欢迎, " + myName + "!");
    safePrintln("  本机IP: " + getLocalIP());
    safePrintln("  端口: " + std::to_string(kGamePort));
    safePrintln("------------------------------------");
    safePrintln("  1. 创建房间 (作为主机)");
    safePrintln("  2. 自动搜索并加入房间");
    safePrintln("  3. 手动输入IP加入房间");
    safePrintln("  0. 退出");
    safePrintln("====================================");
    safePrint("\n请选择: ");

    int choice = -1;
    if (!(std::cin >> choice)) {
        safePrintln("输入无效");
        return 1;
    }
    std::cin.ignore();

    if (choice == 1) {
        safePrint("请输入房间名: ");
        std::string roomName;
        std::getline(std::cin, roomName);
        if (roomName.empty()) roomName = "斗地主房间";

        GameHost host;
        if (!host.start(roomName, myName)) {
            safePrintln("创建房间失败");
            return 1;
        }
        safePrintln("\n等待玩家加入... (输入 /start 开始游戏)");
        host.hostLoop();
    } else if (choice == 2) {
        safePrintln("正在搜索房间...");
        auto rooms = discoverRooms(2000);
        if (rooms.empty()) {
            safePrintln("未发现房间");
            return 0;
        }
        safePrintln("\n发现以下房间:");
        for (size_t i = 0; i < rooms.size(); i++) {
            safePrintln(std::to_string(i + 1) + ". " + rooms[i].roomName + " (" + rooms[i].hostIp + ":" + std::to_string(rooms[i].tcpPort) + ") [" + std::to_string(rooms[i].playerCount) + "/" + std::to_string(rooms[i].maxPlayers) + "]");
        }
        safePrint("\n请选择房间编号: ");
        int sel = -1;
        if (!(std::cin >> sel) || sel < 1 || sel > (int)rooms.size()) {
            safePrintln("无效选择");
            return 0;
        }
        std::cin.ignore();

        GameClient client;
        if (!client.connectTo(rooms[sel - 1].hostIp, rooms[sel - 1].tcpPort, myName)) {
            safePrintln("连接失败");
            return 1;
        }
        safePrintln("已连接，等待游戏开始...");
        client.run();
    } else if (choice == 3) {
        safePrint("请输入主机IP: ");
        std::string ip;
        std::cin >> ip;
        std::cin.ignore();

        GameClient client;
        if (!client.connectTo(ip, kGamePort, myName)) {
            safePrintln("连接失败");
            return 1;
        }
        safePrintln("已连接，等待游戏开始...");
        client.run();
    }

    delete sockInit;
    return 0;
}
