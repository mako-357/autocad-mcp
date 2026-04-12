#include "socket_bridge.h"

#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <map>

// 簡易 JSON パーサー（外部依存なし）
// フォーマット: {"id":"xxx","method":"yyy","params":{...}}\n
static BridgeCommand parseCommand(const std::string& json) {
    BridgeCommand cmd;
    // 超簡易パース: "key": "value" を探す（コロン前後のスペース対応）
    auto extractStr = [&](const std::string& key) -> std::string {
        std::string needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return "";
        pos += needle.size();
        // コロンまでスキップ
        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        pos++; // ':' の次
        // スペースをスキップ
        while (pos < json.size() && json[pos] == ' ') pos++;
        // 値が文字列か確認
        if (pos >= json.size() || json[pos] != '"') return "";
        pos++; // '"' の次
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    };

    cmd.id = extractStr("id");
    cmd.method = extractStr("method");

    // params はオブジェクト全体を抽出
    auto paramsPos = json.find("\"params\"");
    if (paramsPos != std::string::npos) {
        paramsPos = json.find(':', paramsPos + 8);
        if (paramsPos != std::string::npos) paramsPos++;
        // スペースをスキップ
        while (paramsPos < json.size() && json[paramsPos] == ' ') paramsPos++;
        // 中括弧の対応を追跡
        int depth = 0;
        size_t start = paramsPos;
        for (size_t i = paramsPos; i < json.size(); i++) {
            if (json[i] == '{') depth++;
            else if (json[i] == '}') {
                depth--;
                if (depth == 0) {
                    cmd.params = json.substr(start, i - start + 1);
                    break;
                }
            }
        }
    }

    return cmd;
}

static std::string formatResult(const BridgeResult& result) {
    std::ostringstream os;
    os << "{\"id\":\"" << result.id
       << "\",\"success\":" << (result.success ? "true" : "false")
       << ",\"data\":" << result.data << "}\n";
    return os.str();
}

SocketBridge& SocketBridge::instance() {
    static SocketBridge s_instance;
    return s_instance;
}

SocketBridge::~SocketBridge() {
    stop();
}

bool SocketBridge::start(const std::string& socketPath) {
    if (m_running) return false;

    m_socketPath = socketPath;

    // 既存のソケットファイルを削除
    unlink(socketPath.c_str());

    m_serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_serverFd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    if (listen(m_serverFd, 5) < 0) {
        close(m_serverFd);
        m_serverFd = -1;
        unlink(socketPath.c_str());
        return false;
    }

    m_running = true;
    m_serverThread = std::thread(&SocketBridge::serverLoop, this);

    return true;
}

void SocketBridge::stop() {
    if (!m_running) return;

    m_running = false;

    // サーバーソケットを閉じて accept() を中断
    if (m_serverFd >= 0) {
        shutdown(m_serverFd, SHUT_RDWR);
        close(m_serverFd);
        m_serverFd = -1;
    }

    // accept() を起こすためにダミー接続を送る
    int dummy = socket(AF_UNIX, SOCK_STREAM, 0);
    if (dummy >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);
        connect(dummy, (struct sockaddr*)&addr, sizeof(addr));
        close(dummy);
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    unlink(m_socketPath.c_str());
}

void SocketBridge::setHandler(CommandHandler handler) {
    m_handler = handler;
}

bool SocketBridge::hasPendingCommands() {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    return !m_cmdQueue.empty();
}

void SocketBridge::processPendingCommands() {
    std::queue<std::pair<int, BridgeCommand>> pending;
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        std::swap(pending, m_cmdQueue);
    }

    while (!pending.empty()) {
        auto [clientFd, cmd] = pending.front();
        pending.pop();

        BridgeResult result;
        result.id = cmd.id;

        if (m_handler) {
            result = m_handler(cmd);
        } else {
            result.success = false;
            result.data = "\"no handler registered\"";
        }

        // 結果をソケットスレッドに返す
        std::string response = formatResult(result);
        // 直接クライアントに書き込み（メインスレッドから）
        write(clientFd, response.c_str(), response.size());
    }
}

void SocketBridge::serverLoop() {
    while (m_running) {
        // select() でタイムアウト付き accept を実現
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(m_serverFd, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;  // 1秒ごとに m_running をチェック
        tv.tv_usec = 0;

        int ret = select(m_serverFd + 1, &readfds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;  // タイムアウトまたはエラー
        if (!m_running) break;

        int clientFd = accept(m_serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (!m_running) break;
            continue;
        }

        // クライアントにもタイムアウトを設定
        struct timeval clientTimeout;
        clientTimeout.tv_sec = 30;
        clientTimeout.tv_usec = 0;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &clientTimeout, sizeof(clientTimeout));

        handleClient(clientFd);
        close(clientFd);
    }
}

void SocketBridge::handleClient(int clientFd) {
    char buf[4096];
    std::string buffer;

    while (m_running) {
        ssize_t n = read(clientFd, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';
        buffer += buf;

        // 改行区切りでコマンドを処理
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer = buffer.substr(pos + 1);

            if (line.empty()) continue;

            BridgeCommand cmd = parseCommand(line);

            // デバッグログ
            FILE* logf = fopen("/tmp/gfp-arx-debug.log", "a");
            if (logf) {
                fprintf(logf, "[RECV] id=%s method='%s' params='%s'\n",
                        cmd.id.c_str(), cmd.method.c_str(), cmd.params.c_str());
                fflush(logf);
            }

            BridgeResult result;
            result.id = cmd.id;

            if (cmd.method == "ping") {
                result.success = true;
                result.data = "\"pong\"";
            } else if (m_handler) {
                if (logf) { fprintf(logf, "[HANDLER] calling handler...\n"); fflush(logf); }
                result = m_handler(cmd);
                if (logf) { fprintf(logf, "[HANDLER] done: %s\n", result.data.c_str()); fflush(logf); }
            } else {
                result.success = false;
                result.data = "\"no handler\"";
            }

            std::string response = formatResult(result);
            if (logf) { fprintf(logf, "[SEND] %s", response.c_str()); fclose(logf); }
            write(clientFd, response.c_str(), response.size());
        }
    }
}
