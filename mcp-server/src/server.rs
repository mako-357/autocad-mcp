use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{ServerCapabilities, ServerInfo};
use rmcp::{ServerHandler, schemars, tool, tool_handler, tool_router};
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::json;

use crate::bridge::AcadBridge;

/// AutoCAD MCP サーバー
#[derive(Clone)]
pub struct AutocadMcpServer {
    tool_router: ToolRouter<Self>,
}

impl AutocadMcpServer {
    pub fn new() -> Self {
        let tool_router = Self::tool_router();
        Self { tool_router }
    }
}

// --- ツール入力スキーマ ---

#[derive(Debug, Deserialize, JsonSchema)]
pub struct EvalLispInput {
    #[schemars(description = "評価する AutoLISP 式。例: (+ 1 2), (command \"LINE\" \"0,0\" \"100,100\" \"\"), (getvar \"CLAYER\")")]
    pub expression: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct AcadCommandInput {
    #[schemars(description = "実行する AutoLISP command 式。例: (command \"CIRCLE\" \"0,0\" 100), (command \"ZOOM\" \"E\")")]
    pub command: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct GetVariableInput {
    #[schemars(description = "取得するシステム変数名。例: CLAYER, DIMSCALE, OSMODE, LUNITS")]
    pub name: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DrawLineInput {
    #[schemars(description = "始点 X 座標")]
    pub x1: f64,
    #[schemars(description = "始点 Y 座標")]
    pub y1: f64,
    #[schemars(description = "始点 Z 座標（省略時 0）")]
    pub z1: Option<f64>,
    #[schemars(description = "終点 X 座標")]
    pub x2: f64,
    #[schemars(description = "終点 Y 座標")]
    pub y2: f64,
    #[schemars(description = "終点 Z 座標（省略時 0）")]
    pub z2: Option<f64>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct EmptyInput {}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DrawRectangleInput {
    #[schemars(description = "左下 X 座標")]
    pub x: f64,
    #[schemars(description = "左下 Y 座標")]
    pub y: f64,
    #[schemars(description = "幅")]
    pub width: f64,
    #[schemars(description = "高さ")]
    pub height: f64,
}

// --- ツール実装 ---

fn bridge_call(method: &str, params: serde_json::Value) -> String {
    match AcadBridge::send(method, params) {
        Ok(resp) if resp.success => {
            serde_json::to_string_pretty(&resp.data).unwrap_or_else(|_| resp.data.to_string())
        }
        Ok(resp) => format!("エラー: {}", resp.data),
        Err(e) => format!("通信エラー: {}", e),
    }
}

#[tool_router]
impl AutocadMcpServer {
    /// AutoLISP 式を評価する。AutoCAD の全機能にアクセスできる汎用ツール。
    #[tool(name = "eval_lisp", description = "AutoLISP 式を評価します。AutoCAD の全コマンド・関数にアクセス可能。例: (+ 1 2), (command \"LINE\" \"0,0\" \"100,100\" \"\"), (setvar \"CLAYER\" \"0\"), (entlast), (ssget \"X\")")]
    async fn eval_lisp(&self, Parameters(input): Parameters<EvalLispInput>) -> String {
        bridge_call("eval_lisp", json!({ "expression": input.expression }))
    }

    /// AutoCAD コマンドを実行する（LISP の command 関数経由）
    #[tool(name = "acad_command", description = "AutoCAD コマンドを LISP 経由で実行します。例: (command \"CIRCLE\" \"0,0\" 100), (command \"ZOOM\" \"E\"), (command \"LAYER\" \"M\" \"MyLayer\" \"\")")]
    async fn acad_command(&self, Parameters(input): Parameters<AcadCommandInput>) -> String {
        bridge_call("acad_command", json!({ "command": input.command }))
    }

    /// AutoCAD のシステム変数を取得する
    #[tool(name = "get_variable", description = "AutoCAD のシステム変数を取得します。例: CLAYER(現在のレイヤー), DIMSCALE(寸法スケール), LUNITS(長さの単位)")]
    async fn get_variable(&self, Parameters(input): Parameters<GetVariableInput>) -> String {
        bridge_call("get_variable", json!({ "name": input.name }))
    }

    /// AutoCAD に線分を描画する
    #[tool(name = "draw_line", description = "AutoCAD に線分を描画します")]
    async fn draw_line(&self, Parameters(input): Parameters<DrawLineInput>) -> String {
        let z1 = input.z1.unwrap_or(0.0);
        let z2 = input.z2.unwrap_or(0.0);
        bridge_call("draw_line", json!({
            "x1": input.x1, "y1": input.y1, "z1": z1,
            "x2": input.x2, "y2": input.y2, "z2": z2,
        }))
    }

    /// AutoCAD に四角形を描画する（4本の線分）
    #[tool(name = "draw_rectangle", description = "AutoCAD に四角形を描画します")]
    async fn draw_rectangle(&self, Parameters(input): Parameters<DrawRectangleInput>) -> String {
        let x = input.x;
        let y = input.y;
        let w = input.width;
        let h = input.height;

        let lines = vec![
            (x, y, x + w, y),
            (x + w, y, x + w, y + h),
            (x + w, y + h, x, y + h),
            (x, y + h, x, y),
        ];

        let mut ids = Vec::new();
        for (x1, y1, x2, y2) in lines {
            match AcadBridge::send("draw_line", json!({
                "x1": x1, "y1": y1, "z1": 0,
                "x2": x2, "y2": y2, "z2": 0,
            })) {
                Ok(resp) if resp.success => ids.push(resp.data.to_string()),
                Ok(resp) => return format!("エラー: {}", resp.data),
                Err(e) => return format!("通信エラー: {}", e),
            }
        }

        format!("四角形を描画しました: 位置({},{}) サイズ {}x{}", x, y, w, h)
    }

    /// 現在の図面情報を取得する
    #[tool(name = "get_drawing_info", description = "現在開いている AutoCAD 図面の情報を取得します")]
    async fn get_drawing_info(&self, Parameters(_input): Parameters<EmptyInput>) -> String {
        bridge_call("get_drawing_info", json!({}))
    }

    /// 図面のレイヤー一覧を取得する
    #[tool(name = "list_layers", description = "AutoCAD 図面のレイヤー一覧を取得します")]
    async fn list_layers(&self, Parameters(_input): Parameters<EmptyInput>) -> String {
        bridge_call("list_layers", json!({}))
    }
}

#[tool_handler]
impl ServerHandler for AutocadMcpServer {
    fn get_info(&self) -> ServerInfo {
        let mut info = ServerInfo::new(ServerCapabilities::builder().enable_tools().build());
        info.instructions = Some("AutoCAD を操作するための MCP サーバーです。eval_lisp で AutoCAD の全機能にアクセスできます。".into());
        info
    }
}
