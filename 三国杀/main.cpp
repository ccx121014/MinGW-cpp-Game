/*
 * 三国杀 - 局域网联机版 v3.0
 * 使用文本行协议 + UDP发现 + TCP通信
 * 支持 Windows/Linux 跨平台编译
 *
 * 编译命令(Windows): g++ -std=c++11 -O2 -o sanguosha.exe main.cpp -lws2_32 -static
 * 编译命令(Linux):   g++ -std=c++11 -O2 -o sanguosha main.cpp
 *
 * v3.0 改动:
 *   - 二进制协议改为文本行协议 (COMMAND|param1|param2|...)
 *   - UDP广播发现房间 (替代网段扫描)
 *   - 多线程 accept/clientLoop (替代非阻塞轮询)
 *   - RAII SocketInit (替代手动 WSACleanup)
 *   - 保留所有游戏特性: 25武将、12装备、技能系统、单机AI、彩色UI
 */

// ===================== 系统头文件 =====================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ===================== 跨平台 Socket 定义 =====================
#ifdef _WIN32
# ifndef _WIN32_WINNT
#   define _WIN32_WINNT 0x0600
# endif
# ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h>
# include <windows.h>
# include <ws2tcpip.h>
# include <conio.h>
  using socket_t = SOCKET;
  static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
# include <arpa/inet.h>
# include <netdb.h>
# include <netinet/in.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <unistd.h>
# include <fcntl.h>
  using socket_t = int;
  static constexpr socket_t kInvalidSocket = -1;
#endif

// ===================== 常量 =====================
static constexpr uint16_t kDiscoveryPort = 37021; // UDP发现端口
static constexpr uint16_t kGamePort = 9528;        // TCP游戏端口
static constexpr int kMaxPlayers = 8;
static constexpr int kMaxLine = 4096;
static constexpr int kMaxNameLen = 32;
static constexpr int kMaxRoomName = 32;
static constexpr int kMaxMsgLen = 512;
static constexpr int kMaxHandSize = 30;
static constexpr int kTurnTimeout = 60;
static constexpr int kRespondTimeout = 15;

// ===================== 全局打印锁 =====================
static std::mutex g_printMutex;

// ===================== 工具函数 =====================
static void msleep(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void closeSocket(socket_t s) {
    if (s != kInvalidSocket) {
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
    }
}

static bool setReuseAddr(socket_t s) {
    int opt = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == 0;
}

static bool setBroadcast(socket_t s) {
    int opt = 1;
    return setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&opt, sizeof(opt)) == 0;
}

static std::string sockaddrToIp(const sockaddr_in& addr) {
    const char* p = inet_ntoa(addr.sin_addr);
    return p ? std::string(p) : std::string("0.0.0.0");
}

static std::string trimCRLF(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string item;
    while (std::getline(iss, item, delim))
        parts.push_back(item);
    return parts;
}

// 逐字符 recv 直到遇到 \n
static bool recvLine(socket_t s, std::string& out_line) {
    out_line.clear();
    char ch;
    while (true) {
        int n = recv(s, &ch, 1, 0);
        if (n <= 0) return false;
        if (ch == '\n') return true;
        if (ch != '\r') out_line += ch;
    }
}

// 确保全部发送
static bool sendAll(socket_t s, const std::string& data) {
    const char* ptr = data.c_str();
    int remaining = (int)data.size();
    while (remaining > 0) {
        int sent = send(s, ptr, remaining, 0);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

// 发送一行 (加 \n)
static bool sendLine(socket_t s, const std::string& line) {
    return sendAll(s, line + "\n");
}

// 获取本地IP
static std::string getLocalIP() {
    socket_t tmpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (tmpSock == kInvalidSocket) return "127.0.0.1";
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    std::string result = "127.0.0.1";
    if (connect(tmpSock, (struct sockaddr*)&serv, sizeof(serv)) == 0) {
        struct sockaddr_in local;
        socklen_t len = sizeof(local);
        if (getsockname(tmpSock, (struct sockaddr*)&local, &len) == 0) {
            const char* p = inet_ntoa(local.sin_addr);
            if (p) result = p;
        }
    }
    closeSocket(tmpSock);
    if (result == "127.0.0.1") {
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        struct hostent* he = gethostbyname(hostname);
        if (he && he->h_addr_list[0]) {
            const char* p = inet_ntoa(*(struct in_addr*)he->h_addr_list[0]);
            if (p) result = p;
        }
    }
    return result;
}

// ===================== SocketInit (RAII) =====================
struct SocketInit {
    SocketInit() {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    }
    ~SocketInit() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

// ===================== Windows ANSI 颜色 =====================
#ifdef _WIN32
static HANDLE hConsole = NULL;
static WORD defaultAttr = 0;

void initColor() {
    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hConsole, &info);
    defaultAttr = info.wAttributes;
}

enum ConsoleColor {
    C_RED = FOREGROUND_RED | FOREGROUND_INTENSITY,
    C_GREEN = FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    C_BLUE = FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_YELLOW = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    C_CYAN = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_MAGENTA = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_WHITE = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_GRAY = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
    C_DARK_RED = FOREGROUND_RED,
    C_DARK_GREEN = FOREGROUND_GREEN,
    C_DARK_YELLOW = FOREGROUND_RED | FOREGROUND_GREEN,
};

void setColor(WORD c) {
    if (hConsole) SetConsoleTextAttribute(hConsole, c);
}

void resetColor() {
    if (hConsole) SetConsoleTextAttribute(hConsole, defaultAttr);
}

#define COL_RED(f)       do { setColor(C_RED); f; resetColor(); } while(0)
#define COL_GREEN(f)     do { setColor(C_GREEN); f; resetColor(); } while(0)
#define COL_BLUE(f)      do { setColor(C_BLUE); f; resetColor(); } while(0)
#define COL_YELLOW(f)    do { setColor(C_YELLOW); f; resetColor(); } while(0)
#define COL_CYAN(f)      do { setColor(C_CYAN); f; resetColor(); } while(0)
#define COL_MAGENTA(f)   do { setColor(C_MAGENTA); f; resetColor(); } while(0)
#define COL_WHITE(f)     do { setColor(C_WHITE); f; resetColor(); } while(0)
#define COL_GRAY(f)      do { setColor(C_GRAY); f; resetColor(); } while(0)
#else
#define initColor()       do {} while(0)
#define COL_RED(f)        f
#define COL_GREEN(f)      f
#define COL_BLUE(f)       f
#define COL_YELLOW(f)     f
#define COL_CYAN(f)       f
#define COL_MAGENTA(f)    f
#define COL_WHITE(f)      f
#define COL_GRAY(f)       f
#endif

// ===================== 卡牌类型 =====================
enum CardType {
    CARD_SHA = 0, CARD_SHAN, CARD_TAO,
    CARD_JUEDOU, CARD_NANMAN, CARD_WANJIAN,
    CARD_WUZHONG, CARD_JUEDAO, CARD_SHUNQIAN,
    CARD_GUOHE, CARD_TAOYUAN, CARD_WUGU,
    CARD_HUOGONG, CARD_TIEJI, CARD_LEBUSI,
    // 装备
    CARD_EQ_ZHUGE,    // 诸葛连弩
    CARD_EQ_QINGGANG,  // 青釭剑
    CARD_EQ_QINGLONG,  // 青龙偃月刀
    CARD_EQ_GUANSHI,   // 贯石斧
    CARD_EQ_FANGTIAN,  // 方天画戟
    CARD_EQ_QIXING,    // 七星宝刀
    CARD_EQ_BAGUA,     // 八卦阵
    CARD_EQ_RENJIE,    // 仁王盾
    CARD_EQ_JUEYING,   // 绝影
    CARD_EQ_DILU,      // 的卢
    CARD_EQ_CHITU,     // 赤兔
    CARD_EQ_DAWAN,     // 大宛
    CARD_MAX_TYPE
};

// ===================== 卡牌分类 =====================
inline bool isEquipCard(int type) { return type >= CARD_EQ_ZHUGE && type < CARD_MAX_TYPE; }
inline bool isWeaponCard(int type) { return type >= CARD_EQ_ZHUGE && type <= CARD_EQ_QIXING; }
inline bool isArmorCard(int type) { return type >= CARD_EQ_BAGUA && type <= CARD_EQ_RENJIE; }
inline bool isHorseCard(int type) { return type >= CARD_EQ_JUEYING && type <= CARD_EQ_DAWAN; }
inline bool isDefHorse(int type) { return type == CARD_EQ_JUEYING || type == CARD_EQ_DILU; }
inline bool isAtkHorse(int type) { return type == CARD_EQ_CHITU || type == CARD_EQ_DAWAN; }

// ===================== 花色 =====================
enum CardSuit { SUIT_SPADE = 0, SUIT_HEART, SUIT_CLUB, SUIT_DIAMOND };
inline bool isRedSuit(int s) { return s == SUIT_HEART || s == SUIT_DIAMOND; }

// ===================== 武将类型(25个) =====================
enum HeroType {
    HERO_LIUBEI = 0, HERO_GUANYU, HERO_ZHANGFEI, HERO_ZHAOYUN,
    HERO_ZHUGELIANG, HERO_MACHAO, HERO_HUANGZHONG,
    HERO_CAOCAO, HERO_SIMAYI, HERO_XIAHOU_DUN,
    HERO_ZHANGLIAO, HERO_XUCHU, HERO_GUOJIA,
    HERO_LVBU, HERO_DIAOCHAN,
    HERO_SUNQUAN, HERO_ZHOUYU, HERO_DAQIAO, HERO_XIAOQIAO,
    HERO_GANNING, HERO_HUANGGAI, HERO_WEIYAN, HERO_JIANGWEI,
    HERO_ZHENJI, HERO_HUATUO,
    HERO_MAX
};

// ===================== 身份 =====================
enum Identity { IDENTITY_LORD = 0, IDENTITY_LOYAL, IDENTITY_REBEL, IDENTITY_SPY, IDENTITY_UNKNOWN };

// ===================== 游戏阶段 =====================
enum Phase {
    PHASE_NOT_STARTED = 0, PHASE_JUDGE, PHASE_DRAW,
    PHASE_PLAY, PHASE_DISCARD, PHASE_RESPOND, PHASE_GAME_OVER
};

// ===================== 装备槽 =====================
enum EquipSlot { EQ_WEAPON = 0, EQ_ARMOR, EQ_DEF_HORSE, EQ_ATK_HORSE, EQ_MAX };

// ===================== 武将数据 =====================
struct HeroData {
    HeroType type;
    const char* name;
    const char* kingdom; // 蜀/魏/吴/群
    const char* skill;
    const char* skillDesc;
    int maxHp;
};

const HeroData HERO_TABLE[HERO_MAX] = {
    {HERO_LIUBEI,      "刘备",   "蜀", "仁德", "出牌阶段，你可以将任意数量的手牌交给其他角色(每回合给出两张以上时回复1点体力)", 4},
    {HERO_GUANYU,      "关羽",   "蜀", "武圣", "你可以将一张红色手牌当【杀】使用或打出", 4},
    {HERO_ZHANGFEI,    "张飞",   "蜀", "咆哮", "出牌阶段，你使用【杀】无次数限制", 4},
    {HERO_ZHAOYUN,     "赵云",   "蜀", "龙胆", "你可以将【杀】当【闪】、【闪】当【杀】使用", 4},
    {HERO_ZHUGELIANG,  "诸葛亮", "蜀", "观星", "准备阶段，你可以观看牌堆顶的X张牌并调整顺序", 3},
    {HERO_MACHAO,      "马超",   "蜀", "铁骑", "你使用【杀】指定目标后，可进行判定，若为红色则不可出闪", 4},
    {HERO_HUANGZHONG,  "黄忠",   "蜀", "烈弓", "你的【杀】无视距离限制，目标手牌数不大于你时可强制命中", 4},
    {HERO_CAOCAO,      "曹操",   "魏", "奸雄", "受到伤害后，你可以获得造成伤害的牌", 4},
    {HERO_SIMAYI,      "司马懿", "魏", "反馈", "受到伤害后，你可以获得伤害来源的一张牌(手牌或装备区)", 3},
    {HERO_XIAHOU_DUN,  "夏侯惇", "魏", "刚烈", "受到伤害后，可令伤害来源进行判定，若不为红桃则受1点伤害", 4},
    {HERO_ZHANGLIAO,   "张辽",   "魏", "突袭", "摸牌阶段，你可以少摸一张牌，获得一名其他角色的一张手牌", 4},
    {HERO_XUCHU,       "许褚",   "魏", "裸衣", "摸牌阶段可少摸一张，本回合【杀】或【决斗】伤害+1", 4},
    {HERO_GUOJIA,      "郭嘉",   "魏", "遗计", "受到1点伤害后，你可以摸两张牌并将一张手牌交给任意角色", 3},
    {HERO_LVBU,        "吕布",   "群", "无双", "你使用【杀】需两张【闪】抵消；【决斗】对方需出两张【杀】", 4},
    {HERO_DIAOCHAN,    "貂蝉",   "群", "离间", "出牌阶段限一次，令两名男性角色进行【决斗】", 3},
    {HERO_SUNQUAN,     "孙权",   "吴", "制衡", "出牌阶段限一次，你可以弃置任意数量的手牌并摸等量的牌", 4},
    {HERO_ZHOUYU,      "周瑜",   "吴", "英姿", "摸牌阶段多摸一张牌；【反间】令一名角色猜花色", 3},
    {HERO_DAQIAO,      "大乔",   "吴", "国色", "出牌阶段，你可以将一张方块牌当【乐不思蜀】使用", 3},
    {HERO_XIAOQIAO,    "小乔",   "吴", "天香", "当你受到伤害时，你可以弃一张红桃手牌将伤害转移给其他角色", 3},
    {HERO_GANNING,     "甘宁",   "吴", "奇袭", "你可以将一张黑色手牌当【过河拆桥】使用", 4},
    {HERO_HUANGGAI,    "黄盖",   "吴", "苦肉", "出牌阶段限一次，你可以弃一张手牌失去1点体力并摸两张牌", 4},
    {HERO_WEIYAN,      "魏延",   "蜀", "狂骨", "你造成伤害后，若伤害来源体力比你低，你回复1点体力", 4},
    {HERO_JIANGWEI,    "姜维",   "蜀", "挑衅", "出牌阶段限一次，你可以令一名角色选择对你使用【杀】或让你摸一张牌", 3},
    {HERO_ZHENJI,      "甄姬",   "魏", "洛神", "准备阶段，你可以进行判定，若为黑色则获得此判定牌并可继续", 3},
    {HERO_HUATUO,      "华佗",   "群", "急救", "你的回合外，你可以将红色手牌当【桃】使用", 3},
};

// ===================== 装备效果 =====================
struct EquipEffect {
    int type;
    const char* name;
    int range;       // 武器攻击距离(-1表示特殊)
    const char* desc;
};

const EquipEffect EQUIP_TABLE[] = {
    {CARD_EQ_ZHUGE,    "诸葛连弩", 1, "你可以无限次使用【杀】"},
    {CARD_EQ_QINGGANG, "青釭剑",   2, "你的【杀】无视防具"},
    {CARD_EQ_QINGLONG, "青龙偃月刀", 3, "你使用的【杀】被闪抵消后可对另一目标使用"},
    {CARD_EQ_GUANSHI,  "贯石斧",   3, "你的【杀】被闪抵消时，可弃两张牌强制命中"},
    {CARD_EQ_FANGTIAN, "方天画戟", 4, "你使用的【杀】若为最后一张手牌可指定最多3个目标"},
    {CARD_EQ_QIXING,   "七星宝刀", 2, "你使用的【杀】无视距离"},
    {CARD_EQ_BAGUA,    "八卦阵",  -1, "当你需要出【闪】时，可判定，若为红色则视为出闪"},
    {CARD_EQ_RENJIE,   "仁王盾",  -1, "黑色【杀】对你无效"},
    {CARD_EQ_JUEYING,  "绝影",    -1, "你不能被【顺手牵羊】和【过河拆桥】"},
    {CARD_EQ_DILU,     "的卢",    -1, "其他角色与你的距离+1"},
    {CARD_EQ_CHITU,    "赤兔",    -1, "你与其他角色的距离-1"},
    {CARD_EQ_DAWAN,    "大宛",    -1, "你与其他角色的距离-1"},
};

inline const char* getEquipName(int type) {
    for (int i = 0; i < 12; i++) {
        if (EQUIP_TABLE[i].type == type) return EQUIP_TABLE[i].name;
    }
    return "未知装备";
}

inline int getWeaponRange(int equipType) {
    if (!isWeaponCard(equipType)) return 1;
    for (int i = 0; i < 12; i++) {
        if (EQUIP_TABLE[i].type == equipType) return EQUIP_TABLE[i].range;
    }
    return 1;
}

// ===================== 辅助函数 =====================
inline const char* getCardName(int type) {
    switch(type) {
        case CARD_SHA:       return "杀";
        case CARD_SHAN:      return "闪";
        case CARD_TAO:       return "桃";
        case CARD_JUEDOU:    return "决斗";
        case CARD_NANMAN:    return "南蛮入侵";
        case CARD_WANJIAN:   return "万箭齐发";
        case CARD_WUZHONG:   return "无中生有";
        case CARD_JUEDAO:     return "借刀杀人";
        case CARD_SHUNQIAN:   return "顺手牵羊";
        case CARD_GUOHE:     return "过河拆桥";
        case CARD_TAOYUAN:   return "桃园结义";
        case CARD_WUGU:      return "五谷丰登";
        case CARD_HUOGONG:   return "火攻";
        case CARD_TIEJI:     return "铁索连环";
        case CARD_LEBUSI:    return "乐不思蜀";
        default:             return getEquipName(type);
    }
}

inline const char* getSuitSymbol(int suit) {
    switch(suit) {
        case SUIT_SPADE:    return "♠";
        case SUIT_HEART:    return "♥";
        case SUIT_CLUB:     return "♣";
        case SUIT_DIAMOND:  return "♦";
        default:            return "?";
    }
}

inline const char* getNumberStr(int n) {
    static const char* nums[] = {"","A","2","3","4","5","6","7","8","9","10","J","Q","K"};
    if (n >= 1 && n <= 13) return nums[n];
    return "?";
}

inline const char* getHeroName(int hero) {
    if (hero >= 0 && hero < HERO_MAX) return HERO_TABLE[hero].name;
    return "未知";
}

inline const char* getHeroKingdom(int hero) {
    if (hero >= 0 && hero < HERO_MAX) return HERO_TABLE[hero].kingdom;
    return "?";
}

inline const char* getIdentityName(int identity) {
    switch(identity) {
        case IDENTITY_LORD:    return "主公";
        case IDENTITY_LOYAL:   return "忠臣";
        case IDENTITY_REBEL:   return "反贼";
        case IDENTITY_SPY:     return "内奸";
        default:               return "未知";
    }
}

inline const char* getPhaseName(int phase) {
    switch(phase) {
        case PHASE_JUDGE:    return "判定阶段";
        case PHASE_DRAW:     return "摸牌阶段";
        case PHASE_PLAY:     return "出牌阶段";
        case PHASE_DISCARD:  return "弃牌阶段";
        case PHASE_RESPOND:  return "响应阶段";
        case PHASE_GAME_OVER:return "游戏结束";
        default:             return "准备阶段";
    }
}

// ===================== 数据结构 =====================
struct CardInfo {
    int type;
    int suit;
    int number;
    int cardId;
};

// ===================== 牌堆管理 =====================
class CardDeck {
public:
    CardInfo deck[300];
    CardInfo discardPile[300];
    int deckTop, discardTop, nextCardId;

    CardDeck() { deckTop = 0; discardTop = 0; nextCardId = 1; }

    void addCard(int type, int suit, int number) {
        CardInfo c; c.type = type; c.suit = suit; c.number = number; c.cardId = nextCardId++;
        deck[deckTop++] = c;
    }

    void shuffle() {
        for (int i = deckTop - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            CardInfo tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
        }
    }

    void initStandardDeck() {
        deckTop = 0; discardTop = 0; nextCardId = 1;
        // 杀 - 30张
        for (int i = 0; i < 8; i++) addCard(CARD_SHA, SUIT_SPADE, (i%13)+1);
        for (int i = 0; i < 7; i++) addCard(CARD_SHA, SUIT_CLUB, (i%13)+1);
        for (int i = 0; i < 8; i++) addCard(CARD_SHA, SUIT_HEART, (i%13)+1);
        for (int i = 0; i < 7; i++) addCard(CARD_SHA, SUIT_DIAMOND, (i%13)+1);
        // 闪 - 15张
        for (int i = 0; i < 5; i++) addCard(CARD_SHAN, SUIT_HEART, (i%13)+1);
        for (int i = 0; i < 5; i++) addCard(CARD_SHAN, SUIT_DIAMOND, (i%13)+1);
        for (int i = 0; i < 5; i++) addCard(CARD_SHAN, SUIT_CLUB, (i%13)+1);
        // 桃 - 8张
        for (int i = 0; i < 4; i++) addCard(CARD_TAO, SUIT_HEART, (i%13)+1);
        for (int i = 0; i < 4; i++) addCard(CARD_TAO, SUIT_DIAMOND, (i%13)+1);
        // 锦囊牌
        addCard(CARD_JUEDOU, SUIT_SPADE, 1); addCard(CARD_JUEDOU, SUIT_CLUB, 1); addCard(CARD_JUEDOU, SUIT_DIAMOND, 1);
        addCard(CARD_NANMAN, SUIT_SPADE, 7); addCard(CARD_NANMAN, SUIT_SPADE, 13); addCard(CARD_NANMAN, SUIT_CLUB, 7);
        addCard(CARD_WANJIAN, SUIT_HEART, 1);
        addCard(CARD_WUZHONG, SUIT_HEART, 7); addCard(CARD_WUZHONG, SUIT_HEART, 8); addCard(CARD_WUZHONG, SUIT_HEART, 9); addCard(CARD_WUZHONG, SUIT_HEART, 11);
        addCard(CARD_GUOHE, SUIT_SPADE, 3); addCard(CARD_GUOHE, SUIT_SPADE, 4); addCard(CARD_GUOHE, SUIT_CLUB, 3); addCard(CARD_GUOHE, SUIT_CLUB, 4);
        addCard(CARD_SHUNQIAN, SUIT_SPADE, 3); addCard(CARD_SHUNQIAN, SUIT_SPADE, 4); addCard(CARD_SHUNQIAN, SUIT_DIAMOND, 3); addCard(CARD_SHUNQIAN, SUIT_DIAMOND, 4);
        addCard(CARD_TAOYUAN, SUIT_HEART, 1);
        addCard(CARD_WUGU, SUIT_HEART, 3); addCard(CARD_WUGU, SUIT_HEART, 4);
        addCard(CARD_HUOGONG, SUIT_HEART, 2); addCard(CARD_HUOGONG, SUIT_DIAMOND, 12);
        addCard(CARD_TIEJI, SUIT_SPADE, 11); addCard(CARD_TIEJI, SUIT_SPADE, 12);
        addCard(CARD_LEBUSI, SUIT_SPADE, 6); addCard(CARD_LEBUSI, SUIT_CLUB, 6); addCard(CARD_LEBUSI, SUIT_HEART, 6);
        // 装备牌
        addCard(CARD_EQ_ZHUGE, SUIT_CLUB, 1); addCard(CARD_EQ_ZHUGE, SUIT_DIAMOND, 1);
        addCard(CARD_EQ_QINGGANG, SUIT_SPADE, 6); addCard(CARD_EQ_QINGGANG, SUIT_SPADE, 12);
        addCard(CARD_EQ_QINGLONG, SUIT_SPADE, 5);
        addCard(CARD_EQ_GUANSHI, SUIT_SPADE, 5); addCard(CARD_EQ_GUANSHI, SUIT_CLUB, 5);
        addCard(CARD_EQ_FANGTIAN, SUIT_DIAMOND, 12);
        addCard(CARD_EQ_QIXING, SUIT_SPADE, 1);
        addCard(CARD_EQ_BAGUA, SUIT_SPADE, 2); addCard(CARD_EQ_BAGUA, SUIT_DIAMOND, 2);
        addCard(CARD_EQ_RENJIE, SUIT_CLUB, 2);
        addCard(CARD_EQ_JUEYING, SUIT_SPADE, 13);
        addCard(CARD_EQ_DILU, SUIT_CLUB, 5);
        addCard(CARD_EQ_CHITU, SUIT_HEART, 5); addCard(CARD_EQ_CHITU, SUIT_DIAMOND, 13);
        addCard(CARD_EQ_DAWAN, SUIT_SPADE, 13);
        shuffle();
    }

    CardInfo drawCard() {
        if (deckTop <= 0) {
            if (discardTop <= 0) { CardInfo e; memset(&e,0,sizeof(e)); e.cardId=-1; return e; }
            for (int i = 0; i < discardTop; i++) deck[i] = discardPile[i];
            deckTop = discardTop; discardTop = 0; shuffle();
        }
        return deck[--deckTop];
    }

    CardInfo peekCard() {
        if (deckTop <= 0) { CardInfo e; memset(&e,0,sizeof(e)); e.cardId=-1; return e; }
        return deck[deckTop - 1];
    }

    void discardCard(const CardInfo& card) {
        if (discardTop < 300) discardPile[discardTop++] = card;
    }

    int remaining() const { return deckTop; }
};

// 前向声明全局变量(在GameLogic中使用)
static CardInfo g_lastDamageCard;
static int g_lastDamageSource = -1;

// ===================== 游戏玩家 =====================
struct GamePlayer {
    char name[kMaxNameLen];
    int hero; int identity;
    int hp; int maxHp;
    CardInfo hand[kMaxHandSize];
    int handCount;
    CardInfo equips[EQ_MAX]; // 装备区
    bool alive; bool isLord;
    int seatId;
    int shaUsed; int wuzhongUsed; int zhihengUsed;
    int kurouUsed; int tiaoxinUsed; int lijianUsed;
    bool lebusiTarget; // 被乐不思蜀标记
    bool skipPlay;     // 跳过出牌阶段(乐不思蜀判定非红桃)
    int tiejiLinked;   // 铁索连环标记
    int rendUsed;      // 刘备仁德: 本回合已给出牌数
    bool luoyiActive;  // 许褚裸衣: 本回合生效
    int guanxingUsed;  // 诸葛亮观星: 是否已使用
};

// ===================== 游戏逻辑 =====================
class GameLogic {
public:
    GamePlayer players[kMaxPlayers];
    int playerCount;
    int currentSeat;
    Phase currentPhase;
    CardDeck deck;
    int lordSeat;
    bool gameStarted;
    bool gameOver;
    int winnerFaction;
    bool isSinglePlayer;
    int aiPlayerCount;
    
    // 手牌同步队列: 记录需要发送DRAW消息的牌和目标玩家
    struct SyncEntry { int seat; CardInfo card; };
    static const int kMaxSync = 32;
    SyncEntry syncQueue[kMaxSync];
    int syncCount;
    
    void addSync(int seat, const CardInfo& card) {
        if (syncCount < kMaxSync) {
            syncQueue[syncCount].seat = seat;
            syncQueue[syncCount].card = card;
            syncCount++;
        }
    }
    void clearSync() { syncCount = 0; }

    GameLogic() {
        playerCount = 0; currentSeat = 0; currentPhase = PHASE_NOT_STARTED;
        gameStarted = false; gameOver = false; winnerFaction = -1;
        isSinglePlayer = false; aiPlayerCount = 0;
        syncCount = 0;
        memset(players, 0, sizeof(players));
    }

    void initGame(const char names[kMaxPlayers][kMaxNameLen], int count) {
        playerCount = count; gameStarted = false; gameOver = false; winnerFaction = -1;
        memset(players, 0, sizeof(players));
        for (int i = 0; i < count; i++) {
            strncpy(players[i].name, names[i], kMaxNameLen - 1);
            players[i].name[kMaxNameLen - 1] = '\0';
            players[i].seatId = i; players[i].alive = true;
            players[i].handCount = 0; players[i].shaUsed = 0;
            players[i].wuzhongUsed = 0; players[i].zhihengUsed = 0;
            players[i].kurouUsed = 0; players[i].tiaoxinUsed = 0;
            players[i].lijianUsed = 0; players[i].lebusiTarget = false;
            players[i].skipPlay = false;
            players[i].tiejiLinked = 0;
            players[i].rendUsed = 0; players[i].luoyiActive = false;
            players[i].guanxingUsed = 0;
            memset(players[i].equips, 0, sizeof(players[i].equips));
        }
        assignIdentities(); assignHeroes();
        deck.initStandardDeck();
        players[lordSeat].maxHp++; players[lordSeat].hp = players[lordSeat].maxHp;
        gameStarted = true;
    }

    void assignIdentities() {
        int identities[kMaxPlayers]; int idx = 0;
        identities[idx++] = IDENTITY_LORD;
        if (playerCount >= 3) identities[idx++] = IDENTITY_LOYAL;
        if (playerCount >= 7) identities[idx++] = IDENTITY_LOYAL;
        if (playerCount >= 2) identities[idx++] = IDENTITY_REBEL;
        if (playerCount >= 5) identities[idx++] = IDENTITY_REBEL;
        if (playerCount >= 8) identities[idx++] = IDENTITY_REBEL;
        if (playerCount >= 4) identities[idx++] = IDENTITY_SPY;
        // 打乱所有身份(包括主公位置)
        for (int i = idx - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = identities[i]; identities[i] = identities[j]; identities[j] = tmp;
        }
        lordSeat = -1;
        for (int i = 0; i < playerCount; i++) {
            players[i].identity = identities[i];
            players[i].isLord = (identities[i] == IDENTITY_LORD);
            if (identities[i] == IDENTITY_LORD) lordSeat = i;
        }
    }

    void assignHeroes() {
        bool used[HERO_MAX]; memset(used, 0, sizeof(used));
        for (int i = 0; i < playerCount; i++) {
            int hero;
            do { hero = rand() % HERO_MAX; } while (used[hero]);
            used[hero] = true;
            players[i].hero = hero;
            players[i].maxHp = HERO_TABLE[hero].maxHp;
            players[i].hp = players[i].maxHp;
        }
    }

    // 重新随机分配武将(用于手动触发)
    void reshuffleHeroes() {
        assignHeroes();
    }

    // 计算攻击距离
    int getAttackRange(int seat) {
        int range = 1;
        if (players[seat].equips[EQ_WEAPON].cardId > 0) {
            range = getWeaponRange(players[seat].equips[EQ_WEAPON].type);
        }
        return range;
    }

    // 计算两座位间距离
    int getDistance(int from, int to) {
        if (from == to) return 0;
        int dist = 0; int cur = from;
        while (cur != to) { cur = (cur + 1) % playerCount; if (players[cur].alive) dist++; }
        int dist2 = 0; cur = from;
        while (cur != to) { cur = (cur - 1 + playerCount) % playerCount; if (players[cur].alive) dist2++; }
        if (dist2 < dist) dist = dist2;
        // 防御马 +1
        if (players[to].equips[EQ_DEF_HORSE].cardId > 0) dist++;
        // 攻击马 -1
        if (players[from].equips[EQ_ATK_HORSE].cardId > 0) dist--;
        return dist > 0 ? dist : 1;
    }

    bool canUseSha(int seat) {
        if (players[seat].hero == HERO_ZHANGFEI) return true;
        if (players[seat].equips[EQ_WEAPON].type == CARD_EQ_ZHUGE) return true;
        return players[seat].shaUsed < 1;
    }

    bool hasCardType(int seat, int cardType) {
        for (int i = 0; i < players[seat].handCount; i++)
            if (players[seat].hand[i].type == cardType) return true;
        return false;
    }

    bool hasRedCard(int seat) {
        for (int i = 0; i < players[seat].handCount; i++)
            if (isRedSuit(players[seat].hand[i].suit)) return true;
        return false;
    }

    bool hasBlackCard(int seat) {
        for (int i = 0; i < players[seat].handCount; i++)
            if (!isRedSuit(players[seat].hand[i].suit)) return true;
        return false;
    }

    bool removeCard(int seat, int cardId) {
        for (int i = 0; i < players[seat].handCount; i++) {
            if (players[seat].hand[i].cardId == cardId) {
                for (int j = i; j < players[seat].handCount - 1; j++)
                    players[seat].hand[j] = players[seat].hand[j + 1];
                players[seat].handCount--;
                return true;
            }
        }
        return false;
    }

    void addCardToHand(int seat, const CardInfo& card) {
        if (players[seat].handCount < kMaxHandSize)
            players[seat].hand[players[seat].handCount++] = card;
    }

    // 装备卡牌
    void equipCard(int seat, const CardInfo& card) {
        EquipSlot slot;
        if (isWeaponCard(card.type)) slot = EQ_WEAPON;
        else if (isArmorCard(card.type)) slot = EQ_ARMOR;
        else if (isDefHorse(card.type)) slot = EQ_DEF_HORSE;
        else slot = EQ_ATK_HORSE;
        // 旧装备弃掉
        if (players[seat].equips[slot].cardId > 0) {
            deck.discardCard(players[seat].equips[slot]);
        }
        players[seat].equips[slot] = card;
    }

    bool canBeStolen(int seat) {
        // 绝影免疫
        return !(players[seat].equips[EQ_DEF_HORSE].type == CARD_EQ_JUEYING);
    }

    // 杀命中判定(考虑仁王盾、八卦阵)
    bool canShaHit(int targetSeat, int shaSuit) {
        if (!players[targetSeat].alive) return false;
        // 仁王盾: 黑色杀无效
        if (players[targetSeat].equips[EQ_ARMOR].type == CARD_EQ_RENJIE && !isRedSuit(shaSuit))
            return false;
        return true;
    }

    // 八卦阵判定
    bool baguaJudge(int seat) {
        if (players[seat].equips[EQ_ARMOR].type == CARD_EQ_BAGUA) {
            CardInfo judgeCard = deck.drawCard();
            if (judgeCard.cardId >= 0) {
                deck.discardCard(judgeCard);
                return isRedSuit(judgeCard.suit); // 红色=出闪成功
            }
        }
        return false;
    }

    // 造成伤害(带递归深度保护，防止刚烈/天香无限递归)
    void dealDamage(int targetSeat, int damage, int sourceSeat, int depth = 0) {
        if (!players[targetSeat].alive) return;
        if (depth > 4) return; // 递归深度保护，防止刚烈+天香无限循环

        // 小乔-天香: 受到伤害时，可弃一张红桃手牌将伤害转移给其他角色
        if (players[targetSeat].hero == HERO_XIAOQIAO && sourceSeat >= 0 && damage > 0 && depth < 3) {
            // AI自动判断: 有红桃手牌且有其他存活角色时转移
            bool hasHeart = false; int heartIdx = -1;
            for (int i = 0; i < players[targetSeat].handCount; i++) {
                if (players[targetSeat].hand[i].suit == SUIT_HEART) { hasHeart = true; heartIdx = i; break; }
            }
            // 找一个转移目标(优先敌方)
            int transferTarget = -1;
            if (hasHeart) {
                for (int i = 0; i < playerCount; i++) {
                    if (i != targetSeat && i != sourceSeat && players[i].alive) {
                        if (players[i].identity != players[targetSeat].identity) { transferTarget = i; break; }
                    }
                }
                if (transferTarget < 0) {
                    for (int i = 0; i < playerCount; i++) {
                        if (i != targetSeat && players[i].alive) { transferTarget = i; break; }
                    }
                }
            }
            if (hasHeart && transferTarget >= 0) {
                // 弃红桃手牌
                CardInfo heartCard = players[targetSeat].hand[heartIdx];
                removeCard(targetSeat, heartCard.cardId);
                deck.discardCard(heartCard);
                // 转移伤害
                dealDamage(transferTarget, damage, targetSeat, depth + 1);
                return;
            }
        }

        players[targetSeat].hp -= damage;

        // 曹操-奸雄: 受到伤害后，获得造成伤害的牌
        if (players[targetSeat].alive && players[targetSeat].hero == HERO_CAOCAO
            && sourceSeat >= 0 && g_lastDamageCard.cardId > 0) {
            addCardToHand(targetSeat, g_lastDamageCard);
            addSync(targetSeat, g_lastDamageCard);
            g_lastDamageCard.cardId = 0; // 避免重复获取
        }
        // 司马懿-反馈: 受到伤害后，获得伤害来源的一张牌(手牌或装备区)
        if (players[targetSeat].alive && players[targetSeat].hero == HERO_SIMAYI && sourceSeat >= 0 && players[sourceSeat].alive) {
            // 优先偷装备，其次手牌
            bool stole = false;
            for (int e = 0; e < EQ_MAX; e++) {
                if (players[sourceSeat].equips[e].cardId > 0) {
                    addCardToHand(targetSeat, players[sourceSeat].equips[e]);
                    addSync(targetSeat, players[sourceSeat].equips[e]);
                    players[sourceSeat].equips[e].cardId = 0;
                    stole = true; break;
                }
            }
            if (!stole && players[sourceSeat].handCount > 0) {
                int idx = rand() % players[sourceSeat].handCount;
                addCardToHand(targetSeat, players[sourceSeat].hand[idx]);
                addSync(targetSeat, players[sourceSeat].hand[idx]);
                removeCard(sourceSeat, players[sourceSeat].hand[idx].cardId);
            }
        }
        // 夏侯惇-刚烈: 受到伤害后，可令伤害来源进行判定，若不为红桃则受1点伤害
        if (players[targetSeat].alive && players[targetSeat].hero == HERO_XIAHOU_DUN
            && sourceSeat >= 0 && players[sourceSeat].alive && depth < 3) {
            CardInfo judge = deck.drawCard();
            if (judge.cardId >= 0) {
                deck.discardCard(judge);
                if (judge.suit != SUIT_HEART) {
                    // 判定不为红桃，伤害来源受1点伤害(刚烈伤害不是卡牌伤害，清除g_lastDamageCard)
                    CardInfo savedDamageCard = g_lastDamageCard;
                    g_lastDamageCard.cardId = 0;
                    dealDamage(sourceSeat, 1, targetSeat, depth + 1);
                    g_lastDamageCard = savedDamageCard; // 恢复以供后续技能使用
                }
            }
        }
        // 郭嘉-遗计: 受到1点伤害后，摸两张牌
        if (players[targetSeat].alive && players[targetSeat].hero == HERO_GUOJIA) {
            for (int i = 0; i < 2; i++) {
                CardInfo c = deck.drawCard();
                if (c.cardId >= 0) { addCardToHand(targetSeat, c); addSync(targetSeat, c); }
            }
        }
        // 魏延-狂骨: 你造成伤害后，若你体力比你低，你回复1点体力
        if (sourceSeat >= 0 && players[sourceSeat].alive && players[sourceSeat].hero == HERO_WEIYAN
            && players[sourceSeat].hp < players[sourceSeat].maxHp) {
            players[sourceSeat].hp++;
        }

        if (players[targetSeat].hp <= 0) {
            // 华佗-急救: 回合外可将红色手牌当桃使用
            bool saved = false;
            if (hasCardType(targetSeat, CARD_TAO)) {
                for (int i = 0; i < players[targetSeat].handCount; i++) {
                    if (players[targetSeat].hand[i].type == CARD_TAO) {
                        deck.discardCard(players[targetSeat].hand[i]);
                        removeCard(targetSeat, players[targetSeat].hand[i].cardId);
                        players[targetSeat].hp = 1;
                        saved = true;
                        break;
                    }
                }
            }
            if (!saved && players[targetSeat].hero == HERO_HUATUO) {
                for (int i = 0; i < players[targetSeat].handCount; i++) {
                    if (isRedSuit(players[targetSeat].hand[i].suit)) {
                        deck.discardCard(players[targetSeat].hand[i]);
                        removeCard(targetSeat, players[targetSeat].hand[i].cardId);
                        players[targetSeat].hp = 1;
                        saved = true;
                        break;
                    }
                }
            }
            if (!saved) {
                players[targetSeat].alive = false;
                onPlayerDeath(targetSeat, sourceSeat);
            }
        }
        checkGameOver();
    }

    void onPlayerDeath(int deadSeat, int killerSeat) {
        if (players[deadSeat].identity == IDENTITY_REBEL) {
            if (killerSeat >= 0 && players[killerSeat].alive) {
                for (int i = 0; i < 3; i++) {
                    CardInfo c = deck.drawCard();
                    if (c.cardId >= 0) { addCardToHand(killerSeat, c); addSync(killerSeat, c); }
                }
            }
        } else if (players[deadSeat].identity == IDENTITY_LOYAL && killerSeat >= 0
                   && players[killerSeat].alive && players[killerSeat].identity == IDENTITY_LORD) {
            // 主公杀忠臣: 弃掉所有手牌和装备
            players[killerSeat].handCount = 0;
            memset(players[killerSeat].hand, 0, sizeof(players[killerSeat].hand));
            for (int i = 0; i < EQ_MAX; i++) {
                if (players[killerSeat].equips[i].cardId > 0) {
                    deck.discardCard(players[killerSeat].equips[i]);
                    players[killerSeat].equips[i].cardId = 0;
                }
            }
        }
        // 弃掉装备
        for (int i = 0; i < EQ_MAX; i++) {
            if (players[deadSeat].equips[i].cardId > 0) deck.discardCard(players[deadSeat].equips[i]);
            players[deadSeat].equips[i].cardId = 0;
        }
    }

    void checkGameOver() {
        if (!players[lordSeat].alive) {
            gameOver = true;
            bool spyAlive = false;
            for (int i = 0; i < playerCount; i++)
                if (players[i].alive && players[i].identity == IDENTITY_SPY) { spyAlive = true; break; }
            winnerFaction = spyAlive ? 2 : 1;
            return;
        }
        bool enemyAlive = false;
        for (int i = 0; i < playerCount; i++)
            if (players[i].alive && (players[i].identity == IDENTITY_REBEL || players[i].identity == IDENTITY_SPY))
                { enemyAlive = true; break; }
        if (!enemyAlive) { gameOver = true; winnerFaction = 0; }
    }

    int aliveCount() const {
        int c = 0;
        for (int i = 0; i < playerCount; i++) if (players[i].alive) c++;
        return c;
    }

    // 辅助: 统计手牌中某类型数量
    int countCardType(int seat, int cardType) {
        int cnt = 0;
        for (int i = 0; i < players[seat].handCount; i++)
            if (players[seat].hand[i].type == cardType) cnt++;
        return cnt;
    }

    // 辅助: 检查装备是否比当前好
    bool isBetterEquip(int seat, const CardInfo& card) {
        if (isWeaponCard(card.type)) {
            // 诸葛连弩: 无限出杀，如果当前没有诸葛连弩就值得装备
            if (card.type == CARD_EQ_ZHUGE)
                return players[seat].equips[EQ_WEAPON].type != CARD_EQ_ZHUGE;
            int curRange = getAttackRange(seat);
            int newRange = getWeaponRange(card.type);
            // 当前武器是诸葛连弩时，任何射程>=2的武器都值得换(除非手牌多杀)
            if (players[seat].equips[EQ_WEAPON].type == CARD_EQ_ZHUGE)
                return newRange >= 2;
            return newRange > curRange;
        }
        if (isArmorCard(card.type))
            return players[seat].equips[EQ_ARMOR].cardId <= 0;
        if (isDefHorse(card.type))
            return players[seat].equips[EQ_DEF_HORSE].cardId <= 0;
        if (isAtkHorse(card.type))
            return players[seat].equips[EQ_ATK_HORSE].cardId <= 0;
        return false;
    }

    // 辅助: 能杀到的最佳目标(集火残血)
    int bestShaTarget(int seat) {
        int best = -1, bestScore = 9999;
        int range = getAttackRange(seat);
        for (int i = 0; i < playerCount; i++) {
            if (i == seat || !players[i].alive) continue;
            int dist = getDistance(seat, i);
            if (dist > range) continue;
            int score = dist * 10;
            score -= (players[i].maxHp - players[i].hp) * 15;
            if (players[seat].identity == IDENTITY_LORD || players[seat].identity == IDENTITY_LOYAL) {
                if (players[i].identity == IDENTITY_REBEL) score -= 10;
                if (players[i].identity == IDENTITY_SPY) score -= 5;
            } else if (players[seat].identity == IDENTITY_REBEL) {
                if (players[i].identity == IDENTITY_LORD) score -= 15;
            } else if (players[seat].identity == IDENTITY_SPY) {
                if (players[i].identity == IDENTITY_LORD && players[i].hp <= 2) score -= 20;
            }
            if (score < bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    int bestShunqianTarget(int seat) {
        int best = -1, bestScore = -9999;
        for (int i = 0; i < playerCount; i++) {
            if (i == seat || !players[i].alive) continue;
            if (getDistance(seat, i) != 1) continue;
            if (!canBeStolen(i)) continue;
            int score = players[i].handCount * 5;
            for (int e = 0; e < 4; e++)
                if (players[i].equips[e].cardId > 0) score += 8;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    int bestGuoheTarget(int seat) {
        int best = -1, bestScore = 0; // 最低分数为0，没有牌/装备的目标不选
        for (int i = 0; i < playerCount; i++) {
            if (i == seat || !players[i].alive) continue;
            // 必须有手牌或装备或判定牌才值得拆
            int handCards = players[i].handCount;
            int equips = 0;
            for (int e = 0; e < 4; e++) if (players[i].equips[e].cardId > 0) equips++;
            if (handCards == 0 && equips == 0 && !players[i].lebusiTarget) continue;
            int score = 0;
            for (int e = 0; e < 4; e++)
                if (players[i].equips[e].cardId > 0) score += 15;
            score += handCards * 3;
            if (players[i].lebusiTarget) score += 20; // 优先拆乐不思蜀
            score += (players[i].maxHp - players[i].hp) * 4;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    int bestJuedouTarget(int seat) {
        int best = -1, bestScore = -9999;
        for (int i = 0; i < playerCount; i++) {
            if (i == seat || !players[i].alive) continue;
            int score = (players[i].maxHp - players[i].hp) * 10;
            score -= countCardType(i, CARD_SHA) * 8;
            if (players[i].hp == 1) score += 20;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    int bestLebusiTarget(int seat) {
        int best = -1, bestScore = -9999;
        for (int i = 0; i < playerCount; i++) {
            if (i == seat || !players[i].alive) continue;
            if (players[i].lebusiTarget) continue;
            int score = players[i].hp * 5 + players[i].handCount * 3;
            for (int e = 0; e < 4; e++)
                if (players[i].equips[e].cardId > 0) score += 5;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    int bestHuogongTarget(int seat) {
        int best = -1, bestScore = -9999;
        for (int i = 0; i < playerCount; i++) {
            if (i == seat || !players[i].alive) continue;
            int score = (players[i].maxHp - players[i].hp) * 10;
            if (players[i].equips[EQ_ARMOR].cardId <= 0) score += 8;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    // AI自动出牌 - 智能评分系统
    int aiChooseCardToPlay(int seat) {
        if (currentPhase != PHASE_PLAY) return -1;
        int bestIdx = -1, bestScore = 0;
        int shaCnt = countCardType(seat, CARD_SHA);
        int flashCnt = countCardType(seat, CARD_SHAN);
        int handOverflow = players[seat].handCount - players[seat].hp;
        bool hasShaTarget = (bestShaTarget(seat) >= 0);

        for (int i = 0; i < players[seat].handCount; i++) {
            int type = players[seat].hand[i].type;
            int score = 0;
            switch (type) {
                case CARD_SHA:
                    if (!canUseSha(seat) || !hasShaTarget) { score = -100; break; }
                    score = 55;
                    if (players[seat].hero == HERO_LVBU) score += 10;
                    if (players[seat].hero == HERO_ZHANGFEI) score += 5;
                    { int tgt = bestShaTarget(seat);
                      if (tgt >= 0 && players[tgt].hp <= 2) score += 15; }
                    if (handOverflow > 1) score += 10;
                    break;
                case CARD_SHAN:
                    // 赵云-龙胆: 闪当杀
                    if (players[seat].hero == HERO_ZHAOYUN && canUseSha(seat) && bestShaTarget(seat) >= 0) {
                        score = 55;
                        { int tgt = bestShaTarget(seat);
                          if (tgt >= 0 && players[tgt].hp <= 2) score += 15; }
                        if (handOverflow > 1) score += 10;
                        // 留牌策略: 保留至少1张闪(除非溢出)
                        if (handOverflow <= 0) score -= 25;
                    } else {
                        score = -100;
                    }
                    break;
                case CARD_TAO:
                    if (players[seat].hp >= players[seat].maxHp) { score = -100; break; }
                    score = (players[seat].hp <= 2) ? 85 : (players[seat].hp <= players[seat].maxHp - 2) ? 60 : 35;
                    if (players[seat].hp == 1) score = 95;
                    break;
                case CARD_WUZHONG:
                    // 无中生有没有使用次数限制
                    score = 50;
                    if (players[seat].handCount < players[seat].hp) score += 15;
                    if (players[seat].handCount <= 2) score += 10;
                    break;
                case CARD_EQ_ZHUGE: case CARD_EQ_QINGGANG: case CARD_EQ_FANGTIAN:
                case CARD_EQ_QIXING: case CARD_EQ_GUANSHI: case CARD_EQ_QINGLONG:
                case CARD_EQ_BAGUA: case CARD_EQ_RENJIE: case CARD_EQ_JUEYING:
                case CARD_EQ_DILU: case CARD_EQ_CHITU: case CARD_EQ_DAWAN:
                    if (!isBetterEquip(seat, players[seat].hand[i])) { score = -100; break; }
                    score = 45 + (isWeaponCard(type) ? 10 : 0);
                    if (handOverflow > 0) score += 8;
                    break;
                case CARD_NANMAN:
                    score = 40 + (shaCnt >= 1 ? 5 : 0);
                    { int e = 0; for (int k = 0; k < playerCount; k++) if (k != seat && players[k].alive) e++;
                      score += e * 5; }
                    if (handOverflow > 1) score += 10;
                    break;
                case CARD_WANJIAN:
                    score = 40 + (flashCnt >= 1 ? 5 : 0);
                    { int e = 0; for (int k = 0; k < playerCount; k++) if (k != seat && players[k].alive) e++;
                      score += e * 5; }
                    if (handOverflow > 1) score += 10;
                    break;
                case CARD_SHUNQIAN:
                    if (bestShunqianTarget(seat) < 0) { score = -100; break; }
                    score = 58; if (handOverflow > 0) score += 5;
                    break;
                case CARD_GUOHE:
                    if (bestGuoheTarget(seat) < 0) { score = -100; break; }
                    score = 48; break;
                case CARD_JUEDOU:
                    if (bestJuedouTarget(seat) < 0) { score = -100; break; }
                    score = 52 + (shaCnt >= 2 ? 12 : 0) + (players[seat].hero == HERO_LVBU ? 15 : 0);
                    if (handOverflow > 1) score += 5;
                    break;
                case CARD_HUOGONG:
                    if (bestHuogongTarget(seat) < 0) { score = -100; break; }
                    score = 35 + (players[seat].handCount >= 5 ? 10 : 0);
                    break;
                case CARD_LEBUSI:
                    if (bestLebusiTarget(seat) < 0) { score = -100; break; }
                    score = 42; break;
                case CARD_JUEDAO: {
                    // 借刀杀人: 对有武器的敌方角色使用
                    bool hasWeaponTarget = false;
                    for (int k = 0; k < playerCount; k++) {
                        if (k == seat || !players[k].alive) continue;
                        if (players[k].equips[EQ_WEAPON].cardId > 0
                            && players[k].identity != players[seat].identity) {
                            hasWeaponTarget = true; break;
                        }
                    }
                    if (!hasWeaponTarget) { score = -100; break; }
                    score = 48; if (handOverflow > 0) score += 5;
                    break;
                }
                case CARD_TIEJI:
                    score = 25;
                    { int e = 0; for (int k = 0; k < playerCount; k++) if (k != seat && players[k].alive) e++;
                      if (e >= 2) score += 10; }
                    break;
                case CARD_TAOYUAN:
                    { bool hurt = false;
                      for (int k = 0; k < playerCount; k++)
                        if (players[k].alive && players[k].hp < players[k].maxHp) { hurt = true; break; }
                      score = hurt ? 38 : -100; }
                    break;
                case CARD_WUGU:
                    score = 42 + (players[seat].handCount <= 3 ? 10 : 0);
                    break;
                default:
                    // 武将技能卡牌替代
                    if (players[seat].hero == HERO_GUANYU && isRedSuit(players[seat].hand[i].suit)
                        && type != CARD_SHA && type != CARD_TAO && !isEquipCard(type)
                        && canUseSha(seat) && hasShaTarget) {
                        // 关羽-武圣: 红色当杀
                        score = 50;
                        if (handOverflow > 1) score += 10;
                    } else if (players[seat].hero == HERO_GANNING && !isRedSuit(players[seat].hand[i].suit)
                        && type != CARD_SHA && type != CARD_SHAN && !isEquipCard(type)) {
                        // 甘宁-奇袭: 黑色当过河拆桥
                        if (bestGuoheTarget(seat) >= 0) {
                            score = 45;
                            if (handOverflow > 0) score += 5;
                        } else { score = -100; }
                    } else if (players[seat].hero == HERO_DAQIAO && players[seat].hand[i].suit == SUIT_DIAMOND
                        && type != CARD_SHA && type != CARD_SHAN && type != CARD_TAO && !isEquipCard(type)) {
                        // 大乔-国色: 方块当乐不思蜀
                        if (bestLebusiTarget(seat) >= 0) {
                            score = 40;
                        } else { score = -100; }
                    } else {
                        score = 0;
                    }
                    break;
            }
            // 留牌策略
            if (handOverflow <= 0) {
                if (type == CARD_TAO && players[seat].hp < players[seat].maxHp) score -= 20;
                if (type == CARD_SHA && shaCnt <= 1) score -= 15;
                if (type == CARD_SHAN) score -= 25;
            }
            if (score > bestScore) { bestScore = score; bestIdx = i; }
        }
        return (bestScore > 0) ? bestIdx : -1;
    }

    // 智能选目标 - 根据锦囊类型选择最优目标
    int aiChooseTarget(int seat, int cardType) {
        switch (cardType) {
            case CARD_SHA:
            case CARD_JUEDOU:
                return bestShaTarget(seat);
            case CARD_SHUNQIAN:
                return bestShunqianTarget(seat);
            case CARD_GUOHE:
                return bestGuoheTarget(seat);
            case CARD_HUOGONG:
                return bestHuogongTarget(seat);
            case CARD_LEBUSI:
                return bestLebusiTarget(seat);
            case CARD_JUEDAO: {
                // 借刀杀人: 选择有武器的敌方角色
                int best = -1;
                for (int k = 0; k < playerCount; k++) {
                    if (k == seat || !players[k].alive) continue;
                    if (players[k].equips[EQ_WEAPON].cardId > 0
                        && players[k].identity != players[seat].identity) {
                        best = k; break;
                    }
                }
                return best;
            }
            default:
                {
                    int best = -1, bestDist = 999;
                    for (int i = 0; i < playerCount; i++) {
                        if (i == seat || !players[i].alive) continue;
                        int d = getDistance(seat, i);
                        if (players[seat].identity == IDENTITY_LORD || players[seat].identity == IDENTITY_LOYAL) {
                            if (players[i].identity == IDENTITY_REBEL) d -= 2;
                        } else if (players[seat].identity == IDENTITY_REBEL) {
                            if (players[i].identity == IDENTITY_LORD) d -= 2;
                        }
                        if (d < bestDist) { bestDist = d; best = i; }
                    }
                    return best;
                }
        }
    }
};

// ===================== RoomInfo =====================
struct RoomInfo {
    std::string id;
    std::string roomName;
    std::string hostIp;
    uint16_t tcpPort;
    int playerCount;
    int maxPlayers;
};

// ===================== DiscoveryResponder (UDP发现响应) =====================
class DiscoveryResponder {
public:
    ~DiscoveryResponder() { stop(); }

    bool start(const std::string& roomId, const std::string& roomName,
               const std::string& hostName, int playerCount, int maxPlayers) {
        if (running_) return true;
        roomId_ = roomId;
        roomName_ = roomName;
        hostName_ = hostName;
        playerCount_ = playerCount;
        maxPlayers_ = maxPlayers;

        udpSock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpSock_ == kInvalidSocket) return false;
        setReuseAddr(udpSock_);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(kDiscoveryPort);
        if (bind(udpSock_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            closeSocket(udpSock_);
            udpSock_ = kInvalidSocket;
            return false;
        }

        // 设置非阻塞模式
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(udpSock_, FIONBIO, &mode);
#else
        fcntl(udpSock_, F_SETFL, O_NONBLOCK);
#endif

        running_ = true;
        thread_ = std::thread(&DiscoveryResponder::run, this);
        return true;
    }

    void stop() {
        running_ = false;
        if (udpSock_ != kInvalidSocket) {
            closeSocket(udpSock_);
            udpSock_ = kInvalidSocket;
        }
        if (thread_.joinable()) thread_.join();
    }

    void updateInfo(int playerCount) {
        playerCount_ = playerCount;
    }

    int getPlayerCount() const { return playerCount_; }

private:
    void run() {
        char buf[1024];
        while (running_) {
            struct sockaddr_in clientAddr;
#ifdef _WIN32
            int addrLen = sizeof(clientAddr);
#else
            socklen_t addrLen = sizeof(clientAddr);
#endif
            int n = recvfrom(udpSock_, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&clientAddr, &addrLen);
            if (n <= 0) { msleep(100); continue; }
            buf[n] = '\0';
            std::string msg = trimCRLF(buf);
            if (msg == "DD_DISCOVER") {
                std::string response = "DD_ROOM|" + roomId_ + "|" + roomName_ + "|"
                    + std::to_string(kGamePort) + "|" + std::to_string(playerCount_)
                    + "|" + std::to_string(maxPlayers_);
                sendto(udpSock_, response.c_str(), (int)response.size(), 0,
                       (struct sockaddr*)&clientAddr, addrLen);
            }
        }
    }

    socket_t udpSock_ = kInvalidSocket;
    std::string roomId_;
    std::string roomName_;
    std::string hostName_;
    int playerCount_ = 0;
    int maxPlayers_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ===================== discoverRooms (UDP发现房间) =====================
static std::vector<RoomInfo> discoverRooms(int waitMs) {
    std::vector<RoomInfo> rooms;

    socket_t udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSock == kInvalidSocket) return rooms;
    setBroadcast(udpSock);

    // 发送广播
    struct sockaddr_in bcastAddr;
    memset(&bcastAddr, 0, sizeof(bcastAddr));
    bcastAddr.sin_family = AF_INET;
    bcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    bcastAddr.sin_port = htons(kDiscoveryPort);

    std::string msg = "DD_DISCOVER\n";
    sendto(udpSock, msg.c_str(), (int)msg.size(), 0,
           (struct sockaddr*)&bcastAddr, sizeof(bcastAddr));

    // 用 select 等待响应
    fd_set readSet;
    struct timeval tv;
    tv.tv_sec = waitMs / 1000;
    tv.tv_usec = (waitMs % 1000) * 1000;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        FD_ZERO(&readSet);
        FD_SET(udpSock, &readSet);
        int remaining = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;

        int sel = select((int)udpSock + 1, &readSet, NULL, NULL, &tv);
        if (sel <= 0) break;

        char buf[1024];
        struct sockaddr_in fromAddr;
#ifdef _WIN32
        int fromLen = sizeof(fromAddr);
#else
        socklen_t fromLen = sizeof(fromAddr);
#endif
        int n = recvfrom(udpSock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr*)&fromAddr, &fromLen);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string line = trimCRLF(buf);
        if (line.substr(0, 8) == "DD_ROOM|") {
            auto parts = split(line, '|');
            if (parts.size() >= 6) {
                RoomInfo ri;
                ri.id = parts[1];
                ri.roomName = parts[2];
                ri.tcpPort = (uint16_t)atoi(parts[3].c_str());
                ri.playerCount = atoi(parts[4].c_str());
                ri.maxPlayers = atoi(parts[5].c_str());
                ri.hostIp = sockaddrToIp(fromAddr);
                rooms.push_back(ri);
            }
        }
    }

    closeSocket(udpSock);
    return rooms;
}

// ===================== GameHost (房主 TCP 服务) =====================
class GameHost {
public:
    ~GameHost() { stop(); }

    bool start(const std::string& roomName, const std::string& hostName, int maxPlayers) {
        if (running_) return true;
        roomName_ = roomName;
        hostName_ = hostName;
        maxPlayers_ = maxPlayers;
        clientCount_ = 0;

        // 生成房间ID
        std::ostringstream oss;
        oss << std::hex << ((uint32_t)rand() ^ (uint32_t)time(NULL));
        roomId_ = oss.str().substr(0, 8);

        listenSock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock_ == kInvalidSocket) return false;
        setReuseAddr(listenSock_);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(kGamePort);
        if (bind(listenSock_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            closeSocket(listenSock_);
            listenSock_ = kInvalidSocket;
            return false;
        }
        if (listen(listenSock_, maxPlayers) != 0) {
            closeSocket(listenSock_);
            listenSock_ = kInvalidSocket;
            return false;
        }

        // 设置非阻塞模式
#ifdef _WIN32
        u_long nbmode = 1;
        ioctlsocket(listenSock_, FIONBIO, &nbmode);
#else
        fcntl(listenSock_, F_SETFL, O_NONBLOCK);
#endif

        running_ = true;
        acceptThread_ = std::thread(&GameHost::acceptLoop, this);
        return true;
    }

    void stop() {
        running_ = false;
        for (int i = 0; i < kMaxPlayers - 1; i++) {
            clients_[i].running = false;
            clients_[i].active = false;
            if (clients_[i].sock != kInvalidSocket) {
                closeSocket(clients_[i].sock);
                clients_[i].sock = kInvalidSocket;
            }
            if (clients_[i].th.joinable()) clients_[i].th.join();
        }
        if (listenSock_ != kInvalidSocket) {
            closeSocket(listenSock_);
            listenSock_ = kInvalidSocket;
        }
        if (acceptThread_.joinable()) acceptThread_.join();
        clientCount_ = 0;
    }

    void removeClient(int index) {
        if (index < 0 || index >= clientCount_) return;
        // 关闭连接
        if (clients_[index].sock != kInvalidSocket) {
            closeSocket(clients_[index].sock);
            clients_[index].sock = kInvalidSocket;
        }
        clients_[index].running = false;
        if (clients_[index].th.joinable()) {
            clients_[index].th.join();
        }
        // 将后面的客户端前移（手动移动每个字段，因为 std::thread 不能移动赋值）
        for (int i = index; i < clientCount_ - 1; i++) {
            clients_[i].sock = clients_[i + 1].sock;
            clients_[i].name = clients_[i + 1].name;
            // th 和 running 不移动，因为线程已经在运行
        }
        clientCount_--;
    }

    bool isRunning() const { return running_; }

    int getPlayerCount() const {
        int cnt = 1; // host
        for (int i = 0; i < kMaxPlayers - 1; i++)
            if (clients_[i].active) cnt++;
        return cnt;
    }

    std::string getRoomId() const { return roomId_; }

    std::string getRoomName() const { return roomName_; }

    std::string getHostName() const { return hostName_; }

    int getMaxPlayers() const { return maxPlayers_; }

    // 发送消息给所有客户端
    void broadcastLine(const std::string& line) {
        for (int i = 0; i < kMaxPlayers - 1; i++) {
            if (clients_[i].active && clients_[i].sock != kInvalidSocket) {
                ::sendLine(clients_[i].sock, line);
            }
        }
    }

    // 发送消息给指定客户端
    void sendToClient(int index, const std::string& line) {
        if (index >= 0 && index < kMaxPlayers - 1 && clients_[index].running && clients_[index].sock != kInvalidSocket) {
            ::sendLine(clients_[index].sock, line);
        }
    }

    // 发送消息给最后一个连接的客户端
    void sendToLast(const std::string& line) {
        if (clientCount_ > 0) sendToClient(clientCount_ - 1, line);
    }

    // 获取客户端名称列表
    std::vector<std::string> getClientNames() const {
        std::vector<std::string> names;
        for (int i = 0; i < kMaxPlayers - 1; i++)
            if (clients_[i].active) names.push_back(clients_[i].name);
        return names;
    }

    // 设置客户端名字
    void setClientName(int index, const std::string& name) {
        if (index >= 0 && index < kMaxPlayers - 1) {
            clients_[index].name = name;
        }
    }

    // 设置客户端激活状态
    void setClientActive(int index, bool active) {
        if (index >= 0 && index < kMaxPlayers - 1) {
            clients_[index].active = active;
        }
    }

    // 获取指定客户端名字
    std::string getClientName(int index) const {
        if (index >= 0 && index < kMaxPlayers - 1) {
            return clients_[index].name;
        }
        return "";
    }

    // 消息回调
    std::function<void(int clientIndex, const std::string& line)> onMessage_;

private:
    void acceptLoop() {
        while (running_) {
            struct sockaddr_in clientAddr;
#ifdef _WIN32
            int addrLen = sizeof(clientAddr);
#else
            socklen_t addrLen = sizeof(clientAddr);
#endif
            socket_t clientSock = accept(listenSock_, (struct sockaddr*)&clientAddr, &addrLen);
            if (clientSock == kInvalidSocket) {
                if (!running_) break;
                msleep(100);
                continue;
            }
            // 找到空闲槽位 (active=false表示未加入，running=false表示线程已退出)
            int idx = -1;
            for (int i = 0; i < maxPlayers_ - 1; i++) {
                if (!clients_[i].active && !clients_[i].running) {
                    if (clients_[i].th.joinable()) clients_[i].th.join();
                    idx = i; break;
                }
            }
            if (idx < 0) {
                // 没有空闲槽位，拒绝连接
                closeSocket(clientSock);
                continue;
            }
            clients_[idx].sock = clientSock;
            clients_[idx].running = true;
            // 不在此处设active，等JOIN消息成功后再设
            clients_[idx].name = "";
            clients_[idx].th = std::thread(&GameHost::clientLoop, this, idx);
            if (idx >= clientCount_) clientCount_ = idx + 1;
        }
    }

    void clientLoop(int index) {
        std::string line;
        while (clients_[index].running && running_) {
            if (!::recvLine(clients_[index].sock, line)) break;
            if (onMessage_) onMessage_(index, line);
        }
        clients_[index].running = false;
        clients_[index].active = false;
        if (clients_[index].sock != kInvalidSocket) {
            closeSocket(clients_[index].sock);
            clients_[index].sock = kInvalidSocket;
        }
        // 通知房主客户端断开
        if (onMessage_) {
            onMessage_(index, "LEAVE");
        }
    }

    struct Client {
        socket_t sock = kInvalidSocket;
        std::string name;
        std::thread th;
        std::atomic<bool> running{false};
        std::atomic<bool> active{false}; // 是否连接有效(可被复用)
    };
    Client clients_[kMaxPlayers - 1]; // 最多7个客户端
    int clientCount_ = 0;
    socket_t listenSock_ = kInvalidSocket;
    std::string roomId_;
    std::string roomName_;
    std::string hostName_;
    int maxPlayers_ = 5;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
};

// ===================== GameClient (客户端 TCP) =====================
class GameClient {
public:
    ~GameClient() { disconnect(); }

    bool connect(const std::string& ip, uint16_t port) {
        if (sock_ != kInvalidSocket) disconnect();
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == kInvalidSocket) return false;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip.c_str());

        // 非阻塞连接
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock_, FIONBIO, &mode);
#else
        fcntl(sock_, F_SETFL, O_NONBLOCK);
#endif
        int ret = ::connect(sock_, (struct sockaddr*)&addr, sizeof(addr));
#ifdef _WIN32
        if (ret != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
        if (ret != 0 && errno != EINPROGRESS) {
#endif
            closeSocket(sock_); sock_ = kInvalidSocket; return false;
        }
        // select 等待连接成功，最多3秒
        fd_set writeSet; FD_ZERO(&writeSet); FD_SET(sock_, &writeSet);
        struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
        int sel = select((int)sock_ + 1, NULL, &writeSet, NULL, &tv);
        if (sel <= 0) { closeSocket(sock_); sock_ = kInvalidSocket; return false; }
        int err = 0;
#ifdef _WIN32
        int errLen = sizeof(err);
#else
        socklen_t errLen = sizeof(err);
#endif
        getsockopt(sock_, SOL_SOCKET, SO_ERROR, (char*)&err, &errLen);
        if (err != 0) { closeSocket(sock_); sock_ = kInvalidSocket; return false; }
        return true;
    }

    void disconnect() {
        if (sock_ != kInvalidSocket) {
            closeSocket(sock_);
            sock_ = kInvalidSocket;
        }
    }

    bool isConnected() const { return sock_ != kInvalidSocket; }

    bool sendLine(const std::string& line) {
        if (sock_ == kInvalidSocket) return false;
        return ::sendLine(sock_, line);
    }

    // 非阻塞 recvLine (用 select 超时)
    bool recvLine(std::string& out_line, int timeoutMs = 100) {
        if (sock_ == kInvalidSocket) return false;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock_, &readSet);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select((int)sock_ + 1, &readSet, NULL, NULL, &tv);
        if (sel <= 0) return false;
        return ::recvLine(sock_, out_line);
    }

private:
    socket_t sock_ = kInvalidSocket;
};

// ===================== 辅助结构 =====================
struct RespondMsg {
    bool responded;
    CardInfo card;
    int seatId;
    char playerName[kMaxNameLen];
};

// ===================== 全局变量 =====================
static SocketInit g_socketInit;
static GameHost g_host;
static GameClient g_client;
static DiscoveryResponder g_discovery;
static GameLogic g_game;
static std::string g_myName;
static std::string g_localIP;
static int g_mySeat = -1;
static std::atomic<bool> g_inGame(false);
static bool g_isSinglePlayer = false;
static bool g_isHost = false;
static bool g_isInRoom = false;
static bool g_waitingForResponse = false;
static int g_responseType = 0;
static int g_responseFrom = -1;
static time_t g_turnStartTime = 0;
static std::vector<std::string> g_chatLog;

// 客户端缓存的房间信息（从ROOM_STATE消息获取）
static std::string g_remoteRoomName;
static std::string g_remoteHostName;
static int g_remotePlayerCount = 0;
static int g_remoteMaxPlayers = 0;
static std::vector<std::string> g_remotePlayerNames;

// 房间状态变化标记（有变化时才刷新界面）
static std::atomic<bool> g_roomDirty(true);

// ===================== 前向声明 =====================
static void resetGlobalState() {
    g_mySeat = -1;
    g_inGame = false;
    g_isSinglePlayer = false;
    g_isHost = false;
    g_isInRoom = false;
    g_waitingForResponse = false;
    g_responseType = 0;
    g_responseFrom = -1;
    g_chatLog.clear();
    g_game = GameLogic();
    g_remoteRoomName.clear();
    g_remoteHostName.clear();
    g_remotePlayerCount = 0;
    g_remoteMaxPlayers = 0;
    g_remotePlayerNames.clear();
}

void roomLobby();
void gameLoop();
void hostDrawPhase();
void hostNextTurn();
void hostEndTurn();
void hostStartGame();
void hostProcessPlayCard(int seat, int cardIndex, int targetSeat);
void hostProcessRespond(int seat, int cardIndex);
void hostProcessSkill(int seat, int skillId, int param1, int param2);
void processAITurn(int seat);
void hostHandleJoin(int clientIdx, const std::string& playerName);
void searchRooms();
void mainMenu();
void sendSystemMsg(const char* fmt, ...);
void broadcastMsg(const std::string& line);
void broadcastPlayerState();

// ===================== UI 函数 =====================
void clearScreen() {
#ifdef _WIN32
    int _unused = system("cls"); (void)_unused;
#else
    int _unused2 = system("clear"); (void)_unused2;
#endif
}

void printBanner() {
    // 双线框，内宽 = 50
    const char* top    = "╔════════════════════════════════════════════════════╗";
    const char* bottom = "╚════════════════════════════════════════════════════╝";
    COL_RED(printf("%s\n", top));
    COL_RED(printf("║"));
    COL_YELLOW(printf("             三国杀 - 局域网联机版 v3.0             "));
    COL_RED(printf("║\n"));
    COL_RED(printf("║"));
    COL_CYAN(printf("       Sanguosha LAN Edition - Text Protocol        "));
    COL_RED(printf("║\n"));
    COL_RED(printf("%s\n", bottom));
    printf("\n");
}

void printColoredCard(const CardInfo& c) {
    if (isRedSuit(c.suit)) COL_RED(printf("%s", getSuitSymbol(c.suit)));
    else printf("%s", getSuitSymbol(c.suit));
    printf("%s%s", getCardName(c.type), getNumberStr(c.number));
}

void showHandCards() {
    if (g_mySeat < 0) return;
    GamePlayer& me = g_game.players[g_mySeat];
    COL_YELLOW(printf("\n┌─ 你的手牌 (%d张) ─ 体力:", me.handCount));
    COL_RED(printf(" %d/%d ", me.hp, me.maxHp));
    COL_YELLOW(printf("─┐\n"));
    for (int i = 0; i < me.handCount; i++) {
        printf("  │ %2d. ", i + 1);
        printColoredCard(me.hand[i]);
        if (isEquipCard(me.hand[i].type)) printf("[%s]", getEquipName(me.hand[i].type));
        printf(" ");
        // 显示技能提示
        if (me.hero == HERO_GUANYU && isRedSuit(me.hand[i].suit) && me.hand[i].type != CARD_TAO && !isEquipCard(me.hand[i].type))
            COL_GREEN(printf("(武圣)"));
        else if (me.hero == HERO_ZHAOYUN && (me.hand[i].type == CARD_SHA || me.hand[i].type == CARD_SHAN))
            COL_GREEN(printf("(龙胆)"));
        else if (me.hero == HERO_GANNING && !isRedSuit(me.hand[i].suit) && me.hand[i].type != CARD_SHA && me.hand[i].type != CARD_SHAN && !isEquipCard(me.hand[i].type))
            COL_GREEN(printf("(奇袭)"));
        else if (me.hero == HERO_DAQIAO && me.hand[i].suit == SUIT_DIAMOND && me.hand[i].type != CARD_TAO && !isEquipCard(me.hand[i].type))
            COL_GREEN(printf("(国色)"));
        else if (me.hero == HERO_HUATUO && isRedSuit(me.hand[i].suit) && me.hand[i].type != CARD_TAO)
            COL_GREEN(printf("(急救)"));
        printf("│\n");
    }
    // 显示装备
    bool hasEquip = false;
    for (int i = 0; i < EQ_MAX; i++) {
        if (me.equips[i].cardId > 0) { hasEquip = true; break; }
    }
    if (hasEquip) {
        COL_CYAN(printf("  └─ 装备: "));
        for (int i = 0; i < EQ_MAX; i++) {
            if (me.equips[i].cardId > 0) printf("[%s] ", getEquipName(me.equips[i].type));
        }
        printf("\n");
    }
    printf("\n");
}

void showAllPlayers() {
    COL_YELLOW(printf("\n┌─ 所有玩家 ─────────────────────────────────────┐\n"));
    for (int i = 0; i < g_game.playerCount; i++) {
        GamePlayer& p = g_game.players[i];
        printf("  │ ");
        if (!p.alive) COL_GRAY(printf("[%d]", i));
        else if (i == g_mySeat) COL_GREEN(printf("[%d]", i));
        else COL_WHITE(printf("[%d]", i));

        if (p.isLord) COL_RED(printf(" ★"));
        else printf("  ");

        printf(" %-8s ", p.name);
        // 势力颜色
        const char* k = getHeroKingdom(p.hero);
        if (strcmp(k, "蜀") == 0) COL_RED(printf("[%s]", k));
        else if (strcmp(k, "魏") == 0) COL_BLUE(printf("[%s]", k));
        else if (strcmp(k, "吴") == 0) COL_GREEN(printf("[%s]", k));
        else COL_MAGENTA(printf("[%s]", k));

        printf(" %-6s", getHeroName(p.hero));

        // 身份
        const char* idName = (p.isLord || i == g_mySeat) ? getIdentityName(p.identity) : "???";
        if (strcmp(idName, "主公") == 0) COL_RED(printf(" %s", idName));
        else if (strcmp(idName, "忠臣") == 0) COL_YELLOW(printf(" %s", idName));
        else if (strcmp(idName, "反贼") == 0) COL_CYAN(printf(" %s", idName));
        else if (strcmp(idName, "内奸") == 0) COL_MAGENTA(printf(" %s", idName));
        else printf(" %s", idName);

        // 血量
        printf(" HP:");
        if (p.hp <= 1 && p.alive) COL_RED(printf("%d", p.hp));
        else if (!p.alive) COL_GRAY(printf("X"));
        else COL_GREEN(printf("%d", p.hp));
        printf("/%d", p.maxHp);

        if (!p.alive) COL_RED(printf(" [阵亡]"));
        if (i == g_mySeat) COL_GREEN(printf(" <<<你"));

        // 装备
        for (int j = 0; j < EQ_MAX; j++) {
            if (p.equips[j].cardId > 0) printf(" [%s]", getEquipName(p.equips[j].type));
        }

        printf("\n");
    }
    COL_YELLOW(printf("  └──────────────────────────────────────────────┘\n\n"));
}

void showRoomStatus() {
    printf("\n");
    if (g_isHost) {
        COL_YELLOW(printf("  房间: %s  (房主: %s)\n", g_host.getRoomName().c_str(), g_host.getHostName().c_str()));
        printf("  玩家 (%d/%d):\n", g_host.getPlayerCount(), g_host.getMaxPlayers());
        COL_GREEN(printf("    [房主] %s\n", g_host.getHostName().c_str()));
        auto names = g_host.getClientNames();
        for (size_t i = 0; i < names.size(); i++)
            printf("    [%d] %s\n", (int)i + 1, names[i].c_str());
    } else {
        // 客户端: 使用从ROOM_STATE消息缓存的远程房间信息
        COL_YELLOW(printf("  房间: %s  (房主: %s)\n", g_remoteRoomName.c_str(), g_remoteHostName.c_str()));
        printf("  玩家 (%d/%d):\n", g_remotePlayerCount, g_remoteMaxPlayers);
        for (size_t i = 0; i < g_remotePlayerNames.size(); i++) {
            if (i == 0) COL_GREEN(printf("    [房主] %s\n", g_remotePlayerNames[i].c_str()));
            else printf("    [%d] %s\n", (int)i, g_remotePlayerNames[i].c_str());
        }
    }
    printf("\n");
}

void showGameStatus() {
    if (g_game.currentSeat >= 0 && g_game.currentSeat < g_game.playerCount) {
        printf("  当前: ");
        if (g_game.currentSeat == g_mySeat) COL_GREEN(printf("%s", g_game.players[g_game.currentSeat].name));
        else printf("%s", g_game.players[g_game.currentSeat].name);
        printf(" 的%s", getPhaseName(g_game.currentPhase));
        // 计时
        time_t now = time(NULL);
        int elapsed = (int)(now - g_turnStartTime);
        int remain = kTurnTimeout - elapsed;
        if (remain < 10) COL_RED(printf("  [剩余%d秒]", remain > 0 ? remain : 0));
        else printf("  [剩余%d秒]", remain > 0 ? remain : 0);
        printf("\n");
    }
    printf("  牌堆剩余: %d张\n", g_game.deck.remaining());
}

// ===================== 序列化辅助 =====================
static std::string serializeCard(const CardInfo& c) {
    return std::to_string(c.type) + "-" + std::to_string(c.suit) + "-"
         + std::to_string(c.number) + "-" + std::to_string(c.cardId);
}

static CardInfo deserializeCard(const std::string& s) {
    auto p = split(s, '-');
    CardInfo c;
    memset(&c, 0, sizeof(c));
    if (p.size() >= 4) {
        c.type = atoi(p[0].c_str());
        c.suit = atoi(p[1].c_str());
        c.number = atoi(p[2].c_str());
        c.cardId = atoi(p[3].c_str());
    }
    return c;
}

static std::string serializePlayerData(int seat) {
    GamePlayer& p = g_game.players[seat];
    std::string s;
    s += std::to_string(p.seatId) + ",";
    s += std::string(p.name) + ",";
    s += std::to_string(p.hero) + ",";
    s += std::to_string(p.identity) + ",";
    s += std::to_string(p.hp) + ",";
    s += std::to_string(p.maxHp) + ",";
    s += std::to_string(p.handCount) + ",";
    s += std::to_string(p.alive ? 1 : 0) + ",";
    s += std::to_string(p.isLord ? 1 : 0) + ",";
    s += std::to_string(p.shaUsed) + ",";
    // 手牌 (用:分隔以便GAME_INFO用;分隔玩家)
    for (int i = 0; i < p.handCount; i++) {
        if (i > 0) s += ":";
        s += serializeCard(p.hand[i]);
    }
    s += ",";
    // 装备 (始终序列化所有4个槽位，保证反序列化位置正确)
    for (int i = 0; i < EQ_MAX; i++) {
        if (i > 0) s += ":";
        s += serializeCard(p.equips[i]);
    }
    return s;
}

static void deserializePlayerData(const std::string& s) {
    // 格式: seatId,name,hero,identity,hp,maxHp,handCount,alive,isLord,shaUsed,card1;card2;...,equip1;equip2;...
    auto p = split(s, ',');
    if (p.size() < 10) return;
    int seat = atoi(p[0].c_str());
    if (seat < 0 || seat >= kMaxPlayers) return;
    GamePlayer& pl = g_game.players[seat];
    strncpy(pl.name, p[1].c_str(), kMaxNameLen - 1);
    pl.hero = atoi(p[2].c_str());
    pl.identity = atoi(p[3].c_str());
    pl.hp = atoi(p[4].c_str());
    pl.maxHp = atoi(p[5].c_str());
    pl.handCount = atoi(p[6].c_str());
    pl.alive = (atoi(p[7].c_str()) != 0);
    pl.isLord = (atoi(p[8].c_str()) != 0);
    pl.shaUsed = atoi(p[9].c_str());
    pl.seatId = seat;
    // 手牌 (p[10])
    if (p.size() > 10 && !p[10].empty()) {
        auto cards = split(p[10], ':');
        for (size_t i = 0; i < cards.size() && (int)i < kMaxHandSize; i++) {
            pl.hand[i] = deserializeCard(cards[i]);
        }
    }
    // 装备 (p[11])
    if (p.size() > 11 && !p[11].empty()) {
        auto equips = split(p[11], ':');
        for (size_t i = 0; i < equips.size() && (int)i < EQ_MAX; i++) {
            pl.equips[i] = deserializeCard(equips[i]);
        }
    }
}

// ===================== 协议解析 (客户端: 处理服务端发来的消息) =====================
void handleServerLine(const std::string& line) {
    auto parts = split(line, '|');
    if (parts.empty()) return;
    const std::string& cmd = parts[0];

    if (cmd == "JOIN_OK") {
        g_isInRoom = true;
        COL_GREEN(printf("[系统] 成功加入房间!\n"));
    }
    else if (cmd == "JOIN_FAIL") {
        if (parts.size() > 1) printf("[系统] 加入失败: %s\n", parts[1].c_str());
    }
    else if (cmd == "ROOM_STATE") {
        // ROOM_STATE|name|host|count|max|hostName,p2,p3,...
        if (parts.size() >= 6) {
            g_remoteRoomName = parts[1];
            g_remoteHostName = parts[2];
            g_remotePlayerCount = atoi(parts[3].c_str());
            g_remoteMaxPlayers = atoi(parts[4].c_str());
            g_remotePlayerNames.clear();
            // parts[5]开始是玩家名列表
            for (size_t i = 5; i < parts.size(); i++) {
                if (!parts[i].empty()) g_remotePlayerNames.push_back(parts[i]);
            }
            g_roomDirty = true; // 标记需要刷新
        }
    }
    else if (cmd == "GAME_INFO") {
        // GAME_INFO|count|player1_data;player2_data;...
        if (parts.size() >= 3) {
            int count = atoi(parts[1].c_str());
            g_game.playerCount = count;
            auto pdata = split(parts[2], ';');
            for (size_t i = 0; i < pdata.size() && (int)i < count; i++) {
                deserializePlayerData(pdata[i]);
            }
            // 找到自己的座位
            g_mySeat = -1;
            for (int i = 0; i < count; i++) {
                if (strcmp(g_game.players[i].name, g_myName.c_str()) == 0) {
                    g_mySeat = i; break;
                }
            }
            g_game.gameStarted = true;
            g_game.gameOver = false;
            g_inGame = true;
            COL_GREEN(printf("\n[系统] ====== 游戏开始! ======\n"));
        }
    }
    else if (cmd == "PHASE") {
        // PHASE|phase|seat|name
        if (parts.size() >= 4) {
            g_game.currentPhase = (Phase)atoi(parts[1].c_str());
            g_game.currentSeat = atoi(parts[2].c_str());
            g_turnStartTime = time(NULL);
            if (g_inGame) printf("[系统] --- %s 的%s ---\n", parts[3].c_str(), getPhaseName(g_game.currentPhase));
        }
    }
    else if (cmd == "DRAW") {
        // DRAW|count|card1,card2,...|playerName
        if (parts.size() >= 4) {
            int count = atoi(parts[1].c_str());
            const std::string& pname = parts[3];
            if (pname == g_myName) {
                auto cards = split(parts[2], ':');
                for (size_t i = 0; i < cards.size(); i++) {
                    CardInfo c = deserializeCard(cards[i]);
                    if (c.cardId >= 0) g_game.addCardToHand(g_mySeat, c);
                }
                COL_GREEN(printf("[摸牌] 你摸了 %d 张牌\n", count));
            } else {
                printf("[摸牌] %s 摸了 %d 张牌\n", pname.c_str(), count);
            }
        }
    }
    else if (cmd == "PLAY") {
        // PLAY|cardType|suit|number|cardId|fromSeat|toSeat|fromName|toName
        if (parts.size() >= 9) {
            int cardType = atoi(parts[1].c_str());
            int fromSeat = atoi(parts[5].c_str());
            int toSeat = atoi(parts[6].c_str());
            const std::string& fromName = parts[7];
            const std::string& toName = parts[8];
            int cardId = atoi(parts[4].c_str());

            if (toSeat >= 0)
                printf("[出牌] %s 对 %s 使用了 [", fromName.c_str(), toName.c_str());
            else
                printf("[出牌] %s 使用了 [", fromName.c_str());
            COL_YELLOW(printf("%s", getCardName(cardType)));
            printf("]\n");

            // 客户端: 只有收到PLAY广播时才从手牌移除
            // 房主在hostProcessPlayCard中已经移除了，不需要重复移除
            // 但客户端需要同步状态
            if (!g_isHost && fromSeat == g_mySeat) {
                g_game.removeCard(g_mySeat, cardId);
            }
        }
    }
    else if (cmd == "DISCARD") {
        // DISCARD|seat|count|playerName
        if (parts.size() >= 4) {
            int seat = atoi(parts[1].c_str());
            int count = atoi(parts[2].c_str());
            printf("[弃牌] %s 弃了 %d 张牌\n", parts[3].c_str(), count);
            // 客户端: 移除自己的手牌
            if (seat == g_mySeat) {
                while (count > 0 && g_game.players[seat].handCount > g_game.players[seat].hp) {
                    g_game.players[seat].handCount--;
                    count--;
                }
            }
        }
    }
    else if (cmd == "ASK_DODGE") {
        // ASK_DODGE|fromSeat|toSeat|fromName|toName
        if (parts.size() >= 5) {
            int toSeat = atoi(parts[2].c_str());
            int fromSeat = atoi(parts[1].c_str());
            if (toSeat == g_mySeat) {
                g_waitingForResponse = true;
                g_responseType = CARD_SHAN;
                g_responseFrom = fromSeat;
                COL_RED(printf("\n[响应] %s 对你使用了【杀】! 出【闪】? (编号/0放弃)\n", parts[3].c_str()));
            }
        }
    }
    else if (cmd == "ASK_PEACH") {
        // ASK_PEACH|fromSeat|toSeat|fromName|toName
        if (parts.size() >= 5) {
            int toSeat = atoi(parts[2].c_str());
            int fromSeat = atoi(parts[1].c_str());
            if (toSeat == g_mySeat) {
                g_waitingForResponse = true;
                g_responseType = CARD_TAO;
                g_responseFrom = fromSeat;
                COL_RED(printf("\n[响应] 你已濒死! 使用【桃】? (编号/0放弃)\n"));
            }
        }
    }
    else if (cmd == "RESPOND") {
        if (parts.size() >= 3) {
            bool responded = (parts[1] == "1");
            if (responded && parts.size() >= 8) {
                printf("[响应] %s 出了 [%s]\n", parts[7].c_str(), getCardName(atoi(parts[2].c_str())));
            } else if (!responded && parts.size() >= 7) {
                printf("[响应] %s 未响应\n", parts[6].c_str());
            }
            // 如果正在等待响应，清除状态（客户端接收到房主的RESPOND广播）
            if (g_waitingForResponse) {
                g_waitingForResponse = false;
                g_responseType = 0;
            }
        }
    }
    else if (cmd == "EQUIP") {
        // EQUIP|cardType|suit|number|cardId|seat|name
        if (parts.size() >= 7) {
            int cardType = atoi(parts[1].c_str());
            int suit = atoi(parts[2].c_str());
            int number = atoi(parts[3].c_str());
            int cardId = atoi(parts[4].c_str());
            int seat = atoi(parts[5].c_str());
            const std::string& pname = parts[6];
            COL_CYAN(printf("[装备] %s 装备了 [%s]\n", pname.c_str(), getEquipName(cardType)));
            // 客户端: 从自己手牌中移除装备牌并更新装备区
            if (seat == g_mySeat) {
                g_game.removeCard(g_mySeat, cardId);
            }
            // 更新装备区(所有玩家都需要知道)
            CardInfo eqCard;
            eqCard.type = cardType; eqCard.suit = suit;
            eqCard.number = number; eqCard.cardId = cardId;
            EquipSlot slot;
            if (isWeaponCard(cardType)) slot = EQ_WEAPON;
            else if (isArmorCard(cardType)) slot = EQ_ARMOR;
            else if (isDefHorse(cardType)) slot = EQ_DEF_HORSE;
            else slot = EQ_ATK_HORSE;
            // 旧装备弃置
            if (g_game.players[seat].equips[slot].cardId > 0) {
                g_game.deck.discardCard(g_game.players[seat].equips[slot]);
            }
            g_game.players[seat].equips[slot] = eqCard;
        }
    }
    else if (cmd == "PLAYER_STATE") {
        // PLAYER_STATE|seat,hp,maxHp,alive,equips,lebusi,luoyi;seat2,...
        if (parts.size() >= 2) {
            auto playerStates = split(parts[1], ';');
            for (size_t ps = 0; ps < playerStates.size(); ps++) {
                auto fields = split(playerStates[ps], ',');
                if (fields.size() >= 5) {
                    int seat = atoi(fields[0].c_str());
                    int hp = atoi(fields[1].c_str());
                    int maxHp = atoi(fields[2].c_str());
                    bool alive = (atoi(fields[3].c_str()) != 0);
                    if (seat >= 0 && seat < kMaxPlayers) {
                        g_game.players[seat].hp = hp;
                        g_game.players[seat].maxHp = maxHp;
                        g_game.players[seat].alive = alive;
                        // 装备区 (fields[4])
                        if (fields.size() >= 5 && !fields[4].empty()) {
                            auto equips = split(fields[4], ':');
                            for (size_t e = 0; e < equips.size() && (int)e < EQ_MAX; e++) {
                                g_game.players[seat].equips[e] = deserializeCard(equips[e]);
                            }
                        }
                        // 特殊状态
                        if (fields.size() >= 6)
                            g_game.players[seat].lebusiTarget = (atoi(fields[5].c_str()) != 0);
                        if (fields.size() >= 7)
                            g_game.players[seat].luoyiActive = (atoi(fields[6].c_str()) != 0);
                    }
                }
            }
        }
    }
    else if (cmd == "SYSTEM") {
        if (parts.size() > 1) COL_YELLOW(printf("[系统] %s\n", parts[1].c_str()));
    }
    else if (cmd == "CHAT") {
        if (parts.size() > 2) {
            g_chatLog.push_back("[" + parts[1] + "] " + parts[2]);
            if (g_chatLog.size() > 50) g_chatLog.erase(g_chatLog.begin());
        }
    }
    else if (cmd == "GAME_OVER") {
        // GAME_OVER|winner|winnerName
        if (parts.size() >= 3) {
            g_game.gameOver = true;
            g_inGame = false;
            printf("\n"); printBanner();
            COL_GREEN(printf("  游戏结束! %s 获胜!\n\n", parts[2].c_str()));
        }
    }
    else if (cmd == "PING") {
        g_client.sendLine("PONG");
    }
}

// ===================== 房主处理客户端消息 =====================
void handleClientMessage(int clientIdx, const std::string& line) {
    auto parts = split(line, '|');
    if (parts.empty()) return;
    const std::string& cmd = parts[0];

    if (cmd == "JOIN") {
        if (parts.size() > 1) hostHandleJoin(clientIdx, parts[1]);
    }
    else if (cmd == "LEAVE") {
        if (clientIdx >= 0 && clientIdx < kMaxPlayers - 1) {
            // 获取玩家名字（在setActive(false)之前获取）
            std::string playerName = g_host.getClientName(clientIdx);
            if (!playerName.empty()) {
                g_host.broadcastLine("SYSTEM|" + playerName + " 离开了房间");
            }
            // 标记槽位空闲
            g_host.setClientActive(clientIdx, false);
            g_host.broadcastLine("ROOM_STATE|" + g_host.getRoomName() + "|" + g_host.getHostName()
                + "|" + std::to_string(g_host.getPlayerCount()) + "|" + std::to_string(g_host.getMaxPlayers())
                + "|" + g_host.getHostName());
            g_roomDirty = true; // 标记需要刷新
        }
    }
    else if (cmd == "CHAT") {
        if (parts.size() > 2) {
            std::string chatLine = "CHAT|" + parts[1] + "|" + parts[2];
            g_host.broadcastLine(chatLine);
            // 房主也加入聊天记录
            g_chatLog.push_back("[" + parts[1] + "] " + parts[2]);
            if (g_chatLog.size() > 50) g_chatLog.erase(g_chatLog.begin());
        }
    }
    else if (cmd == "PONG") {
        // 心跳响应，忽略
    }
    else if (cmd == "RESPOND") {
        // 客户端响应: RESPOND|cardIndex
        if (parts.size() > 1 && g_inGame) {
            int idx = atoi(parts[1].c_str());
            // 通过查找客户端名字对应的游戏座位
            std::string clientName = g_host.getClientName(clientIdx);
            int seat = -1;
            for (int i = 0; i < g_game.playerCount; i++) {
                if (strcmp(g_game.players[i].name, clientName.c_str()) == 0) { seat = i; break; }
            }
            if (seat < 0) seat = clientIdx + 1; // 回退到旧逻辑
            hostProcessRespond(seat, idx);
        }
    }
    else if (cmd == "PLAY") {
        // 客户端出牌: PLAY|cardIndex|targetSeat
        if (parts.size() > 1 && g_inGame) {
            int cardIdx = atoi(parts[1].c_str());
            int targetSeat = (parts.size() > 2) ? atoi(parts[2].c_str()) : -1;
            // 通过查找客户端名字对应的游戏座位
            std::string clientName = g_host.getClientName(clientIdx);
            int seat = -1;
            for (int i = 0; i < g_game.playerCount; i++) {
                if (strcmp(g_game.players[i].name, clientName.c_str()) == 0) { seat = i; break; }
            }
            if (seat < 0) seat = clientIdx + 1; // 回退到旧逻辑
            hostProcessPlayCard(seat, cardIdx, targetSeat);
        }
    }
}

// ===================== 房主处理加入 =====================
void hostHandleJoin(int clientIdx, const std::string& playerName) {
    if (g_host.getPlayerCount() >= g_host.getMaxPlayers()) {
        g_host.sendToClient(clientIdx, "JOIN_FAIL|房间已满");
        return;
    }
    // 检查重名
    if (playerName == g_host.getHostName()) {
        g_host.sendToClient(clientIdx, "JOIN_FAIL|名字重复");
        return;
    }
    auto names = g_host.getClientNames();
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == playerName) {
            g_host.sendToClient(clientIdx, "JOIN_FAIL|名字重复");
            return;
        }
    }
    // 检查通过后才设置名字和激活
    g_host.setClientName(clientIdx, playerName);
    g_host.setClientActive(clientIdx, true);
    g_host.sendToClient(clientIdx, "JOIN_OK|OK");

    // 构建房间状态并广播 (此时active已设置，getPlayerCount()会正确返回)
    std::string roomState = "ROOM_STATE|" + g_host.getRoomName() + "|" + g_host.getHostName()
        + "|" + std::to_string(g_host.getPlayerCount()) + "|" + std::to_string(g_host.getMaxPlayers())
        + "|" + g_host.getHostName();
    auto clientNames = g_host.getClientNames();
    for (size_t i = 0; i < clientNames.size(); i++) {
        if (!clientNames[i].empty()) roomState += "," + clientNames[i];
    }
    g_host.broadcastLine(roomState);
    g_host.broadcastLine("SYSTEM|" + playerName + " 加入了房间");
    g_roomDirty = true; // 标记需要刷新
}

// ===================== 广播消息辅助 =====================
void broadcastMsg(const std::string& line) {
    if (g_isSinglePlayer) {
        // 单机模式下解析并输出关键消息，让玩家看到AI的行动
        auto parts = split(line, '|');
        if (parts.size() >= 2) {
            if (parts[0] == "PLAY" && parts.size() >= 9) {
                int toSeat = atoi(parts[6].c_str());
                if (toSeat >= 0)
                    printf("[出牌] %s 对 %s 使用了 [%s]\n", parts[7].c_str(), parts[8].c_str(), getCardName(atoi(parts[1].c_str())));
                else
                    printf("[出牌] %s 使用了 [%s]\n", parts[7].c_str(), getCardName(atoi(parts[1].c_str())));
            } else if (parts[0] == "EQUIP" && parts.size() >= 7) {
                printf("[装备] %s 装备了 [%s]\n", parts[6].c_str(), getEquipName(atoi(parts[1].c_str())));
            }
        }
        return;
    }
    if (g_isHost) g_host.broadcastLine(line);
    else g_client.sendLine(line);
}

void sendSystemMsg(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("[系统] %s\n", buf);
    if (g_isSinglePlayer) return;
    broadcastMsg("SYSTEM|" + std::string(buf));
}

// 广播所有玩家的HP和存活状态给客户端
// 格式: PLAYER_STATE|seat1,hp1,maxHp1,alive1,equips1;seat2,hp2,maxHp2,alive2,equips2;...
void broadcastPlayerState() {
    // 先发送技能导致的手牌同步消息(郭嘉遗计/曹操奸雄/司马懿反馈等)
    if (!g_isSinglePlayer && g_game.syncCount > 0) {
        for (int i = 0; i < g_game.syncCount; i++) {
            int s = g_game.syncQueue[i].seat;
            if (s >= 0 && s < g_game.playerCount) {
                std::string drawLine = "DRAW|1|" + serializeCard(g_game.syncQueue[i].card)
                    + "|" + std::string(g_game.players[s].name);
                broadcastMsg(drawLine);
            }
        }
        g_game.clearSync();
    }
    if (g_isSinglePlayer) { g_game.clearSync(); return; }
    std::string msg = "PLAYER_STATE|";
    for (int i = 0; i < g_game.playerCount; i++) {
        if (i > 0) msg += ";";
        msg += std::to_string(i) + "," + std::to_string(g_game.players[i].hp) + ","
             + std::to_string(g_game.players[i].maxHp) + ","
             + std::to_string(g_game.players[i].alive ? 1 : 0) + ",";
        // 装备区序列化
        for (int e = 0; e < EQ_MAX; e++) {
            if (e > 0) msg += ":";
            msg += serializeCard(g_game.players[i].equips[e]);
        }
        // 特殊状态
        msg += "," + std::to_string(g_game.players[i].lebusiTarget ? 1 : 0);
        msg += "," + std::to_string(g_game.players[i].luoyiActive ? 1 : 0);
    }
    broadcastMsg(msg);
}

// ===================== 游戏开始 =====================
void hostStartGame() {
    int totalPlayers;
    if (g_isSinglePlayer) {
        totalPlayers = g_game.playerCount;
    } else {
        auto clientNames = g_host.getClientNames();
        // 过滤掉空名字，只统计有效玩家
        std::vector<std::string> validNames;
        for (auto& n : clientNames) if (!n.empty()) validNames.push_back(n);
        totalPlayers = 1 + (int)validNames.size(); // 1=房主 + 有效客户端
        if (totalPlayers < 2) {
            printf("[系统] 至少需要2名玩家!\n");
            return;
        }
    }

    char names[kMaxPlayers][kMaxNameLen];
    if (g_isSinglePlayer) {
        // 直接设置名字（g_game.players尚未初始化）
        strncpy(names[0], g_myName.c_str(), kMaxNameLen - 1);
        const char* aiNamesList[] = {"曹操", "孙权", "吕布", "诸葛亮", "赵云", "关羽", "貂蝉"};
        for (int i = 1; i < totalPlayers; i++)
            strncpy(names[i], aiNamesList[i-1], kMaxNameLen - 1);
    } else {
        strncpy(names[0], g_host.getHostName().c_str(), kMaxNameLen - 1);
        auto clientNames = g_host.getClientNames();
        int nameIdx = 1;
        for (size_t i = 0; i < clientNames.size(); i++) {
            if (!clientNames[i].empty()) {
                strncpy(names[nameIdx], clientNames[i].c_str(), kMaxNameLen - 1);
                nameIdx++;
            }
        }
    }

    g_game.initGame(names, totalPlayers);

    // 发初始手牌
    for (int i = 0; i < totalPlayers; i++) {
        int drawCount = 4;
        for (int j = 0; j < drawCount; j++) {
            CardInfo c = g_game.deck.drawCard();
            if (c.cardId >= 0) g_game.addCardToHand(i, c);
        }
    }

    // 序列化并发送游戏信息
    std::string gameInfo = "GAME_INFO|" + std::to_string(totalPlayers) + "|";
    for (int i = 0; i < totalPlayers; i++) {
        if (i > 0) gameInfo += ";";
        gameInfo += serializePlayerData(i);
    }
    g_inGame = true;
    broadcastMsg(gameInfo);

    // 房主自己也需要初始化
    g_mySeat = 0;
    for (int i = 0; i < totalPlayers; i++)
        if (strcmp(g_game.players[i].name, g_myName.c_str()) == 0) { g_mySeat = i; break; }

    g_game.currentSeat = g_game.lordSeat;
    g_game.currentPhase = PHASE_DRAW;
    g_turnStartTime = time(NULL);

    std::string phaseLine = "PHASE|" + std::to_string(PHASE_DRAW) + "|"
        + std::to_string(g_game.currentSeat) + "|" + g_game.players[g_game.currentSeat].name;
    broadcastMsg(phaseLine);
    hostDrawPhase();
}

// ===================== 摸牌阶段 =====================
void hostDrawPhase() {
    int seat = g_game.currentSeat;
    if (!g_game.players[seat].alive) { hostNextTurn(); return; }

    // 乐不思蜀判定
    if (g_game.players[seat].lebusiTarget) {
        CardInfo judge = g_game.deck.drawCard();
        if (judge.cardId >= 0) {
            g_game.deck.discardCard(judge);
            bool skip = (judge.suit != SUIT_HEART); // 非红桃=跳过出牌阶段
            sendSystemMsg("%s 的乐不思蜀判定: %s%s", g_game.players[seat].name,
                         getSuitSymbol(judge.suit), skip ? "(跳过出牌)" : "(可出牌)");
            if (skip) {
                sendSystemMsg("%s 被【乐不思蜀】影响，将跳过出牌阶段!", g_game.players[seat].name);
                g_game.players[seat].skipPlay = true;
            }
        }
        g_game.players[seat].lebusiTarget = false;
    }

    // 诸葛亮-观星: 观看牌堆顶X张牌(X=min(5,存活人数))，AI自动选择保留
    if (g_game.players[seat].hero == HERO_ZHUGELIANG && g_game.players[seat].guanxingUsed == 0) {
        int starCount = g_game.aliveCount();
        if (starCount > 5) starCount = 5;
        if (starCount < 1) starCount = 1;
        // 查看牌堆顶的牌，AI选择保留好的牌(杀/闪/桃优先)
        CardInfo starCards[5];
        int actualStars = 0;
        for (int i = 0; i < starCount; i++) {
            CardInfo c = g_game.deck.drawCard();
            if (c.cardId >= 0) { starCards[actualStars++] = c; }
        }
        if (actualStars > 0) {
            // 简化: 将有用的牌放回牌堆顶(杀/闪/桃优先)，其余放牌堆底
            int priority[5]; int pc = 0;
            int others[5]; int oc = 0;
            for (int i = 0; i < actualStars; i++) {
                if (starCards[i].type == CARD_SHA || starCards[i].type == CARD_SHAN
                    || starCards[i].type == CARD_TAO || starCards[i].type == CARD_WUZHONG) {
                    priority[pc++] = i;
                } else {
                    others[oc++] = i;
                }
            }
            // 优先牌放回牌堆顶
            for (int i = pc - 1; i >= 0; i--) {
                g_game.deck.deck[g_game.deck.deckTop++] = starCards[priority[i]];
            }
            // 其余牌放牌堆底
            for (int i = 0; i < oc; i++) {
                // 移动牌堆所有牌上移一位，腾出底部位置
                for (int j = g_game.deck.deckTop; j > 0; j--)
                    g_game.deck.deck[j] = g_game.deck.deck[j - 1];
                g_game.deck.deck[0] = starCards[others[i]];
                g_game.deck.deckTop++;
            }
            g_game.players[seat].guanxingUsed = 1;
            sendSystemMsg("诸葛亮发动【观星】，观看了牌堆顶%d张牌", actualStars);
        }
    }

    // 许褚-裸衣: 摸牌阶段可少摸一张，本回合杀/决斗伤害+1
    if (g_game.players[seat].hero == HERO_XUCHU) {
        // AI: 如果手牌中有杀或决斗，则发动裸衣
        bool hasShaOrJuedou = g_game.hasCardType(seat, CARD_SHA) || g_game.hasCardType(seat, CARD_JUEDOU);
        if (hasShaOrJuedou && g_game.players[seat].hp > 1) {
            g_game.players[seat].luoyiActive = true;
            sendSystemMsg("许褚发动【裸衣】，本回合杀/决斗伤害+1");
        }
    }

    // 摸牌
    int drawCount = 2;
    // 周瑜-英姿: 多摸1张
    if (g_game.players[seat].hero == HERO_ZHOUYU) drawCount = 3;
    // 许褚-裸衣: 少摸1张
    if (g_game.players[seat].hero == HERO_XUCHU && g_game.players[seat].luoyiActive) drawCount = 1;

    // 张辽-突袭: 少摸1张，改为偷1张
    if (g_game.players[seat].hero == HERO_ZHANGLIAO) {
        drawCount = 1;
        int target = g_game.aiChooseTarget(seat, true);
        if (target >= 0 && g_game.players[target].handCount > 0) {
            int idx = rand() % g_game.players[target].handCount;
            CardInfo stolen = g_game.players[target].hand[idx];
            g_game.addCardToHand(seat, stolen);
            g_game.removeCard(target, stolen.cardId);
            sendSystemMsg("张辽发动【突袭】，从 %s 处获得一张手牌", g_game.players[target].name);
            // 发送DRAW消息同步张辽的手牌
            if (!g_isSinglePlayer) {
                std::string stealLine = "DRAW|1|" + serializeCard(stolen) + "|" + std::string(g_game.players[seat].name);
                broadcastMsg(stealLine);
            }
        }
    }

    // 构建摸牌消息
    std::string drawLine = "DRAW|" + std::to_string(drawCount) + "|";
    std::string cardStr;
    for (int i = 0; i < drawCount; i++) {
        CardInfo c = g_game.deck.drawCard();
        if (c.cardId >= 0) {
            g_game.addCardToHand(seat, c);
            if (i > 0) cardStr += ":";
            cardStr += serializeCard(c);
        }
    }
    drawLine += cardStr + "|" + std::string(g_game.players[seat].name);
    broadcastMsg(drawLine);

    // 甄姬-洛神
    if (g_game.players[seat].hero == HERO_ZHENJI) {
        for (int t = 0; t < 3; t++) {
            CardInfo judge = g_game.deck.drawCard();
            if (judge.cardId >= 0 && !isRedSuit(judge.suit)) {
                g_game.addCardToHand(seat, judge);
                sendSystemMsg("甄姬发动【洛神】，获得一张黑色判定牌");
                // 发送DRAW消息同步甄姬的手牌
                if (!g_isSinglePlayer) {
                    std::string luoshenLine = "DRAW|1|" + serializeCard(judge) + "|" + std::string(g_game.players[seat].name);
                    broadcastMsg(luoshenLine);
                }
            } else if (judge.cardId >= 0) {
                g_game.deck.discardCard(judge);
                break;
            }
        }
    }

    g_game.currentPhase = PHASE_PLAY;
    std::string phaseLine = "PHASE|" + std::to_string(PHASE_PLAY) + "|"
        + std::to_string(seat) + "|" + std::string(g_game.players[seat].name);
    broadcastMsg(phaseLine);
    g_game.players[seat].shaUsed = 0;
    g_game.players[seat].wuzhongUsed = 0;
    g_game.players[seat].zhihengUsed = 0;
    g_game.players[seat].kurouUsed = 0;
    g_game.players[seat].tiaoxinUsed = 0;
    g_game.players[seat].lijianUsed = 0;
    g_game.players[seat].rendUsed = 0;
    g_game.players[seat].luoyiActive = false;
    g_game.players[seat].guanxingUsed = 0;
    g_turnStartTime = time(NULL);

    // 乐不思蜀: 跳过出牌阶段，直接进入弃牌阶段
    if (g_game.players[seat].skipPlay) {
        g_game.players[seat].skipPlay = false;
        sendSystemMsg("%s 跳过出牌阶段!", g_game.players[seat].name);
        hostEndTurn();
        return;
    }

    // AI自动出牌
    if (g_isSinglePlayer && seat != g_mySeat) {
        processAITurn(seat);
    }
}

// ===================== 处理出牌 =====================
void hostProcessPlayCard(int seat, int cardIndex, int targetSeat) {
    if (seat < 0 || seat >= g_game.playerCount) return;
    if (!g_game.players[seat].alive || g_game.currentPhase != PHASE_PLAY) return;
    if (cardIndex < 0 || cardIndex >= g_game.players[seat].handCount) return;

    CardInfo card = g_game.players[seat].hand[cardIndex];

    // 赵云-龙胆: 出牌阶段闪当杀
    if (g_game.players[seat].hero == HERO_ZHAOYUN && card.type == CARD_SHAN
        && g_game.currentPhase == PHASE_PLAY && g_game.canUseSha(seat)) {
        card.type = CARD_SHA;
        sendSystemMsg("%s 发动【龙胆】，将闪当杀使用!", g_game.players[seat].name);
    }

    // 关羽-武圣: 红色手牌当杀使用
    if (g_game.players[seat].hero == HERO_GUANYU && isRedSuit(card.suit)
        && card.type != CARD_SHA && card.type != CARD_TAO && !isEquipCard(card.type)
        && g_game.canUseSha(seat) && g_game.currentPhase == PHASE_PLAY) {
        card.type = CARD_SHA;
        sendSystemMsg("%s 发动【武圣】，将红色手牌当杀使用!", g_game.players[seat].name);
    }

    // 甘宁-奇袭: 黑色手牌当过河拆桥使用
    if (g_game.players[seat].hero == HERO_GANNING && !isRedSuit(card.suit)
        && card.type != CARD_SHA && card.type != CARD_SHAN && !isEquipCard(card.type)
        && g_game.currentPhase == PHASE_PLAY) {
        card.type = CARD_GUOHE;
        sendSystemMsg("%s 发动【奇袭】，将黑色手牌当过河拆桥使用!", g_game.players[seat].name);
    }

    // 大乔-国色: 方块牌当乐不思蜀使用
    if (g_game.players[seat].hero == HERO_DAQIAO && card.suit == SUIT_DIAMOND
        && card.type != CARD_SHA && card.type != CARD_SHAN && card.type != CARD_TAO
        && !isEquipCard(card.type) && g_game.currentPhase == PHASE_PLAY) {
        card.type = CARD_LEBUSI;
        sendSystemMsg("%s 发动【国色】，将方块牌当乐不思蜀使用!", g_game.players[seat].name);
    }

    // 装备牌直接装备
    if (isEquipCard(card.type)) {
        g_game.removeCard(seat, card.cardId);
        g_game.equipCard(seat, card);
        std::string equipLine = "EQUIP|" + std::to_string(card.type) + "|"
            + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
            + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
            + std::string(g_game.players[seat].name);
        broadcastMsg(equipLine);
        // 装备后检查游戏是否结束
        if (g_game.gameOver) {
            std::string winnerName = "主公方";
            if (g_game.winnerFaction == 1) winnerName = "反贼方";
            else if (g_game.winnerFaction == 2) winnerName = "内奸";
            broadcastMsg("GAME_OVER|" + std::to_string(g_game.winnerFaction) + "|" + winnerName);
        }
        return;
    }

    switch (card.type) {
        case CARD_SHA: {
            if (!g_game.canUseSha(seat)) { if (seat == g_mySeat) printf("[系统] 本回合已使用过杀!\n"); return; }
            if (targetSeat < 0 || !g_game.players[targetSeat].alive || targetSeat == seat) {
                if (seat == g_mySeat) printf("[系统] 无效目标!\n");
                return;
            }
            int range = g_game.getAttackRange(seat);
            if (g_game.players[seat].hero != HERO_HUANGZHONG && g_game.players[seat].equips[EQ_WEAPON].type != CARD_EQ_QIXING) {
                if (g_game.getDistance(seat, targetSeat) > range) {
                    if (seat == g_mySeat) printf("[系统] 距离不够! (距离%d, 攻击范围%d)\n", g_game.getDistance(seat, targetSeat), range);
                    return;
                }
            }
            // 青釭剑无视防具
            bool ignoreArmor = (g_game.players[seat].equips[EQ_WEAPON].type == CARD_EQ_QINGGANG);
            // 仁王盾判定
            if (!ignoreArmor && g_game.players[targetSeat].equips[EQ_ARMOR].type == CARD_EQ_RENJIE && !isRedSuit(card.suit)) {
                if (seat == g_mySeat) printf("[系统] 仁王盾抵消了黑色杀!\n");
                return;
            }
            g_game.players[seat].shaUsed++;
            g_game.removeCard(seat, card.cardId);
            g_game.deck.discardCard(card);

            // 方天画戟: 若杀为最后一张手牌，可指定最多3个目标
            bool fangtianMulti = (g_game.players[seat].equips[EQ_WEAPON].type == CARD_EQ_FANGTIAN
                                  && g_game.players[seat].handCount == 0);
            if (fangtianMulti) {
                sendSystemMsg("%s 发动【方天画戟】，杀指定多个目标!", g_game.players[seat].name);
            }

            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(targetSeat) + "|" + std::string(g_game.players[seat].name) + "|"
                + std::string(g_game.players[targetSeat].name);
            broadcastMsg(playLine);

            // 方天画戟: 额外选择最多2个目标(简化: AI自动选择)
            int extraTargets[2] = {-1, -1};
            if (fangtianMulti) {
                int found = 0;
                for (int i = 0; i < g_game.playerCount && found < 2; i++) {
                    if (i == seat || i == targetSeat || !g_game.players[i].alive) continue;
                    if (g_game.getDistance(seat, i) <= range
                        && g_game.players[i].identity != g_game.players[seat].identity) {
                        extraTargets[found++] = i;
                    }
                }
            }

            // 记录伤害牌(用于曹操奸雄)
            g_lastDamageCard = card;
            g_lastDamageSource = seat;

            // 马超-铁骑: 杀指定目标后判定，红色则不可出闪
            bool maChaoForceHit = false;
            if (g_game.players[seat].hero == HERO_MACHAO) {
                CardInfo judge = g_game.deck.drawCard();
                if (judge.cardId >= 0) {
                    g_game.deck.discardCard(judge);
                    if (isRedSuit(judge.suit)) {
                        maChaoForceHit = true;
                        sendSystemMsg("马超发动【铁骑】，判定为红色，目标不可出闪!");
                    }
                }
            }

            // 黄忠-烈弓: 目标手牌数不大于自己时可强制命中
            bool huangzhongForceHit = false;
            if (g_game.players[seat].hero == HERO_HUANGZHONG
                && g_game.players[targetSeat].handCount <= g_game.players[seat].handCount) {
                huangzhongForceHit = true;
                sendSystemMsg("黄忠发动【烈弓】，强制命中!");
            }

            // 八卦阵判定 (马超铁骑/黄忠烈弓强制命中时跳过, 青釭剑无视防具)
            if (!maChaoForceHit && !huangzhongForceHit && !ignoreArmor && g_game.baguaJudge(targetSeat)) {
                sendSystemMsg("%s 的八卦阵判定成功，视为出闪!", g_game.players[targetSeat].name);
                break;
            }

            // 强制命中(马超铁骑/黄忠烈弓)，直接造成伤害
            if (maChaoForceHit || huangzhongForceHit) {
                RespondMsg rm; memset(&rm, 0, sizeof(rm));
                rm.responded = false; rm.seatId = targetSeat;
                strncpy(rm.playerName, g_game.players[targetSeat].name, kMaxNameLen - 1);
                broadcastMsg("RESPOND|0|0|0|0|0|" + std::to_string(rm.seatId) + "|" + std::string(rm.playerName));
                int dmg = (g_game.players[seat].luoyiActive) ? 2 : 1;
                g_lastDamageCard = card;
                int hpBefore = g_game.players[targetSeat].hp;
                g_game.dealDamage(targetSeat, dmg, seat);
                if (g_game.players[targetSeat].hp < hpBefore) {
                    sendSystemMsg("%s 受到 %d 点伤害! (HP:%d/%d)", g_game.players[targetSeat].name, dmg,
                                 g_game.players[targetSeat].hp, g_game.players[targetSeat].maxHp);
                } else if (g_game.players[targetSeat].alive) {
                    sendSystemMsg("%s 的伤害被转移或抵消! (HP:%d/%d)", g_game.players[targetSeat].name,
                                 g_game.players[targetSeat].hp, g_game.players[targetSeat].maxHp);
                }
                break;
            }

            // 房主被攻击时，也需要等待响应（通过广播ASK_DODGE给所有客户端，房主自己在gameLoop中处理）
            if (targetSeat == g_mySeat && !g_isSinglePlayer) {
                std::string askLine = "ASK_DODGE|" + std::to_string(seat) + "|"
                    + std::to_string(targetSeat) + "|" + std::string(g_game.players[seat].name) + "|"
                    + std::string(g_game.players[targetSeat].name);
                broadcastMsg(askLine);
                g_waitingForResponse = true; g_responseType = CARD_SHAN; g_responseFrom = seat;
                return; // 等待玩家输入（房主在gameLoop中响应）
            }

            // AI/远程自动处理（单机AI或被攻击的是其他客户端）
            if (targetSeat != g_mySeat || g_isSinglePlayer) {
                bool dodged = false;
                // 吕布-无双: 需要两张闪才能抵消
                int shanNeeded = (g_game.players[seat].hero == HERO_LVBU) ? 2 : 1;
                int shanUsed = 0;
                // 优先出闪
                while (shanUsed < shanNeeded) {
                    bool foundShan = false;
                    for (int i = 0; i < g_game.players[targetSeat].handCount; i++) {
                        if (g_game.players[targetSeat].hand[i].type == CARD_SHAN) {
                            RespondMsg rm; memset(&rm, 0, sizeof(rm));
                            rm.responded = true; rm.card = g_game.players[targetSeat].hand[i];
                            rm.seatId = targetSeat;
                            strncpy(rm.playerName, g_game.players[targetSeat].name, kMaxNameLen - 1);
                            g_game.removeCard(targetSeat, rm.card.cardId);
                            g_game.deck.discardCard(rm.card);
                            broadcastMsg("RESPOND|1|" + std::to_string(rm.card.type) + "|" + std::to_string(rm.card.suit) + "|" + std::to_string(rm.card.number) + "|" + std::to_string(rm.card.cardId) + "|" + std::to_string(rm.seatId) + "|" + std::string(rm.playerName));
                            shanUsed++; foundShan = true;
                            break;
                        }
                    }
                    // 赵云-龙胆: 杀当闪
                    if (!foundShan && g_game.players[targetSeat].hero == HERO_ZHAOYUN) {
                        for (int i = 0; i < g_game.players[targetSeat].handCount; i++) {
                            if (g_game.players[targetSeat].hand[i].type == CARD_SHA) {
                                RespondMsg rm; memset(&rm, 0, sizeof(rm));
                                rm.responded = true; rm.card = g_game.players[targetSeat].hand[i];
                                rm.seatId = targetSeat;
                                strncpy(rm.playerName, g_game.players[targetSeat].name, kMaxNameLen - 1);
                                g_game.removeCard(targetSeat, rm.card.cardId);
                                g_game.deck.discardCard(rm.card);
                                broadcastMsg("RESPOND|1|" + std::to_string(rm.card.type) + "|" + std::to_string(rm.card.suit) + "|" + std::to_string(rm.card.number) + "|" + std::to_string(rm.card.cardId) + "|" + std::to_string(rm.seatId) + "|" + std::string(rm.playerName));
                                sendSystemMsg("%s 发动【龙胆】，将杀当闪打出!", g_game.players[targetSeat].name);
                                shanUsed++; foundShan = true;
                                break;
                            }
                        }
                    }
                    // 关羽-武圣: 红色当杀(不能当闪，武圣仅限杀)
                    // 赵云-龙胆已在上方处理(杀当闪)
                    // 华佗-急救: 红色当桃(不能当闪)
                    if (!foundShan) break; // 没有更多闪了
                }
                if (shanUsed >= shanNeeded) dodged = true;

                // 贯石斧: 杀被闪抵消时，可弃两张牌强制命中
                if (dodged && g_game.players[seat].equips[EQ_WEAPON].type == CARD_EQ_GUANSHI
                    && g_game.players[seat].handCount >= 2) {
                    // AI自动弃2张牌强制命中(选择最不重要的牌)
                    // 先弃第一张: 优先弃非杀非闪非桃的牌
                    int dIdx = -1;
                    for (int i = 0; i < g_game.players[seat].handCount; i++) {
                        int t = g_game.players[seat].hand[i].type;
                        if (t != CARD_SHA && t != CARD_SHAN && t != CARD_TAO) { dIdx = i; break; }
                    }
                    if (dIdx < 0) dIdx = 0;
                    g_game.deck.discardCard(g_game.players[seat].hand[dIdx]);
                    g_game.removeCard(seat, g_game.players[seat].hand[dIdx].cardId);
                    // 再弃第二张
                    if (g_game.players[seat].handCount > 0) {
                        dIdx = 0;
                        for (int i = 0; i < g_game.players[seat].handCount; i++) {
                            int t = g_game.players[seat].hand[i].type;
                            if (t != CARD_SHA && t != CARD_SHAN && t != CARD_TAO) { dIdx = i; break; }
                        }
                        g_game.deck.discardCard(g_game.players[seat].hand[dIdx]);
                        g_game.removeCard(seat, g_game.players[seat].hand[dIdx].cardId);
                    }
                    dodged = false; // 强制命中
                    sendSystemMsg("%s 发动贯石斧效果，弃两张牌强制命中!", g_game.players[seat].name);
                }

                // 青龙偃月刀: 杀被闪抵消后，可对另一目标再出一张杀
                if (dodged && g_game.players[seat].equips[EQ_WEAPON].type == CARD_EQ_QINGLONG) {
                    // 检查是否有另一张杀
                    int shaIdx = -1;
                    for (int i = 0; i < g_game.players[seat].handCount; i++) {
                        if (g_game.players[seat].hand[i].type == CARD_SHA) { shaIdx = i; break; }
                    }
                    // 赵云龙胆: 闪当杀
                    if (shaIdx < 0 && g_game.players[seat].hero == HERO_ZHAOYUN) {
                        for (int i = 0; i < g_game.players[seat].handCount; i++) {
                            if (g_game.players[seat].hand[i].type == CARD_SHAN) { shaIdx = i; break; }
                        }
                    }
                    // 关羽武圣: 红色当杀
                    if (shaIdx < 0 && g_game.players[seat].hero == HERO_GUANYU) {
                        for (int i = 0; i < g_game.players[seat].handCount; i++) {
                            if (isRedSuit(g_game.players[seat].hand[i].suit)
                                && g_game.players[seat].hand[i].type != CARD_SHA
                                && g_game.players[seat].hand[i].type != CARD_TAO
                                && !isEquipCard(g_game.players[seat].hand[i].type)) { shaIdx = i; break; }
                        }
                    }
                    if (shaIdx >= 0) {
                        // 选择另一个目标
                        int newTarget = -1;
                        int range = g_game.getAttackRange(seat);
                        for (int i = 0; i < g_game.playerCount; i++) {
                            if (i == seat || i == targetSeat || !g_game.players[i].alive) continue;
                            if (g_game.getDistance(seat, i) <= range
                                && g_game.players[i].identity != g_game.players[seat].identity) {
                                newTarget = i; break;
                            }
                        }
                        if (newTarget >= 0) {
                            // 出杀
                            CardInfo shaCard = g_game.players[seat].hand[shaIdx];
                            g_game.deck.discardCard(shaCard);
                            g_game.removeCard(seat, shaCard.cardId);
                            sendSystemMsg("%s 发动【青龙偃月刀】，对 %s 再出一张杀!",
                                         g_game.players[seat].name, g_game.players[newTarget].name);
                            // 简化: 检查新目标能否闪
                            bool newDodged = false;
                            for (int i = 0; i < g_game.players[newTarget].handCount; i++) {
                                if (g_game.players[newTarget].hand[i].type == CARD_SHAN) {
                                    g_game.deck.discardCard(g_game.players[newTarget].hand[i]);
                                    g_game.removeCard(newTarget, g_game.players[newTarget].hand[i].cardId);
                                    newDodged = true;
                                    sendSystemMsg("%s 出闪抵消了杀!", g_game.players[newTarget].name);
                                    break;
                                }
                            }
                            if (!newDodged) {
                                int dmg2 = (g_game.players[seat].luoyiActive) ? 2 : 1;
                                g_lastDamageCard.cardId = 0;
                                g_game.dealDamage(newTarget, dmg2, seat);
                                sendSystemMsg("%s 受到 %d 点伤害!", g_game.players[newTarget].name, dmg2);
                            }
                        }
                    }
                }

                if (!dodged) {
                    // 没闪，受到伤害
                    RespondMsg rm; memset(&rm, 0, sizeof(rm));
                    rm.responded = false; rm.seatId = targetSeat;
                    strncpy(rm.playerName, g_game.players[targetSeat].name, kMaxNameLen - 1);
                    broadcastMsg("RESPOND|0|0|0|0|0|" + std::to_string(rm.seatId) + "|" + std::string(rm.playerName));
                    int dmg = (g_game.players[seat].luoyiActive) ? 2 : 1;
                    g_lastDamageCard = card;
                    int hpBefore = g_game.players[targetSeat].hp;
                    g_game.dealDamage(targetSeat, dmg, seat);
                    // 检查伤害是否被转移(天香)或被桃救回
                    if (g_game.players[targetSeat].hp < hpBefore) {
                        sendSystemMsg("%s 受到 %d 点伤害! (HP:%d/%d)", g_game.players[targetSeat].name, dmg,
                                     g_game.players[targetSeat].hp, g_game.players[targetSeat].maxHp);
                    } else if (g_game.players[targetSeat].alive) {
                        sendSystemMsg("%s 的伤害被转移或抵消! (HP:%d/%d)", g_game.players[targetSeat].name,
                                     g_game.players[targetSeat].hp, g_game.players[targetSeat].maxHp);
                    }
                }
            }
            // 方天画戟: 对额外目标造成简化杀效果
            for (int et = 0; et < 2; et++) {
                if (extraTargets[et] < 0) break;
                int etSeat = extraTargets[et];
                if (!g_game.players[etSeat].alive) continue;
                sendSystemMsg("%s 的方天画戟额外命中 %s!", g_game.players[seat].name, g_game.players[etSeat].name);
                // 简化: 检查能否闪
                bool etDodged = false;
                for (int i = 0; i < g_game.players[etSeat].handCount; i++) {
                    if (g_game.players[etSeat].hand[i].type == CARD_SHAN) {
                        g_game.deck.discardCard(g_game.players[etSeat].hand[i]);
                        g_game.removeCard(etSeat, g_game.players[etSeat].hand[i].cardId);
                        etDodged = true;
                        sendSystemMsg("%s 出闪抵消了杀!", g_game.players[etSeat].name);
                        break;
                    }
                }
                if (!etDodged) {
                    int etDmg = (g_game.players[seat].luoyiActive) ? 2 : 1;
                    g_lastDamageCard.cardId = 0;
                    g_game.dealDamage(etSeat, etDmg, seat);
                    sendSystemMsg("%s 受到 %d 点伤害!", g_game.players[etSeat].name, etDmg);
                }
            }
            break;
        }
        case CARD_TAO: {
            if (g_game.players[seat].hp >= g_game.players[seat].maxHp) {
                if (seat == g_mySeat) printf("[系统] 体力已满!\n");
                return;
            }
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            g_game.players[seat].hp++;
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(-1) + "|" + std::string(g_game.players[seat].name) + "|none";
            broadcastMsg(playLine);
            sendSystemMsg("%s 使用桃回复体力 (HP:%d/%d)", g_game.players[seat].name,
                         g_game.players[seat].hp, g_game.players[seat].maxHp);
            break;
        }
        case CARD_WUZHONG: {
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(-1) + "|" + std::string(g_game.players[seat].name) + "|none";
            broadcastMsg(playLine);
            // 摸2张
            std::string drawLine = "DRAW|2|";
            std::string cardStr;
            for (int i = 0; i < 2; i++) {
                CardInfo c = g_game.deck.drawCard();
                if (c.cardId >= 0) {
                    g_game.addCardToHand(seat, c);
                    if (i > 0) cardStr += ":";
                    cardStr += serializeCard(c);
                }
            }
            drawLine += cardStr + "|" + std::string(g_game.players[seat].name);
            broadcastMsg(drawLine);
            break;
        }
        case CARD_JUEDOU: {
            if (targetSeat < 0 || !g_game.players[targetSeat].alive || targetSeat == seat) {
                if (seat == g_mySeat) printf("[系统] 无效目标!\n");
                return;
            }
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(targetSeat) + "|" + std::string(g_game.players[seat].name) + "|"
                + std::string(g_game.players[targetSeat].name);
            broadcastMsg(playLine);
            // 吕布-无双: 决斗时对方需出两张杀
            int tgtShaNeeded = (g_game.players[seat].hero == HERO_LVBU) ? 2 : 1;
            int srcShaNeeded = (g_game.players[targetSeat].hero == HERO_LVBU) ? 2 : 1;
            // 计算可用杀数(考虑武将技能: 赵云龙胆-闪当杀, 关羽武圣-红色当杀)
            int tgtShaCount = g_game.countCardType(targetSeat, CARD_SHA);
            if (g_game.players[targetSeat].hero == HERO_ZHAOYUN) tgtShaCount += g_game.countCardType(targetSeat, CARD_SHAN);
            if (g_game.players[targetSeat].hero == HERO_GUANYU) {
                for (int h = 0; h < g_game.players[targetSeat].handCount; h++)
                    if (isRedSuit(g_game.players[targetSeat].hand[h].suit)
                        && g_game.players[targetSeat].hand[h].type != CARD_SHA && g_game.players[targetSeat].hand[h].type != CARD_TAO
                        && !isEquipCard(g_game.players[targetSeat].hand[h].type)) tgtShaCount++;
            }
            int srcShaCount = g_game.countCardType(seat, CARD_SHA);
            if (g_game.players[seat].hero == HERO_ZHAOYUN) srcShaCount += g_game.countCardType(seat, CARD_SHAN);
            if (g_game.players[seat].hero == HERO_GUANYU) {
                for (int h = 0; h < g_game.players[seat].handCount; h++)
                    if (isRedSuit(g_game.players[seat].hand[h].suit)
                        && g_game.players[seat].hand[h].type != CARD_SHA && g_game.players[seat].hand[h].type != CARD_TAO
                        && !isEquipCard(g_game.players[seat].hand[h].type)) srcShaCount++;
            }
            // 决斗: 双方轮流出杀，先无法出杀的一方受到伤害
            g_lastDamageCard = card;
            if (tgtShaCount < tgtShaNeeded) {
                // 目标无法出足够的杀
                int dmg = (g_game.players[seat].luoyiActive) ? 2 : 1;
                g_game.dealDamage(targetSeat, dmg, seat);
                sendSystemMsg("%s 决斗落败，受到%d点伤害!", g_game.players[targetSeat].name, dmg);
            } else if (srcShaCount < srcShaNeeded) {
                int dmg = (g_game.players[targetSeat].luoyiActive) ? 2 : 1;
                g_game.dealDamage(seat, dmg, targetSeat);
                sendSystemMsg("%s 决斗落败，受到%d点伤害!", g_game.players[seat].name, dmg);
            } else {
                // 双方都有足够的杀，交替出杀直到一方用完
                int tgtRemaining = tgtShaCount;
                int srcRemaining = srcShaCount;
                while (true) {
                    // 目标先出杀(消耗tgtShaNeeded张)
                    if (tgtRemaining < tgtShaNeeded) {
                        int dmg = (g_game.players[seat].luoyiActive) ? 2 : 1;
                        g_game.dealDamage(targetSeat, dmg, seat);
                        sendSystemMsg("%s 决斗落败!", g_game.players[targetSeat].name);
                        break;
                    }
                    for (int x = 0; x < tgtShaNeeded; x++) {
                        // 优先出真杀，其次赵云龙胆(闪当杀)，最后关羽武圣(红色当杀)
                        int j = -1;
                        for (int k = 0; k < g_game.players[targetSeat].handCount; k++) {
                            if (g_game.players[targetSeat].hand[k].type == CARD_SHA) { j = k; break; }
                        }
                        if (j < 0 && g_game.players[targetSeat].hero == HERO_ZHAOYUN) {
                            for (int k = 0; k < g_game.players[targetSeat].handCount; k++) {
                                if (g_game.players[targetSeat].hand[k].type == CARD_SHAN) { j = k; break; }
                            }
                        }
                        if (j < 0 && g_game.players[targetSeat].hero == HERO_GUANYU) {
                            for (int k = 0; k < g_game.players[targetSeat].handCount; k++) {
                                if (isRedSuit(g_game.players[targetSeat].hand[k].suit)
                                    && g_game.players[targetSeat].hand[k].type != CARD_SHA
                                    && g_game.players[targetSeat].hand[k].type != CARD_TAO
                                    && !isEquipCard(g_game.players[targetSeat].hand[k].type)) { j = k; break; }
                            }
                        }
                        if (j >= 0) {
                            g_game.deck.discardCard(g_game.players[targetSeat].hand[j]);
                            g_game.removeCard(targetSeat, g_game.players[targetSeat].hand[j].cardId);
                            tgtRemaining--;
                        } else { tgtRemaining = 0; }
                    }
                    // 源出杀(消耗srcShaNeeded张)
                    if (srcRemaining < srcShaNeeded) {
                        int dmg = (g_game.players[targetSeat].luoyiActive) ? 2 : 1;
                        g_game.dealDamage(seat, dmg, targetSeat);
                        sendSystemMsg("%s 决斗落败!", g_game.players[seat].name);
                        break;
                    }
                    for (int x = 0; x < srcShaNeeded; x++) {
                        int j = -1;
                        for (int k = 0; k < g_game.players[seat].handCount; k++) {
                            if (g_game.players[seat].hand[k].type == CARD_SHA) { j = k; break; }
                        }
                        if (j < 0 && g_game.players[seat].hero == HERO_ZHAOYUN) {
                            for (int k = 0; k < g_game.players[seat].handCount; k++) {
                                if (g_game.players[seat].hand[k].type == CARD_SHAN) { j = k; break; }
                            }
                        }
                        if (j < 0 && g_game.players[seat].hero == HERO_GUANYU) {
                            for (int k = 0; k < g_game.players[seat].handCount; k++) {
                                if (isRedSuit(g_game.players[seat].hand[k].suit)
                                    && g_game.players[seat].hand[k].type != CARD_SHA
                                    && g_game.players[seat].hand[k].type != CARD_TAO
                                    && !isEquipCard(g_game.players[seat].hand[k].type)) { j = k; break; }
                            }
                        }
                        if (j >= 0) {
                            g_game.deck.discardCard(g_game.players[seat].hand[j]);
                            g_game.removeCard(seat, g_game.players[seat].hand[j].cardId);
                            srcRemaining--;
                        } else { srcRemaining = 0; }
                    }
                }
            }
            break;
        }
        case CARD_NANMAN: {
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(-1) + "|" + std::string(g_game.players[seat].name) + "|none";
            broadcastMsg(playLine);
            g_lastDamageCard = card;
            for (int i = 0; i < g_game.playerCount; i++) {
                if (i != seat && g_game.players[i].alive) {
                    // 赵云龙胆: 闪可以当杀响应南蛮
                    bool hasShaResp = g_game.hasCardType(i, CARD_SHA);
                    if (!hasShaResp && g_game.players[i].hero == HERO_ZHAOYUN && g_game.hasCardType(i, CARD_SHAN))
                        hasShaResp = true;
                    if (!hasShaResp) {
                        g_game.dealDamage(i, 1, seat);
                        sendSystemMsg("%s 未出杀，受到南蛮入侵伤害!", g_game.players[i].name);
                    } else {
                        // 优先出真杀，其次赵云龙胆(闪当杀)
                        int j = -1;
                        for (int k = 0; k < g_game.players[i].handCount; k++) {
                            if (g_game.players[i].hand[k].type == CARD_SHA) { j = k; break; }
                        }
                        if (j < 0 && g_game.players[i].hero == HERO_ZHAOYUN) {
                            for (int k = 0; k < g_game.players[i].handCount; k++) {
                                if (g_game.players[i].hand[k].type == CARD_SHAN) { j = k; break; }
                            }
                        }
                        if (j >= 0) {
                            if (g_game.players[i].hand[j].type == CARD_SHAN)
                                sendSystemMsg("%s 发动【龙胆】，将闪当杀打出!", g_game.players[i].name);
                            g_game.deck.discardCard(g_game.players[i].hand[j]);
                            g_game.removeCard(i, g_game.players[i].hand[j].cardId);
                        }
                    }
                }
            }
            break;
        }
        case CARD_WANJIAN: {
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(-1) + "|" + std::string(g_game.players[seat].name) + "|none";
            broadcastMsg(playLine);
            g_lastDamageCard = card;
            for (int i = 0; i < g_game.playerCount; i++) {
                if (i != seat && g_game.players[i].alive) {
                    // 八卦阵判定
                    if (g_game.baguaJudge(i)) {
                        sendSystemMsg("%s 的八卦阵判定成功，视为出闪!", g_game.players[i].name);
                        continue;
                    }
                    // 赵云龙胆: 杀可以当闪响应万箭
                    bool hasShanResp = g_game.hasCardType(i, CARD_SHAN);
                    if (!hasShanResp && g_game.players[i].hero == HERO_ZHAOYUN && g_game.hasCardType(i, CARD_SHA))
                        hasShanResp = true;
                    if (!hasShanResp) {
                        g_game.dealDamage(i, 1, seat);
                        sendSystemMsg("%s 未出闪，受到万箭齐发伤害!", g_game.players[i].name);
                    } else {
                        // 优先出真闪，其次赵云龙胆(杀当闪)
                        int j = -1;
                        for (int k = 0; k < g_game.players[i].handCount; k++) {
                            if (g_game.players[i].hand[k].type == CARD_SHAN) { j = k; break; }
                        }
                        if (j < 0 && g_game.players[i].hero == HERO_ZHAOYUN) {
                            for (int k = 0; k < g_game.players[i].handCount; k++) {
                                if (g_game.players[i].hand[k].type == CARD_SHA) { j = k; break; }
                            }
                        }
                        if (j >= 0) {
                            if (g_game.players[i].hand[j].type == CARD_SHA)
                                sendSystemMsg("%s 发动【龙胆】，将杀当闪打出!", g_game.players[i].name);
                            g_game.deck.discardCard(g_game.players[i].hand[j]);
                            g_game.removeCard(i, g_game.players[i].hand[j].cardId);
                        }
                    }
                }
            }
            break;
        }
        case CARD_GUOHE: {
            if (targetSeat < 0 || !g_game.players[targetSeat].alive || targetSeat == seat) {
                if (seat == g_mySeat) printf("[系统] 无效目标!\n");
                return;
            }
            if (!g_game.canBeStolen(targetSeat)) {
                if (seat == g_mySeat) printf("[系统] 绝影免疫!\n");
                return;
            }
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(targetSeat) + "|" + std::string(g_game.players[seat].name) + "|"
                + std::string(g_game.players[targetSeat].name);
            broadcastMsg(playLine);
            // 过河拆桥: 优先拆乐不思蜀标记，其次拆装备，最后拆手牌
            bool dismantled = false;
            // 优先拆乐不思蜀
            if (g_game.players[targetSeat].lebusiTarget) {
                g_game.players[targetSeat].lebusiTarget = false;
                sendSystemMsg("%s 拆掉了 %s 的乐不思蜀!", g_game.players[seat].name, g_game.players[targetSeat].name);
                dismantled = true;
            }
            // 其次拆装备(随机选一个装备槽)
            if (!dismantled) {
                int equipSlots[EQ_MAX]; int ec = 0;
                for (int e = 0; e < EQ_MAX; e++)
                    if (g_game.players[targetSeat].equips[e].cardId > 0) equipSlots[ec++] = e;
                if (ec > 0) {
                    int sel = equipSlots[rand() % ec];
                    g_game.deck.discardCard(g_game.players[targetSeat].equips[sel]);
                    g_game.players[targetSeat].equips[sel].cardId = 0;
                    sendSystemMsg("%s 拆掉了 %s 的一张装备", g_game.players[seat].name, g_game.players[targetSeat].name);
                    dismantled = true;
                }
            }
            // 最后拆手牌
            if (!dismantled && g_game.players[targetSeat].handCount > 0) {
                int idx = rand() % g_game.players[targetSeat].handCount;
                g_game.deck.discardCard(g_game.players[targetSeat].hand[idx]);
                g_game.removeCard(targetSeat, g_game.players[targetSeat].hand[idx].cardId);
                sendSystemMsg("%s 拆掉了 %s 的一张牌", g_game.players[seat].name, g_game.players[targetSeat].name);
            }
            break;
        }
        case CARD_SHUNQIAN: {
            if (targetSeat < 0 || !g_game.players[targetSeat].alive || targetSeat == seat) {
                if (seat == g_mySeat) printf("[系统] 无效目标!\n");
                return;
            }
            // 顺手牵羊: 距离必须为1
            if (g_game.getDistance(seat, targetSeat) > 1) {
                if (seat == g_mySeat) printf("[系统] 距离不够! 顺手牵羊需要距离1 (当前距离%d)\n", g_game.getDistance(seat, targetSeat));
                return;
            }
            if (!g_game.canBeStolen(targetSeat)) {
                if (seat == g_mySeat) printf("[系统] 绝影免疫!\n");
                return;
            }
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(targetSeat) + "|" + std::string(g_game.players[seat].name) + "|"
                + std::string(g_game.players[targetSeat].name);
            broadcastMsg(playLine);
            if (g_game.players[targetSeat].handCount > 0) {
                int idx = rand() % g_game.players[targetSeat].handCount;
                CardInfo stolen = g_game.players[targetSeat].hand[idx];
                g_game.addCardToHand(seat, stolen);
                g_game.removeCard(targetSeat, stolen.cardId);
                sendSystemMsg("%s 从 %s 处偷走了一张牌", g_game.players[seat].name, g_game.players[targetSeat].name);
                // 发送DRAW消息同步偷牌者的手牌
                if (!g_isSinglePlayer) {
                    std::string stealLine = "DRAW|1|" + serializeCard(stolen) + "|" + std::string(g_game.players[seat].name);
                    broadcastMsg(stealLine);
                }
            }
            break;
        }
        case CARD_TAOYUAN: {
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(-1) + "|" + std::string(g_game.players[seat].name) + "|none";
            broadcastMsg(playLine);
            for (int i = 0; i < g_game.playerCount; i++)
                if (g_game.players[i].alive && g_game.players[i].hp < g_game.players[i].maxHp) g_game.players[i].hp++;
            sendSystemMsg("桃园结义! 所有存活角色回复1点体力");
            break;
        }
        case CARD_WUGU: {
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(-1) + "|" + std::string(g_game.players[seat].name) + "|none";
            broadcastMsg(playLine);
            for (int i = 0; i < g_game.playerCount; i++) {
                if (g_game.players[i].alive) {
                    CardInfo c = g_game.deck.drawCard();
                    if (c.cardId >= 0) {
                        g_game.addCardToHand(i, c);
                        // 单独发送DRAW消息给每位玩家
                        std::string wuguDraw = "DRAW|1|" + serializeCard(c) + "|" + std::string(g_game.players[i].name);
                        broadcastMsg(wuguDraw);
                    }
                }
            }
            sendSystemMsg("五谷丰登! 所有存活角色各摸1张牌");
            break;
        }
        case CARD_HUOGONG: {
            if (targetSeat < 0 || !g_game.players[targetSeat].alive || targetSeat == seat) {
                if (seat == g_mySeat) printf("[系统] 无效目标!\n");
                return;
            }
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(targetSeat) + "|" + std::string(g_game.players[seat].name) + "|"
                + std::string(g_game.players[targetSeat].name);
            broadcastMsg(playLine);
            // 火攻规则: 目标展示一张手牌，攻击者需弃同花色牌
            if (g_game.players[targetSeat].handCount > 0) {
                int revealIdx = rand() % g_game.players[targetSeat].handCount;
                int targetSuit = g_game.players[targetSeat].hand[revealIdx].suit;
                // 攻击者需要弃同花色牌
                bool hasMatching = false;
                int matchIdx = -1;
                for (int h = 0; h < g_game.players[seat].handCount; h++) {
                    if (g_game.players[seat].hand[h].suit == targetSuit) {
                        hasMatching = true; matchIdx = h; break;
                    }
                }
                if (hasMatching && matchIdx >= 0) {
                    g_game.deck.discardCard(g_game.players[seat].hand[matchIdx]);
                    g_game.removeCard(seat, g_game.players[seat].hand[matchIdx].cardId);
                    g_lastDamageCard = card;
                    g_game.dealDamage(targetSeat, 1, seat);
                    sendSystemMsg("火攻成功! %s 对 %s 造成1点伤害!", g_game.players[seat].name, g_game.players[targetSeat].name);
                } else {
                    sendSystemMsg("火攻失败! %s 没有 %s 花色的牌", g_game.players[seat].name, getSuitSymbol(targetSuit));
                }
            } else {
                sendSystemMsg("火攻失败! %s 没有手牌", g_game.players[targetSeat].name);
            }
            break;
        }
        case CARD_TIEJI: {
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(targetSeat >= 0 ? targetSeat : -1) + "|" + std::string(g_game.players[seat].name) + "|none";
            broadcastMsg(playLine);
            if (targetSeat >= 0) g_game.players[targetSeat].tiejiLinked++;
            sendSystemMsg("%s 使用铁索连环!", g_game.players[seat].name);
            break;
        }
        case CARD_LEBUSI: {
            if (targetSeat < 0 || !g_game.players[targetSeat].alive || targetSeat == seat) {
                if (seat == g_mySeat) printf("[系统] 无效目标!\n");
                return;
            }
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(targetSeat) + "|" + std::string(g_game.players[seat].name) + "|"
                + std::string(g_game.players[targetSeat].name);
            broadcastMsg(playLine);
            g_game.players[targetSeat].lebusiTarget = true;
            sendSystemMsg("%s 对 %s 使用了乐不思蜀!", g_game.players[seat].name, g_game.players[targetSeat].name);
            break;
        }
        case CARD_JUEDAO: {
            // 借刀杀人: 令有武器的角色对另一角色出杀，不出杀则获得其武器
            if (targetSeat < 0 || !g_game.players[targetSeat].alive || targetSeat == seat) {
                if (seat == g_mySeat) printf("[系统] 无效目标!\n");
                return;
            }
            // 目标必须有武器
            if (g_game.players[targetSeat].equips[EQ_WEAPON].cardId <= 0) {
                if (seat == g_mySeat) printf("[系统] 目标没有武器，无法借刀杀人!\n");
                return;
            }
            g_game.removeCard(seat, card.cardId); g_game.deck.discardCard(card);
            std::string playLine = "PLAY|" + std::to_string(card.type) + "|"
                + std::to_string(card.suit) + "|" + std::to_string(card.number) + "|"
                + std::to_string(card.cardId) + "|" + std::to_string(seat) + "|"
                + std::to_string(targetSeat) + "|" + std::string(g_game.players[seat].name) + "|"
                + std::string(g_game.players[targetSeat].name);
            broadcastMsg(playLine);
            // AI自动选择杀的目标(人类玩家简化处理: AI帮选或目标不出杀)
            int shaTarget = -1;
            // 所有被借刀的角色都由AI自动选目标
            shaTarget = g_game.aiChooseTarget(targetSeat, CARD_SHA);
            // 检查目标是否能出杀
            bool hasSha = g_game.hasCardType(targetSeat, CARD_SHA);
            if (g_game.players[targetSeat].hero == HERO_ZHAOYUN && g_game.hasCardType(targetSeat, CARD_SHAN)) hasSha = true;
            if (g_game.players[targetSeat].hero == HERO_GUANYU) {
                for (int i = 0; i < g_game.players[targetSeat].handCount; i++) {
                    if (isRedSuit(g_game.players[targetSeat].hand[i].suit)
                        && g_game.players[targetSeat].hand[i].type != CARD_SHA
                        && g_game.players[targetSeat].hand[i].type != CARD_TAO
                        && !isEquipCard(g_game.players[targetSeat].hand[i].type)) { hasSha = true; break; }
                }
            }
            if (hasSha && shaTarget >= 0 && g_game.players[shaTarget].alive) {
                // 出杀: 移除杀牌并对目标造成杀的效果
                int shaIdx = -1;
                for (int i = 0; i < g_game.players[targetSeat].handCount; i++) {
                    if (g_game.players[targetSeat].hand[i].type == CARD_SHA) { shaIdx = i; break; }
                }
                if (shaIdx < 0 && g_game.players[targetSeat].hero == HERO_ZHAOYUN) {
                    for (int i = 0; i < g_game.players[targetSeat].handCount; i++) {
                        if (g_game.players[targetSeat].hand[i].type == CARD_SHAN) { shaIdx = i; break; }
                    }
                }
                if (shaIdx < 0 && g_game.players[targetSeat].hero == HERO_GUANYU) {
                    for (int i = 0; i < g_game.players[targetSeat].handCount; i++) {
                        if (isRedSuit(g_game.players[targetSeat].hand[i].suit)
                            && g_game.players[targetSeat].hand[i].type != CARD_SHA
                            && g_game.players[targetSeat].hand[i].type != CARD_TAO
                            && !isEquipCard(g_game.players[targetSeat].hand[i].type)) { shaIdx = i; break; }
                    }
                }
                if (shaIdx >= 0) {
                    g_game.deck.discardCard(g_game.players[targetSeat].hand[shaIdx]);
                    g_game.removeCard(targetSeat, g_game.players[targetSeat].hand[shaIdx].cardId);
                    sendSystemMsg("%s 被【借刀杀人】，对 %s 出杀!", g_game.players[targetSeat].name, g_game.players[shaTarget].name);
                    // 简化: 直接触发杀的效果(不等待闪响应)
                    int dmg = (g_game.players[targetSeat].luoyiActive) ? 2 : 1;
                    // 检查目标是否能闪
                    bool canDodge = g_game.hasCardType(shaTarget, CARD_SHAN);
                    if (g_game.players[shaTarget].hero == HERO_ZHAOYUN && g_game.hasCardType(shaTarget, CARD_SHA)) canDodge = true;
                    if (canDodge) {
                        // 闪避
                        int shanIdx = -1;
                        for (int i = 0; i < g_game.players[shaTarget].handCount; i++) {
                            if (g_game.players[shaTarget].hand[i].type == CARD_SHAN) { shanIdx = i; break; }
                        }
                        if (shanIdx < 0 && g_game.players[shaTarget].hero == HERO_ZHAOYUN) {
                            for (int i = 0; i < g_game.players[shaTarget].handCount; i++) {
                                if (g_game.players[shaTarget].hand[i].type == CARD_SHA) { shanIdx = i; break; }
                            }
                        }
                        if (shanIdx >= 0) {
                            g_game.deck.discardCard(g_game.players[shaTarget].hand[shanIdx]);
                            g_game.removeCard(shaTarget, g_game.players[shaTarget].hand[shanIdx].cardId);
                            sendSystemMsg("%s 出闪抵消了杀!", g_game.players[shaTarget].name);
                        } else {
                            g_lastDamageCard.cardId = 0;
                            g_game.dealDamage(shaTarget, dmg, targetSeat);
                            sendSystemMsg("%s 受到 %d 点伤害!", g_game.players[shaTarget].name, dmg);
                        }
                    } else {
                        g_lastDamageCard.cardId = 0;
                        g_game.dealDamage(shaTarget, dmg, targetSeat);
                        sendSystemMsg("%s 受到 %d 点伤害!", g_game.players[shaTarget].name, dmg);
                    }
                }
            } else {
                // 不出杀: 获得其武器
                CardInfo weapon = g_game.players[targetSeat].equips[EQ_WEAPON];
                if (weapon.cardId > 0) {
                    g_game.addCardToHand(seat, weapon);
                    g_game.players[targetSeat].equips[EQ_WEAPON].cardId = 0;
                    g_game.players[targetSeat].equips[EQ_WEAPON].type = 0;
                    sendSystemMsg("%s 不出杀，%s 获得其武器 [%s]!",
                                 g_game.players[targetSeat].name, g_game.players[seat].name,
                                 getEquipName(weapon.type));
                    // 发送DRAW消息同步获得武器的手牌
                    if (!g_isSinglePlayer) {
                        std::string weaponLine = "DRAW|1|" + serializeCard(weapon) + "|" + std::string(g_game.players[seat].name);
                        broadcastMsg(weaponLine);
                    }
                }
            }
            break;
        }
        default:
            if (seat == g_mySeat) printf("[系统] 此牌暂不支持\n");
            break;
    }

    if (g_game.gameOver) {
        std::string winnerName = "主公方";
        if (g_game.winnerFaction == 1) winnerName = "反贼方";
        else if (g_game.winnerFaction == 2) winnerName = "内奸";
        broadcastMsg("GAME_OVER|" + std::to_string(g_game.winnerFaction) + "|" + winnerName);
    }
    broadcastPlayerState();
}

// ===================== 处理主动技能 =====================
void hostProcessSkill(int seat, int skillId, int param1, int param2) {
    if (seat < 0 || seat >= g_game.playerCount) return;
    if (!g_game.players[seat].alive || g_game.currentPhase != PHASE_PLAY) return;
    int hero = g_game.players[seat].hero;

    switch (skillId) {
        case 1: { // 孙权-制衡
            if (hero != HERO_SUNQUAN || g_game.players[seat].zhihengUsed >= 1) return;
            // param1: 弃牌数量
            int n = param1;
            if (n <= 0 || n > g_game.players[seat].handCount) return;
            // 智能选择弃牌: 优先弃低价值牌(非杀/闪/桃)，再弃多余的杀/闪
            int toDiscard[30]; int dc = 0;
            // 第一轮: 选非杀/闪/桃的牌
            for (int i = 0; i < g_game.players[seat].handCount && dc < n; i++) {
                int t = g_game.players[seat].hand[i].type;
                if (t != CARD_SHA && t != CARD_SHAN && t != CARD_TAO) {
                    bool dup = false;
                    for (int k = 0; k < dc; k++) if (toDiscard[k] == i) { dup = true; break; }
                    if (!dup) toDiscard[dc++] = i;
                }
            }
            // 第二轮: 选多余的杀(保留1张)
            if (dc < n) {
                int shaCnt = 0;
                for (int i = 0; i < g_game.players[seat].handCount && dc < n; i++) {
                    int t = g_game.players[seat].hand[i].type;
                    if (t == CARD_SHA) {
                        shaCnt++;
                        if (shaCnt > 1) {
                            bool dup = false;
                            for (int k = 0; k < dc; k++) if (toDiscard[k] == i) { dup = true; break; }
                            if (!dup) toDiscard[dc++] = i;
                        }
                    }
                }
            }
            // 第三轮: 选多余的闪(保留1张)
            if (dc < n) {
                int shanCnt = 0;
                for (int i = 0; i < g_game.players[seat].handCount && dc < n; i++) {
                    int t = g_game.players[seat].hand[i].type;
                    if (t == CARD_SHAN) {
                        shanCnt++;
                        if (shanCnt > 1) {
                            bool dup = false;
                            for (int k = 0; k < dc; k++) if (toDiscard[k] == i) { dup = true; break; }
                            if (!dup) toDiscard[dc++] = i;
                        }
                    }
                }
            }
            // 第四轮: 剩余的从末尾选(需要正确跳过已选中的索引)
            while (dc < n) {
                bool found = false;
                for (int idx = g_game.players[seat].handCount - 1; idx >= 0; idx--) {
                    bool dup = false;
                    for (int k = 0; k < dc; k++) if (toDiscard[k] == idx) { dup = true; break; }
                    if (!dup) { toDiscard[dc++] = idx; found = true; break; }
                }
                if (!found) break; // 没有可选的牌了
            }
            // 从大到小索引弃牌(避免索引偏移)
            for (int k = dc - 1; k >= 0; k--) {
                int idx = toDiscard[k];
                g_game.deck.discardCard(g_game.players[seat].hand[idx]);
                g_game.removeCard(seat, g_game.players[seat].hand[idx].cardId);
            }
            // 摸等量的牌
            std::string drawLine = "DRAW|" + std::to_string(n) + "|";
            std::string cardStr;
            for (int i = 0; i < n; i++) {
                CardInfo c = g_game.deck.drawCard();
                if (c.cardId >= 0) {
                    g_game.addCardToHand(seat, c);
                    if (i > 0) cardStr += ":";
                    cardStr += serializeCard(c);
                }
            }
            drawLine += cardStr + "|" + std::string(g_game.players[seat].name);
            broadcastMsg(drawLine);
            g_game.players[seat].zhihengUsed = 1;
            sendSystemMsg("%s 发动【制衡】，弃%d张牌摸%d张牌", g_game.players[seat].name, n, n);
            break;
        }
        case 2: { // 黄盖-苦肉
            if (hero != HERO_HUANGGAI || g_game.players[seat].kurouUsed >= 1) return;
            if (g_game.players[seat].hp <= 1) return;
            if (g_game.players[seat].handCount < 1) return;
            // 弃一张手牌
            int idx = g_game.players[seat].handCount - 1;
            g_game.deck.discardCard(g_game.players[seat].hand[idx]);
            g_game.removeCard(seat, g_game.players[seat].hand[idx].cardId);
            // 失去1点体力
            g_game.players[seat].hp--;
            // 摸两张牌
            std::string drawLine = "DRAW|2|";
            std::string cardStr;
            for (int i = 0; i < 2; i++) {
                CardInfo c = g_game.deck.drawCard();
                if (c.cardId >= 0) {
                    g_game.addCardToHand(seat, c);
                    if (i > 0) cardStr += ":";
                    cardStr += serializeCard(c);
                }
            }
            drawLine += cardStr + "|" + std::string(g_game.players[seat].name);
            broadcastMsg(drawLine);
            g_game.players[seat].kurouUsed = 1;
            sendSystemMsg("%s 发动【苦肉】，失去1点体力摸两张牌 (HP:%d/%d)",
                         g_game.players[seat].name, g_game.players[seat].hp, g_game.players[seat].maxHp);
            break;
        }
        case 3: { // 貂蝉-离间
            if (hero != HERO_DIAOCHAN || g_game.players[seat].lijianUsed >= 1) return;
            // param1, param2: 两名男性角色座位号
            int t1 = param1, t2 = param2;
            if (t1 < 0 || t2 < 0 || t1 == t2 || t1 == seat || t2 == seat) return;
            if (!g_game.players[t1].alive || !g_game.players[t2].alive) return;
            // 简化: 令t1对t2决斗(t1需出杀, 否则受伤)
            g_game.players[seat].lijianUsed = 1;
            sendSystemMsg("%s 发动【离间】，令 %s 与 %s 决斗!",
                         g_game.players[seat].name, g_game.players[t1].name, g_game.players[t2].name);
            // t1先出杀
            bool t1HasSha = g_game.hasCardType(t1, CARD_SHA);
            bool t2HasSha = g_game.hasCardType(t2, CARD_SHA);
            if (!t1HasSha) {
                g_game.dealDamage(t1, 1, t2);
                sendSystemMsg("%s 无杀可用，受到1点伤害!", g_game.players[t1].name);
            } else if (!t2HasSha) {
                g_game.dealDamage(t2, 1, t1);
                sendSystemMsg("%s 无杀可用，受到1点伤害!", g_game.players[t2].name);
            } else {
                // 都有杀，随机一方受伤
                int r = rand() % 2;
                g_game.dealDamage(r == 0 ? t1 : t2, 1, r == 0 ? t2 : t1);
            }
            break;
        }
        case 4: { // 姜维-挑衅
            if (hero != HERO_JIANGWEI || g_game.players[seat].tiaoxinUsed >= 1) return;
            int target = param1;
            if (target < 0 || target == seat || !g_game.players[target].alive) return;
            g_game.players[seat].tiaoxinUsed = 1;
            sendSystemMsg("%s 对 %s 发动【挑衅】!", g_game.players[seat].name, g_game.players[target].name);
            // 目标选择: 对姜维出杀 或 让姜维摸一张牌
            bool targetHasSha = g_game.hasCardType(target, CARD_SHA);
            if (targetHasSha) {
                // AI: 如果目标有杀且姜维HP>1，出杀；否则让姜维摸牌
                if (g_game.players[seat].hp > 1) {
                    // 出杀
                    for (int i = 0; i < g_game.players[target].handCount; i++) {
                        if (g_game.players[target].hand[i].type == CARD_SHA) {
                            g_game.deck.discardCard(g_game.players[target].hand[i]);
                            g_game.removeCard(target, g_game.players[target].hand[i].cardId);
                            break;
                        }
                    }
                    g_lastDamageCard.cardId = 0;
                    g_game.dealDamage(seat, 1, target);
                    sendSystemMsg("%s 对 %s 出杀，造成1点伤害!", g_game.players[target].name, g_game.players[seat].name);
                } else {
                    // 让姜维摸牌
                    CardInfo c = g_game.deck.drawCard();
                    if (c.cardId >= 0) {
                        g_game.addCardToHand(seat, c);
                        std::string drawLine = "DRAW|1|" + serializeCard(c) + "|" + std::string(g_game.players[seat].name);
                        broadcastMsg(drawLine);
                        sendSystemMsg("%s 让 %s 摸了一张牌", g_game.players[target].name, g_game.players[seat].name);
                    }
                }
            } else {
                // 让姜维摸牌
                CardInfo c = g_game.deck.drawCard();
                if (c.cardId >= 0) {
                    g_game.addCardToHand(seat, c);
                    std::string drawLine = "DRAW|1|" + serializeCard(c) + "|" + std::string(g_game.players[seat].name);
                    broadcastMsg(drawLine);
                    sendSystemMsg("%s 无杀可用，%s 摸一张牌", g_game.players[target].name, g_game.players[seat].name);
                }
            }
            break;
        }
        case 5: { // 刘备-仁德
            if (hero != HERO_LIUBEI) return;
            // param1: 目标座位, param2: 给牌数量
            int target = param1;
            int n = param2;
            if (target < 0 || target == seat || !g_game.players[target].alive || n <= 0) return;
            if (n > g_game.players[seat].handCount) n = g_game.players[seat].handCount;
            // 给最后n张牌
            for (int i = 0; i < n; i++) {
                int idx = g_game.players[seat].handCount - 1;
                g_game.addCardToHand(target, g_game.players[seat].hand[idx]);
                g_game.removeCard(seat, g_game.players[seat].hand[idx].cardId);
            }
            g_game.players[seat].rendUsed += n;
            // 给出两张以上时回复1点体力
            if (g_game.players[seat].rendUsed >= 2 && g_game.players[seat].hp < g_game.players[seat].maxHp) {
                g_game.players[seat].hp++;
                sendSystemMsg("%s 发动【仁德】，给出%d张牌并回复1点体力 (HP:%d/%d)",
                             g_game.players[seat].name, n, g_game.players[seat].hp, g_game.players[seat].maxHp);
            } else {
                sendSystemMsg("%s 发动【仁德】，给 %s %d张牌", g_game.players[seat].name, g_game.players[target].name, n);
            }
            break;
        }
    }
    if (g_game.gameOver) {
        std::string winnerName = "主公方";
        if (g_game.winnerFaction == 1) winnerName = "反贼方";
        else if (g_game.winnerFaction == 2) winnerName = "内奸";
        broadcastMsg("GAME_OVER|" + std::to_string(g_game.winnerFaction) + "|" + winnerName);
    }
    broadcastPlayerState();
}

// ===================== 处理响应 =====================
void hostProcessRespond(int seat, int cardIndex) {
    // 安全检查: g_responseFrom必须有效
    int sourceSeat = g_responseFrom;
    if (sourceSeat < 0 || sourceSeat >= g_game.playerCount) sourceSeat = seat;

    if (g_responseType == CARD_SHAN) {
        if (cardIndex >= 0 && cardIndex < g_game.players[seat].handCount) {
            CardInfo& c = g_game.players[seat].hand[cardIndex];
            bool valid = (c.type == CARD_SHAN);
            // 赵云-龙胆: 杀当闪
            if (g_game.players[seat].hero == HERO_ZHAOYUN && c.type == CARD_SHA) valid = true;
            // 关羽-武圣: 红色当杀(不能当闪，武圣仅限杀)
            if (valid) {
                std::string respondLine = "RESPOND|1|" + std::to_string(c.type) + "|"
                    + std::to_string(c.suit) + "|" + std::to_string(c.number) + "|"
                    + std::to_string(c.cardId) + "|" + std::to_string(seat) + "|"
                    + std::string(g_game.players[seat].name);
                g_game.removeCard(seat, c.cardId); g_game.deck.discardCard(c);
                broadcastMsg(respondLine);
            } else {
                std::string respondLine = "RESPOND|0|0|0|0|0|" + std::to_string(seat)
                    + "|" + std::string(g_game.players[seat].name);
                broadcastMsg(respondLine);
                int dmg = (g_game.players[sourceSeat].luoyiActive) ? 2 : 1;
                g_game.dealDamage(seat, dmg, sourceSeat);
                sendSystemMsg("%s 受到 %d 点伤害!", g_game.players[seat].name, dmg);
            }
        } else {
            std::string respondLine = "RESPOND|0|0|0|0|0|" + std::to_string(seat)
                + "|" + std::string(g_game.players[seat].name);
            broadcastMsg(respondLine);
            int dmg = (g_game.players[sourceSeat].luoyiActive) ? 2 : 1;
            g_game.dealDamage(seat, dmg, sourceSeat);
            sendSystemMsg("%s 受到 %d 点伤害!", g_game.players[seat].name, dmg);
        }
    } else if (g_responseType == CARD_TAO) {
        if (cardIndex >= 0 && cardIndex < g_game.players[seat].handCount
            && g_game.players[seat].hand[cardIndex].type == CARD_TAO) {
            CardInfo& c = g_game.players[seat].hand[cardIndex];
            std::string respondLine = "RESPOND|1|" + std::to_string(c.type) + "|"
                + std::to_string(c.suit) + "|" + std::to_string(c.number) + "|"
                + std::to_string(c.cardId) + "|" + std::to_string(seat) + "|"
                + std::string(g_game.players[seat].name);
            g_game.removeCard(seat, c.cardId); g_game.deck.discardCard(c);
            g_game.players[seat].hp++;
            broadcastMsg(respondLine);
            sendSystemMsg("%s 使用桃自救!", g_game.players[seat].name);
        } else {
            g_game.players[seat].alive = false;
            g_game.onPlayerDeath(seat, sourceSeat);
            sendSystemMsg("%s 阵亡!", g_game.players[seat].name);
        }
    }
    g_waitingForResponse = false; g_responseType = 0; g_responseFrom = -1;
    if (g_game.gameOver) {
        std::string winnerName = "主公方";
        if (g_game.winnerFaction == 1) winnerName = "反贼方";
        else if (g_game.winnerFaction == 2) winnerName = "内奸";
        broadcastMsg("GAME_OVER|" + std::to_string(g_game.winnerFaction) + "|" + winnerName);
    }
    broadcastPlayerState();
}

// ===================== AI出牌 =====================
void processAITurn(int seat) {
    if (!g_game.players[seat].alive || g_game.gameOver) return;

    // AI先使用主动技能
    int hero = g_game.players[seat].hero;

    // 孙权-制衡: 如果手牌中有不需要的牌(锦囊/装备已有)，弃掉换新牌
    if (hero == HERO_SUNQUAN && g_game.players[seat].zhihengUsed == 0 && g_game.players[seat].handCount >= 3) {
        // 弃掉手牌中价值最低的牌(保留杀/闪/桃)
        int discardable = 0;
        for (int i = 0; i < g_game.players[seat].handCount; i++) {
            int t = g_game.players[seat].hand[i].type;
            if (t != CARD_SHA && t != CARD_SHAN && t != CARD_TAO) discardable++;
        }
        if (discardable >= 2) {
            hostProcessSkill(seat, 1, discardable, 0);
            msleep(700);
        }
    }

    // 黄盖-苦肉: 如果HP>2且手牌少，发动苦肉
    if (hero == HERO_HUANGGAI && g_game.players[seat].kurouUsed == 0 && g_game.players[seat].hp > 2 && g_game.players[seat].handCount < 3) {
        hostProcessSkill(seat, 2, 0, 0);
        msleep(700);
    }

    // 貂蝉-离间: 找两个敌方男性角色决斗
    if (hero == HERO_DIAOCHAN && g_game.players[seat].lijianUsed == 0) {
        int t1 = -1, t2 = -1;
        for (int i = 0; i < g_game.playerCount; i++) {
            if (i == seat || !g_game.players[i].alive) continue;
            // 简化: 所有非自己角色都算"男性"
            if (g_game.players[i].identity != g_game.players[seat].identity) {
                if (t1 < 0) t1 = i;
                else if (t2 < 0) { t2 = i; break; }
            }
        }
        if (t1 >= 0 && t2 >= 0) {
            hostProcessSkill(seat, 3, t1, t2);
            msleep(700);
        }
    }

    // 姜维-挑衅: 对手牌少的敌方角色使用
    if (hero == HERO_JIANGWEI && g_game.players[seat].tiaoxinUsed == 0 && g_game.players[seat].hp > 1) {
        int target = g_game.aiChooseTarget(seat, CARD_SHA);
        if (target >= 0) {
            hostProcessSkill(seat, 4, target, 0);
            msleep(700);
        }
    }

    // 刘备-仁德: 给队友牌
    if (hero == HERO_LIUBEI && g_game.players[seat].handCount > g_game.players[seat].hp + 1) {
        // 找一个队友(同身份)
        int target = -1;
        for (int i = 0; i < g_game.playerCount; i++) {
            if (i == seat || !g_game.players[i].alive) continue;
            if (g_game.players[i].identity == g_game.players[seat].identity
                || (g_game.players[seat].identity == IDENTITY_LORD && g_game.players[i].identity == IDENTITY_LOYAL)
                || (g_game.players[seat].identity == IDENTITY_LOYAL && g_game.players[i].identity == IDENTITY_LORD)) {
                target = i; break;
            }
        }
        if (target >= 0) {
            int giveCount = g_game.players[seat].handCount - g_game.players[seat].hp;
            if (giveCount > 2) giveCount = 2;
            hostProcessSkill(seat, 5, target, giveCount);
            msleep(700);
        }
    }

    // AI循环出牌
    for (int round = 0; round < 10; round++) {
        if (g_game.gameOver) break;
        int ci = g_game.aiChooseCardToPlay(seat);
        if (ci < 0) break;
        CardInfo& c = g_game.players[seat].hand[ci];
        // 计算实际卡牌类型(考虑武将技能替代)
        int effectiveType = c.type;
        if (hero == HERO_GUANYU && isRedSuit(c.suit) && c.type != CARD_SHA && c.type != CARD_TAO && !isEquipCard(c.type) && g_game.canUseSha(seat))
            effectiveType = CARD_SHA;
        if (hero == HERO_GANNING && !isRedSuit(c.suit) && c.type != CARD_SHA && c.type != CARD_SHAN && !isEquipCard(c.type))
            effectiveType = CARD_GUOHE;
        if (hero == HERO_DAQIAO && c.suit == SUIT_DIAMOND && c.type != CARD_SHA && c.type != CARD_SHAN && c.type != CARD_TAO && !isEquipCard(c.type))
            effectiveType = CARD_LEBUSI;
        int target = -1;
        if (effectiveType == CARD_SHA || effectiveType == CARD_JUEDOU || effectiveType == CARD_GUOHE
            || effectiveType == CARD_SHUNQIAN || effectiveType == CARD_HUOGONG || effectiveType == CARD_LEBUSI) {
            target = g_game.aiChooseTarget(seat, effectiveType);
        }
        hostProcessPlayCard(seat, ci, target);
        // 停顿一下让玩家看清AI的行动
        msleep(700);
    }
    hostEndTurn();
}

// ===================== 结束回合 =====================
void hostEndTurn() {
    int seat = g_game.currentSeat;
    int excess = g_game.players[seat].handCount - g_game.players[seat].hp;
    if (excess > 0) {
        std::string phaseLine = "PHASE|" + std::to_string(PHASE_DISCARD) + "|"
            + std::to_string(seat) + "|" + std::string(g_game.players[seat].name);
        broadcastMsg(phaseLine);
        if (seat == g_mySeat) {
            int toDiscard[30]; int dc = 0;
            while (dc < excess) {
                showHandCards();
                printf("需要弃掉 %d 张牌(还需%d张)，输入编号(空格分隔): ", excess, excess - dc);
                char input[256];
                if (!fgets(input, sizeof(input), stdin)) {
                    // EOF: 随机弃牌
                    for (int i = excess - dc - 1; i >= 0; i--) {
                        int idx = rand() % g_game.players[seat].handCount;
                        g_game.deck.discardCard(g_game.players[seat].hand[idx]);
                        g_game.removeCard(seat, g_game.players[seat].hand[idx].cardId);
                    }
                    break;
                }
                dc = 0;
                char* p = strtok(input, " \n");
                while (p && dc < excess) {
                    int idx = atoi(p) - 1;
                    if (idx >= 0 && idx < g_game.players[seat].handCount) {
                        bool dup = false;
                        for (int k = 0; k < dc; k++) if (toDiscard[k] == idx) { dup = true; break; }
                        if (!dup) toDiscard[dc++] = idx;
                    }
                    p = strtok(NULL, " \n");
                }
            }
            for (int i = dc - 1; i >= 0; i--) {
                g_game.deck.discardCard(g_game.players[seat].hand[toDiscard[i]]);
                g_game.removeCard(seat, g_game.players[seat].hand[toDiscard[i]].cardId);
            }
        } else {
            // AI智能弃牌: 按优先级弃牌(保留桃>闪>杀>装备>锦囊)
            while (g_game.players[seat].handCount > g_game.players[seat].hp) {
                int discardIdx = -1;
                // 优先弃锦囊牌(非关键锦囊)
                for (int i = 0; i < g_game.players[seat].handCount; i++) {
                    int t = g_game.players[seat].hand[i].type;
                    if (t == CARD_GUOHE || t == CARD_SHUNQIAN || t == CARD_HUOGONG
                        || t == CARD_TIEJI || t == CARD_JUEDOU) {
                        discardIdx = i; break;
                    }
                }
                // 其次弃装备牌(已有同类型装备的)
                if (discardIdx < 0) {
                    for (int i = 0; i < g_game.players[seat].handCount; i++) {
                        if (isEquipCard(g_game.players[seat].hand[i].type)) {
                            discardIdx = i; break;
                        }
                    }
                }
                // 其次弃多余的杀(保留1张)
                if (discardIdx < 0) {
                    int shaCount = g_game.countCardType(seat, CARD_SHA);
                    if (shaCount > 1) {
                        for (int i = 0; i < g_game.players[seat].handCount; i++) {
                            if (g_game.players[seat].hand[i].type == CARD_SHA) {
                                discardIdx = i; break;
                            }
                        }
                    }
                }
                // 其次弃多余的闪(保留1张)
                if (discardIdx < 0) {
                    int shanCount = g_game.countCardType(seat, CARD_SHAN);
                    if (shanCount > 1) {
                        for (int i = 0; i < g_game.players[seat].handCount; i++) {
                            if (g_game.players[seat].hand[i].type == CARD_SHAN) {
                                discardIdx = i; break;
                            }
                        }
                    }
                }
                // 最后弃杀或闪(只保留桃)
                if (discardIdx < 0) {
                    for (int i = 0; i < g_game.players[seat].handCount; i++) {
                        int t = g_game.players[seat].hand[i].type;
                        if (t == CARD_SHA || t == CARD_SHAN) {
                            discardIdx = i; break;
                        }
                    }
                }
                // 实在没得选了，弃第一张
                if (discardIdx < 0) discardIdx = 0;
                g_game.deck.discardCard(g_game.players[seat].hand[discardIdx]);
                g_game.removeCard(seat, g_game.players[seat].hand[discardIdx].cardId);
            }
        }
        std::string discardLine = "DISCARD|" + std::to_string(seat) + "|"
            + std::to_string(excess) + "|" + std::string(g_game.players[seat].name);
        broadcastMsg(discardLine);
    }
    hostNextTurn();
}

void hostNextTurn() {
    if (g_game.gameOver) return;
    int next = g_game.currentSeat;
    do { next = (next + 1) % g_game.playerCount; } while (!g_game.players[next].alive && next != g_game.currentSeat);
    g_game.currentSeat = next;
    if (g_game.gameOver) {
        std::string winnerName = "主公方";
        if (g_game.winnerFaction == 1) winnerName = "反贼方";
        else if (g_game.winnerFaction == 2) winnerName = "内奸";
        broadcastMsg("GAME_OVER|" + std::to_string(g_game.winnerFaction) + "|" + winnerName);
        return;
    }
    hostDrawPhase();
}

// ===================== 搜索房间 (UDP广播发现) =====================
void searchRooms() {
    clearScreen(); printBanner();
    COL_CYAN(printf("  你的IP: %s:%d\n", g_localIP.c_str(), kGamePort));
    printf("[系统] 正在搜索局域网房间...\n\n");

    auto rooms = discoverRooms(2000); // 2秒超时

    clearScreen(); printBanner();
    if (rooms.empty()) {
        printf("[系统] 未发现局域网房间\n");
        printf("提示: 请确保房主已创建房间，且在同一网段\n");
        printf("按回车继续..."); char buf[16]; if (!fgets(buf, sizeof(buf), stdin)) { buf[0] = '\0'; }
        return;
    }

    COL_GREEN(printf("  发现以下房间:\n\n"));
    for (size_t i = 0; i < rooms.size(); i++) {
        printf("  %d. [%s] (%s:%d) %d/%d人\n",
            (int)i + 1, rooms[i].roomName.c_str(), rooms[i].hostIp.c_str(),
            rooms[i].tcpPort, rooms[i].playerCount, rooms[i].maxPlayers);
    }
    printf("\n  0. 取消\n");
    printf("  请选择房间编号加入: ");
    char choice[16]; if (!fgets(choice, sizeof(choice), stdin)) { choice[0] = '\0'; }
    int idx = atoi(choice) - 1;
    if (idx < 0 || idx >= (int)rooms.size()) return;

    RoomInfo& selected = rooms[idx];

    // TCP连接到房主
    if (!g_client.connect(selected.hostIp, selected.tcpPort)) {
        printf("[系统] 连接失败!\n");
        printf("按回车继续..."); char buf[64]; if (!fgets(buf, sizeof(buf), stdin)) { buf[0] = '\0'; }
        return;
    }
    g_isHost = false;

    // 发送加入请求
    g_client.sendLine("JOIN|" + g_myName);
    printf("[系统] 正在连接 [%s] ...\n", selected.roomName.c_str());

    // 等待响应
    time_t connStart = time(NULL);
    while (!g_isInRoom && time(NULL) - connStart < 5) {
        std::string line;
        if (g_client.recvLine(line, 100)) {
            handleServerLine(line);
        }
        msleep(50);
    }
    if (g_isInRoom) {
        g_isSinglePlayer = false;
        roomLobby();
    } else {
        printf("[系统] 连接失败，房间可能已满或已开始游戏\n");
        printf("按回车继续..."); char buf[64]; if (!fgets(buf, sizeof(buf), stdin)) { buf[0] = '\0'; }
    }
}

// ===================== 主菜单 =====================
void mainMenu() {
    while (true) {
        clearScreen(); printBanner();
        COL_CYAN(printf("  你的IP: %s:%d\n", g_localIP.c_str(), kGamePort));
        printf("  名字: %s\n\n", g_myName.c_str());
        printf("  1. 创建房间(联机)\n");
        printf("  2. 搜索房间(自动发现)\n");
        printf("  3. 手动输入IP加入\n");
        printf("  4. 单机模式(与AI对战)\n");
        printf("  5. 退出\n\n");
        printf("  请选择: ");
        char choice[16]; if (!fgets(choice, sizeof(choice), stdin)) { choice[0] = '\0'; }
        int c = atoi(choice);
        if (c == 1) {
            printf("  房间名: "); char rn[kMaxRoomName]; if (!fgets(rn, sizeof(rn), stdin)) { rn[0] = '\0'; } rn[strcspn(rn, "\n")] = '\0';
            printf("  最大人数(2-8): "); char ms[8]; if (!fgets(ms, sizeof(ms), stdin)) { ms[0] = '\0'; } int mp = atoi(ms);
            if (mp < 2) mp = 2;
            if (mp > 8) mp = 8;
            g_isHost = true;
            g_isSinglePlayer = false;

            // 启动 TCP 监听
            if (!g_host.start(rn, g_myName, mp)) {
                printf("[错误] 无法启动服务器! 端口 %d 可能被占用\n", kGamePort);
                printf("按回车继续..."); char buf[64]; if (!fgets(buf, sizeof(buf), stdin)) { buf[0] = '\0'; }
                continue;
            }

            // 启动 UDP 发现响应
            g_discovery.start(g_host.getRoomId(), rn, g_myName, 1, mp);
            g_isInRoom = true;

            // 设置消息回调
            g_host.onMessage_ = handleClientMessage;

            printf("\n[系统] 房间 [%s] 已创建! 监听端口: %d\n", rn, kGamePort);
            printf("[系统] 你的IP: %s:%d\n", g_localIP.c_str(), kGamePort);
            printf("[系统] 等待玩家加入...\n");
            roomLobby();
            // 房间关闭后更新发现信息
            g_discovery.updateInfo(g_host.getPlayerCount());
        } else if (c == 2) {
            searchRooms();
        } else if (c == 3) {
            printf("  房主IP: "); char ip[64]; if (!fgets(ip, sizeof(ip), stdin)) { ip[0] = '\0'; } ip[strcspn(ip, "\n")] = '\0';
            if (!g_client.connect(ip, kGamePort)) {
                printf("[系统] 连接失败! 请检查IP和端口\n");
                printf("按回车继续..."); if (fgets(ip, sizeof(ip), stdin)) {}
                continue;
            }
            g_isHost = false;
            g_client.sendLine("JOIN|" + g_myName);
            printf("[系统] 正在连接...\n");
            time_t connStart = time(NULL);
            while (!g_isInRoom && time(NULL) - connStart < 5) {
                std::string line;
                if (g_client.recvLine(line, 100)) {
                    handleServerLine(line);
                }
                msleep(50);
            }
            if (g_isInRoom) {
                g_isSinglePlayer = false;
                roomLobby();
            } else {
                printf("[系统] 连接失败\n");
                printf("按回车继续..."); if (fgets(ip, sizeof(ip), stdin)) {}
            }
        } else if (c == 4) {
            // 单机模式
            printf("  AI数量(1-7): "); char aiStr[8]; if (!fgets(aiStr, sizeof(aiStr), stdin)) { aiStr[0] = '\0'; }
            int aiCount = atoi(aiStr); if (aiCount < 1) aiCount = 1; if (aiCount > 7) aiCount = 7;
            g_isSinglePlayer = true;
            g_isHost = false;
            char names[kMaxPlayers][kMaxNameLen];
            strncpy(names[0], g_myName.c_str(), kMaxNameLen - 1);
            const char* aiNames[] = {"曹操", "孙权", "吕布", "诸葛亮", "赵云", "关羽", "貂蝉"};
            for (int i = 0; i < aiCount; i++) strncpy(names[i + 1], aiNames[i], kMaxNameLen - 1);
            g_game.playerCount = aiCount + 1;
            hostStartGame();
            if (g_inGame) gameLoop();
        } else if (c == 5) return;
    }
}

// ===================== 房间等待 =====================
void roomLobby() {
    while (g_isInRoom && !g_inGame) {
        // 客户端模式下 poll 消息
        if (!g_isHost && g_client.isConnected()) {
            std::string line;
            while (g_client.recvLine(line, 50)) {
                handleServerLine(line);
            }
        }

        // 只在有变化时才刷新界面
        if (g_roomDirty) {
            g_roomDirty = false;
            clearScreen(); printBanner(); showRoomStatus();
            if (g_isHost) {
                printf("  1. 开始游戏  2. 离开房间\n");
            } else {
                printf("  1. 离开房间\n");
            }
            if (!g_chatLog.empty()) {
                printf("\n  聊天:\n");
                int s = (int)g_chatLog.size() - 5; if (s < 0) s = 0;
                for (int i = s; i < (int)g_chatLog.size(); i++) printf("    %s\n", g_chatLog[i].c_str());
            }
            printf("\n  选择: "); fflush(stdout);
        }

        // 非阻塞输入: 用select检测stdin，超时0.5秒后重新检查消息
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(0, &readSet);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        int sel = select(1, &readSet, NULL, NULL, &tv);
        if (sel > 0) {
            char ch[16];
            if (fgets(ch, sizeof(ch), stdin)) {
                int c = atoi(ch);
                if (c == 1 && g_isHost) {
                    hostStartGame();
                    if (g_inGame) { gameLoop(); return; }
                }
                else if (c == 1 && !g_isHost) {
                    g_client.sendLine("LEAVE");
                    g_client.disconnect();
                    g_isInRoom = false; return;
                }
                else if (c == 2 && g_isHost) {
                    g_host.broadcastLine("SYSTEM|房间已关闭");
                    g_host.stop();
                    g_discovery.stop();
                    g_isInRoom = false; return;
                }
            }
        }
    }
    // 客户端收到GAME_INFO后g_inGame=true，需要进入游戏循环
    if (g_inGame) { gameLoop(); }
}

// ===================== 游戏主循环 =====================
void gameLoop() {
    while (g_inGame && !g_game.gameOver) {
        // 网络断开检测
        if (!g_isSinglePlayer && !g_isHost && !g_client.isConnected()) {
            printf("[系统] 网络连接断开!\n");
            break;
        }

        // 客户端模式下 poll 消息
        if (!g_isSinglePlayer && !g_isHost && g_client.isConnected()) {
            std::string line;
            while (g_client.recvLine(line, 50)) {
                handleServerLine(line);
            }
        }

        if (g_waitingForResponse) {
            printf(">>> 输入手牌编号出牌(0放弃): ");
            char input[16]; if (!fgets(input, sizeof(input), stdin)) { input[0] = '\0'; }
            int idx = atoi(input) - 1;
            if (!g_isSinglePlayer && !g_isHost) {
                // 客户端: 发送RESPOND给房主，让房主处理并广播
                g_client.sendLine("RESPOND|" + std::to_string(idx));
                // 等待房主处理后的广播
                continue;
            } else {
                hostProcessRespond(g_mySeat, idx);
            }
            continue;
        }

        if (g_game.currentPhase == PHASE_PLAY && g_game.currentSeat == g_mySeat) {
            clearScreen(); printBanner(); showGameStatus(); showAllPlayers(); showHandCards();
            // 显示可用技能
            int myHero = g_game.players[g_mySeat].hero;
            printf(">>> 出牌 (编号出牌/0结束/h手牌/p玩家/s技能/q退出): ");
            char input[256]; if (!fgets(input, sizeof(input), stdin)) { input[0] = '\0'; }
            if (input[0] == 'h' || input[0] == 'H') { showHandCards(); continue; }
            if (input[0] == 'p' || input[0] == 'P') { showAllPlayers(); continue; }
            if (input[0] == 'q' || input[0] == 'Q') { g_inGame = false; break; }
            if (input[0] == 's' || input[0] == 'S') {
                // 技能菜单
                printf("\n  === 可用技能 ===\n");
                bool hasSkill = false;
                if (myHero == HERO_SUNQUAN && g_game.players[g_mySeat].zhihengUsed == 0) {
                    printf("  1. 制衡 - 弃置任意手牌并摸等量的牌\n"); hasSkill = true;
                }
                if (myHero == HERO_HUANGGAI && g_game.players[g_mySeat].kurouUsed == 0 && g_game.players[g_mySeat].hp > 1) {
                    printf("  2. 苦肉 - 弃一张牌失去1点体力摸两张牌\n"); hasSkill = true;
                }
                if (myHero == HERO_DIAOCHAN && g_game.players[g_mySeat].lijianUsed == 0) {
                    printf("  3. 离间 - 令两名男性角色决斗\n"); hasSkill = true;
                }
                if (myHero == HERO_JIANGWEI && g_game.players[g_mySeat].tiaoxinUsed == 0) {
                    printf("  4. 挑衅 - 令一名角色对你出杀或让你摸牌\n"); hasSkill = true;
                }
                if (myHero == HERO_LIUBEI) {
                    printf("  5. 仁德 - 将手牌交给其他角色\n"); hasSkill = true;
                }
                if (!hasSkill) {
                    printf("  (无可用技能)\n");
                    printf("  按回车继续..."); char buf[16]; if (!fgets(buf, sizeof(buf), stdin)) { buf[0] = '\0'; }
                    continue;
                }
                printf("  0. 取消\n");
                printf("  选择技能: ");
                char sk[16]; if (!fgets(sk, sizeof(sk), stdin)) { sk[0] = '\0'; }
                int skillId = atoi(sk);
                if (skillId == 0) continue;
                if (skillId == 1 && myHero == HERO_SUNQUAN) {
                    printf("  弃牌数量(1-%d): ", g_game.players[g_mySeat].handCount);
                    char n[16]; if (!fgets(n, sizeof(n), stdin)) { n[0] = '\0'; }
                    hostProcessSkill(g_mySeat, 1, atoi(n), 0);
                } else if (skillId == 2 && myHero == HERO_HUANGGAI) {
                    hostProcessSkill(g_mySeat, 2, 0, 0);
                } else if (skillId == 3 && myHero == HERO_DIAOCHAN) {
                    showAllPlayers();
                    printf("  选择第一个男性角色(座位): ");
                    char t1[16]; if (!fgets(t1, sizeof(t1), stdin)) { t1[0] = '\0'; }
                    printf("  选择第二个男性角色(座位): ");
                    char t2[16]; if (!fgets(t2, sizeof(t2), stdin)) { t2[0] = '\0'; }
                    hostProcessSkill(g_mySeat, 3, atoi(t1), atoi(t2));
                } else if (skillId == 4 && myHero == HERO_JIANGWEI) {
                    showAllPlayers();
                    printf("  选择目标(座位): ");
                    char t[16]; if (!fgets(t, sizeof(t), stdin)) { t[0] = '\0'; }
                    hostProcessSkill(g_mySeat, 4, atoi(t), 0);
                } else if (skillId == 5 && myHero == HERO_LIUBEI) {
                    showAllPlayers();
                    printf("  选择目标(座位): ");
                    char t[16]; if (!fgets(t, sizeof(t), stdin)) { t[0] = '\0'; }
                    printf("  给牌数量(1-%d): ", g_game.players[g_mySeat].handCount);
                    char n[16]; if (!fgets(n, sizeof(n), stdin)) { n[0] = '\0'; }
                    hostProcessSkill(g_mySeat, 5, atoi(t), atoi(n));
                }
                continue;
            }
            int cardIdx = atoi(input) - 1;
            if (cardIdx < 0) { hostEndTurn(); continue; }
            if (cardIdx >= g_game.players[g_mySeat].handCount) {
                printf("[系统] 无效牌号!\n");
                printf("按回车继续..."); char buf[16]; if (!fgets(buf, sizeof(buf), stdin)) { buf[0] = '\0'; }
                continue;
            }
            CardInfo& card = g_game.players[g_mySeat].hand[cardIdx];
            // 计算实际卡牌类型(考虑武将技能替代)
            int effectiveType = card.type;
            if (myHero == HERO_GUANYU && isRedSuit(card.suit) && card.type != CARD_SHA && card.type != CARD_TAO && !isEquipCard(card.type) && g_game.canUseSha(g_mySeat))
                effectiveType = CARD_SHA;
            if (myHero == HERO_GANNING && !isRedSuit(card.suit) && card.type != CARD_SHA && card.type != CARD_SHAN && !isEquipCard(card.type))
                effectiveType = CARD_GUOHE;
            if (myHero == HERO_DAQIAO && card.suit == SUIT_DIAMOND && card.type != CARD_SHA && card.type != CARD_SHAN && card.type != CARD_TAO && !isEquipCard(card.type))
                effectiveType = CARD_LEBUSI;
            int needTarget = 0;
            switch (effectiveType) {
                case CARD_SHA: case CARD_JUEDOU: case CARD_GUOHE: case CARD_SHUNQIAN:
                case CARD_HUOGONG: case CARD_LEBUSI: case CARD_JUEDAO: needTarget = 1; break;
                default: break;
            }
            int targetSeat = -1;
            if (needTarget) {
                showAllPlayers();
                printf("选择目标(座位编号): "); char ts[16]; if (!fgets(ts, sizeof(ts), stdin)) { ts[0] = '\0'; }
                targetSeat = atoi(ts);
                if (targetSeat < 0 || targetSeat >= g_game.playerCount || targetSeat == g_mySeat || !g_game.players[targetSeat].alive) {
                    printf("[系统] 无效目标!\n");
                    continue;
                }
            }
            if (!g_isSinglePlayer && !g_isHost) {
                // 客户端: 发送PLAY消息给房主处理
                std::string playMsg = "PLAY|" + std::to_string(cardIdx) + "|" + std::to_string(targetSeat);
                g_client.sendLine(playMsg);
                // 等待房主处理后的广播，短暂等待避免CPU空转
                msleep(100);
                continue;
            } else {
                hostProcessPlayCard(g_mySeat, cardIdx, targetSeat);
            }
        } else if (g_game.currentPhase == PHASE_DISCARD && g_game.currentSeat == g_mySeat) {
            // 弃牌阶段: 等待房主广播DISCARD消息处理
            // 房主在hostEndTurn中处理，客户端等待广播
            if (!g_isHost) {
                // 客户端: 等待DISCARD广播
                msleep(100);
            }
        } else {
            msleep(200);
        }
    }

    // 显示游戏结果
    if (g_game.gameOver) {
        const char* factionName = "主公方";
        if (g_game.winnerFaction == 1) factionName = "反贼方";
        else if (g_game.winnerFaction == 2) factionName = "内奸";
        printf("\n");
        COL_GREEN(printf("  游戏结束! %s 获胜!\n\n", factionName));
        printf("  按回车返回..."); fflush(stdout);
        char buf[64]; if (fgets(buf, sizeof(buf), stdin)) {}
    }

    // 清理
    resetGlobalState();
    if (g_isHost) {
        g_host.onMessage_ = nullptr;
        g_host.stop();
        g_discovery.stop();
    } else {
        g_client.disconnect();
    }
}

// ===================== 主函数 =====================
int main() {
    srand((unsigned int)time(NULL) + (unsigned int)clock());
    initColor();

#ifdef _WIN32
    // 设置控制台为 UTF-8 输出
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    g_localIP = getLocalIP();

    // 取消标准输出的缓冲，确保管道下也能正常显示
    setbuf(stdout, NULL);
    clearScreen(); printBanner();
    printf("  请输入你的名字: ");
    fflush(stdout);
    char nameBuf[kMaxNameLen]; if (!fgets(nameBuf, sizeof(nameBuf), stdin)) { nameBuf[0] = '\0'; }
    nameBuf[strcspn(nameBuf, "\n")] = '\0';
    if (nameBuf[0] == '\0') strncpy(nameBuf, "Player", kMaxNameLen - 1);
    g_myName = nameBuf;

    mainMenu();

    COL_GREEN(printf("\n  感谢游玩三国杀! 再见!\n\n"));
    return 0;
}
