use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{ServerCapabilities, ServerInfo};
use rmcp::{ServerHandler, schemars, tool, tool_handler, tool_router};
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::json;

use crate::bridge::AcadBridge;

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

// =====================================================================
// 入力スキーマ
// =====================================================================

#[derive(Debug, Deserialize, JsonSchema)]
pub struct EmptyInput {}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct EvalLispInput {
    #[schemars(description = "AutoLISP 式。例: (+ 1 2), (getvar \"CLAYER\"), (ssget \"X\")")]
    pub expression: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct AcadCommandInput {
    #[schemars(description = "AutoLISP command 式。例: (command \"CIRCLE\" \"0,0\" 100)")]
    pub command: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct GetVariableInput {
    #[schemars(description = "変数名。例: CLAYER, DIMSCALE, OSMODE")]
    pub name: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SetVariableInput {
    #[schemars(description = "変数名")]
    pub name: String,
    #[schemars(description = "設定値（文字列または数値）")]
    pub value: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DrawLineInput {
    pub x1: f64, pub y1: f64,
    pub z1: Option<f64>,
    pub x2: f64, pub y2: f64,
    pub z2: Option<f64>,
    pub layer: Option<String>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DrawCircleInput {
    pub cx: f64, pub cy: f64, pub cz: Option<f64>,
    pub radius: f64,
    pub layer: Option<String>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DrawArcInput {
    pub cx: f64, pub cy: f64, pub cz: Option<f64>,
    pub radius: f64,
    #[schemars(description = "開始角度（度）")]
    pub start_angle: f64,
    #[schemars(description = "終了角度（度）")]
    pub end_angle: f64,
    pub layer: Option<String>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct Point2D { pub x: f64, pub y: f64 }

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DrawPolylineInput {
    pub points: Vec<Point2D>,
    pub closed: Option<bool>,
    pub layer: Option<String>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DrawTextInput {
    pub x: f64, pub y: f64, pub z: Option<f64>,
    pub text: String,
    #[schemars(description = "文字高さ（省略時 2.5）")]
    pub height: Option<f64>,
    #[schemars(description = "回転角度（度）")]
    pub rotation: Option<f64>,
    pub layer: Option<String>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DrawRectangleInput {
    pub x: f64, pub y: f64,
    pub width: f64, pub height: f64,
    pub layer: Option<String>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct EntityIdInput {
    pub entity_id: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct MoveEntityInput {
    pub entity_id: String,
    pub dx: f64, pub dy: f64, pub dz: Option<f64>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct CopyEntityInput {
    pub entity_id: String,
    pub dx: Option<f64>, pub dy: Option<f64>, pub dz: Option<f64>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct RotateEntityInput {
    pub entity_id: String,
    pub cx: f64, pub cy: f64,
    #[schemars(description = "回転角度（度）")]
    pub angle: f64,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct ScaleEntityInput {
    pub entity_id: String,
    pub cx: f64, pub cy: f64,
    pub factor: f64,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SetEntityLayerInput {
    pub entity_id: String,
    pub layer: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SetEntityColorInput {
    pub entity_id: String,
    #[schemars(description = "色番号 (1=赤,2=黄,3=緑,4=シアン,5=青,6=マゼンタ,7=白)")]
    pub color: i32,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct CreateLayerInput {
    pub name: String,
    #[schemars(description = "色番号（省略時 7=白）")]
    pub color: Option<i32>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SetLayerPropertyInput {
    pub name: String,
    pub color: Option<i32>,
    pub off: Option<bool>,
    pub locked: Option<bool>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SelectByLayerInput { pub layer: String }

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SelectByTypeInput {
    #[schemars(description = "AcDbLine, AcDbCircle, AcDbArc, AcDbPolyline, AcDbText 等")]
    pub r#type: String,
}

// =====================================================================
// ツール実装
// =====================================================================

fn call(method: &str, params: serde_json::Value) -> String {
    match AcadBridge::send(method, params) {
        Ok(r) if r.success => serde_json::to_string_pretty(&r.data).unwrap_or_else(|_| r.data.to_string()),
        Ok(r) => format!("Error: {}", r.data),
        Err(e) => format!("Connection error: {}", e),
    }
}

fn with_layer(mut p: serde_json::Value, layer: &Option<String>) -> serde_json::Value {
    if let Some(l) = layer { p["layer"] = json!(l); }
    p
}

#[tool_router]
impl AutocadMcpServer {
    // --- LISP ---
    #[tool(name = "eval_lisp", description = "AutoLISP 式を評価（全機能アクセス可能）")]
    async fn eval_lisp(&self, Parameters(i): Parameters<EvalLispInput>) -> String {
        call("eval_lisp", json!({"expression": i.expression}))
    }

    #[tool(name = "acad_command", description = "AutoCAD コマンドを LISP 経由で実行")]
    async fn acad_command(&self, Parameters(i): Parameters<AcadCommandInput>) -> String {
        call("acad_command", json!({"command": i.command}))
    }

    // --- 変数 ---
    #[tool(name = "get_variable", description = "システム変数を取得")]
    async fn get_variable(&self, Parameters(i): Parameters<GetVariableInput>) -> String {
        call("get_variable", json!({"name": i.name}))
    }

    #[tool(name = "set_variable", description = "システム変数を設定")]
    async fn set_variable(&self, Parameters(i): Parameters<SetVariableInput>) -> String {
        call("set_variable", json!({"name": i.name, "value": i.value}))
    }

    // --- 図面情報 ---
    #[tool(name = "get_drawing_info", description = "図面情報を取得")]
    async fn get_drawing_info(&self, Parameters(_): Parameters<EmptyInput>) -> String { call("get_drawing_info", json!({})) }

    #[tool(name = "get_bounds", description = "図面の範囲（EXTMIN/EXTMAX）を取得")]
    async fn get_bounds(&self, Parameters(_): Parameters<EmptyInput>) -> String { call("get_bounds", json!({})) }

    #[tool(name = "count_entities", description = "エンティティ数を取得")]
    async fn count_entities(&self, Parameters(_): Parameters<EmptyInput>) -> String { call("count_entities", json!({})) }

    // --- レイヤー ---
    #[tool(name = "list_layers", description = "レイヤー一覧（名前・色・表示状態）")]
    async fn list_layers(&self, Parameters(_): Parameters<EmptyInput>) -> String { call("list_layers", json!({})) }

    #[tool(name = "create_layer", description = "レイヤーを作成")]
    async fn create_layer(&self, Parameters(i): Parameters<CreateLayerInput>) -> String {
        call("create_layer", json!({"name": i.name, "color": i.color.unwrap_or(7)}))
    }

    #[tool(name = "set_layer_property", description = "レイヤーのプロパティを変更")]
    async fn set_layer_property(&self, Parameters(i): Parameters<SetLayerPropertyInput>) -> String {
        let mut p = json!({"name": i.name});
        if let Some(c) = i.color { p["color"] = json!(c); }
        if let Some(o) = i.off { p["off"] = json!(o.to_string()); }
        if let Some(l) = i.locked { p["locked"] = json!(l.to_string()); }
        call("set_layer_property", p)
    }

    // --- 描画 ---
    #[tool(name = "draw_line", description = "線分を描画")]
    async fn draw_line(&self, Parameters(i): Parameters<DrawLineInput>) -> String {
        call("draw_line", with_layer(json!({"x1":i.x1,"y1":i.y1,"z1":i.z1.unwrap_or(0.0),"x2":i.x2,"y2":i.y2,"z2":i.z2.unwrap_or(0.0)}), &i.layer))
    }

    #[tool(name = "draw_circle", description = "円を描画")]
    async fn draw_circle(&self, Parameters(i): Parameters<DrawCircleInput>) -> String {
        call("draw_circle", with_layer(json!({"cx":i.cx,"cy":i.cy,"cz":i.cz.unwrap_or(0.0),"radius":i.radius}), &i.layer))
    }

    #[tool(name = "draw_arc", description = "円弧を描画（角度は度数）")]
    async fn draw_arc(&self, Parameters(i): Parameters<DrawArcInput>) -> String {
        call("draw_arc", with_layer(json!({"cx":i.cx,"cy":i.cy,"cz":i.cz.unwrap_or(0.0),"radius":i.radius,"start_angle":i.start_angle,"end_angle":i.end_angle}), &i.layer))
    }

    #[tool(name = "draw_polyline", description = "ポリラインを描画")]
    async fn draw_polyline(&self, Parameters(i): Parameters<DrawPolylineInput>) -> String {
        let pts: Vec<Vec<f64>> = i.points.iter().map(|p| vec![p.x, p.y]).collect();
        call("draw_polyline", with_layer(json!({"points": pts, "closed": i.closed.unwrap_or(false)}), &i.layer))
    }

    #[tool(name = "draw_text", description = "テキストを描画")]
    async fn draw_text(&self, Parameters(i): Parameters<DrawTextInput>) -> String {
        call("draw_text", with_layer(json!({"x":i.x,"y":i.y,"z":i.z.unwrap_or(0.0),"text":i.text,"height":i.height.unwrap_or(2.5),"rotation":i.rotation.unwrap_or(0.0)}), &i.layer))
    }

    #[tool(name = "draw_rectangle", description = "四角形を描画（閉じたポリライン）")]
    async fn draw_rectangle(&self, Parameters(i): Parameters<DrawRectangleInput>) -> String {
        let pts = json!([[i.x,i.y],[i.x+i.width,i.y],[i.x+i.width,i.y+i.height],[i.x,i.y+i.height]]);
        call("draw_polyline", with_layer(json!({"points": pts, "closed": true}), &i.layer))
    }

    // --- 編集 ---
    #[tool(name = "delete_entity", description = "エンティティを削除")]
    async fn delete_entity(&self, Parameters(i): Parameters<EntityIdInput>) -> String {
        call("delete_entity", json!({"entity_id": i.entity_id}))
    }

    #[tool(name = "move_entity", description = "エンティティを移動")]
    async fn move_entity(&self, Parameters(i): Parameters<MoveEntityInput>) -> String {
        call("move_entity", json!({"entity_id":i.entity_id,"dx":i.dx,"dy":i.dy,"dz":i.dz.unwrap_or(0.0)}))
    }

    #[tool(name = "copy_entity", description = "エンティティをコピー")]
    async fn copy_entity(&self, Parameters(i): Parameters<CopyEntityInput>) -> String {
        call("copy_entity", json!({"entity_id":i.entity_id,"dx":i.dx.unwrap_or(0.0),"dy":i.dy.unwrap_or(0.0),"dz":i.dz.unwrap_or(0.0)}))
    }

    #[tool(name = "rotate_entity", description = "エンティティを回転（角度は度数）")]
    async fn rotate_entity(&self, Parameters(i): Parameters<RotateEntityInput>) -> String {
        call("rotate_entity", json!({"entity_id":i.entity_id,"cx":i.cx,"cy":i.cy,"angle":i.angle}))
    }

    #[tool(name = "scale_entity", description = "エンティティをスケール")]
    async fn scale_entity(&self, Parameters(i): Parameters<ScaleEntityInput>) -> String {
        call("scale_entity", json!({"entity_id":i.entity_id,"cx":i.cx,"cy":i.cy,"factor":i.factor}))
    }

    #[tool(name = "set_entity_layer", description = "エンティティのレイヤーを変更")]
    async fn set_entity_layer(&self, Parameters(i): Parameters<SetEntityLayerInput>) -> String {
        call("set_entity_layer", json!({"entity_id":i.entity_id,"layer":i.layer}))
    }

    #[tool(name = "set_entity_color", description = "エンティティの色を変更")]
    async fn set_entity_color(&self, Parameters(i): Parameters<SetEntityColorInput>) -> String {
        call("set_entity_color", json!({"entity_id":i.entity_id,"color":i.color}))
    }

    // --- クエリ ---
    #[tool(name = "get_entity", description = "エンティティの詳細情報を取得")]
    async fn get_entity(&self, Parameters(i): Parameters<EntityIdInput>) -> String {
        call("get_entity", json!({"entity_id": i.entity_id}))
    }

    #[tool(name = "select_by_layer", description = "レイヤーで全エンティティIDを取得")]
    async fn select_by_layer(&self, Parameters(i): Parameters<SelectByLayerInput>) -> String {
        call("select_by_layer", json!({"layer": i.layer}))
    }

    #[tool(name = "select_by_type", description = "タイプで全エンティティIDを取得")]
    async fn select_by_type(&self, Parameters(i): Parameters<SelectByTypeInput>) -> String {
        call("select_by_type", json!({"type": i.r#type}))
    }
}

#[tool_handler]
impl ServerHandler for AutocadMcpServer {
    fn get_info(&self) -> ServerInfo {
        let mut info = ServerInfo::new(ServerCapabilities::builder().enable_tools().build());
        info.instructions = Some(
            "AutoCAD MCP Server — 描画(line/circle/arc/polyline/text/rectangle)、\
             編集(move/copy/rotate/scale/delete)、レイヤー管理、クエリ、eval_lisp。".into()
        );
        info
    }
}
