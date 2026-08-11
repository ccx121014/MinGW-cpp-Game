/*
  局域网联机狼人杀（最小可运行骨架）
  - 每台电脑运行同一个程序：既能“创建房间(充当该房间主机/服务器)”，也能“加入房间(作为客户端)”
  - 房间发现：UDP广播（局域网内自动发现房间）
  - 房间通信：TCP（主机负责转发聊天、维护玩家列表；提供一个 /start 的简单发牌入口）

  编译（Windows + MinGW64 / Dev-C++）：
    g++ main.cpp -std=c++17 -O2 -lws2_32 -o lan_werewolf.exe

  编译（Linux/macOS）：
    g++ main.cpp -std=c++17 -O2 -pthread -o lan_werewolf
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// 兼容老版本MinGW/Dev-C++：inet_pton/inet_ntop 需要提升 _WIN32_WINNT
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socklen_t = int;
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

static constexpr uint16_t kDiscoveryPort = 37020;
static constexpr int kMaxLine = 4096;

static std::mutex g_print_mu;
static void safePrintln(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_print_mu);
  std::cout << s << std::endl;
}

static void safePrint(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_print_mu);
  std::cout << s << std::flush;
}

static void msleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------- Socket 基础封装 ----------------

struct SocketInit {
  SocketInit() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      throw std::runtime_error("WSAStartup 失败");
    }
#endif
  }
  ~SocketInit() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
};

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
  // 兼容老MinGW：使用 inet_ntoa（线程不安全但够用）
  const char* p = inet_ntoa(addr.sin_addr);
  return p ? std::string(p) : std::string("0.0.0.0");
#else
  char buf[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, (void*)&addr.sin_addr, buf, sizeof(buf));
  return std::string(buf);
#endif
}

static bool parseIPv4(const std::string& ip, in_addr& out) {
#ifdef _WIN32
  // 兼容老MinGW：使用 inet_addr（不支持IPv6，这里只需要IPv4）
  unsigned long a = inet_addr(ip.c_str());
  if (a == INADDR_NONE) return false;
  out.s_addr = a;
  return true;
#else
  return inet_pton(AF_INET, ip.c_str(), &out) == 1;
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

static std::string randomHex(size_t n) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<int> dist(0, 15);
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; i++) {
    int x = dist(rng);
    s.push_back("0123456789abcdef"[x]);
  }
  return s;
}

// 读取一行（以 '\n' 结束），返回 false 表示断开
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

static bool sendLine(socket_t s, const std::string& line) { return sendAll(s, line + "\n"); }

// ---------------- UDP 发现房间 ----------------

struct RoomInfo {
  std::string id;
  std::string roomName;
  std::string hostIp;
  uint16_t tcpPort = 0;
  int playerCount = 0;
  int maxPlayers = 0;
};

// Host侧：接收 DISCOVER 并返回 ROOM 信息
class DiscoveryResponder {
 public:
  DiscoveryResponder() = default;
  ~DiscoveryResponder() { stop(); }

  void start(std::function<std::string()> roomResponseProvider) {
    stop();
    running_.store(true);
    provider_ = std::move(roomResponseProvider);
    th_ = std::thread([this] { loop(); });
  }

  void stop() {
    running_.store(false);
    if (sock_ != kInvalidSocket) {
      closeSocket(sock_);
      sock_ = kInvalidSocket;
    }
    if (th_.joinable()) th_.join();
  }

 private:
  void loop() {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == kInvalidSocket) {
      safePrintln("[ERR] UDP socket 创建失败");
      return;
    }
    setReuseAddr(sock_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(kDiscoveryPort);
    if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
      safePrintln("[ERR] UDP bind 失败（可能被占用）");
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
      if (n <= 0) {
        msleep(10);
        continue;
      }
      buf[n] = 0;
      std::string msg = trimCRLF(std::string(buf));
      if (msg == "WW_DISCOVER") {
        std::string resp = provider_ ? provider_() : "";
        if (!resp.empty()) {
          std::string out = resp + "\n";
          sendto(sock_, out.c_str(), (int)out.size(), 0, (sockaddr*)&from, fromlen);
        }
      }
    }
  }

 private:
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

  // 发送广播
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(kDiscoveryPort);
  to.sin_addr.s_addr = INADDR_BROADCAST;  // 255.255.255.255
  const char* msg = "WW_DISCOVER\n";
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
    // 格式：WW_ROOM|<id>|<roomName>|<tcpPort>|<playerCount>|<maxPlayers>
    auto parts = split(line, '|');
    if (parts.size() == 6 && parts[0] == "WW_ROOM") {
      RoomInfo ri;
      ri.id = parts[1];
      ri.roomName = parts[2];
      ri.hostIp = sockaddrToIp(from);
      ri.tcpPort = (uint16_t)std::stoi(parts[3]);
      ri.playerCount = std::stoi(parts[4]);
      ri.maxPlayers = std::stoi(parts[5]);

      bool dup = false;
      for (auto& r : rooms) {
        if (r.id == ri.id && r.hostIp == ri.hostIp && r.tcpPort == ri.tcpPort) {
          dup = true;
          break;
        }
      }
      if (!dup) rooms.push_back(std::move(ri));
    }
  }

  closeSocket(s);
  return rooms;
}

// ---------------- TCP 房间主机 ----------------

struct ClientState {
  socket_t sock = kInvalidSocket;
  std::string name;
  std::string ip;  // 客户端IP（用于限制同一IP只能有一个连接）
  std::thread th;
};

// ---------------- 游戏逻辑（标准版最小流程） ----------------

enum class Role {
  Villager,
  Werewolf,
  HiddenWolf,      // 隐狼：验人显示好人
  WolfKing,        // 狼王：死亡可带走一人（简化实现）
  WhiteWolfKing,   // 白狼王：死亡可带走一人（简化实现）
  WolfBeauty,      // 狼美人：夜晚魅惑一人，狼美人死亡时被魅惑者殉情
  Seer,
  Witch,
  Hunter,
  Guard,
  Knight,          // 骑士：白天可发动决斗，查验目标是否为狼人
  Raven,           // 乌鸦：夜晚诅咒一人，白天被诅咒者多一票
  Bear,            // 熊：天亮时咆哮（查验相邻玩家是否有狼人）
  Idiot,           // 白痴：被投票出局不死但失去投票权（简化）
  Elder,           // 长老：夜里第一次被刀不死（简化）
  Gravedigger,     // 守墓人：白天处决后得知被处决者身份（私聊）
  Thief,           // 盗贼：开局从两张身份中选一张变身（简化）
  Cupid,           // 丘比特：开局连两名恋人（简化）
  WildChild,       // 野孩子：开局选偶像，偶像死后变狼（简化）
  Piper,           // 吹笛人：夜晚蛊惑一人，被蛊惑者无法投票（简化）
  Bomber,          // 炸弹人：被投票处决时爆炸，带走所有投他的人（简化）
  Magician,        // 魔术师：夜晚可交换两人身份（简化）
  DreamCatcher,    // 摄梦人：夜晚选择摄梦目标，连续两晚摄梦同一人则目标死亡（简化）
};

static std::string roleToString(Role r) {
  switch (r) {
    case Role::Villager: return "村民";
    case Role::Werewolf: return "狼人";
    case Role::HiddenWolf: return "隐狼";
    case Role::WolfKing: return "狼王";
    case Role::WhiteWolfKing: return "白狼王";
    case Role::WolfBeauty: return "狼美人";
    case Role::Seer: return "预言家";
    case Role::Witch: return "女巫";
    case Role::Hunter: return "猎人";
    case Role::Guard: return "守卫";
    case Role::Knight: return "骑士";
    case Role::Raven: return "乌鸦";
    case Role::Bear: return "熊";
    case Role::Idiot: return "白痴";
    case Role::Elder: return "长老";
    case Role::Gravedigger: return "守墓人";
    case Role::Thief: return "盗贼";
    case Role::Cupid: return "丘比特";
    case Role::WildChild: return "野孩子";
    case Role::Piper: return "吹笛人";
    case Role::Bomber: return "炸弹人";
    case Role::Magician: return "魔术师";
    case Role::DreamCatcher: return "摄梦人";
  }
  return "村民";
}

static bool isWolfRole(Role r) {
  return r == Role::Werewolf || r == Role::HiddenWolf || r == Role::WolfKing || r == Role::WhiteWolfKing || r == Role::WolfBeauty;
}

// 查表：5-20人固定配比（你可以按自己习惯继续调）
static std::vector<Role> buildRoleListByPlayers(int n) {
  std::vector<Role> roles;
  auto add = [&](Role r, int c) {
    for (int i = 0; i < c; i++) roles.push_back(r);
  };
  switch (n) {
    case 5:
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Villager, 1);
      break;
    case 6:
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Villager, 1);
      break;
    case 7:
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Villager, 1);
      break;
    case 8:
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Villager, 1);
      break;
    case 9:
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Villager, 1);
      break;
    case 10:
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 1);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Villager, 1);
      break;
    case 11:
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 1);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Villager, 1);
      break;
    case 12:
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      break;
    case 13:
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      add(Role::Idiot, 1);
      break;
    case 14:
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      add(Role::Idiot, 1);
      add(Role::Piper, 1);
      add(Role::Thief, 1);
      break;
    case 15:
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 3);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      add(Role::Idiot, 1);
      add(Role::Piper, 1);
      break;
    case 16:
      add(Role::WhiteWolfKing, 1);
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 2);
      add(Role::WolfBeauty, 1);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      add(Role::Idiot, 1);
      add(Role::Piper, 1);
      add(Role::Villager, n - (int)roles.size());
      break;
    case 17:
      add(Role::WhiteWolfKing, 1);
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 2);
      add(Role::WolfBeauty, 1);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      add(Role::Idiot, 1);
      add(Role::Piper, 1);
      add(Role::Bomber, 1);
      add(Role::Villager, n - (int)roles.size());
      break;
    case 18:
      add(Role::WhiteWolfKing, 1);
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 2);
      add(Role::WolfBeauty, 1);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      add(Role::Idiot, 1);
      add(Role::Piper, 1);
      add(Role::Bomber, 1);
      add(Role::Magician, 1);
      add(Role::Villager, n - (int)roles.size());
      break;
    case 19:
      add(Role::WhiteWolfKing, 1);
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 3);
      add(Role::WolfBeauty, 1);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      add(Role::Idiot, 1);
      add(Role::Piper, 1);
      add(Role::Bomber, 1);
      add(Role::Magician, 1);
      add(Role::Villager, n - (int)roles.size());
      break;
    case 20:
      add(Role::WhiteWolfKing, 1);
      add(Role::WolfKing, 1);
      add(Role::HiddenWolf, 1);
      add(Role::Werewolf, 3);
      add(Role::WolfBeauty, 1);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Hunter, 1);
      add(Role::Guard, 1);
      add(Role::Knight, 1);
      add(Role::Raven, 1);
      add(Role::Bear, 1);
      add(Role::Elder, 1);
      add(Role::Idiot, 1);
      add(Role::Piper, 1);
      add(Role::Bomber, 1);
      add(Role::Magician, 1);
      add(Role::DreamCatcher, 1);
      add(Role::Villager, n - (int)roles.size());
      break;
    default:
      // fallback：至少5人，最大20人
      if (n < 5) n = 5;
      if (n > 20) n = 20;
      add(Role::Werewolf, 2);
      add(Role::Seer, 1);
      add(Role::Witch, 1);
      add(Role::Villager, n - (int)roles.size());
      break;
  }
  return roles;
}

enum class Phase {
  Lobby,
  Night,
  DayTalk,
  Vote,
  GameOver,
};

static std::string phaseToString(Phase p) {
  switch (p) {
    case Phase::Lobby: return "LOBBY";
    case Phase::Night: return "NIGHT";
    case Phase::DayTalk: return "DAY_TALK";
    case Phase::Vote: return "VOTE";
    case Phase::GameOver: return "GAME_OVER";
  }
  return "LOBBY";
}

struct GamePlayer {
  socket_t sock = kInvalidSocket;
  std::string name;
  Role role = Role::Villager;
  bool alive = true;
  bool witchSaveUsed = false;
  bool witchPoisonUsed = false;
  bool hunterShotUsed = false;
  bool elderShieldUsed = false;  // 长老：夜里第一次被刀免死
  bool idiotRevealed = false;    // 白痴：被投票后亮身份
  bool idiotNoVote = false;      // 白痴：失去投票权
  bool piperCharmed = false;     // 吹笛人：被蛊惑
  bool bomberExploded = false;   // 炸弹人：已爆炸
};

class RoomHost {
 public:
  ~RoomHost() { stop(); }

  bool start(const std::string& roomName, int maxPlayers, uint16_t preferredPort) {
    stop();
    roomName_ = roomName;
    maxPlayers_ = maxPlayers;
    roomId_ = randomHex(8);

    listenSock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock_ == kInvalidSocket) {
      safePrintln("[ERR] TCP socket 创建失败");
      return false;
    }
    setReuseAddr(listenSock_);

    // 绑定端口：若 preferredPort 被占用，自动往后尝试
    bool bound = false;
    for (int i = 0; i < 50; i++) {
      uint16_t p = (uint16_t)(preferredPort + i);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(p);
      if (bind(listenSock_, (sockaddr*)&addr, sizeof(addr)) == 0) {
        tcpPort_ = p;
        bound = true;
        break;
      }
    }
    if (!bound) {
      safePrintln("[ERR] TCP bind 失败（端口占用）");
      closeSocket(listenSock_);
      listenSock_ = kInvalidSocket;
      return false;
    }

    if (listen(listenSock_, 16) != 0) {
      safePrintln("[ERR] listen 失败");
      closeSocket(listenSock_);
      listenSock_ = kInvalidSocket;
      return false;
    }

    running_.store(true);
    acceptTh_ = std::thread([this] { acceptLoop(); });

    responder_.start([this] { return discoveryResponse(); });
    safePrintln("[SYS] 房间已创建：\"" + roomName_ + "\" 端口=" + std::to_string(tcpPort_));
    return true;
  }

  void stop() {
    running_.store(false);
    stopGame();
    responder_.stop();
    if (listenSock_ != kInvalidSocket) {
      closeSocket(listenSock_);
      listenSock_ = kInvalidSocket;
    }
    if (acceptTh_.joinable()) acceptTh_.join();

    std::vector<ClientState*> toJoin;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& kv : clients_) toJoin.push_back(&kv.second);
    }
    for (auto* cs : toJoin) {
      if (cs->sock != kInvalidSocket) closeSocket(cs->sock);
      if (cs->th.joinable()) cs->th.join();
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      clients_.clear();
      started_.store(false);
    }
    {
      std::lock_guard<std::mutex> lk(gameMu_);
      gamePlayers_.clear();
    }
  }

  uint16_t tcpPort() const { return tcpPort_; }
  std::string roomId() const { return roomId_; }
  std::string roomName() const { return roomName_; }
  int maxPlayers() const { return maxPlayers_; }

  void broadcastSys(const std::string& text) { broadcastLine("SYS|" + text); }

  void handleHostCommand(const std::string& cmdLine) {
    if (cmdLine == "/players") {
      safePrintln(playersString());
      return;
    }
    if (cmdLine == "/start") {
      startGame();
      return;
    }
    if (cmdLine == "/vote") {
      requestVoteNow();
      return;
    }
    if (cmdLine == "/help") {
      safePrintln("房主可用指令：/players  /start(开始游戏)  /vote(强制进入投票)  /help  /leave");
      return;
    }
    safePrintln("[SYS] 未知指令，输入 /help 查看。");
  }

  void hostChat(const std::string& hostName, const std::string& text) {
    broadcastLine("CHAT|" + hostName + "|" + text);
  }

 private:
  std::string discoveryResponse() const {
    // WW_ROOM|<id>|<roomName>|<tcpPort>|<playerCount>|<maxPlayers>
    int pc = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      pc = (int)clients_.size();
    }
    std::string name = roomName_;
    if (gameRunning_.load()) name += " [对局中]";
    std::ostringstream oss;
    oss << "WW_ROOM|" << roomId_ << "|" << name << "|" << tcpPort_ << "|" << pc << "|" << maxPlayers_;
    return oss.str();
  }

  void acceptLoop() {
    while (running_.load()) {
      sockaddr_in cli{};
      socklen_t len = sizeof(cli);
      socket_t cs = accept(listenSock_, (sockaddr*)&cli, &len);
      if (cs == kInvalidSocket) {
        msleep(10);
        continue;
      }
      setReuseAddr(cs);

      // 先收一行 JOIN|name
      std::string line;
      if (!recvLine(cs, line)) {
        closeSocket(cs);
        continue;
      }
      auto parts = split(line, '|');
      if (parts.size() != 2 || parts[0] != "JOIN") {
        sendLine(cs, "SYS|握手失败");
        closeSocket(cs);
        continue;
      }

      // 对局中禁止加入
      if (gameRunning_.load()) {
        sendLine(cs, "SYS|房间正在对局中，无法加入。请等待对局结束或创建新房间。");
        closeSocket(cs);
        continue;
      }
      std::string name = parts[1];
      if (name.empty()) name = "匿名";

      // 获取客户端IP（inet_ntoa 非线程安全，先复制到本地缓冲区）
      char ipBuf[INET_ADDRSTRLEN];
      strncpy(ipBuf, inet_ntoa(cli.sin_addr), INET_ADDRSTRLEN - 1);
      ipBuf[INET_ADDRSTRLEN - 1] = '\0';
      std::string clientIp(ipBuf);

      {
        std::lock_guard<std::mutex> lk(mu_);
        if ((int)clients_.size() >= maxPlayers_) {
          sendLine(cs, "SYS|房间已满");
          closeSocket(cs);
          continue;
        }
        // 同一IP只能有一个连接
        bool ipExists = false;
        for (auto& kv : clients_) {
          if (kv.second.ip == clientIp) {
            ipExists = true;
            break;
          }
        }
        if (ipExists) {
          sendLine(cs, "SYS|该IP已有一个玩家在本房间，同一台电脑只能有一个玩家。");
          closeSocket(cs);
          continue;
        }
        // 去重名：简单处理
        std::string base = name;
        int suffix = 2;
        while (nameExistsUnlocked(name)) {
          name = base + std::to_string(suffix++);
        }

        ClientState st;
        st.sock = cs;
        st.name = name;
        st.ip = clientIp;
        clients_[cs] = std::move(st);
      }
      {
        std::lock_guard<std::mutex> lk(gameMu_);
        GamePlayer gp;
        gp.sock = cs;
        gp.name = name;
        gp.role = Role::Villager;
        gp.alive = true;
        gamePlayers_[cs] = std::move(gp);
      }

      sendLine(cs, "SYS|加入成功，你的昵称=" + name);
      sendLine(cs, "SYS|输入 /help 查看指令；输入 /leave 退出房间。");
      sendPlayers();
      broadcastSys(name + " 加入了房间");

      {
        std::lock_guard<std::mutex> lk(mu_);
        clients_[cs].th = std::thread([this, cs] { clientLoop(cs); });
      }
    }
  }

  bool nameExistsUnlocked(const std::string& name) const {
    for (auto& kv : clients_) {
      if (kv.second.name == name) return true;
    }
    return false;
  }

  void clientLoop(socket_t cs) {
    while (running_.load()) {
      std::string line;
      if (!recvLine(cs, line)) break;
      auto parts = split(line, '|');
      if (parts.empty()) continue;

      if (parts[0] == "CHAT" && parts.size() >= 2) {
        std::string text = line.substr(5);  // "CHAT|" 后面所有
        std::string name = getName(cs);
        // 夜晚阶段：只允许狼人聊天（WOLF_CHAT），非狼人聊天被拦截
        {
          std::lock_guard<std::mutex> lk2(gameMu_);
          if (phase_ == Phase::Night && gameRunning_.load()) {
            auto itp = gamePlayers_.find(cs);
            if (itp == gamePlayers_.end() || !itp->second.alive || !isWolfRole(itp->second.role)) {
              sendLine(cs, "SYS|夜晚阶段不能聊天，请等待你的行动回合。");
              continue;
            }
            // 狼人夜晚聊天：在锁外调用 broadcastWolfChat（避免死锁）
          } else {
            broadcastLine("CHAT|" + name + "|" + text);
            continue;
          }
        }
        broadcastWolfChat(name, text);
        continue;
      } else if (parts[0] == "WOLF_CHAT" && parts.size() >= 2) {
        // 狼人专用聊天（客户端主动发送）
        std::string text = line.substr(9);  // "WOLF_CHAT|" 后面所有
        std::string name = getName(cs);
        {
          std::lock_guard<std::mutex> lk2(gameMu_);
          if (!gameRunning_.load()) {
            sendLine(cs, "SYS|游戏尚未开始。");
            continue;
          }
          auto itp = gamePlayers_.find(cs);
          if (itp == gamePlayers_.end() || !itp->second.alive || !isWolfRole(itp->second.role)) {
            sendLine(cs, "SYS|只有狼人才能使用狼人聊天。");
            continue;
          }
          if (phase_ != Phase::Night) {
            sendLine(cs, "SYS|狼人聊天仅在夜晚可用。");
            continue;
          }
        }
        broadcastWolfChat(name, text);
      } else if (parts[0] == "ACT" && parts.size() >= 2) {
        // ACT|<TYPE>|<PAYLOAD...>
        std::string type = parts[1];
        std::string payload;
        if (parts.size() >= 3) {
          // 把后面的 | 保留（payload 可能包含 |）
          size_t pos2 = line.find('|');          // after ACT
          pos2 = (pos2 == std::string::npos) ? std::string::npos : line.find('|', pos2 + 1);  // after TYPE
          if (pos2 != std::string::npos) payload = line.substr(pos2 + 1);
        }
        handleAction(cs, type, payload);
      } else if (parts[0] == "CMD" && parts.size() >= 2) {
        std::string cmd = line.substr(4);  // "CMD|" 后面所有
        handleClientCommand(cs, cmd);
      } else if (parts[0] == "LEAVE") {
        break;
      } else {
        sendLine(cs, "SYS|未知消息格式");
      }
    }

    std::string leftName;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = clients_.find(cs);
      if (it != clients_.end()) {
        leftName = it->second.name;
      }
    }

    closeSocket(cs);
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = clients_.find(cs);
      if (it != clients_.end()) {
        // 线程对象在当前线程，不能 join 自己；把它 detach
        if (it->second.th.joinable()) it->second.th.detach();
        clients_.erase(it);
      }
    }
    onDisconnect(cs, leftName);
    sendPlayers();
    if (!leftName.empty()) broadcastSys(leftName + " 离开了房间");
  }

  std::string getName(socket_t cs) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = clients_.find(cs);
    if (it == clients_.end()) return "未知";
    return it->second.name;
  }

  void broadcastLine(const std::string& line) {
    std::vector<socket_t> socks;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& kv : clients_) socks.push_back(kv.first);
    }
    for (auto s : socks) {
      sendLine(s, line);
    }
  }

  // 仅向所有存活狼人广播消息
  void broadcastWolfChat(const std::string& senderName, const std::string& text) {
    std::vector<socket_t> wolfSocks;
    {
      std::lock_guard<std::mutex> lk(gameMu_);
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive && isWolfRole(kv.second.role)) {
          wolfSocks.push_back(kv.first);
        }
      }
    }
    std::string msg = "WOLF_CHAT|" + senderName + "|" + text;
    for (auto s : wolfSocks) {
      sendLine(s, msg);
    }
  }

  void sendPlayers() {
    std::vector<std::string> names;
    {
      std::lock_guard<std::mutex> lk(mu_);
      names.reserve(clients_.size());
      for (auto& kv : clients_) names.push_back(kv.second.name);
    }
    broadcastLine("PLAYERS|" + join(names, '|'));
  }

  std::string playersString() const {
    std::vector<std::string> names;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& kv : clients_) names.push_back(kv.second.name);
    }
    std::ostringstream oss;
    oss << "[SYS] 当前玩家(" << names.size() << "/" << maxPlayers_ << "): " << join(names, ',');
    return oss.str();
  }

  void handleClientCommand(socket_t cs, const std::string& cmd) {
    if (cmd == "/players") {
      sendLine(cs, "SYS|" + playersString());
      return;
    }
    if (cmd == "/help") {
      sendLine(cs, "SYS|可用指令：/players /help /leave /menu");
      sendLine(cs, "SYS|游戏动作：/kill /check /guard /save /poison /vote /shoot /pass /love /idol /choose /charm /curse /swap /dream /duel（也可在提示后直接输入编号）");
      sendLine(cs, "SYS|提示：房主在房主控制台输入 /start 开始游戏；/vote 可强制进入投票。");
      return;
    }
    if (cmd == "/leave") {
      sendLine(cs, "SYS|正在退出…");
      sendLine(cs, "SYS|BYE");
      // 让 clientLoop 读到断开即可
      closeSocket(cs);
      return;
    }
    sendLine(cs, "SYS|未知指令，输入 /help 查看。");
  }

  void startGame() {
    if (gameRunning_.load()) {
      safePrintln("[SYS] 游戏已在进行中");
      return;
    }

    std::vector<socket_t> socks;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& kv : clients_) socks.push_back(kv.first);
    }
    if ((int)socks.size() < 5) {
      std::string msg = "人数不足：标准版最少 5 人才能开始。当前人数=" + std::to_string(socks.size());
      safePrintln("[SYS] " + msg);
      broadcastSys(msg);
      return;
    }

    // 生成角色（5-20人查表配比）
    int n = (int)socks.size();
    if (n > 20) {
      std::string msg = "人数过多：当前版本最多支持 20 人。当前人数=" + std::to_string(n);
      safePrintln("[SYS] " + msg);
      broadcastSys(msg);
      return;
    }
    std::vector<Role> roles = buildRoleListByPlayers(n);
    if ((int)roles.size() != n) {
      safePrintln("[ERR] 角色表配置错误：roles.size()!=n");
      return;
    }

    // 使用更好的随机种子：混合时间 + 随机设备
    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(),
                       (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count()};
    std::mt19937 rng(seed);
    std::shuffle(roles.begin(), roles.end(), rng);

    {
      std::lock_guard<std::mutex> lk(gameMu_);
      gamePlayers_.clear();  // 清空上一局残留的玩家数据
      phase_ = Phase::Lobby;
      day_ = 0;
      night_ = 0;
      voteNow_ = false;
      knightDuelSuccess_ = false;
      wolfKillTarget_.clear();
      guardProtectTarget_.clear();
      lastGuardProtectTarget_.clear();
      witchPoisonTarget_.clear();
      witchSaveDecision_ = -1;
      witchPoisonPass_ = false;
      votes_.clear();
      wolfVotes_.clear();
      seerChecked_ = false;
      hunterShotTarget_.clear();
      pendingWolfKingSock_ = kInvalidSocket;
      wolfKingShotTarget_.clear();
      wolfKingPass_ = false;
      loverOf_.clear();
      cupidDone_ = false;
      wildChildSock_ = kInvalidSocket;
      wildChildIdolSock_ = kInvalidSocket;
      wildChildConverted_ = false;
      thiefSock_ = kInvalidSocket;
      thiefChosen_ = false;
      thiefPass_ = false;
      wolfBeautySock_ = kInvalidSocket;
      wolfBeautyCharmTarget_.clear();
      ravenSock_ = kInvalidSocket;
      ravenCurseTarget_.clear();
      piperSock_ = kInvalidSocket;
      magicianSock_ = kInvalidSocket;
      magicianSwapA_.clear();
      magicianSwapB_.clear();
      dreamCatcherSock_ = kInvalidSocket;
      dreamCatcherTarget_.clear();
      dreamCatcherLastTarget_.clear();
      setupDone_ = false;

      // 重新构建 gamePlayers_（只包含当前在线玩家）
      for (size_t i = 0; i < socks.size(); i++) {
        auto s = socks[i];
        GamePlayer gp;
        gp.sock = s;
        {
          std::lock_guard<std::mutex> lk(mu_);
          auto itc = clients_.find(s);
          if (itc != clients_.end()) gp.name = itc->second.name;
        }
        gp.role = roles[i];
        gp.alive = true;
        gamePlayers_[s] = std::move(gp);
      }
    }

    started_.store(true);
    gameRunning_.store(true);

    broadcastSys("游戏开始！将进入夜晚。输入 /help 查看指令；需要操作时也可直接输入编号。");
    broadcastAlive();

    // 私发身份
    for (auto s : socks) {
      GamePlayer gp;
      {
        std::lock_guard<std::mutex> lk(gameMu_);
        auto it = gamePlayers_.find(s);
        if (it == gamePlayers_.end()) continue;
        gp = it->second;
      }
      sendLine(s, "ROLE|" + roleToString(gp.role));
      sendLine(s, "PRIV|你的身份是：" + roleToString(gp.role));
    }

    // 狼人互相看到队友身份
    {
      std::lock_guard<std::mutex> lk(gameMu_);
      std::vector<socket_t> wolfSocks;
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive && isWolfRole(kv.second.role)) wolfSocks.push_back(kv.first);
      }
      for (auto ws : wolfSocks) {
        std::string teammates;
        for (auto ws2 : wolfSocks) {
          if (ws2 == ws) continue;
          if (!teammates.empty()) teammates += "、";
          teammates += gamePlayers_[ws2].name + "(" + roleToString(gamePlayers_[ws2].role) + ")";
        }
        if (!teammates.empty()) {
          sendLine(ws, "PRIV|你的狼人队友有：" + teammates);
        } else {
          sendLine(ws, "PRIV|你是唯一的狼人。");
        }
      }
    }

    if (gameTh_.joinable()) gameTh_.join();
    gameTh_ = std::thread([this] { gameLoop(); });
  }

  void stopGame() {
    gameRunning_.store(false);
    gameCv_.notify_all();
    if (gameTh_.joinable()) gameTh_.join();
    {
      std::lock_guard<std::mutex> lk(gameMu_);
      phase_ = Phase::Lobby;
      voteNow_ = false;
      // 清理游戏状态，防止残留到下一局
      gamePlayers_.clear();
      loverOf_.clear();
      wolfVotes_.clear();
      votes_.clear();
      wolfKillTarget_.clear();
      guardProtectTarget_.clear();
      lastGuardProtectTarget_.clear();
      witchPoisonTarget_.clear();
      witchPoisonPass_ = false;
      witchSaveDecision_ = -1;
      seerChecked_ = false;
      hunterShotTarget_.clear();
      pendingHunterSock_ = kInvalidSocket;
      hunterPass_ = false;
      wolfBeautyCharmTarget_.clear();
      wolfBeautySock_ = kInvalidSocket;
      ravenCurseTarget_.clear();
      ravenSock_ = kInvalidSocket;
      piperSock_ = kInvalidSocket;
      magicianSock_ = kInvalidSocket;
      magicianSwapA_.clear();
      magicianSwapB_.clear();
      dreamCatcherSock_ = kInvalidSocket;
      dreamCatcherTarget_.clear();
      dreamCatcherLastTarget_.clear();
      thiefSock_ = kInvalidSocket;
      thiefChosen_ = false;
      thiefPass_ = false;
      wildChildIdolSock_ = kInvalidSocket;
      wildChildSock_ = kInvalidSocket;
      wildChildConverted_ = false;
      pendingWolfKingSock_ = kInvalidSocket;
      wolfKingShotTarget_.clear();
      wolfKingPass_ = false;
      knightDuelSuccess_ = false;
      setupDone_ = false;
    }
    started_.store(false);
  }

  void requestVoteNow() {
    std::lock_guard<std::mutex> lk(gameMu_);
    voteNow_ = true;
    gameCv_.notify_all();
  }

  void onDisconnect(socket_t cs, const std::string& leftName) {
    // 简化处理：游戏中断线视为死亡
    bool needCheck = false;
    {
      std::lock_guard<std::mutex> lk(gameMu_);
      auto it = gamePlayers_.find(cs);
      if (it != gamePlayers_.end()) {
        if (gameRunning_.load() && it->second.alive) {
          it->second.alive = false;
          needCheck = true;
        }
      }
    }
    if (needCheck) {
      broadcastLine("SYS|" + leftName + " 断开连接，按死亡处理。");
      broadcastAlive();
      gameCv_.notify_all();
    }
  }

  void handleAction(socket_t cs, const std::string& type, const std::string& payload) {
    if (!gameRunning_.load()) {
      sendLine(cs, "SYS|游戏尚未开始（等待房主 /start）");
      return;
    }

    std::unique_lock<std::mutex> lk(gameMu_);
    auto it = gamePlayers_.find(cs);
    if (it == gamePlayers_.end()) return;
    GamePlayer& me = it->second;
    // 猎人/狼王死亡后仍可开枪（特殊技能）
    bool isPostDeathAction = (type == "HUNTER_SHOOT" || type == "WOLFKING_SHOOT");
    if (!me.alive && !isPostDeathAction) {
      sendLine(cs, "PRIV|你已死亡，无法操作。");
      return;
    }

    auto normPayload = trimCRLF(payload);
    auto targetOkLocked = [&](const std::string& name) -> bool {
      if (name.empty()) return false;
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive && kv.second.name == name) return true;
      }
      return false;
    };

    if (type == "PASS") {
      // 放弃：按当前“待处理技能”判断
      if (pendingHunterSock_ == cs) {
        hunterPass_ = true;
      } else if (pendingWolfKingSock_ == cs) {
        wolfKingPass_ = true;
      } else if (thiefSock_ == cs) {
        thiefPass_ = true;
      } else {
        witchPoisonPass_ = true;
      }
      gameCv_.notify_all();
      return;
    }

    if (type == "WOLF_KILL") {
      if (!isWolfRole(me.role) || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能刀人。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      // 不能刀自己
      if (normPayload == me.name) {
        sendLine(cs, "PRIV|不能刀自己。");
        return;
      }
      wolfVotes_[cs] = normPayload;  // 允许改票
      gameCv_.notify_all();
      sendLine(cs, "PRIV|已选择击杀：" + normPayload);
      return;
    }

    if (type == "GUARD_PROTECT") {
      if (me.role != Role::Guard || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能守护。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      // 不能连续两晚守同一人
      if (normPayload == lastGuardProtectTarget_) {
        sendLine(cs, "PRIV|不能连续两晚守护同一个人。");
        return;
      }
      guardProtectTarget_ = normPayload;
      gameCv_.notify_all();
      sendLine(cs, "PRIV|已选择守护：" + normPayload);
      return;
    }

    if (type == "SEER_CHECK") {
      if (me.role != Role::Seer || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能验人。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      if (seerChecked_) {
        sendLine(cs, "PRIV|今晚已验过人。");
        return;
      }
      seerChecked_ = true;
      // 立即返回结果
      Role tr = Role::Villager;
      for (auto& kv : gamePlayers_) {
        if (kv.second.name == normPayload) tr = kv.second.role;
      }
      // 魔术师交换：如果验的是魔术师交换的目标之一，显示另一人的身份
      std::string displayName = normPayload;
      if (!magicianSwapA_.empty() && !magicianSwapB_.empty()) {
        if (normPayload == magicianSwapA_) displayName = magicianSwapB_;
        else if (normPayload == magicianSwapB_) displayName = magicianSwapA_;
      }
      Role displayRole = Role::Villager;
      for (auto& kv : gamePlayers_) {
        if (kv.second.name == displayName) displayRole = kv.second.role;
      }
      bool seenWolf = (displayRole == Role::Werewolf || displayRole == Role::WolfKing || displayRole == Role::WhiteWolfKing || displayRole == Role::WolfBeauty);
      // 隐狼验人显示好人
      sendLine(cs, std::string("PRIV|你验的 ") + normPayload + " 是：" + (seenWolf ? "狼人" : "好人"));
      gameCv_.notify_all();
      return;
    }

    if (type == "WITCH_SAVE") {
      if (me.role != Role::Witch || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能用解药。");
        return;
      }
      if (me.witchSaveUsed) {
        sendLine(cs, "PRIV|解药已用过。");
        return;
      }
      std::string v = normPayload;
      std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
      if (v == "yes" || v == "y" || v == "1") {
        witchSaveDecision_ = 1;
      } else if (v == "no" || v == "n" || v == "0") {
        witchSaveDecision_ = 0;
      } else {
        sendLine(cs, "PRIV|请输入 yes/no");
        return;
      }
      gameCv_.notify_all();
      return;
    }

    if (type == "WITCH_POISON") {
      if (me.role != Role::Witch || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能用毒药。");
        return;
      }
      if (me.witchPoisonUsed) {
        sendLine(cs, "PRIV|毒药已用过。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      witchPoisonTarget_ = normPayload;
      gameCv_.notify_all();
      sendLine(cs, "PRIV|已选择毒杀：" + normPayload);
      return;
    }

    if (type == "VOTE") {
      if (phase_ != Phase::Vote) {
        sendLine(cs, "PRIV|当前不是投票阶段。");
        return;
      }
      if (me.idiotNoVote) {
        sendLine(cs, "PRIV|你已失去投票权，无法投票。");
        return;
      }
      if (me.piperCharmed) {
        sendLine(cs, "PRIV|你已被吹笛人蛊惑，无法投票。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      // 不能投自己
      if (normPayload == me.name) {
        sendLine(cs, "PRIV|不能投自己。");
        return;
      }
      votes_[cs] = normPayload;  // 允许改票
      gameCv_.notify_all();
      sendLine(cs, "PRIV|你投给了：" + normPayload);
      return;
    }

    if (type == "HUNTER_SHOOT") {
      if (pendingHunterSock_ != cs) {
        sendLine(cs, "PRIV|当前你不能开枪。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      hunterShotTarget_ = normPayload;
      gameCv_.notify_all();
      return;
    }

    if (type == "WOLFKING_SHOOT") {
      if (pendingWolfKingSock_ != cs) {
        sendLine(cs, "PRIV|当前你不能开枪。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      wolfKingShotTarget_ = normPayload;
      gameCv_.notify_all();
      return;
    }

    if (type == "CUPID_LINK") {
      if (me.role != Role::Cupid) {
        sendLine(cs, "PRIV|你不是丘比特。");
        return;
      }
      if (setupDone_ || cupidDone_) {
        sendLine(cs, "PRIV|丘比特已完成连人。");
        return;
      }
      auto parts2 = split(normPayload, '|');  // name1|name2
      if (parts2.size() != 2) {
        sendLine(cs, "PRIV|格式错误：请用 /love A B 或输入两个编号。");
        return;
      }
      std::string a = parts2[0], b = parts2[1];
      if (a == b || !targetOkLocked(a) || !targetOkLocked(b)) {
        sendLine(cs, "PRIV|恋人目标无效（必须是两个不同的存活玩家昵称）。");
        return;
      }
      socket_t sa = kInvalidSocket, sb = kInvalidSocket;
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive && kv.second.name == a) sa = kv.first;
        if (kv.second.alive && kv.second.name == b) sb = kv.first;
      }
      if (sa == kInvalidSocket || sb == kInvalidSocket) {
        sendLine(cs, "PRIV|恋人目标无效。");
        return;
      }
      loverOf_[sa] = sb;
      loverOf_[sb] = sa;
      cupidDone_ = true;
      sendLine(sa, "PRIV|你与 " + b + " 成为恋人（恋人死亡你也会殉情）。");
      sendLine(sb, "PRIV|你与 " + a + " 成为恋人（恋人死亡你也会殉情）。");
      sendLine(cs, "PRIV|连人完成。");
      gameCv_.notify_all();
      return;
    }

    if (type == "WILDCHILD_IDOL") {
      if (me.role != Role::WildChild) {
        sendLine(cs, "PRIV|你不是野孩子。");
        return;
      }
      if (wildChildIdolSock_ != kInvalidSocket) {
        sendLine(cs, "PRIV|你已选择过偶像。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      socket_t idol = kInvalidSocket;
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive && kv.second.name == normPayload) idol = kv.first;
      }
      if (idol == kInvalidSocket || idol == cs) {
        sendLine(cs, "PRIV|目标无效（不能选择自己）。");
        return;
      }
      wildChildSock_ = cs;
      wildChildIdolSock_ = idol;
      sendLine(cs, "PRIV|你选择的偶像是：" + normPayload);
      gameCv_.notify_all();
      return;
    }

    if (type == "THIEF_CHOOSE") {
      if (me.role != Role::Thief) {
        sendLine(cs, "PRIV|你不是盗贼。");
        return;
      }
      if (thiefChosen_) {
        sendLine(cs, "PRIV|你已选择过身份。");
        return;
      }
      std::string v = normPayload;
      if (v != "1" && v != "2" && v != "3") {
        sendLine(cs, "PRIV|请输入 1、2 或 3");
        return;
      }
      Role chosen;
      if (v == "1") chosen = thiefOpt1_;
      else if (v == "2") chosen = thiefOpt2_;
      else chosen = thiefOpt3_;
      me.role = chosen;
      thiefChosen_ = true;
      sendLine(cs, "PRIV|你已变身为：" + roleToString(chosen));
      sendLine(cs, "ROLE|" + roleToString(chosen));
      gameCv_.notify_all();
      return;
    }

    // 狼美人：夜晚魅惑
    if (type == "WOLF_BEAUTY_CHARM") {
      if (me.role != Role::WolfBeauty || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能魅惑。");
        return;
      }
      if (!wolfBeautyCharmTarget_.empty()) {
        sendLine(cs, "PRIV|你已魅惑过目标，不能重复魅惑。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      wolfBeautySock_ = cs;
      wolfBeautyCharmTarget_ = normPayload;
      gameCv_.notify_all();
      sendLine(cs, "PRIV|已魅惑：" + normPayload);
      return;
    }

    // 乌鸦：夜晚诅咒
    if (type == "RAVEN_CURSE") {
      if (me.role != Role::Raven || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能诅咒。");
        return;
      }
      if (!ravenCurseTarget_.empty()) {
        sendLine(cs, "PRIV|你已诅咒过目标，不能重复诅咒。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      ravenSock_ = cs;
      ravenCurseTarget_ = normPayload;
      gameCv_.notify_all();
      sendLine(cs, "PRIV|已诅咒：" + normPayload + "（白天被诅咒者多一票）");
      return;
    }

    // 吹笛人：夜晚蛊惑
    if (type == "PIPER_CHARM") {
      if (me.role != Role::Piper || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能蛊惑。");
        return;
      }
      if (piperSock_ != kInvalidSocket) {
        sendLine(cs, "PRIV|你已蛊惑过目标，不能重复蛊惑。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      piperSock_ = cs;
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive && kv.second.name == normPayload) {
          kv.second.piperCharmed = true;
          break;
        }
      }
      gameCv_.notify_all();
      sendLine(cs, "PRIV|已蛊惑：" + normPayload);
      return;
    }

    // 魔术师：夜晚交换两人身份（简化：仅交换验人结果）
    if (type == "MAGICIAN_SWAP") {
      if (me.role != Role::Magician || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能交换。");
        return;
      }
      if (!magicianSwapA_.empty()) {
        sendLine(cs, "PRIV|你已交换过目标，不能重复交换。");
        return;
      }
      auto parts2 = split(normPayload, '|');
      if (parts2.size() != 2) {
        sendLine(cs, "PRIV|格式错误：请用 /swap A B（A/B 为编号或昵称）。");
        return;
      }
      std::string a = parts2[0], b = parts2[1];
      if (a == b || !targetOkLocked(a) || !targetOkLocked(b)) {
        sendLine(cs, "PRIV|目标无效（必须是两个不同的存活玩家昵称）。");
        return;
      }
      magicianSock_ = cs;
      magicianSwapA_ = a;
      magicianSwapB_ = b;
      gameCv_.notify_all();
      sendLine(cs, "PRIV|已交换 " + a + " 和 " + b + " 的身份查验结果。");
      return;
    }

    // 摄梦人：夜晚摄梦
    if (type == "DREAM_CATCH") {
      if (me.role != Role::DreamCatcher || phase_ != Phase::Night) {
        sendLine(cs, "PRIV|当前不能摄梦。");
        return;
      }
      if (!dreamCatcherTarget_.empty()) {
        sendLine(cs, "PRIV|你已摄梦过目标，不能重复摄梦。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      dreamCatcherSock_ = cs;
      dreamCatcherTarget_ = normPayload;
      // 连续两晚摄梦同一人则目标死亡
      if (dreamCatcherTarget_ == dreamCatcherLastTarget_ && !dreamCatcherTarget_.empty()) {
        std::vector<socket_t> dcDiedSocks;
        std::vector<std::string> dcDiedNames;
        killByNameLocked(dreamCatcherTarget_, dcDiedSocks, dcDiedNames, "dream", false);
        if (!dcDiedNames.empty()) {
          broadcastSys("摄梦人连续两晚摄梦 " + dreamCatcherTarget_ + "，目标在梦中死亡。");
        }
      }
      gameCv_.notify_all();
      sendLine(cs, "PRIV|已摄梦：" + normPayload);
      return;
    }

    // 骑士：白天决斗
    if (type == "KNIGHT_DUEL") {
      if (me.role != Role::Knight || phase_ != Phase::DayTalk) {
        sendLine(cs, "PRIV|当前不能决斗。");
        return;
      }
      if (!targetOkLocked(normPayload)) {
        sendLine(cs, "PRIV|目标无效（必须是存活玩家昵称）。");
        return;
      }
      // 查验目标是否为狼人
      bool targetIsWolf = false;
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive && kv.second.name == normPayload && isWolfRole(kv.second.role)) {
          targetIsWolf = true;
          break;
        }
      }
      if (targetIsWolf) {
        // 目标为狼人，目标死亡，骑士存活，白天立即结束进入夜晚
        std::vector<socket_t> duelDiedSocks;
        std::vector<std::string> duelDiedNames;
        killByNameLocked(normPayload, duelDiedSocks, duelDiedNames, "knight", false);
        broadcastSys("骑士 " + me.name + " 发动决斗，" + normPayload + " 是狼人，被当场处决！白天结束，进入夜晚。");
        // 强制跳过投票直接进入夜晚
        knightDuelSuccess_ = true;
        voteNow_ = true;
        gameCv_.notify_all();
      } else {
        // 目标不是狼人，骑士死亡，白天继续
        std::vector<socket_t> duelDiedSocks;
        std::vector<std::string> duelDiedNames;
        killByNameLocked(me.name, duelDiedSocks, duelDiedNames, "knight", false);
        broadcastSys("骑士 " + me.name + " 发动决斗，" + normPayload + " 不是狼人，骑士以死谢罪！");
        gameCv_.notify_all();
      }
      return;
    }

    sendLine(cs, "PRIV|未知 ACT 类型");
  }

 private:
  void sendState(const std::string& text) { broadcastLine("STATE|" + phaseToString(phase_) + "|" + text); }

  void sendActionToRole(Role r, const std::string& action, const std::string& hint = "") {
    std::lock_guard<std::mutex> lk(gameMu_);
    for (auto& kv : gamePlayers_) {
      if (kv.second.alive && kv.second.role == r) {
        sendLine(kv.first, "ACTION|" + action);
        if (!hint.empty()) sendLine(kv.first, "PRIV|" + hint);
      }
    }
  }

  void sendActionToSocket(socket_t s, const std::string& action, const std::string& hint = "") {
    sendLine(s, "ACTION|" + action);
    if (!hint.empty()) sendLine(s, "PRIV|" + hint);
  }

  void broadcastAlive() {
    std::vector<std::string> names;
    {
      std::lock_guard<std::mutex> lk(gameMu_);
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive) names.push_back(kv.second.name);
      }
    }
    std::sort(names.begin(), names.end());
    broadcastLine("ALIVE|" + join(names, '|'));
  }

  int aliveCountLocked() const {
    int c = 0;
    for (auto& kv : gamePlayers_) {
      if (kv.second.alive) c++;
    }
    return c;
  }

  int aliveRoleCountLocked(Role r) const {
    int c = 0;
    for (auto& kv : gamePlayers_) {
      if (kv.second.alive && kv.second.role == r) c++;
    }
    return c;
  }

  int aliveWolfCountLocked() const {
    int c = 0;
    for (auto& kv : gamePlayers_) {
      if (kv.second.alive && isWolfRole(kv.second.role)) c++;
    }
    return c;
  }

  bool isRoleAliveLocked(Role r) const { return aliveRoleCountLocked(r) > 0; }

  socket_t findAliveByRoleLocked(Role r) const {
    for (auto& kv : gamePlayers_) {
      if (kv.second.alive && kv.second.role == r) return kv.first;
    }
    return kInvalidSocket;
  }

  // 等待条件成立或超时（必须持有 gameMu_ 的 unique_lock）
  bool waitFor(std::unique_lock<std::mutex>& lk, std::function<bool()> pred, int timeoutMs) {
    return gameCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !gameRunning_.load() || pred(); });
  }

  socket_t findAliveSockByNameLocked(const std::string& name) const {
    for (auto& kv : gamePlayers_) {
      if (kv.second.alive && kv.second.name == name) return kv.first;
    }
    return kInvalidSocket;
  }

  // 杀死玩家：处理长老免死、恋人殉情、野孩子变狼
  bool killByNameLocked(const std::string& name,
                        std::vector<socket_t>& diedSocks,
                        std::vector<std::string>& diedNames,
                        const std::string& reason,
                        bool nightWolfKill) {
    socket_t s = findAliveSockByNameLocked(name);
    if (s == kInvalidSocket) return false;
    auto it = gamePlayers_.find(s);
    if (it == gamePlayers_.end()) return false;

    GamePlayer& p = it->second;
    if (!p.alive) return false;

    // 长老：夜里第一次被狼刀免死（毒药/投票不免）
    if (nightWolfKill && p.role == Role::Elder && !p.elderShieldUsed) {
      p.elderShieldUsed = true;
      sendLine(s, "PRIV|你昨晚被袭击，但长老护身生效，你活了下来（只生效一次）。");
      broadcastSys("昨晚有人被袭击但幸存。");
      return false;
    }

    p.alive = false;
    diedSocks.push_back(s);
    diedNames.push_back(p.name);

    // 野孩子：偶像死后变狼（只触发一次）
    if (!wildChildConverted_ && wildChildIdolSock_ == s && wildChildSock_ != kInvalidSocket) {
      auto itw = gamePlayers_.find(wildChildSock_);
      if (itw != gamePlayers_.end() && itw->second.alive && itw->second.role == Role::WildChild) {
        itw->second.role = Role::Werewolf;
        wildChildConverted_ = true;
        sendLine(wildChildSock_, "PRIV|你的偶像已死亡，你从现在起变成狼人！");
        sendLine(wildChildSock_, "ROLE|狼人");
        // 通知所有狼人新队友
        std::string newWolfName = itw->second.name;
        for (auto& kv : gamePlayers_) {
          if (kv.second.alive && isWolfRole(kv.second.role) && kv.first != wildChildSock_) {
            sendLine(kv.first, "PRIV|" + newWolfName + " 变成了狼人，现在是你的队友！");
          }
        }
        // 告诉新狼人现有队友
        std::string teammates;
        for (auto& kv : gamePlayers_) {
          if (kv.second.alive && isWolfRole(kv.second.role) && kv.first != wildChildSock_) {
            if (!teammates.empty()) teammates += "、";
            teammates += kv.second.name;
          }
        }
        if (!teammates.empty()) {
          sendLine(wildChildSock_, "PRIV|你的狼人队友有：" + teammates);
        }
      }
    }

    // 恋人殉情（递归一次）
    auto itL = loverOf_.find(s);
    if (itL != loverOf_.end()) {
      socket_t other = itL->second;
      loverOf_.erase(s);
      loverOf_.erase(other);
      auto ito = gamePlayers_.find(other);
      if (ito != gamePlayers_.end() && ito->second.alive) {
        ito->second.alive = false;
        diedSocks.push_back(other);
        diedNames.push_back(ito->second.name);
        sendLine(other, "PRIV|你的恋人死亡，你殉情而亡。");
        // 触发殉情者的连锁效果：野孩子变狼
        if (!wildChildConverted_ && wildChildIdolSock_ == other && wildChildSock_ != kInvalidSocket) {
          auto itwc = gamePlayers_.find(wildChildSock_);
          if (itwc != gamePlayers_.end() && itwc->second.alive && itwc->second.role == Role::WildChild) {
            itwc->second.role = Role::Werewolf;
            wildChildConverted_ = true;
            sendLine(wildChildSock_, "PRIV|你的偶像已死亡，你从现在起变成狼人！");
            sendLine(wildChildSock_, "ROLE|狼人");
            for (auto& kv : gamePlayers_) {
              if (kv.second.alive && isWolfRole(kv.second.role) && kv.first != wildChildSock_) {
                sendLine(kv.first, "PRIV|" + itwc->second.name + " 变成了狼人，现在是你的队友！");
              }
            }
          }
        }
      }
    }

    (void)reason;
    return true;
  }

  void wolfKingDeathShot(socket_t deadSock, const std::string& deadName) {
    auto it = gamePlayers_.find(deadSock);
    if (it == gamePlayers_.end()) return;
    Role r = it->second.role;
    if (!(r == Role::WolfKing || r == Role::WhiteWolfKing)) return;

    pendingWolfKingSock_ = deadSock;
    wolfKingShotTarget_.clear();
    wolfKingPass_ = false;
    sendLine(deadSock, "PRIV|[" + roleToString(r) + "] 你的技能：死亡时可开枪带走一人。");
    sendLine(deadSock, "PRIV|你死亡了，可以开枪带走 1 人：输入编号或 /shoot <昵称>，或 /pass 放弃（30秒超时）。");
    sendLine(deadSock, "ACTION|WOLFKING_SHOOT");
    // 等待开枪（这里假设调用者持有 gameMu_ 的 unique_lock）
    // timeout由调用者 waitFor 实现
    (void)deadName;
  }

  void gameLoop() {
    while (running_.load() && gameRunning_.load()) {
      // 夜晚
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        phase_ = Phase::Night;
        night_++;
        voteNow_ = false;
        knightDuelSuccess_ = false;
        wolfVotes_.clear();
        wolfKillTarget_.clear();
        lastGuardProtectTarget_ = guardProtectTarget_;  // 记录上一晚守护目标
        guardProtectTarget_.clear();
        witchPoisonTarget_.clear();
        witchPoisonPass_ = false;
        hunterPass_ = false;
        witchSaveDecision_ = -1;
        seerChecked_ = false;
        hunterShotTarget_.clear();
        pendingHunterSock_ = kInvalidSocket;
        // 新角色状态每晚清空
        wolfBeautyCharmTarget_.clear();
        ravenCurseTarget_.clear();
        magicianSwapA_.clear();
        magicianSwapB_.clear();
        // dreamCatcherTarget_ 在结算后清空，此处不清空
      }

      sendState("第 " + std::to_string(night_) + " 夜开始。");
      broadcastAlive();

      // 开局特殊阶段（仅执行一次）：丘比特/野孩子/盗贼
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        if (!setupDone_) {
          std::mt19937 rng((uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());

          auto aliveSocks = [&]() {
            std::vector<socket_t> v;
            for (auto& kv : gamePlayers_) {
              if (kv.second.alive) v.push_back(kv.first);
            }
            return v;
          };
          auto pickAliveExcept = [&](socket_t ex1, socket_t ex2) -> socket_t {
            std::vector<socket_t> v;
            for (auto& kv : gamePlayers_) {
              if (!kv.second.alive) continue;
              if (kv.first == ex1 || kv.first == ex2) continue;
              v.push_back(kv.first);
            }
            if (v.empty()) return kInvalidSocket;
            std::uniform_int_distribution<size_t> dist(0, v.size() - 1);
            return v[dist(rng)];
          };

          // 丘比特：连两名恋人
          socket_t cupid = findAliveByRoleLocked(Role::Cupid);
          if (cupid != kInvalidSocket) {
            sendLine(cupid, "PRIV|【丘比特】你的技能：开局连两名恋人，恋人死亡另一方殉情。");
            sendLine(cupid, "PRIV|请选择两名恋人。输入 /love A B（A/B 可为编号或昵称），或直接输入两个编号如：2 5");
            sendLine(cupid, "ACTION|CUPID_LINK");
            waitFor(lk, [&] { return cupidDone_; }, 60000);
            if (!cupidDone_) {
              auto v = aliveSocks();
              if (v.size() >= 2) {
                socket_t a = pickAliveExcept(kInvalidSocket, kInvalidSocket);
                socket_t b = pickAliveExcept(a, kInvalidSocket);
                if (a != kInvalidSocket && b != kInvalidSocket) {
                  loverOf_[a] = b;
                  loverOf_[b] = a;
                  cupidDone_ = true;
                  sendLine(a, "PRIV|你与 " + gamePlayers_[b].name + " 成为恋人（恋人死亡你也会殉情）。");
                  sendLine(b, "PRIV|你与 " + gamePlayers_[a].name + " 成为恋人（恋人死亡你也会殉情）。");
                  sendLine(cupid, "PRIV|超时：已自动连人。");
                }
              }
            }
          }

          // 野孩子：选偶像
          socket_t wc = findAliveByRoleLocked(Role::WildChild);
          if (wc != kInvalidSocket) {
            sendLine(wc, "PRIV|【野孩子】你的技能：开局选偶像，偶像死后变狼。");
            sendLine(wc, "PRIV|请选择你的偶像。输入 /idol <编号或昵称>（不能选自己）。");
            sendLine(wc, "ACTION|WILDCHILD_IDOL");
            waitFor(lk, [&] { return wildChildIdolSock_ != kInvalidSocket; }, 60000);
            if (wildChildIdolSock_ == kInvalidSocket) {
              socket_t idol = pickAliveExcept(wc, kInvalidSocket);
              wildChildSock_ = wc;
              wildChildIdolSock_ = idol;
              if (idol != kInvalidSocket) {
                sendLine(wc, "PRIV|超时：已自动选择偶像：" + gamePlayers_[idol].name);
              }
            }
          }

          // 盗贼：选牌变身（标准规则：从底牌池 3 选 1，有狼牌则强制为狼）
          socket_t thief = findAliveByRoleLocked(Role::Thief);
          if (thief != kInvalidSocket) {
            thiefSock_ = thief;
            // 底牌池：包含游戏中未分配的所有角色（好人 + 狼人）
            std::vector<Role> goodPool = {Role::Villager, Role::Seer, Role::Witch, Role::Hunter, Role::Guard,
                                           Role::Gravedigger, Role::Elder, Role::Idiot, Role::Knight, Role::Raven,
                                           Role::Bear, Role::DreamCatcher, Role::Magician};
            std::vector<Role> wolfPool = {Role::Werewolf, Role::HiddenWolf, Role::WolfKing, Role::WhiteWolfKing, Role::WolfBeauty};
            // 从好人池抽 2 张，从狼池抽 1 张（保证有狼）
            std::uniform_int_distribution<size_t> gDist(0, goodPool.size() - 1);
            thiefOpt1_ = goodPool[gDist(rng)];
            do { thiefOpt2_ = goodPool[gDist(rng)]; } while (thiefOpt2_ == thiefOpt1_);
            std::uniform_int_distribution<size_t> wDist(0, wolfPool.size() - 1);
            thiefOpt3_ = wolfPool[wDist(rng)];
            // 随机打乱 3 张牌的顺序
            int order[3] = {0, 1, 2};
            std::shuffle(order, order + 3, rng);
            Role opts[3] = {thiefOpt1_, thiefOpt2_, thiefOpt3_};
            thiefOpt1_ = opts[order[0]];
            thiefOpt2_ = opts[order[1]];
            thiefOpt3_ = opts[order[2]];

            bool hasWolf = (isWolfRole(thiefOpt1_) || isWolfRole(thiefOpt2_) || isWolfRole(thiefOpt3_));
            std::string wolfHint = hasWolf ? "底牌中有狼人牌，你将变身为狼人阵营！" : "底牌中没有狼人牌。";
            sendLine(thief, "PRIV|【盗贼】你的技能：从 3 张底牌中选 1 张变身。" + wolfHint);
            sendLine(thief, "PRIV|请选择一张身份牌变身：");
            sendLine(thief, "PRIV|  1) " + roleToString(thiefOpt1_));
            sendLine(thief, "PRIV|  2) " + roleToString(thiefOpt2_));
            sendLine(thief, "PRIV|  3) " + roleToString(thiefOpt3_));
            sendLine(thief, "PRIV|输入 1/2/3，或 /choose 1/2/3；/pass 视为默认选1（60秒超时）。");
            sendLine(thief, "ACTION|THIEF_CHOOSE");
            waitFor(lk, [&] { return thiefChosen_ || thiefPass_; }, 60000);
            if (!thiefChosen_) {
              Role chosen = thiefOpt1_;
              gamePlayers_[thief].role = chosen;
              thiefChosen_ = true;
              sendLine(thief, "PRIV|已默认变身为：" + roleToString(chosen));
              sendLine(thief, "ROLE|" + roleToString(chosen));
            }
            // 如果盗贼变身为狼人，通知所有狼人新队友
            if (isWolfRole(gamePlayers_[thief].role)) {
              std::string thiefName = gamePlayers_[thief].name;
              for (auto& kv : gamePlayers_) {
                if (kv.second.alive && isWolfRole(kv.second.role) && kv.first != thief) {
                  sendLine(kv.first, "PRIV|" + thiefName + "（盗贼）变身为狼人，现在是你的队友！");
                }
              }
              // 告诉盗贼现有狼人队友
              std::string teammates;
              for (auto& kv : gamePlayers_) {
                if (kv.second.alive && isWolfRole(kv.second.role) && kv.first != thief) {
                  if (!teammates.empty()) teammates += "、";
                  teammates += kv.second.name;
                }
              }
              if (!teammates.empty()) {
                sendLine(thief, "PRIV|你的狼人队友有：" + teammates);
              }
            }
          }

          setupDone_ = true;
        }
      }

      // 下发行动提示（夜晚）
      {
        std::lock_guard<std::mutex> lk(gameMu_);
        for (auto& kv : gamePlayers_) {
          if (kv.second.alive && isWolfRole(kv.second.role)) {
            sendLine(kv.first, "ACTION|WOLF_KILL");
            sendLine(kv.first, "PRIV|【狼人】你的技能：每晚投票选杀一人。输入编号或 /kill <昵称>。");
          }
        }
      }
      sendActionToRole(Role::Seer, "SEER_CHECK", "【预言家】你的技能：每晚查验一人是否为狼人。输入编号或 /check <昵称>。");
      sendActionToRole(Role::Guard, "GUARD_PROTECT", "【守卫】你的技能：每晚守护一人，不能连续两晚守同一人。输入编号或 /guard <昵称>。");
      sendActionToRole(Role::WolfBeauty, "WOLF_BEAUTY_CHARM", "【狼美人】你的技能：每晚魅惑一人，你死亡时被魅惑者殉情。输入编号或 /charm <昵称>。");
      sendActionToRole(Role::Raven, "RAVEN_CURSE", "【乌鸦】你的技能：每晚诅咒一人，白天被诅咒者多一票。输入编号或 /curse <昵称>。");
      sendActionToRole(Role::Magician, "MAGICIAN_SWAP", "【魔术师】你的技能：每晚交换两人身份（影响预言家验人）。输入 /swap A B（A/B为编号或昵称）。");
      sendActionToRole(Role::DreamCatcher, "DREAM_CATCH", "【摄梦人】你的技能：每晚摄梦一人，连续两晚摄梦同一人则目标死亡。输入编号或 /dream <昵称>。");
      sendActionToRole(Role::Piper, "PIPER_CHARM", "【吹笛人】你的技能：每晚蛊惑一人，被蛊惑者无法投票。输入编号或 /charm <昵称>。");
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        if (aliveWolfCountLocked() > 0) {
          // 等待狼刀（最多 60 秒）
          waitFor(lk, [&] { return !wolfVotes_.empty(); }, 60000);
          // 简化：统计票数，取最高票（平票取第一）
          std::map<std::string, int> cnt;
          for (auto& kv : wolfVotes_) cnt[kv.second]++;
          int best = 0;
          std::string bestName;
          for (auto& kv : cnt) {
            if (kv.second > best) {
              best = kv.second;
              bestName = kv.first;
            }
          }
          wolfKillTarget_ = bestName;
        }
      }

      // 等待其他夜晚角色行动（最多 30 秒）
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        // 等待预言家验人
        waitFor(lk, [&] { return seerChecked_ || findAliveByRoleLocked(Role::Seer) == kInvalidSocket; }, 30000);
        // 等待守卫守护（guardProtectTarget_ 不为空或无守卫）
        waitFor(lk, [&] { return !guardProtectTarget_.empty() || findAliveByRoleLocked(Role::Guard) == kInvalidSocket; }, 3000);
        // 等待狼美人魅惑
        waitFor(lk, [&] { return !wolfBeautyCharmTarget_.empty() || findAliveByRoleLocked(Role::WolfBeauty) == kInvalidSocket; }, 3000);
        // 等待乌鸦诅咒
        waitFor(lk, [&] { return !ravenCurseTarget_.empty() || findAliveByRoleLocked(Role::Raven) == kInvalidSocket; }, 3000);
        // 等待魔术师交换
        waitFor(lk, [&] { return !magicianSwapA_.empty() || findAliveByRoleLocked(Role::Magician) == kInvalidSocket; }, 3000);
        // 等待摄梦人
        waitFor(lk, [&] { return !dreamCatcherTarget_.empty() || findAliveByRoleLocked(Role::DreamCatcher) == kInvalidSocket; }, 3000);
        // 等待吹笛人
        waitFor(lk, [&] { return piperSock_ != kInvalidSocket || findAliveByRoleLocked(Role::Piper) == kInvalidSocket; }, 3000);
      }

      // 告知女巫并询问解药/毒药
      socket_t witchSock = kInvalidSocket;
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        witchSock = findAliveByRoleLocked(Role::Witch);
        if (witchSock != kInvalidSocket) {
          auto& w = gamePlayers_[witchSock];
          if (!wolfKillTarget_.empty()) {
            sendLine(witchSock, "PRIV|今晚狼人选择击杀：" + wolfKillTarget_);
          } else {
            sendLine(witchSock, "PRIV|今晚狼人未选择击杀（或超时）。");
          }
          if (!w.witchSaveUsed && !wolfKillTarget_.empty()) {
            sendLine(witchSock, "PRIV|【女巫】你的技能：有一瓶解药（救被刀者）和一瓶毒药（毒杀一人），同一晚不能同时使用。");
            sendLine(witchSock, "PRIV|是否使用解药？输入 /save yes 或 /save no");
            sendActionToSocket(witchSock, "WITCH_SAVE");
            waitFor(lk, [&] { return witchSaveDecision_ != -1; }, 30000);
            if (witchSaveDecision_ == 1) w.witchSaveUsed = true;
          }
          // 女巫同一晚不能同时使用解药和毒药
          if (!w.witchPoisonUsed && witchSaveDecision_ != 1) {
            sendLine(witchSock, "PRIV|是否使用毒药？输入编号或 /poison <昵称>，或 /pass 放弃");
            sendActionToSocket(witchSock, "WITCH_POISON");
            waitFor(lk, [&] { return !witchPoisonTarget_.empty() || witchPoisonPass_; }, 30000);
            if (!witchPoisonTarget_.empty()) w.witchPoisonUsed = true;
          }
        }
      }

      // 结算夜晚死亡
      std::vector<socket_t> diedSocks;
      std::vector<std::string> died;
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        auto kill = wolfKillTarget_;
        bool saved = (witchSaveDecision_ == 1);
        bool protectedOk = (!guardProtectTarget_.empty() && guardProtectTarget_ == kill);

        if (!kill.empty() && !saved && !protectedOk) killByNameLocked(kill, diedSocks, died, "wolf", true);
        if (!witchPoisonTarget_.empty()) killByNameLocked(witchPoisonTarget_, diedSocks, died, "poison", false);

        // 狼王/白狼王死亡技能（按“已死名单”逐个处理）
        for (size_t i = 0; i < diedSocks.size(); i++) {
          auto ds = diedSocks[i];
          auto itp = gamePlayers_.find(ds);
          if (itp == gamePlayers_.end()) continue;
          Role rr = itp->second.role;
          if (!(rr == Role::WolfKing || rr == Role::WhiteWolfKing)) continue;

          wolfKingDeathShot(ds, itp->second.name);
          waitFor(lk, [&] { return !wolfKingShotTarget_.empty() || wolfKingPass_; }, 30000);
          if (!wolfKingShotTarget_.empty()) {
            killByNameLocked(wolfKingShotTarget_, diedSocks, died, "wolfking", false);
            broadcastSys(roleToString(rr) + " 开枪带走：" + wolfKingShotTarget_);
          }
          pendingWolfKingSock_ = kInvalidSocket;
          wolfKingShotTarget_.clear();
          wolfKingPass_ = false;
        }

        // 狼美人死亡：被魅惑者殉情
        for (size_t i = 0; i < diedSocks.size(); i++) {
          auto ds = diedSocks[i];
          auto itp = gamePlayers_.find(ds);
          if (itp == gamePlayers_.end()) continue;
          if (itp->second.role != Role::WolfBeauty) continue;
          if (!wolfBeautyCharmTarget_.empty()) {
            killByNameLocked(wolfBeautyCharmTarget_, diedSocks, died, "wolfbeauty", false);
            broadcastSys("狼美人死亡，被魅惑的 " + wolfBeautyCharmTarget_ + " 殉情而亡。");
          }
        }

        // 猎人被狼刀死时可以开枪（被毒死不能开枪）
        for (size_t i = 0; i < diedSocks.size(); i++) {
          auto ds = diedSocks[i];
          auto itp = gamePlayers_.find(ds);
          if (itp == gamePlayers_.end()) continue;
          if (itp->second.role != Role::Hunter) continue;
          // 判断死因：如果是被毒死的（witchPoisonTarget 匹配），不能开枪
          bool poisonedDeath = false;
          if (!witchPoisonTarget_.empty() && itp->second.name == witchPoisonTarget_) {
            poisonedDeath = true;
          }
          if (poisonedDeath) continue;

          pendingHunterSock_ = ds;
          hunterShotTarget_.clear();
          hunterPass_ = false;
          sendLine(ds, "PRIV|【猎人】你的技能：被狼刀死或被投票处决时可开枪带走一人（被毒死不能开枪）。");
          sendLine(ds, "PRIV|你被狼刀杀死，可以开枪带走 1 人：输入编号或 /shoot <昵称>，或 /pass 放弃（30秒超时）。");
          sendLine(ds, "ACTION|HUNTER_SHOOT");
          waitFor(lk, [&] { return !hunterShotTarget_.empty() || hunterPass_; }, 30000);
          if (!hunterShotTarget_.empty()) {
            killByNameLocked(hunterShotTarget_, diedSocks, died, "hunter", false);
            broadcastSys("猎人开枪带走：" + hunterShotTarget_);
          }
          pendingHunterSock_ = kInvalidSocket;
          hunterShotTarget_.clear();
          hunterPass_ = false;
        }
      }

      if (died.empty()) {
        broadcastSys("天亮了：昨晚是平安夜。");
      } else {
        broadcastSys("天亮了：昨晚死亡：" + join(died, ','));
      }
      broadcastAlive();

      // 熊的咆哮：查验左右相邻玩家是否有狼人
      {
        std::lock_guard<std::mutex> lk(gameMu_);
        socket_t bearSock = findAliveByRoleLocked(Role::Bear);
        if (bearSock != kInvalidSocket) {
          // 按存活玩家列表顺序找左右邻居
          std::vector<std::string> aliveNames;
          for (auto& kv : gamePlayers_) {
            if (kv.second.alive) aliveNames.push_back(kv.second.name);
          }
          std::sort(aliveNames.begin(), aliveNames.end());
          auto itBear = std::find(aliveNames.begin(), aliveNames.end(), gamePlayers_[bearSock].name);
          bool hasWolfNeighbor = false;
          if (itBear != aliveNames.end()) {
            size_t idx = itBear - aliveNames.begin();
            if (idx > 0) {
              std::string left = aliveNames[idx - 1];
              for (auto& kv : gamePlayers_) {
                if (kv.second.name == left && kv.second.alive && isWolfRole(kv.second.role)) {
                  hasWolfNeighbor = true; break;
                }
              }
            }
            if (!hasWolfNeighbor && idx + 1 < aliveNames.size()) {
              std::string right = aliveNames[idx + 1];
              for (auto& kv : gamePlayers_) {
                if (kv.second.name == right && kv.second.alive && isWolfRole(kv.second.role)) {
                  hasWolfNeighbor = true; break;
                }
              }
            }
          }
          if (hasWolfNeighbor) {
            broadcastSys("熊咆哮了！熊的左右邻居中有狼人！");
          } else {
            broadcastSys("熊没有咆哮，左右邻居中没有狼人。");
          }
        }
      }

      // 摄梦人更新上一晚目标
      {
        std::lock_guard<std::mutex> lk(gameMu_);
        dreamCatcherLastTarget_ = dreamCatcherTarget_;
        dreamCatcherTarget_.clear();
      }

      if (checkWinAndMaybeEnd()) break;

      // 白天发言（可跳过）
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        phase_ = Phase::DayTalk;
        voteNow_ = false;
      }
      day_++;
      sendState("第 " + std::to_string(day_) + " 天白天发言。房主可输入 /vote 立刻开始投票（或 60 秒后自动开始）。");
      // 给骑士下发决斗 ACTION
      {
        std::lock_guard<std::mutex> lk(gameMu_);
        socket_t knightSock = findAliveByRoleLocked(Role::Knight);
        if (knightSock != kInvalidSocket) {
          sendLine(knightSock, "PRIV|【骑士】你的技能：白天可发动决斗，查验目标是否为狼人。输入编号或 /duel <昵称>。");
          sendLine(knightSock, "ACTION|KNIGHT_DUEL");
        }
      }
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        waitFor(lk, [&] { return voteNow_; }, 60000);
      }

      // 骑士决斗成功：跳过投票直接进入下一轮夜晚
      {
        std::lock_guard<std::mutex> lk(gameMu_);
        if (knightDuelSuccess_) {
          broadcastSys("骑士决斗成功，跳过投票，直接进入夜晚。");
          broadcastAlive();
          if (checkWinAndMaybeEnd()) break;
          continue;  // 回到 while 循环顶部，进入夜晚
        }
      }

      // 投票
      bool skipDayEnd = false;
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        phase_ = Phase::Vote;
        votes_.clear();
      }
      sendState("开始投票：输入编号或 /vote <昵称>。");
      broadcastAlive();
      // 给所有存活玩家下发投票行动
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        for (auto& kv : gamePlayers_) {
          if (kv.second.alive) {
            sendLine(kv.first, "ACTION|VOTE");
            sendLine(kv.first, "PRIV|【投票】白天投票阶段：输入编号或 /vote <昵称> 投票出局一人。");
          }
        }
      }
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        int need = aliveCountLocked();
        waitFor(lk, [&] { return (int)votes_.size() >= need; }, 60000);
      }

      socket_t eliminatedSock = kInvalidSocket;
      std::string eliminatedName;
      Role eliminatedRole = Role::Villager;
      {
        std::unique_lock<std::mutex> lk(gameMu_);
        // 统计
        std::map<std::string, int> cnt;
        for (auto& kv : votes_) cnt[kv.second]++;
        int best = 0;
        std::string bestName;
        bool tie = false;
        for (auto& kv : cnt) {
          if (kv.second > best) {
            best = kv.second;
            bestName = kv.first;
            tie = false;
          } else if (kv.second == best && best > 0) {
            tie = true;
          }
        }
        // 乌鸦诅咒：被诅咒的目标多一票
        if (!ravenCurseTarget_.empty()) {
          // 被诅咒者额外加一票（即使无人投他也有一票）
          cnt[ravenCurseTarget_] += 1;
          // 重新统计最高票（乌鸦诅咒后可能改变结果）
          best = 0;
          bestName.clear();
          tie = false;
          for (auto& kv : cnt) {
            if (kv.second > best) {
              best = kv.second;
              bestName = kv.first;
              tie = false;
            } else if (kv.second == best && best > 0) {
              tie = true;
            }
          }
        }

        // 炸弹人被投票处决时爆炸
        for (auto& kv : gamePlayers_) {
          if (kv.second.alive && kv.second.role == Role::Bomber && kv.second.name == bestName) {
            // 炸弹人爆炸：带走所有投他的人
            std::vector<std::string> bomberVictims;
            for (auto& v : votes_) {
              if (v.second == bestName) {
                bomberVictims.push_back(gamePlayers_[v.first].name);
              }
            }
            if (!bomberVictims.empty()) {
              broadcastSys("炸弹人 " + bestName + " 被投票处决，爆炸带走所有投他的人！");
              for (auto& victim : bomberVictims) {
                std::vector<socket_t> bombDiedSocks;
                std::vector<std::string> bombDiedNames;
                killByNameLocked(victim, bombDiedSocks, bombDiedNames, "bomber", false);
              }
            }
            kv.second.alive = false;  // 炸弹人自己死亡
            kv.second.bomberExploded = true;
            broadcastAlive();
            // 爆炸后检查胜负
            if (checkWinAndMaybeEnd()) { skipDayEnd = true; break; }
            break;
          }
        }

        if (skipDayEnd) continue;  // 炸弹人爆炸后游戏已结束，跳过后续处决

        if (best == 0 || tie) {
          broadcastSys("投票结果：平票或无人投票，本轮无人出局。");
        } else {
          // 处决 bestName（炸弹人已爆炸死亡则跳过）
          bool bomberAlreadyDead = false;
          for (auto& kv : gamePlayers_) {
            if (kv.second.name == bestName && kv.second.bomberExploded) {
              bomberAlreadyDead = true;
              break;
            }
          }
          if (bomberAlreadyDead) {
            broadcastSys("投票结果：" + bestName + " 是炸弹人，已爆炸死亡。");
          } else for (auto& kv : gamePlayers_) {
            if (kv.second.alive && kv.second.name == bestName) {
              // 白痴被投票后翻牌免死，但失去投票权
              if (kv.second.role == Role::Idiot && !kv.second.idiotRevealed) {
                kv.second.idiotRevealed = true;
                kv.second.idiotNoVote = true;
                sendLine(kv.first, "PRIV|你是白痴，翻牌免死！但你将失去投票权。");
                broadcastSys("投票结果：" + bestName + " 是白痴，翻牌免死，失去投票权。");
                break;
              }
              kv.second.alive = false;
              eliminatedSock = kv.first;
              eliminatedName = bestName;
              eliminatedRole = kv.second.role;
              // 使用 killByNameLocked 处理连锁效果（恋人殉情、野孩子变狼）
              std::vector<socket_t> voteDiedSocks;
              std::vector<std::string> voteDiedNames;
              // 回退 alive 设置，让 killByNameLocked 统一处理
              kv.second.alive = true;
              killByNameLocked(bestName, voteDiedSocks, voteDiedNames, "vote", false);
              if (voteDiedNames.size() > 1) {
                broadcastSys("投票结果：" + eliminatedName + " 被处决。" + " 恋人 " + join({voteDiedNames.begin() + 1, voteDiedNames.end()}, ',') + " 殉情。");
              } else {
                broadcastSys("投票结果：" + eliminatedName + " 被处决。");
              }
              break;
            }
          }
        }
      }
      broadcastAlive();

      // 守墓人：得知被处决者身份
      {
        std::lock_guard<std::mutex> lk(gameMu_);
        if (!eliminatedName.empty()) {
          socket_t graveSock = findAliveByRoleLocked(Role::Gravedigger);
          if (graveSock != kInvalidSocket) {
            sendLine(graveSock, "PRIV|守墓人技能：被处决的 " + eliminatedName + " 的身份是 " + roleToString(eliminatedRole));
          }
        }
      }

      // 猎人开枪（若猎人被处决）
      if (eliminatedSock != kInvalidSocket && eliminatedRole == Role::Hunter) {
        std::unique_lock<std::mutex> lk(gameMu_);
        pendingHunterSock_ = eliminatedSock;
        hunterShotTarget_.clear();
        hunterPass_ = false;
        sendLine(eliminatedSock, "PRIV|【猎人】你的技能：被狼刀死或被投票处决时可开枪带走一人（被毒死不能开枪）。");
        sendLine(eliminatedSock, "PRIV|你被投票处决，可以开枪带走 1 人：输入编号或 /shoot <昵称>，或 /pass 放弃（30秒超时）。");
        sendLine(eliminatedSock, "ACTION|HUNTER_SHOOT");
        waitFor(lk, [&] { return !hunterShotTarget_.empty() || hunterPass_; }, 30000);
        std::string hunterTarget = hunterShotTarget_;
        pendingHunterSock_ = kInvalidSocket;
        hunterShotTarget_.clear();
        hunterPass_ = false;
        lk.unlock();
        if (!hunterTarget.empty()) {
          // 使用 killByNameLocked 处理连锁效果（恋人殉情、野孩子变狼）
          std::vector<socket_t> diedSocks2;
          std::vector<std::string> diedNames2;
          {
            std::unique_lock<std::mutex> lk2(gameMu_);
            killByNameLocked(hunterTarget, diedSocks2, diedNames2, "hunter", false);
            // 处理猎人开枪带走的狼王/白狼王开枪
            for (size_t i = 0; i < diedSocks2.size(); i++) {
              auto ds2 = diedSocks2[i];
              auto it2 = gamePlayers_.find(ds2);
              if (it2 == gamePlayers_.end()) continue;
              if (it2->second.role == Role::WolfKing || it2->second.role == Role::WhiteWolfKing) {
                wolfKingDeathShot(ds2, it2->second.name);
                waitFor(lk2, [&] { return !wolfKingShotTarget_.empty() || wolfKingPass_; }, 30000);
                if (!wolfKingShotTarget_.empty()) {
                  killByNameLocked(wolfKingShotTarget_, diedSocks2, diedNames2, "wolfking", false);
                  broadcastSys(it2->second.name + "（狼王/白狼王）临死开枪带走：" + wolfKingShotTarget_);
                }
                pendingWolfKingSock_ = kInvalidSocket;
                wolfKingShotTarget_.clear();
                wolfKingPass_ = false;
              }
            }
            // 处理猎人开枪带走的狼美人殉情
            for (size_t i = 0; i < diedSocks2.size(); i++) {
              auto ds2 = diedSocks2[i];
              auto it2 = gamePlayers_.find(ds2);
              if (it2 == gamePlayers_.end()) continue;
              if (it2->second.role == Role::WolfBeauty && !wolfBeautyCharmTarget_.empty()) {
                killByNameLocked(wolfBeautyCharmTarget_, diedSocks2, diedNames2, "wolfbeauty", false);
                broadcastSys("狼美人死亡，被魅惑的 " + wolfBeautyCharmTarget_ + " 殉情而亡。");
              }
            }
          }
          if (!diedNames2.empty()) {
            broadcastSys("猎人开枪带走：" + join(diedNames2, ','));
          }
          broadcastAlive();
        }
      }

      // 处决的玩家如果是狼王/白狼王，检查是否需要开枪（非猎人情况）
      if (eliminatedSock != kInvalidSocket &&
          (eliminatedRole == Role::WolfKing || eliminatedRole == Role::WhiteWolfKing) &&
          eliminatedRole != Role::Hunter) {
        std::unique_lock<std::mutex> lk(gameMu_);
        wolfKingDeathShot(eliminatedSock, eliminatedName);
        waitFor(lk, [&] { return !wolfKingShotTarget_.empty() || wolfKingPass_; }, 30000);
        std::string wkTarget = wolfKingShotTarget_;
        pendingWolfKingSock_ = kInvalidSocket;
        wolfKingShotTarget_.clear();
        wolfKingPass_ = false;
        lk.unlock();
        if (!wkTarget.empty()) {
          std::vector<socket_t> wkDiedSocks;
          std::vector<std::string> wkDiedNames;
          {
            std::unique_lock<std::mutex> lk2(gameMu_);
            killByNameLocked(wkTarget, wkDiedSocks, wkDiedNames, "wolfking", false);
          }
          if (!wkDiedNames.empty()) {
            broadcastSys(eliminatedName + "（狼王/白狼王）临死开枪带走：" + join(wkDiedNames, ','));
          }
          broadcastAlive();
        }
      }

      // 处决的玩家如果是狼美人，检查魅惑殉情（非猎人情况）
      if (eliminatedSock != kInvalidSocket && eliminatedRole == Role::WolfBeauty &&
          eliminatedRole != Role::Hunter && !wolfBeautyCharmTarget_.empty()) {
        std::vector<socket_t> wbDiedSocks;
        std::vector<std::string> wbDiedNames;
        {
          std::unique_lock<std::mutex> lk(gameMu_);
          killByNameLocked(wolfBeautyCharmTarget_, wbDiedSocks, wbDiedNames, "wolfbeauty", false);
        }
        if (!wbDiedNames.empty()) {
          broadcastSys("狼美人 " + eliminatedName + " 被处决，被魅惑的 " + wolfBeautyCharmTarget_ + " 殉情而亡。");
        }
        broadcastAlive();
      }

      if (checkWinAndMaybeEnd()) break;
    }
  }

  bool checkWinAndMaybeEnd() {
    std::unique_lock<std::mutex> lk(gameMu_);
    int wolves = aliveWolfCountLocked();
    int alive = aliveCountLocked();
    int good = alive - wolves;
    if (wolves <= 0) {
      phase_ = Phase::GameOver;
      lk.unlock();
      broadcastLine("WIN|好人阵营胜利！");
      broadcastSys("好人阵营胜利！");
      gameRunning_.store(false);
      return true;
    }
    if (wolves >= good) {
      phase_ = Phase::GameOver;
      lk.unlock();
      broadcastLine("WIN|狼人阵营胜利！");
      broadcastSys("狼人阵营胜利！");
      gameRunning_.store(false);
      return true;
    }
    // 吹笛人胜利：所有存活玩家（除吹笛人自己）都被蛊惑
    socket_t piperSock = findAliveByRoleLocked(Role::Piper);
    if (piperSock != kInvalidSocket) {
      bool allCharmed = true;
      for (auto& kv : gamePlayers_) {
        if (kv.second.alive && kv.first != piperSock && !kv.second.piperCharmed) {
          allCharmed = false;
          break;
        }
      }
      if (allCharmed && alive >= 1) {
        phase_ = Phase::GameOver;
        lk.unlock();
        broadcastLine("WIN|吹笛人阵营胜利！所有玩家都已被蛊惑！");
        broadcastSys("吹笛人阵营胜利！所有玩家都已被蛊惑！");
        gameRunning_.store(false);
        return true;
      }
    }
    return false;
  }

  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  socket_t listenSock_{kInvalidSocket};
  std::thread acceptTh_;
  mutable std::mutex mu_;
  std::map<socket_t, ClientState> clients_;

  DiscoveryResponder responder_;

  // 游戏状态
  std::atomic<bool> gameRunning_{false};
  std::thread gameTh_;
  mutable std::mutex gameMu_;
  std::condition_variable gameCv_;
  std::map<socket_t, GamePlayer> gamePlayers_;
  Phase phase_{Phase::Lobby};
  int day_ = 0;
  int night_ = 0;
  bool voteNow_ = false;
  bool knightDuelSuccess_ = false;  // 骑士决斗成功，跳过投票直接进入夜晚

  // 夜晚行动数据
  std::map<socket_t, std::string> wolfVotes_;
  std::string wolfKillTarget_;
  std::string guardProtectTarget_;
  std::string lastGuardProtectTarget_;  // 上一晚守护目标（不能连续守同一人）
  bool seerChecked_ = false;
  int witchSaveDecision_ = -1;  // -1未知 0否 1是
  std::string witchPoisonTarget_;
  bool witchPoisonPass_ = false;

  // 白天投票
  std::map<socket_t, std::string> votes_;

  // 猎人开枪
  socket_t pendingHunterSock_ = kInvalidSocket;
  std::string hunterShotTarget_;
  bool hunterPass_ = false;

  // 狼王/白狼王开枪（简化：死亡触发）
  socket_t pendingWolfKingSock_ = kInvalidSocket;
  std::string wolfKingShotTarget_;
  bool wolfKingPass_ = false;

  // 丘比特恋人
  std::map<socket_t, socket_t> loverOf_;  // 双向映射
  bool cupidDone_ = false;

  // 野孩子
  socket_t wildChildSock_ = kInvalidSocket;
  socket_t wildChildIdolSock_ = kInvalidSocket;
  bool wildChildConverted_ = false;

  // 盗贼
  socket_t thiefSock_ = kInvalidSocket;
  Role thiefOpt1_ = Role::Villager;
  Role thiefOpt2_ = Role::Villager;
  Role thiefOpt3_ = Role::Villager;  // 盗贼 3 选 1
  bool thiefChosen_ = false;
  bool thiefPass_ = false;

  // 狼美人
  socket_t wolfBeautySock_ = kInvalidSocket;
  std::string wolfBeautyCharmTarget_;  // 被魅惑的目标

  // 乌鸦
  socket_t ravenSock_ = kInvalidSocket;
  std::string ravenCurseTarget_;       // 被诅咒的目标

  // 吹笛人
  socket_t piperSock_ = kInvalidSocket;

  // 魔术师
  socket_t magicianSock_ = kInvalidSocket;
  std::string magicianSwapA_;
  std::string magicianSwapB_;

  // 摄梦人
  socket_t dreamCatcherSock_ = kInvalidSocket;
  std::string dreamCatcherTarget_;
  std::string dreamCatcherLastTarget_;

  // 开局特殊阶段是否已处理（丘比特/野孩子/盗贼）
  bool setupDone_ = false;

  std::string roomId_;
  std::string roomName_;
  int maxPlayers_ = 0;
  uint16_t tcpPort_ = 0;
};

// ---------------- TCP 客户端 ----------------

class RoomClient {
 public:
  ~RoomClient() { disconnect(); }

  bool connectTo(const std::string& ip, uint16_t port, const std::string& name) {
    disconnect();
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == kInvalidSocket) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!parseIPv4(ip, addr.sin_addr)) {
      closeSocket(sock_);
      sock_ = kInvalidSocket;
      return false;
    }

    if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
      closeSocket(sock_);
      sock_ = kInvalidSocket;
      return false;
    }

    if (!sendLine(sock_, "JOIN|" + name)) {
      disconnect();
      return false;
    }

    running_.store(true);
    recvTh_ = std::thread([this] { recvLoop(); });
    return true;
  }

  void disconnect() {
    running_.store(false);
    if (sock_ != kInvalidSocket) {
      sendLine(sock_, "LEAVE");
      closeSocket(sock_);
      sock_ = kInvalidSocket;
    }
    if (recvTh_.joinable()) recvTh_.join();
    {
      std::lock_guard<std::mutex> lk(mu_);
      players_.clear();
    }
  }

  bool isConnected() const { return sock_ != kInvalidSocket && running_.load(); }

  void sendChat(const std::string& text) {
    if (sock_ == kInvalidSocket) return;
    // CHAT|<text...> （服务端会附加昵称）
    sendLine(sock_, "CHAT|" + text);
  }

  void sendWolfChat(const std::string& text) {
    if (sock_ == kInvalidSocket) return;
    sendLine(sock_, "WOLF_CHAT|" + text);
  }

  // 判断自己是否是狼人角色
  bool isMyRoleWolf() {
    std::lock_guard<std::mutex> lk(mu_);
    return role_ == "狼人" || role_ == "隐狼" || role_ == "狼王" || role_ == "白狼王" || role_ == "狼美人";
  }

  // 判断当前是否处于夜晚且有狼人行动待处理（狼人可以聊天+行动）
  bool isNightWolfAction() {
    std::lock_guard<std::mutex> lk(mu_);
    return phase_ == "NIGHT" && pendingAction_ == "WOLF_KILL";
  }

  void sendCmd(const std::string& cmd) {
    if (sock_ == kInvalidSocket) return;
    sendLine(sock_, "CMD|" + cmd);
  }

  void sendAct(const std::string& type, const std::string& payload) {
    if (sock_ == kInvalidSocket) return;
    if (payload.empty()) {
      sendLine(sock_, "ACT|" + type);
    } else {
      sendLine(sock_, "ACT|" + type + "|" + payload);
    }
  }

  void showAliveMenu() {
    std::lock_guard<std::mutex> lk(mu_);
    if (alive_.empty()) {
      safePrintln("[SYS] 暂无存活玩家列表。");
      return;
    }
    safePrintln("---- 存活玩家 ----");
    for (size_t i = 0; i < alive_.size(); i++) {
      safePrintln("  " + std::to_string(i + 1) + ") " + alive_[i]);
    }
  }

  std::string aliveNameByIndex(int idx) {
    std::lock_guard<std::mutex> lk(mu_);
    if (idx < 1 || idx > (int)alive_.size()) return "";
    return alive_[idx - 1];
  }

  std::string pendingAction() {
    std::lock_guard<std::mutex> lk(mu_);
    return pendingAction_;
  }

  // 判断当前是否处于夜晚且没有待处理行动（即不能输入）
  bool isNightWaiting() {
    std::lock_guard<std::mutex> lk(mu_);
    return phase_ == "NIGHT" && pendingAction_.empty();
  }

  // 在服务器提示 ACTION 后，可直接输入编号
  bool sendByIndexIfPending(int idx) {
    std::string action;
    std::string target;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (pendingAction_.empty()) return false;
      if (idx < 1 || idx > (int)alive_.size()) return false;
      action = pendingAction_;
      target = alive_[idx - 1];
    }

    if (action == "VOTE") {
      sendAct("VOTE", target);
    } else if (action == "WOLF_KILL") {
      sendAct("WOLF_KILL", target);
    } else if (action == "SEER_CHECK") {
      sendAct("SEER_CHECK", target);
    } else if (action == "GUARD_PROTECT") {
      sendAct("GUARD_PROTECT", target);
    } else if (action == "WITCH_POISON") {
      sendAct("WITCH_POISON", target);
    } else if (action == "HUNTER_SHOOT") {
      sendAct("HUNTER_SHOOT", target);
    } else if (action == "WOLFKING_SHOOT") {
      sendAct("WOLFKING_SHOOT", target);
    } else if (action == "WILDCHILD_IDOL") {
      sendAct("WILDCHILD_IDOL", target);
    } else if (action == "WOLF_BEAUTY_CHARM") {
      sendAct("WOLF_BEAUTY_CHARM", target);
    } else if (action == "RAVEN_CURSE") {
      sendAct("RAVEN_CURSE", target);
    } else if (action == "PIPER_CHARM") {
      sendAct("PIPER_CHARM", target);
    } else if (action == "DREAM_CATCH") {
      sendAct("DREAM_CATCH", target);
    } else if (action == "KNIGHT_DUEL") {
      sendAct("KNIGHT_DUEL", target);
    } else if (action == "MAGICIAN_SWAP") {
      // 魔术师需要两个目标，不能用单个编号
      safePrintln("[SYS] 魔术师请使用 /swap A B 命令（A/B 为编号或昵称）。");
      return true;  // 拦截数字输入
    } else {
      return false;
    }
    // 行动已发送，清除 pendingAction_
    {
      std::lock_guard<std::mutex> lk(mu_);
      pendingAction_.clear();
    }
    return true;
  }

 private:
  void recvLoop() {
    while (running_.load()) {
      std::string line;
      if (!recvLine(sock_, line)) break;
      auto parts = split(line, '|');
      if (parts.empty()) continue;

      if (parts[0] == "SYS") {
        if (parts.size() >= 2 && parts[1] == "BYE") {
          break;
        }
        std::string text = (parts.size() >= 2) ? line.substr(4) : "";
        safePrintln("[SYS] " + text);
      } else if (parts[0] == "CHAT" && parts.size() >= 3) {
        std::string name = parts[1];
        std::string text = line.substr(6 + name.size());  // "CHAT|" + name + "|"
        safePrintln(name + ": " + text);
      } else if (parts[0] == "WOLF_CHAT" && parts.size() >= 3) {
        std::string name = parts[1];
        std::string text = line.substr(10 + name.size());  // "WOLF_CHAT|" + name + "|"
        safePrintln("[狼人频道] " + name + ": " + text);
      } else if (parts[0] == "PLAYERS") {
        std::string list = (parts.size() >= 2) ? line.substr(8) : "";
        auto names = split(list, '|');
        {
          std::lock_guard<std::mutex> lk(mu_);
          players_ = std::move(names);
        }
        safePrintln("[SYS] 玩家列表已更新");
      } else if (parts[0] == "ROLE" && parts.size() >= 2) {
        {
          std::lock_guard<std::mutex> lk(mu_);
          role_ = parts[1];
        }
        safePrintln("[SYS] 你的身份是：" + parts[1]);
      } else if (parts[0] == "PRIV" && parts.size() >= 2) {
        std::string text = line.substr(5);
        safePrintln("[私聊] " + text);
      } else if (parts[0] == "STATE" && parts.size() >= 3) {
        std::string phase = parts[1];
        std::string text = line.substr(6 + phase.size());  // "STATE|" + phase + "|"
        {
          std::lock_guard<std::mutex> lk(mu_);
          phase_ = phase;
          pendingAction_.clear();  // 阶段切换时清除旧的行动提示
        }
        safePrintln("[阶段] " + phase + " - " + text);
      } else if (parts[0] == "ALIVE") {
        std::string list = (parts.size() >= 2) ? line.substr(6) : "";
        auto names = split(list, '|');
        names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& s) { return s.empty(); }),
                    names.end());
        {
          std::lock_guard<std::mutex> lk(mu_);
          alive_ = std::move(names);
        }
        showAliveMenu();
      } else if (parts[0] == "ACTION" && parts.size() >= 2) {
        std::lock_guard<std::mutex> lk(mu_);
        pendingAction_ = parts[1];
        safePrintln("[SYS] 需要行动：" + pendingAction_ + "（可直接输入编号，或输入对应 /命令）");
      } else if (parts[0] == "WIN" && parts.size() >= 2) {
        safePrintln("[SYS] " + line.substr(4));
      } else {
        // 忽略未知消息
      }
    }
    running_.store(false);
    if (sock_ != kInvalidSocket) {
      closeSocket(sock_);
      sock_ = kInvalidSocket;
    }
    safePrintln("[SYS] 与房间断开连接。");
  }

 private:
  socket_t sock_{kInvalidSocket};
  std::atomic<bool> running_{false};
  std::thread recvTh_;
  std::mutex mu_;
  std::vector<std::string> players_;
  std::vector<std::string> alive_;
  std::string pendingAction_;
  std::string role_;
  std::string phase_;
};

// ---------------- 控制台交互 ----------------

static std::string readLinePrompt(const std::string& prompt) {
  safePrint(prompt);
  std::string s;
  std::getline(std::cin, s);
  return s;
}

static int readIntPrompt(const std::string& prompt, int defVal) {
  while (true) {
    std::string s = readLinePrompt(prompt);
    if (s.empty()) return defVal;
    try {
      return std::stoi(s);
    } catch (...) {
      safePrintln("[SYS] 请输入整数（或直接回车使用默认值）");
    }
  }
}

static bool isNumberStr(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

static std::string argToName(RoomClient& c, const std::string& arg) {
  if (isNumberStr(arg)) return c.aliveNameByIndex(std::stoi(arg));
  return arg;
}

static void chatLoopAsHost(RoomHost& host, RoomClient& localClient, const std::string& myName) {
  safePrintln("[SYS] 你是房主：本窗口既是主机控制台，也作为本机玩家加入房间。");
  safePrintln("[SYS] 输入聊天；/start 开始游戏；/vote 强制投票；/menu 查看存活名单；/leave 退出并关闭房间。");

  // 房主自己也走客户端协议（连自己）
  while (localClient.isConnected()) {
    std::string s;
    if (!std::getline(std::cin, s)) break;
    if (s.empty()) continue;

    // 夜晚阶段：没有待处理行动时禁止所有输入（仅允许 /menu /help）
    if (localClient.isNightWaiting()) {
      if (s == "/menu") {
        localClient.showAliveMenu();
        continue;
      }
      if (s == "/help") {
        safePrintln("[SYS] 夜晚等待中，可用指令：/menu 查看存活名单；/help 查看帮助。");
        continue;
      }
      safePrintln("[SYS] 夜晚等待中，请等待你的行动回合...");
      continue;
    }

    // 夜晚狼人行动阶段：非数字非命令的文字作为狼人聊天
    if (localClient.isNightWolfAction() && !s.empty() && s[0] != '/' && !isNumberStr(s)) {
      localClient.sendWolfChat(s);
      continue;
    }

    if (!s.empty() && s[0] == '/') {
      if (s == "/leave") {
        localClient.sendCmd("/leave");
        host.stop();
        break;
      }
      auto parts = split(s, ' ');
      std::string cmd = parts.empty() ? "" : parts[0];
      std::string arg = (parts.size() >= 2) ? parts[1] : "";

      // 房主本地控制（注意：/vote <目标> 作为投票动作，不走这里）
      if (cmd == "/start" || cmd == "/players" || cmd == "/help" || (cmd == "/vote" && arg.empty())) {
        host.handleHostCommand(cmd);
        continue;
      }

      // 客户端动作/辅助
      if (cmd == "/menu") {
        localClient.showAliveMenu();
        continue;
      }
      if (cmd == "/pass") {
        localClient.sendAct("PASS", "");
        continue;
      }
      if (cmd == "/choose") {
        // 盗贼选牌：/choose 1 或 /choose 2
        localClient.sendAct("THIEF_CHOOSE", arg);
        continue;
      }
      if (cmd == "/idol") {
        // 野孩子选偶像：/idol <编号或昵称>
        localClient.sendAct("WILDCHILD_IDOL", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/love") {
        // 丘比特连人：/love A B（A/B 可为编号或昵称）
        std::string arg2 = (parts.size() >= 3) ? parts[2] : "";
        std::string a = argToName(localClient, arg);
        std::string b = argToName(localClient, arg2);
        localClient.sendAct("CUPID_LINK", a + "|" + b);
        continue;
      }
      if (cmd == "/save") {
        localClient.sendAct("WITCH_SAVE", arg);
        continue;
      }
      if (cmd == "/kill") {
        localClient.sendAct("WOLF_KILL", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/check") {
        localClient.sendAct("SEER_CHECK", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/guard") {
        localClient.sendAct("GUARD_PROTECT", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/poison") {
        localClient.sendAct("WITCH_POISON", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/shoot") {
        localClient.sendAct("HUNTER_SHOOT", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/vote" && !arg.empty()) {
        localClient.sendAct("VOTE", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/charm") {
        // 狼美人魅惑
        localClient.sendAct("WOLF_BEAUTY_CHARM", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/curse") {
        // 乌鸦诅咒
        localClient.sendAct("RAVEN_CURSE", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/swap") {
        // 魔术师交换：/swap A B
        std::string arg2 = (parts.size() >= 3) ? parts[2] : "";
        std::string a = argToName(localClient, arg);
        std::string b = argToName(localClient, arg2);
        localClient.sendAct("MAGICIAN_SWAP", a + "|" + b);
        continue;
      }
      if (cmd == "/dream") {
        // 摄梦人摄梦
        localClient.sendAct("DREAM_CATCH", argToName(localClient, arg));
        continue;
      }
      if (cmd == "/duel") {
        // 骑士决斗
        localClient.sendAct("KNIGHT_DUEL", argToName(localClient, arg));
        continue;
      }

      // 其他仍按 CMD 发送（如 /leave 已处理）
      localClient.sendCmd(s);
    } else {
      if (isNumberStr(s)) {
        // 盗贼：直接输入 1/2
        if (localClient.pendingAction() == "THIEF_CHOOSE") {
          localClient.sendAct("THIEF_CHOOSE", s);
          continue;
        }
        int idx = std::stoi(s);
        if (localClient.sendByIndexIfPending(idx)) continue;
      } else {
        // 丘比特：支持直接输入两个编号，例如 "2 5"
        auto toks = split(s, ' ');
        if (toks.size() == 2 && isNumberStr(toks[0]) && isNumberStr(toks[1]) && localClient.pendingAction() == "CUPID_LINK") {
          std::string a = localClient.aliveNameByIndex(std::stoi(toks[0]));
          std::string b = localClient.aliveNameByIndex(std::stoi(toks[1]));
          if (!a.empty() && !b.empty()) {
            localClient.sendAct("CUPID_LINK", a + "|" + b);
            continue;
          }
        }
      }
      localClient.sendChat(s);
    }
  }
}

static void chatLoopAsClient(RoomClient& client) {
  safePrintln("[SYS] 输入聊天；/help 查看指令；/menu 查看存活名单；/leave 退出房间。");
  while (client.isConnected()) {
    std::string s;
    if (!std::getline(std::cin, s)) break;
    if (s.empty()) continue;

    // 夜晚阶段：没有待处理行动时禁止所有输入（仅允许 /menu /help）
    if (client.isNightWaiting()) {
      if (s == "/menu") {
        client.showAliveMenu();
        continue;
      }
      if (s == "/help") {
        safePrintln("[SYS] 夜晚等待中，可用指令：/menu 查看存活名单；/help 查看帮助。");
        continue;
      }
      safePrintln("[SYS] 夜晚等待中，请等待你的行动回合...");
      continue;
    }

    // 夜晚狼人行动阶段：非数字非命令的文字作为狼人聊天
    if (client.isNightWolfAction() && !s.empty() && s[0] != '/' && !isNumberStr(s)) {
      client.sendWolfChat(s);
      continue;
    }

    if (!s.empty() && s[0] == '/') {
      if (s == "/leave") {
        client.sendCmd("/leave");
        client.disconnect();
        break;
      }
      auto parts = split(s, ' ');
      std::string cmd = parts.empty() ? "" : parts[0];
      std::string arg = (parts.size() >= 2) ? parts[1] : "";

      if (cmd == "/menu") {
        client.showAliveMenu();
        continue;
      }
      if (cmd == "/pass") {
        client.sendAct("PASS", "");
        continue;
      }
      if (cmd == "/choose") {
        client.sendAct("THIEF_CHOOSE", arg);
        continue;
      }
      if (cmd == "/idol") {
        client.sendAct("WILDCHILD_IDOL", argToName(client, arg));
        continue;
      }
      if (cmd == "/love") {
        std::string arg2 = (parts.size() >= 3) ? parts[2] : "";
        std::string a = argToName(client, arg);
        std::string b = argToName(client, arg2);
        client.sendAct("CUPID_LINK", a + "|" + b);
        continue;
      }
      if (cmd == "/save") {
        client.sendAct("WITCH_SAVE", arg);
        continue;
      }
      if (cmd == "/kill") {
        client.sendAct("WOLF_KILL", argToName(client, arg));
        continue;
      }
      if (cmd == "/check") {
        client.sendAct("SEER_CHECK", argToName(client, arg));
        continue;
      }
      if (cmd == "/guard") {
        client.sendAct("GUARD_PROTECT", argToName(client, arg));
        continue;
      }
      if (cmd == "/poison") {
        client.sendAct("WITCH_POISON", argToName(client, arg));
        continue;
      }
      if (cmd == "/shoot") {
        client.sendAct("HUNTER_SHOOT", argToName(client, arg));
        continue;
      }
      if (cmd == "/vote" && !arg.empty()) {
        client.sendAct("VOTE", argToName(client, arg));
        continue;
      }
      if (cmd == "/charm") {
        client.sendAct("WOLF_BEAUTY_CHARM", argToName(client, arg));
        continue;
      }
      if (cmd == "/curse") {
        client.sendAct("RAVEN_CURSE", argToName(client, arg));
        continue;
      }
      if (cmd == "/swap") {
        std::string arg2 = (parts.size() >= 3) ? parts[2] : "";
        std::string a = argToName(client, arg);
        std::string b = argToName(client, arg2);
        client.sendAct("MAGICIAN_SWAP", a + "|" + b);
        continue;
      }
      if (cmd == "/dream") {
        client.sendAct("DREAM_CATCH", argToName(client, arg));
        continue;
      }
      if (cmd == "/duel") {
        client.sendAct("KNIGHT_DUEL", argToName(client, arg));
        continue;
      }

      // 其它依然走 CMD
      client.sendCmd(s);
    } else {
      if (isNumberStr(s)) {
        if (client.pendingAction() == "THIEF_CHOOSE") {
          client.sendAct("THIEF_CHOOSE", s);
          continue;
        }
        int idx = std::stoi(s);
        if (client.sendByIndexIfPending(idx)) continue;
      } else {
        auto toks = split(s, ' ');
        if (toks.size() == 2 && isNumberStr(toks[0]) && isNumberStr(toks[1]) && client.pendingAction() == "CUPID_LINK") {
          std::string a = client.aliveNameByIndex(std::stoi(toks[0]));
          std::string b = client.aliveNameByIndex(std::stoi(toks[1]));
          if (!a.empty() && !b.empty()) {
            client.sendAct("CUPID_LINK", a + "|" + b);
            continue;
          }
        }
      }
      client.sendChat(s);
    }
  }
}

int main() {
  try {
    SocketInit _wsa;

    safePrintln("======== 局域网狼人杀（骨架）========");
    std::string myName = readLinePrompt("请输入你的昵称：");
    if (myName.empty()) myName = "玩家";

    while (true) {
      safePrintln("\n菜单：");
      safePrintln("  1) 创建房间（当房主）");
      safePrintln("  2) 加入房间（自动发现）");
      safePrintln("  3) 退出");
      int choice = readIntPrompt("请选择 [1-3]：", 3);

      if (choice == 3) break;

      if (choice == 1) {
        std::string roomName = readLinePrompt("房间名：");
        if (roomName.empty()) roomName = "默认房间";
        int maxPlayers = readIntPrompt("最大人数(默认 8)：", 8);
        int port = readIntPrompt("TCP端口(默认 39000；被占用会自动+1尝试)：", 39000);

        RoomHost host;
        if (!host.start(roomName, maxPlayers, (uint16_t)port)) {
          safePrintln("[ERR] 创建房间失败");
          continue;
        }

        // 本机作为玩家加入自己（127.0.0.1）
        RoomClient local;
        if (!local.connectTo("127.0.0.1", host.tcpPort(), myName)) {
          safePrintln("[ERR] 本机加入房间失败");
          host.stop();
          continue;
        }
        chatLoopAsHost(host, local, myName);
        host.stop();
        local.disconnect();
        safePrintln("[SYS] 已退出房间。");

      } else if (choice == 2) {
        safePrintln("[SYS] 正在广播发现房间（约 2 秒）...");
        auto rooms = discoverRooms(2000);
        if (rooms.empty()) {
          safePrintln("[SYS] 未发现任何房间。请确认：同一局域网、Windows 防火墙允许、对方已创建房间。");
          continue;
        }

        safePrintln("发现房间：");
        for (size_t i = 0; i < rooms.size(); i++) {
          const auto& r = rooms[i];
          std::ostringstream oss;
          oss << "  " << (i + 1) << ") " << r.roomName << "  [" << r.playerCount << "/" << r.maxPlayers
              << "]  " << r.hostIp << ":" << r.tcpPort;
          safePrintln(oss.str());
        }
        int idx = readIntPrompt("选择房间编号：", 1);
        if (idx < 1 || idx > (int)rooms.size()) {
          safePrintln("[SYS] 选择无效");
          continue;
        }
        RoomClient client;
        const auto& r = rooms[idx - 1];
        safePrintln("[SYS] 连接到 " + r.hostIp + ":" + std::to_string(r.tcpPort) + " ...");
        if (!client.connectTo(r.hostIp, r.tcpPort, myName)) {
          safePrintln("[ERR] 连接失败");
          continue;
        }
        chatLoopAsClient(client);
        client.disconnect();
        safePrintln("[SYS] 已退出房间。");
      }
    }

    safePrintln("Bye.");
    return 0;
  } catch (const std::exception& e) {
    safePrintln(std::string("[FATAL] ") + e.what());
    return 1;
  }
}
