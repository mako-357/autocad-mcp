#if defined(_DEBUG) && !defined(AC_FULL_DEBUG)
#error _DEBUG should not be defined except in internal Adesk debug builds
#endif

#include <windef.h>
#include <adslib.h>
#include <aced.h>
#include <rxregsvc.h>
#include <dbapserv.h>
#include <dbsymtb.h>
#include <dbobjptr.h>
#include <dbents.h>
#include <acdocman.h>
#include <acedCmdNF.h>

#include "socket_bridge.h"
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <codecvt>
#include <locale>
#include <thread>
#include <cstdio>

static const char* SOCKET_PATH = "/tmp/gfp-arx-bridge.sock";

// --- wchar_t ↔ UTF-8 変換 ---

static std::string wstr_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.to_bytes(wstr);
}

static std::wstring utf8_to_wstr(const std::string& str) {
    if (str.empty()) return L"";
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.from_bytes(str);
}

// --- JSON ヘルパー ---

static std::string extractStr(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    // エスケープ対応
    std::string result;
    for (; pos < json.size() && json[pos] != '"'; pos++) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            if (json[pos] == 'n') result += '\n';
            else if (json[pos] == 't') result += '\t';
            else if (json[pos] == '"') result += '"';
            else if (json[pos] == '\\') result += '\\';
            else result += json[pos];
        } else {
            result += json[pos];
        }
    }
    return result;
}

static double extractNum(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0.0;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0.0;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return std::stod(json.substr(pos));
}

// JSON 文字列エスケープ
static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// --- resbuf → JSON 変換 ---

static std::string resbufToJson(const struct resbuf* rb) {
    if (!rb) return "null";

    // 単一値の場合
    if (!rb->rbnext) {
        switch (rb->restype) {
            case RTSTR:
                return "\"" + jsonEscape(wstr_to_utf8(rb->resval.rstring)) + "\"";
            case RTREAL:
            case RT3DPOINT: {
                std::ostringstream os;
                os << rb->resval.rreal;
                return os.str();
            }
            case RTSHORT: {
                std::ostringstream os;
                os << rb->resval.rint;
                return os.str();
            }
            case RTLONG: {
                std::ostringstream os;
                os << rb->resval.rlong;
                return os.str();
            }
            case RTNONE:
            case RTNIL:
                return "null";
            case RTT:
                return "true";
            default: {
                std::ostringstream os;
                os << "{\"type\":" << rb->restype << "}";
                return os.str();
            }
        }
    }

    // リストの場合
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const struct resbuf* p = rb; p; p = p->rbnext) {
        if (!first) os << ",";
        first = false;
        switch (p->restype) {
            case RTSTR:
                os << "\"" << jsonEscape(wstr_to_utf8(p->resval.rstring)) << "\"";
                break;
            case RTREAL:
                os << p->resval.rreal;
                break;
            case RTSHORT:
                os << p->resval.rint;
                break;
            case RTLONG:
                os << p->resval.rlong;
                break;
            case RTNONE:
            case RTNIL:
                os << "null";
                break;
            case RTT:
                os << "true";
                break;
            case RT3DPOINT:
            case RTPOINT: {
                os << "[" << p->resval.rpoint[0] << ","
                   << p->resval.rpoint[1] << ","
                   << p->resval.rpoint[2] << "]";
                break;
            }
            default:
                os << "{\"type\":" << p->restype << "}";
                break;
        }
    }
    os << "]";
    return os.str();
}

// --- メインスレッド実行キュー ---

struct MainThreadRequest {
    BridgeCommand cmd;
    BridgeResult result;
    bool done = false;
    std::mutex mtx;
    std::condition_variable cv;
};

static std::mutex g_queueMtx;
static std::vector<MainThreadRequest*> g_queue;

// sendStringToExecute で LISP を直接実行（ソケットスレッドから呼べる）
static BridgeResult executeLispDirect(const BridgeCommand& cmd) {
    BridgeResult result;
    result.id = cmd.id;

    if (cmd.method == "eval_lisp") {
        std::string expr = extractStr(cmd.params, "expression");
        if (expr.empty()) {
            result.success = false;
            result.data = "\"expression is required\"";
            return result;
        }

        const char* resultFile = "/tmp/gfp-lisp-result.txt";
        unlink(resultFile);

        // LISP式: 結果を vl-princ-to-string でファイルに書き出す
        std::ostringstream lispCmd;
        lispCmd << "(progn"
                << " (setq gfp:res (vl-princ-to-string " << expr << "))"
                << " (setq gfp:f (open \"/tmp/gfp-lisp-result.txt\" \"w\"))"
                << " (princ gfp:res gfp:f)"
                << " (close gfp:f)"
                << " (princ)"
                << ")\n";

        std::wstring wlispCmd = utf8_to_wstr(lispCmd.str());
        AcApDocument* pDoc = acDocManager->curDocument();
        if (pDoc) {
            acDocManager->sendStringToExecute(pDoc, wlispCmd.c_str(), false, true);
        }

        for (int i = 0; i < 50; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            FILE* f = fopen(resultFile, "r");
            if (f) {
                char buf[8192] = {0};
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                unlink(resultFile);
                result.success = true;
                result.data = "\"" + jsonEscape(std::string(buf, n)) + "\"";
                return result;
            }
        }
        result.success = false;
        result.data = "\"LISP evaluation timeout\"";
    }
    else if (cmd.method == "acad_command") {
        std::string cmdStr = extractStr(cmd.params, "command");
        if (cmdStr.empty()) {
            result.success = false;
            result.data = "\"command is required\"";
            return result;
        }

        const char* doneFile = "/tmp/gfp-cmd-done.txt";
        unlink(doneFile);

        std::ostringstream lispCmd;
        lispCmd << "(progn " << cmdStr
                << " (setq gfp:f (open \"/tmp/gfp-cmd-done.txt\" \"w\"))"
                << " (princ \"OK\" gfp:f)"
                << " (close gfp:f)"
                << " (princ))\n";

        std::wstring wlispCmd = utf8_to_wstr(lispCmd.str());
        AcApDocument* pDoc = acDocManager->curDocument();
        if (pDoc) {
            acDocManager->sendStringToExecute(pDoc, wlispCmd.c_str(), false, true);
        }

        for (int i = 0; i < 100; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            FILE* f = fopen(doneFile, "r");
            if (f) {
                fclose(f);
                unlink(doneFile);
                result.success = true;
                result.data = "\"OK\"";
                return result;
            }
        }
        result.success = false;
        result.data = "\"Command execution timeout\"";
    }

    return result;
}

// メインスレッドで実行するハンドラ
static BridgeResult executeOnMainThread(const BridgeCommand& cmd) {
    BridgeResult result;
    result.id = cmd.id;

    if (cmd.method == "get_variable") {
        std::string varName = extractStr(cmd.params, "name");
        if (varName.empty()) {
            result.success = false;
            result.data = "\"name is required\"";
            return result;
        }

        std::wstring wvar = utf8_to_wstr(varName);
        struct resbuf rb;
        int status = acedGetVar(wvar.c_str(), &rb);

        if (status == RTNORM) {
            result.success = true;
            result.data = resbufToJson(&rb);
            // RTSTR の場合はメモリ解放不要（acedGetVar が管理）
        } else {
            result.success = false;
            result.data = "\"Variable not found: " + varName + "\"";
        }
    }
    else if (cmd.method == "draw_line") {
        double x1 = extractNum(cmd.params, "x1"), y1 = extractNum(cmd.params, "y1"), z1 = extractNum(cmd.params, "z1");
        double x2 = extractNum(cmd.params, "x2"), y2 = extractNum(cmd.params, "y2"), z2 = extractNum(cmd.params, "z2");

        AcDbDatabase* db = acdbHostApplicationServices()->workingDatabase();
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }

        AcDbBlockTable* bt = nullptr;
        db->getBlockTable(bt, AcDb::kForRead);
        AcDbBlockTableRecord* ms = nullptr;
        bt->getAt(ACDB_MODEL_SPACE, ms, AcDb::kForWrite);
        bt->close();

        AcDbLine* line = new AcDbLine(AcGePoint3d(x1, y1, z1), AcGePoint3d(x2, y2, z2));
        AcDbObjectId lineId;
        ms->appendAcDbEntity(lineId, line);
        line->close();
        ms->close();

        std::ostringstream os;
        os << "{\"entity_id\":\"" << lineId.asOldId() << "\"}";
        result.success = true;
        result.data = os.str();
    }
    else if (cmd.method == "get_drawing_info") {
        AcDbDatabase* db = acdbHostApplicationServices()->workingDatabase();
        if (db) {
            std::ostringstream os;
            os << "{\"filename\":\"active_drawing\""
               << ",\"units\":" << db->insunits()
               << "}";
            result.success = true;
            result.data = os.str();
        } else {
            result.success = false;
            result.data = "\"No active drawing\"";
        }
    }
    else if (cmd.method == "list_layers") {
        AcDbDatabase* db = acdbHostApplicationServices()->workingDatabase();
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }

        AcDbLayerTable* lt = nullptr;
        if (db->getLayerTable(lt, AcDb::kForRead) != Acad::eOk) {
            result.success = false; result.data = "\"Failed to get layer table\""; return result;
        }

        std::ostringstream os;
        os << "[";
        AcDbLayerTableIterator* iter = nullptr;
        lt->newIterator(iter);
        bool first = true;
        for (; !iter->done(); iter->step()) {
            AcDbLayerTableRecord* rec = nullptr;
            if (iter->getRecord(rec, AcDb::kForRead) == Acad::eOk) {
                const ACHAR* name = nullptr;
                rec->getName(name);
                if (name) {
                    if (!first) os << ",";
                    os << "\"" << jsonEscape(wstr_to_utf8(name)) << "\"";
                    first = false;
                }
                rec->close();
            }
        }
        delete iter;
        lt->close();
        os << "]";
        result.success = true;
        result.data = os.str();
    }
    else {
        result.success = false;
        result.data = "\"Unknown method: " + cmd.method + "\"";
    }

    return result;
}

// --- コマンドハンドラ（ソケットスレッドから呼ばれる） ---

static BridgeResult handleCommand(const BridgeCommand& cmd) {
    BridgeResult result;
    result.id = cmd.id;

    // ソケットスレッドで即応答
    if (cmd.method == "hello") {
        result.success = true;
        result.data = "\"Hello from AutoCAD ObjectARX!\"";
        return result;
    }

    // eval_lisp / acad_command は sendStringToExecute で直接実行（キュー不要）
    if (cmd.method == "eval_lisp" || cmd.method == "acad_command") {
        return executeLispDirect(cmd);
    }

    // その他はメインスレッドキュー経由
    MainThreadRequest req;
    req.cmd = cmd;

    {
        std::lock_guard<std::mutex> lock(g_queueMtx);
        g_queue.push_back(&req);
    }

    AcApDocument* pDoc = acDocManager->curDocument();
    if (pDoc) {
        acDocManager->sendStringToExecute(pDoc, L"GFPPROCESS\n");
    }

    {
        std::unique_lock<std::mutex> lock(req.mtx);
        req.cv.wait_for(lock, std::chrono::seconds(30), [&]{ return req.done; });
    }

    if (req.done) {
        return req.result;
    }
    result.success = false;
    result.data = "\"Timeout waiting for main thread (30s)\"";
    return result;
}

// --- AutoCAD コマンド ---

void helloCmd() {
    acutPrintf(L"\nGFP MCP Bridge: Hello from ObjectARX on Mac!");
}

void bridgeStatusCmd() {
    auto& bridge = SocketBridge::instance();
    acutPrintf(L"\n[GFP] Socket bridge: %hs", bridge.isRunning() ? "RUNNING" : "STOPPED");
}

void bridgeProcessCmd() {
    std::vector<MainThreadRequest*> pending;
    {
        std::lock_guard<std::mutex> lock(g_queueMtx);
        std::swap(pending, g_queue);
    }
    for (auto* req : pending) {
        req->result = executeOnMainThread(req->cmd);
        {
            std::lock_guard<std::mutex> lock(req->mtx);
            req->done = true;
        }
        req->cv.notify_one();
    }
}

// --- App lifecycle ---

void initApp() {
    acedRegCmds->addCommand(L"GFP_COMMANDS", L"GFPHELLO", L"GFPHELLO",
                            ACRX_CMD_MODAL, helloCmd);
    acedRegCmds->addCommand(L"GFP_COMMANDS", L"GFPSTATUS", L"GFPSTATUS",
                            ACRX_CMD_MODAL, bridgeStatusCmd);
    acedRegCmds->addCommand(L"GFP_COMMANDS", L"GFPPROCESS", L"GFPPROCESS",
                            ACRX_CMD_MODAL, bridgeProcessCmd);

    auto& bridge = SocketBridge::instance();
    bridge.setHandler(handleCommand);

    if (bridge.start(SOCKET_PATH)) {
        acutPrintf(L"\n[GFP] Socket bridge started");
    } else {
        acutPrintf(L"\n[GFP] ERROR: Failed to start socket bridge");
    }
}

void unloadApp() {
    SocketBridge::instance().stop();
    acedRegCmds->removeGroup(L"GFP_COMMANDS");
}

extern "C"
AcRx::AppRetCode acrxEntryPoint(AcRx::AppMsgCode msg, void* appId) {
    switch (msg) {
    case AcRx::kInitAppMsg:
        acrxDynamicLinker->unlockApplication(appId);
        acrxDynamicLinker->registerAppMDIAware(appId);
        initApp();
        break;
    case AcRx::kUnloadAppMsg:
        unloadApp();
        break;
    default: break;
    }
    return AcRx::kRetOK;
}
