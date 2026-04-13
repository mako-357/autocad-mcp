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
#include <dbhatch.h>
#include <acdocman.h>
#include <acedCmdNF.h>
#include <gemat3d.h>

#include "socket_bridge.h"
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <codecvt>
#include <locale>
#include <thread>
#include <cstdio>
#include <cmath>

static const char* SOCKET_PATH = "/tmp/gfp-arx-bridge.sock";

// =====================================================================
// ユーティリティ
// =====================================================================

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

// --- JSON パーサー ---

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
    while (pos < json.size() && (json[pos] == ' ')) pos++;
    try { return std::stod(json.substr(pos)); } catch (...) { return 0.0; }
}

static bool extractBool(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return (pos < json.size() && json[pos] == 't');
}

// JSON 配列から数値ペアを抽出: [[x1,y1],[x2,y2],...]
static std::vector<std::pair<double,double>> extractPoints(const std::string& json, const std::string& key) {
    std::vector<std::pair<double,double>> pts;
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return pts;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return pts;
    pos++; // skip outer [

    while (pos < json.size()) {
        while (pos < json.size() && json[pos] != '[' && json[pos] != ']') pos++;
        if (pos >= json.size() || json[pos] == ']') break;
        pos++; // skip inner [
        double x = 0, y = 0;
        try {
            x = std::stod(json.substr(pos));
            pos = json.find(',', pos);
            if (pos == std::string::npos) break;
            pos++;
            y = std::stod(json.substr(pos));
        } catch (...) { break; }
        pts.push_back({x, y});
        pos = json.find(']', pos);
        if (pos != std::string::npos) pos++;
    }
    return pts;
}

// resbuf → JSON
static std::string resbufToJson(const struct resbuf* rb) {
    if (!rb) return "null";
    if (!rb->rbnext) {
        switch (rb->restype) {
            case RTSTR: return "\"" + jsonEscape(wstr_to_utf8(rb->resval.rstring)) + "\"";
            case RTREAL: { std::ostringstream os; os << rb->resval.rreal; return os.str(); }
            case RTSHORT: { std::ostringstream os; os << rb->resval.rint; return os.str(); }
            case RTLONG: { std::ostringstream os; os << rb->resval.rlong; return os.str(); }
            case RTNONE: case RTNIL: return "null";
            case RTT: return "true";
            case RT3DPOINT: case RTPOINT: {
                std::ostringstream os;
                os << "[" << rb->resval.rpoint[0] << "," << rb->resval.rpoint[1] << "," << rb->resval.rpoint[2] << "]";
                return os.str();
            }
            default: { std::ostringstream os; os << "{\"type\":" << rb->restype << "}"; return os.str(); }
        }
    }
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const struct resbuf* p = rb; p; p = p->rbnext) {
        if (!first) os << ",";
        first = false;
        struct resbuf single = *p;
        single.rbnext = nullptr;
        os << resbufToJson(&single);
    }
    os << "]";
    return os.str();
}

// =====================================================================
// DB ヘルパー
// =====================================================================

static AcDbDatabase* getDb() {
    return acdbHostApplicationServices()->workingDatabase();
}

static AcDbBlockTableRecord* getModelSpace(AcDbDatabase* db, AcDb::OpenMode mode = AcDb::kForWrite) {
    AcDbBlockTable* bt = nullptr;
    db->getBlockTable(bt, AcDb::kForRead);
    AcDbBlockTableRecord* ms = nullptr;
    bt->getAt(ACDB_MODEL_SPACE, ms, mode);
    bt->close();
    return ms;
}

static std::string entityIdJson(AcDbObjectId id) {
    std::ostringstream os;
    os << "{\"entity_id\":\"" << id.asOldId() << "\"}";
    return os.str();
}

static AcDbObjectId parseEntityId(const std::string& params) {
    std::string idStr = extractStr(params, "entity_id");
    if (idStr.empty()) {
        // 数値として直接取得を試みる
        double idNum = extractNum(params, "entity_id");
        if (idNum > 0) {
            AcDbObjectId id;
            id.setFromOldId((Adesk::IntDbId)(long long)idNum);
            return id;
        }
        return AcDbObjectId::kNull;
    }
    try {
        long long idVal = std::stoll(idStr);
        AcDbObjectId id;
        id.setFromOldId((Adesk::IntDbId)idVal);
        return id;
    } catch (...) {
        return AcDbObjectId::kNull;
    }
}

// =====================================================================
// メインスレッド実行キュー
// =====================================================================

struct MainThreadRequest {
    BridgeCommand cmd;
    BridgeResult result;
    bool done = false;
    std::mutex mtx;
    std::condition_variable cv;
};

static std::mutex g_queueMtx;
static std::vector<MainThreadRequest*> g_queue;

// =====================================================================
// LISP 直接実行（sendStringToExecute 経由）
// =====================================================================

static BridgeResult executeLispDirect(const BridgeCommand& cmd) {
    BridgeResult result;
    result.id = cmd.id;

    if (cmd.method == "eval_lisp") {
        std::string expr = extractStr(cmd.params, "expression");
        if (expr.empty()) { result.success = false; result.data = "\"expression is required\""; return result; }

        const char* resultFile = "/tmp/gfp-lisp-result.txt";
        unlink(resultFile);

        std::ostringstream lispCmd;
        lispCmd << "(progn"
                << " (setq gfp:res (vl-princ-to-string " << expr << "))"
                << " (setq gfp:f (open \"/tmp/gfp-lisp-result.txt\" \"w\"))"
                << " (princ gfp:res gfp:f)(close gfp:f)(princ))\n";

        AcApDocument* pDoc = acDocManager->curDocument();
        if (pDoc) acDocManager->sendStringToExecute(pDoc, utf8_to_wstr(lispCmd.str()).c_str(), false, true);

        for (int i = 0; i < 50; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            FILE* f = fopen(resultFile, "r");
            if (f) {
                char buf[8192] = {0};
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f); unlink(resultFile);
                result.success = true;
                result.data = "\"" + jsonEscape(std::string(buf, n)) + "\"";
                return result;
            }
        }
        result.success = false; result.data = "\"LISP evaluation timeout\"";
    }
    else if (cmd.method == "acad_command") {
        std::string cmdStr = extractStr(cmd.params, "command");
        if (cmdStr.empty()) { result.success = false; result.data = "\"command is required\""; return result; }

        const char* doneFile = "/tmp/gfp-cmd-done.txt";
        unlink(doneFile);

        std::ostringstream lispCmd;
        lispCmd << "(progn " << cmdStr
                << " (setq gfp:f (open \"/tmp/gfp-cmd-done.txt\" \"w\"))"
                << " (princ \"OK\" gfp:f)(close gfp:f)(princ))\n";

        AcApDocument* pDoc = acDocManager->curDocument();
        if (pDoc) acDocManager->sendStringToExecute(pDoc, utf8_to_wstr(lispCmd.str()).c_str(), false, true);

        for (int i = 0; i < 100; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            FILE* f = fopen(doneFile, "r");
            if (f) { fclose(f); unlink(doneFile); result.success = true; result.data = "\"OK\""; return result; }
        }
        result.success = false; result.data = "\"Command execution timeout\"";
    }
    return result;
}

// =====================================================================
// メインスレッド実行ハンドラ（直接 ObjectARX API）
// =====================================================================

static BridgeResult executeOnMainThread(const BridgeCommand& cmd) {
    BridgeResult result;
    result.id = cmd.id;
    AcDbDatabase* db = getDb();

    // ---------- システム変数 ----------
    if (cmd.method == "get_variable") {
        std::string varName = extractStr(cmd.params, "name");
        if (varName.empty()) { result.success = false; result.data = "\"name is required\""; return result; }
        struct resbuf rb;
        if (acedGetVar(utf8_to_wstr(varName).c_str(), &rb) == RTNORM) {
            result.success = true; result.data = resbufToJson(&rb);
        } else {
            result.success = false; result.data = "\"Variable not found\"";
        }
    }
    else if (cmd.method == "set_variable") {
        std::string varName = extractStr(cmd.params, "name");
        std::string valStr = extractStr(cmd.params, "value");
        double valNum = extractNum(cmd.params, "value");
        struct resbuf rb;
        if (!valStr.empty()) {
            rb.restype = RTSTR;
            std::wstring wval = utf8_to_wstr(valStr);
            rb.resval.rstring = const_cast<wchar_t*>(wval.c_str());
            int s = acedSetVar(utf8_to_wstr(varName).c_str(), &rb);
            result.success = (s == RTNORM); result.data = result.success ? "\"OK\"" : "\"Failed\"";
        } else {
            rb.restype = RTREAL; rb.resval.rreal = valNum;
            int s = acedSetVar(utf8_to_wstr(varName).c_str(), &rb);
            result.success = (s == RTNORM); result.data = result.success ? "\"OK\"" : "\"Failed\"";
        }
    }

    // ---------- 図面情報 ----------
    else if (cmd.method == "get_drawing_info") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        std::ostringstream os;
        os << "{\"filename\":\"active_drawing\",\"units\":" << db->insunits() << "}";
        result.success = true; result.data = os.str();
    }
    else if (cmd.method == "get_bounds") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        AcGePoint3d mn = db->extmin(), mx = db->extmax();
        std::ostringstream os;
        os << "{\"min\":[" << mn.x << "," << mn.y << "," << mn.z
           << "],\"max\":[" << mx.x << "," << mx.y << "," << mx.z << "]}";
        result.success = true; result.data = os.str();
    }

    // ---------- レイヤー ----------
    else if (cmd.method == "list_layers") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        AcDbLayerTable* lt = nullptr;
        if (db->getLayerTable(lt, AcDb::kForRead) != Acad::eOk) {
            result.success = false; result.data = "\"Failed to get layer table\""; return result;
        }
        std::ostringstream os; os << "[";
        AcDbLayerTableIterator* iter = nullptr; lt->newIterator(iter);
        bool first = true;
        for (; !iter->done(); iter->step()) {
            AcDbLayerTableRecord* rec = nullptr;
            if (iter->getRecord(rec, AcDb::kForRead) == Acad::eOk) {
                const ACHAR* name = nullptr; rec->getName(name);
                if (name) {
                    if (!first) os << ",";
                    os << "{\"name\":\"" << jsonEscape(wstr_to_utf8(name)) << "\""
                       << ",\"color\":" << rec->color().colorIndex()
                       << ",\"off\":" << (rec->isOff() ? "true" : "false")
                       << ",\"locked\":" << (rec->isLocked() ? "true" : "false")
                       << ",\"frozen\":" << (rec->isFrozen() ? "true" : "false")
                       << "}";
                    first = false;
                }
                rec->close();
            }
        }
        delete iter; lt->close();
        os << "]"; result.success = true; result.data = os.str();
    }
    else if (cmd.method == "create_layer") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        std::string name = extractStr(cmd.params, "name");
        int color = (int)extractNum(cmd.params, "color");
        if (name.empty()) { result.success = false; result.data = "\"name is required\""; return result; }
        if (color <= 0) color = 7; // white

        AcDbLayerTable* lt = nullptr;
        db->getLayerTable(lt, AcDb::kForWrite);
        AcDbLayerTableRecord* rec = new AcDbLayerTableRecord();
        rec->setName(utf8_to_wstr(name).c_str());
        AcCmColor c; c.setColorIndex(color);
        rec->setColor(c);
        Acad::ErrorStatus es = lt->add(rec);
        rec->close(); lt->close();
        result.success = (es == Acad::eOk);
        result.data = result.success ? "\"OK\"" : "\"Layer already exists or error\"";
    }
    else if (cmd.method == "set_layer_property") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        std::string name = extractStr(cmd.params, "name");
        AcDbLayerTable* lt = nullptr;
        db->getLayerTable(lt, AcDb::kForRead);
        AcDbObjectId layerId;
        if (lt->getAt(utf8_to_wstr(name).c_str(), layerId) != Acad::eOk) {
            lt->close(); result.success = false; result.data = "\"Layer not found\""; return result;
        }
        lt->close();

        AcDbLayerTableRecord* rec = nullptr;
        acdbOpenObject(rec, layerId, AcDb::kForWrite);
        if (!rec) { result.success = false; result.data = "\"Cannot open layer\""; return result; }

        // color
        double colorVal = extractNum(cmd.params, "color");
        if (colorVal > 0) { AcCmColor c; c.setColorIndex((int)colorVal); rec->setColor(c); }
        // off
        std::string offStr = extractStr(cmd.params, "off");
        if (offStr == "true") rec->setIsOff(true);
        else if (offStr == "false") rec->setIsOff(false);
        // locked
        std::string lockedStr = extractStr(cmd.params, "locked");
        if (lockedStr == "true") rec->setIsLocked(true);
        else if (lockedStr == "false") rec->setIsLocked(false);

        rec->close();
        result.success = true; result.data = "\"OK\"";
    }

    // ---------- 描画: 線分 ----------
    else if (cmd.method == "draw_line") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        double x1=extractNum(cmd.params,"x1"), y1=extractNum(cmd.params,"y1"), z1=extractNum(cmd.params,"z1");
        double x2=extractNum(cmd.params,"x2"), y2=extractNum(cmd.params,"y2"), z2=extractNum(cmd.params,"z2");
        std::string layer = extractStr(cmd.params, "layer");

        AcDbBlockTableRecord* ms = getModelSpace(db);
        AcDbLine* ent = new AcDbLine(AcGePoint3d(x1,y1,z1), AcGePoint3d(x2,y2,z2));
        if (!layer.empty()) ent->setLayer(utf8_to_wstr(layer).c_str());
        AcDbObjectId id; ms->appendAcDbEntity(id, ent);
        ent->close(); ms->close();
        result.success = true; result.data = entityIdJson(id);
    }

    // ---------- 描画: 円 ----------
    else if (cmd.method == "draw_circle") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        double cx=extractNum(cmd.params,"cx"), cy=extractNum(cmd.params,"cy"), cz=extractNum(cmd.params,"cz");
        double r=extractNum(cmd.params,"radius");
        std::string layer = extractStr(cmd.params, "layer");

        AcDbBlockTableRecord* ms = getModelSpace(db);
        AcDbCircle* ent = new AcDbCircle(AcGePoint3d(cx,cy,cz), AcGeVector3d::kZAxis, r);
        if (!layer.empty()) ent->setLayer(utf8_to_wstr(layer).c_str());
        AcDbObjectId id; ms->appendAcDbEntity(id, ent);
        ent->close(); ms->close();
        result.success = true; result.data = entityIdJson(id);
    }

    // ---------- 描画: 円弧 ----------
    else if (cmd.method == "draw_arc") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        double cx=extractNum(cmd.params,"cx"), cy=extractNum(cmd.params,"cy"), cz=extractNum(cmd.params,"cz");
        double r=extractNum(cmd.params,"radius");
        double sa=extractNum(cmd.params,"start_angle"), ea=extractNum(cmd.params,"end_angle");
        std::string layer = extractStr(cmd.params, "layer");

        AcDbBlockTableRecord* ms = getModelSpace(db);
        AcDbArc* ent = new AcDbArc(AcGePoint3d(cx,cy,cz), r, sa * M_PI/180.0, ea * M_PI/180.0);
        if (!layer.empty()) ent->setLayer(utf8_to_wstr(layer).c_str());
        AcDbObjectId id; ms->appendAcDbEntity(id, ent);
        ent->close(); ms->close();
        result.success = true; result.data = entityIdJson(id);
    }

    // ---------- 描画: ポリライン ----------
    else if (cmd.method == "draw_polyline") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        auto pts = extractPoints(cmd.params, "points");
        bool closed = extractBool(cmd.params, "closed");
        std::string layer = extractStr(cmd.params, "layer");

        if (pts.size() < 2) { result.success = false; result.data = "\"Need at least 2 points\""; return result; }

        AcDbBlockTableRecord* ms = getModelSpace(db);
        AcDbPolyline* ent = new AcDbPolyline((unsigned int)pts.size());
        for (size_t i = 0; i < pts.size(); i++) {
            ent->addVertexAt((unsigned int)i, AcGePoint2d(pts[i].first, pts[i].second));
        }
        if (closed) ent->setClosed(Adesk::kTrue);
        if (!layer.empty()) ent->setLayer(utf8_to_wstr(layer).c_str());
        AcDbObjectId id; ms->appendAcDbEntity(id, ent);
        ent->close(); ms->close();
        result.success = true; result.data = entityIdJson(id);
    }

    // ---------- 描画: テキスト ----------
    else if (cmd.method == "draw_text") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        double x=extractNum(cmd.params,"x"), y=extractNum(cmd.params,"y"), z=extractNum(cmd.params,"z");
        double height=extractNum(cmd.params,"height");
        double rotation=extractNum(cmd.params,"rotation");
        std::string text = extractStr(cmd.params, "text");
        std::string layer = extractStr(cmd.params, "layer");
        if (height <= 0) height = 2.5;

        AcDbBlockTableRecord* ms = getModelSpace(db);
        AcDbText* ent = new AcDbText(AcGePoint3d(x,y,z), utf8_to_wstr(text).c_str(), AcDbObjectId::kNull, height, rotation * M_PI/180.0);
        if (!layer.empty()) ent->setLayer(utf8_to_wstr(layer).c_str());
        AcDbObjectId id; ms->appendAcDbEntity(id, ent);
        ent->close(); ms->close();
        result.success = true; result.data = entityIdJson(id);
    }

    // ---------- 編集: 削除 ----------
    else if (cmd.method == "delete_entity") {
        AcDbObjectId id = parseEntityId(cmd.params);
        if (id.isNull()) { result.success = false; result.data = "\"Invalid entity_id\""; return result; }
        AcDbEntity* ent = nullptr;
        if (acdbOpenObject(ent, id, AcDb::kForWrite) == Acad::eOk) {
            ent->erase();
            ent->close();
            result.success = true; result.data = "\"OK\"";
        } else {
            result.success = false; result.data = "\"Cannot open entity\"";
        }
    }

    // ---------- 編集: 移動 ----------
    else if (cmd.method == "move_entity") {
        AcDbObjectId id = parseEntityId(cmd.params);
        if (id.isNull()) { result.success = false; result.data = "\"Invalid entity_id\""; return result; }
        double dx=extractNum(cmd.params,"dx"), dy=extractNum(cmd.params,"dy"), dz=extractNum(cmd.params,"dz");

        AcDbEntity* ent = nullptr;
        if (acdbOpenObject(ent, id, AcDb::kForWrite) == Acad::eOk) {
            AcGeMatrix3d mat;
            mat.setToTranslation(AcGeVector3d(dx, dy, dz));
            ent->transformBy(mat);
            ent->close();
            result.success = true; result.data = "\"OK\"";
        } else {
            result.success = false; result.data = "\"Cannot open entity\"";
        }
    }

    // ---------- 編集: コピー ----------
    else if (cmd.method == "copy_entity") {
        AcDbObjectId srcId = parseEntityId(cmd.params);
        if (srcId.isNull()) { result.success = false; result.data = "\"Invalid entity_id\""; return result; }
        double dx=extractNum(cmd.params,"dx"), dy=extractNum(cmd.params,"dy"), dz=extractNum(cmd.params,"dz");

        AcDbEntity* srcEnt = nullptr;
        if (acdbOpenObject(srcEnt, srcId, AcDb::kForRead) != Acad::eOk) {
            result.success = false; result.data = "\"Cannot open entity\""; return result;
        }

        AcDbEntity* clone = AcDbEntity::cast(srcEnt->clone());
        srcEnt->close();

        if (!clone) { result.success = false; result.data = "\"Clone failed\""; return result; }

        if (dx != 0 || dy != 0 || dz != 0) {
            AcGeMatrix3d mat; mat.setToTranslation(AcGeVector3d(dx, dy, dz));
            clone->transformBy(mat);
        }

        AcDbBlockTableRecord* ms = getModelSpace(db);
        AcDbObjectId newId; ms->appendAcDbEntity(newId, clone);
        clone->close(); ms->close();
        result.success = true; result.data = entityIdJson(newId);
    }

    // ---------- 編集: 回転 ----------
    else if (cmd.method == "rotate_entity") {
        AcDbObjectId id = parseEntityId(cmd.params);
        if (id.isNull()) { result.success = false; result.data = "\"Invalid entity_id\""; return result; }
        double cx=extractNum(cmd.params,"cx"), cy=extractNum(cmd.params,"cy");
        double angle=extractNum(cmd.params,"angle"); // degrees

        AcDbEntity* ent = nullptr;
        if (acdbOpenObject(ent, id, AcDb::kForWrite) == Acad::eOk) {
            AcGeMatrix3d mat;
            mat.setToRotation(angle * M_PI / 180.0, AcGeVector3d::kZAxis, AcGePoint3d(cx, cy, 0));
            ent->transformBy(mat);
            ent->close();
            result.success = true; result.data = "\"OK\"";
        } else {
            result.success = false; result.data = "\"Cannot open entity\"";
        }
    }

    // ---------- 編集: スケール ----------
    else if (cmd.method == "scale_entity") {
        AcDbObjectId id = parseEntityId(cmd.params);
        if (id.isNull()) { result.success = false; result.data = "\"Invalid entity_id\""; return result; }
        double cx=extractNum(cmd.params,"cx"), cy=extractNum(cmd.params,"cy");
        double factor=extractNum(cmd.params,"factor");

        AcDbEntity* ent = nullptr;
        if (acdbOpenObject(ent, id, AcDb::kForWrite) == Acad::eOk) {
            AcGeMatrix3d mat;
            mat.setToScaling(factor, AcGePoint3d(cx, cy, 0));
            ent->transformBy(mat);
            ent->close();
            result.success = true; result.data = "\"OK\"";
        } else {
            result.success = false; result.data = "\"Cannot open entity\"";
        }
    }

    // ---------- 編集: レイヤー変更 ----------
    else if (cmd.method == "set_entity_layer") {
        AcDbObjectId id = parseEntityId(cmd.params);
        if (id.isNull()) { result.success = false; result.data = "\"Invalid entity_id\""; return result; }
        std::string layer = extractStr(cmd.params, "layer");

        AcDbEntity* ent = nullptr;
        if (acdbOpenObject(ent, id, AcDb::kForWrite) == Acad::eOk) {
            ent->setLayer(utf8_to_wstr(layer).c_str());
            ent->close();
            result.success = true; result.data = "\"OK\"";
        } else {
            result.success = false; result.data = "\"Cannot open entity\"";
        }
    }

    // ---------- 編集: 色変更 ----------
    else if (cmd.method == "set_entity_color") {
        AcDbObjectId id = parseEntityId(cmd.params);
        if (id.isNull()) { result.success = false; result.data = "\"Invalid entity_id\""; return result; }
        int color = (int)extractNum(cmd.params, "color");

        AcDbEntity* ent = nullptr;
        if (acdbOpenObject(ent, id, AcDb::kForWrite) == Acad::eOk) {
            ent->setColorIndex(color);
            ent->close();
            result.success = true; result.data = "\"OK\"";
        } else {
            result.success = false; result.data = "\"Cannot open entity\"";
        }
    }

    // ---------- クエリ: エンティティ情報 ----------
    else if (cmd.method == "get_entity") {
        AcDbObjectId id = parseEntityId(cmd.params);
        if (id.isNull()) { result.success = false; result.data = "\"Invalid entity_id\""; return result; }

        AcDbEntity* ent = nullptr;
        if (acdbOpenObject(ent, id, AcDb::kForRead) != Acad::eOk) {
            result.success = false; result.data = "\"Cannot open entity\""; return result;
        }

        std::ostringstream os;
        os << "{\"entity_id\":\"" << id.asOldId() << "\""
           << ",\"type\":\"" << wstr_to_utf8(ent->isA()->name()) << "\""
           << ",\"layer\":\"" << jsonEscape(wstr_to_utf8(ent->layer())) << "\""
           << ",\"color\":" << ent->colorIndex();

        // タイプ別の追加情報
        if (ent->isKindOf(AcDbLine::desc())) {
            AcDbLine* line = AcDbLine::cast(ent);
            AcGePoint3d s = line->startPoint(), e = line->endPoint();
            os << ",\"start\":[" << s.x << "," << s.y << "," << s.z << "]"
               << ",\"end\":[" << e.x << "," << e.y << "," << e.z << "]";
        }
        else if (ent->isKindOf(AcDbCircle::desc())) {
            AcDbCircle* c = AcDbCircle::cast(ent);
            AcGePoint3d ct = c->center();
            os << ",\"center\":[" << ct.x << "," << ct.y << "," << ct.z << "]"
               << ",\"radius\":" << c->radius();
        }
        else if (ent->isKindOf(AcDbArc::desc())) {
            AcDbArc* a = AcDbArc::cast(ent);
            AcGePoint3d ct = a->center();
            os << ",\"center\":[" << ct.x << "," << ct.y << "," << ct.z << "]"
               << ",\"radius\":" << a->radius()
               << ",\"start_angle\":" << (a->startAngle() * 180.0 / M_PI)
               << ",\"end_angle\":" << (a->endAngle() * 180.0 / M_PI);
        }
        else if (ent->isKindOf(AcDbText::desc())) {
            AcDbText* t = AcDbText::cast(ent);
            AcGePoint3d pos = t->position();
            os << ",\"position\":[" << pos.x << "," << pos.y << "," << pos.z << "]"
               << ",\"text\":\"" << jsonEscape(wstr_to_utf8(t->textStringConst())) << "\""
               << ",\"height\":" << t->height();
        }
        else if (ent->isKindOf(AcDbPolyline::desc())) {
            AcDbPolyline* pl = AcDbPolyline::cast(ent);
            os << ",\"num_vertices\":" << pl->numVerts()
               << ",\"closed\":" << (pl->isClosed() ? "true" : "false")
               << ",\"vertices\":[";
            for (unsigned int i = 0; i < pl->numVerts(); i++) {
                AcGePoint2d pt; pl->getPointAt(i, pt);
                if (i > 0) os << ",";
                os << "[" << pt.x << "," << pt.y << "]";
            }
            os << "]";
        }

        os << "}";
        ent->close();
        result.success = true; result.data = os.str();
    }

    // ---------- クエリ: レイヤーでフィルタ ----------
    else if (cmd.method == "select_by_layer") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        std::string layer = extractStr(cmd.params, "layer");
        if (layer.empty()) { result.success = false; result.data = "\"layer is required\""; return result; }

        AcDbBlockTableRecord* ms = getModelSpace(db, AcDb::kForRead);
        AcDbBlockTableRecordIterator* iter = nullptr;
        ms->newIterator(iter);

        std::ostringstream os; os << "[";
        bool first = true;
        for (; !iter->done(); iter->step()) {
            AcDbEntity* ent = nullptr;
            if (iter->getEntity(ent, AcDb::kForRead) == Acad::eOk) {
                std::string entLayer = wstr_to_utf8(ent->layer());
                if (entLayer == layer) {
                    if (!first) os << ",";
                    os << "\"" << ent->objectId().asOldId() << "\"";
                    first = false;
                }
                ent->close();
            }
        }
        delete iter; ms->close();
        os << "]"; result.success = true; result.data = os.str();
    }

    // ---------- クエリ: タイプでフィルタ ----------
    else if (cmd.method == "select_by_type") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        std::string type = extractStr(cmd.params, "type"); // "AcDbLine", "AcDbCircle", etc.
        if (type.empty()) { result.success = false; result.data = "\"type is required\""; return result; }

        AcDbBlockTableRecord* ms = getModelSpace(db, AcDb::kForRead);
        AcDbBlockTableRecordIterator* iter = nullptr;
        ms->newIterator(iter);

        std::ostringstream os; os << "[";
        bool first = true;
        for (; !iter->done(); iter->step()) {
            AcDbEntity* ent = nullptr;
            if (iter->getEntity(ent, AcDb::kForRead) == Acad::eOk) {
                std::string entType = wstr_to_utf8(ent->isA()->name());
                if (entType == type) {
                    if (!first) os << ",";
                    os << "\"" << ent->objectId().asOldId() << "\"";
                    first = false;
                }
                ent->close();
            }
        }
        delete iter; ms->close();
        os << "]"; result.success = true; result.data = os.str();
    }

    // ---------- クエリ: エンティティ数 ----------
    else if (cmd.method == "count_entities") {
        if (!db) { result.success = false; result.data = "\"No active drawing\""; return result; }
        AcDbBlockTableRecord* ms = getModelSpace(db, AcDb::kForRead);
        AcDbBlockTableRecordIterator* iter = nullptr;
        ms->newIterator(iter);
        int count = 0;
        for (; !iter->done(); iter->step()) count++;
        delete iter; ms->close();
        std::ostringstream os; os << count;
        result.success = true; result.data = os.str();
    }

    else {
        result.success = false;
        result.data = "\"Unknown method: " + cmd.method + "\"";
    }

    return result;
}

// =====================================================================
// コマンドハンドラ（ソケットスレッドから呼ばれる）
// =====================================================================

static BridgeResult handleCommand(const BridgeCommand& cmd) {
    BridgeResult result;
    result.id = cmd.id;

    if (cmd.method == "hello") {
        result.success = true;
        result.data = "\"Hello from AutoCAD ObjectARX!\"";
        return result;
    }

    // LISP 系は sendStringToExecute で直接実行
    if (cmd.method == "eval_lisp" || cmd.method == "acad_command") {
        return executeLispDirect(cmd);
    }

    // その他は全てメインスレッドキュー経由
    MainThreadRequest req;
    req.cmd = cmd;

    { std::lock_guard<std::mutex> lock(g_queueMtx); g_queue.push_back(&req); }

    AcApDocument* pDoc = acDocManager->curDocument();
    if (pDoc) acDocManager->sendStringToExecute(pDoc, L"GFPPROCESS\n");

    { std::unique_lock<std::mutex> lock(req.mtx);
      req.cv.wait_for(lock, std::chrono::seconds(30), [&]{ return req.done; }); }

    return req.done ? req.result : (result.success = false, result.data = "\"Timeout (30s)\"", result);
}

// =====================================================================
// AutoCAD コマンド登録
// =====================================================================

void helloCmd() { acutPrintf(L"\nGFP MCP Bridge: Hello from ObjectARX on Mac!"); }
void bridgeStatusCmd() {
    auto& b = SocketBridge::instance();
    acutPrintf(L"\n[GFP] Socket bridge: %hs", b.isRunning() ? "RUNNING" : "STOPPED");
}
void bridgeProcessCmd() {
    std::vector<MainThreadRequest*> pending;
    { std::lock_guard<std::mutex> lock(g_queueMtx); std::swap(pending, g_queue); }
    for (auto* req : pending) {
        req->result = executeOnMainThread(req->cmd);
        { std::lock_guard<std::mutex> lock(req->mtx); req->done = true; }
        req->cv.notify_one();
    }
}

void initApp() {
    acedRegCmds->addCommand(L"GFP_COMMANDS", L"GFPHELLO", L"GFPHELLO", ACRX_CMD_MODAL, helloCmd);
    acedRegCmds->addCommand(L"GFP_COMMANDS", L"GFPSTATUS", L"GFPSTATUS", ACRX_CMD_MODAL, bridgeStatusCmd);
    acedRegCmds->addCommand(L"GFP_COMMANDS", L"GFPPROCESS", L"GFPPROCESS", ACRX_CMD_MODAL, bridgeProcessCmd);
    auto& bridge = SocketBridge::instance();
    bridge.setHandler(handleCommand);
    if (bridge.start(SOCKET_PATH)) acutPrintf(L"\n[GFP] Socket bridge started");
    else acutPrintf(L"\n[GFP] ERROR: Failed to start socket bridge");
}

void unloadApp() { SocketBridge::instance().stop(); acedRegCmds->removeGroup(L"GFP_COMMANDS"); }

extern "C"
AcRx::AppRetCode acrxEntryPoint(AcRx::AppMsgCode msg, void* appId) {
    switch (msg) {
    case AcRx::kInitAppMsg:
        acrxDynamicLinker->unlockApplication(appId);
        acrxDynamicLinker->registerAppMDIAware(appId);
        initApp();
        break;
    case AcRx::kUnloadAppMsg: unloadApp(); break;
    default: break;
    }
    return AcRx::kRetOK;
}
