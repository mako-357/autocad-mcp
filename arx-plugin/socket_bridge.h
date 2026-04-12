#pragma once

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <map>

// MCP サーバーと ObjectARX プラグイン間の Unix Domain Socket ブリッジ
// ソケットスレッドでコマンドを受信し、メインスレッドで実行結果を返す

struct BridgeCommand {
    std::string id;       // リクエストID（レスポンスの対応付け用）
    std::string method;   // "draw_line", "list_layers", etc.
    std::string params;   // JSON パラメータ
};

struct BridgeResult {
    std::string id;
    bool success;
    std::string data;     // JSON レスポンス
};

// コマンド処理コールバック（メインスレッドで呼ばれる）
using CommandHandler = std::function<BridgeResult(const BridgeCommand&)>;

class SocketBridge {
public:
    static SocketBridge& instance();

    // ソケットサーバーを開始（バックグラウンドスレッド）
    bool start(const std::string& socketPath);

    // ソケットサーバーを停止
    void stop();

    // コマンドハンドラを登録
    void setHandler(CommandHandler handler);

    // 未処理のコマンドがあるかチェック
    bool hasPendingCommands();

    // キューからコマンドを取り出して処理（AutoCAD メインスレッドから呼ぶ）
    void processPendingCommands();

    // ソケットパスを取得
    const std::string& socketPath() const { return m_socketPath; }

    bool isRunning() const { return m_running; }

private:
    SocketBridge() = default;
    ~SocketBridge();

    void serverLoop();
    void handleClient(int clientFd);
    void enqueueResult(const std::string& id, const BridgeResult& result);

    std::string m_socketPath;
    int m_serverFd = -1;
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;

    // コマンドキュー（ソケットスレッド → メインスレッド）
    std::mutex m_cmdMutex;
    std::queue<std::pair<int, BridgeCommand>> m_cmdQueue; // clientFd + command

    // 結果キュー（メインスレッド → ソケットスレッド）
    std::mutex m_resultMutex;
    std::condition_variable m_resultCv;
    std::map<std::string, BridgeResult> m_results;

    CommandHandler m_handler;
};
