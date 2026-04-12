# AutoCAD MCP

Claude (or any LLM) から AutoCAD を操作するための [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) サーバー。

ObjectARX C++ プラグインを AutoCAD 内にロードし、Unix Domain Socket 経由で MCP サーバーと通信することで、LLM が AutoCAD の全機能にアクセスできます。

## Architecture

```
Claude ──stdio──> MCP Server (Rust)
                       │
                  Unix Socket (/tmp/gfp-arx-bridge.sock)
                       │
                  ObjectARX Plugin (.bundle)
                       │
                  AutoCAD
```

- **読み取り操作**: ソケットスレッドで直接実行（高速）
- **書き込み操作**: メインスレッドキュー経由（AutoCAD API の要件）
- **LISP 実行**: `sendStringToExecute` + 一時ファイルで結果取得

## MCP Tools

| Tool | Description |
|------|-------------|
| `eval_lisp` | 任意の AutoLISP 式を評価。AutoCAD の全機能にアクセス可能 |
| `acad_command` | AutoCAD コマンドを LISP 経由で実行 |
| `get_variable` | AutoCAD システム変数を取得 |
| `draw_line` | 線分を描画 |
| `draw_rectangle` | 四角形を描画 |
| `get_drawing_info` | 図面情報を取得 |
| `list_layers` | レイヤー一覧を取得 |

`eval_lisp` を使えば、上記以外の任意の AutoCAD 操作が可能です:

```lisp
;; 円を描く
(command "CIRCLE" "0,0" 1000)

;; レイヤーを作成
(command "LAYER" "M" "MyLayer" "C" "1" "MyLayer" "")

;; ポリラインを描く
(command "PLINE" "0,0" "100,0" "100,50" "0,50" "C")

;; 全エンティティ数を取得
(sslength (ssget "X"))
```

## Prerequisites

- **AutoCAD 2025+** (Mac or Windows)
- **ObjectARX SDK** (free, Autodesk account required)
  - Download from [Autodesk Platform Services](https://aps.autodesk.com/developer/overview/objectarx-autocad-sdk)
  - Install to `/Library/Developer/Autodesk/ObjectARX 2027/` (Mac)
- **Xcode** (Mac) or **Visual Studio** (Windows)
- **xcodegen** (Mac): `brew install xcodegen`
- **Rust** (for MCP server): [rustup.rs](https://rustup.rs/)

## Build

### ObjectARX Plugin (Mac)

```bash
cd arx-plugin
xcodegen generate
xcodebuild -project gfp-arx-bridge.xcodeproj \
  -target gfp-arx-bridge \
  -configuration Debug build
```

Output: `build/Debug/gfp-arx-bridge.bundle`

### MCP Server

```bash
cd mcp-server
cargo build
```

## Setup

### 1. Load the plugin in AutoCAD

1. Open AutoCAD
2. Type `APPLOAD`
3. Navigate to `arx-plugin/build/Debug/gfp-arx-bridge.bundle`
4. Load it

You should see: `[GFP] Socket bridge started`

### 2. Configure Claude Code

Add to your project's `.mcp.json`:

```json
{
  "mcpServers": {
    "autocad": {
      "command": "/path/to/autocad-mcp-server",
      "args": []
    }
  }
}
```

### 3. Use it

Ask Claude to draw, query, or modify your AutoCAD drawings!

## Platform Support

| Platform | Status | Communication |
|----------|--------|---------------|
| macOS (Apple Silicon) | Supported | Unix Domain Socket |
| macOS (Intel) | Supported | Unix Domain Socket |
| Windows | Planned | Named Pipe (coming soon) |

## How it works

### ObjectARX Plugin (`arx-plugin/`)

A native AutoCAD plugin built with the ObjectARX SDK. It:

1. Starts a Unix Domain Socket server on a background thread
2. Receives JSON commands from the MCP server
3. Executes AutoCAD API calls on the main thread (for write operations) or directly (for read operations)
4. Returns results as JSON

Key implementation details:
- Write operations use `sendStringToExecute` to dispatch to AutoCAD's main thread, with condition variables for synchronization
- `eval_lisp` sends LISP expressions via `sendStringToExecute` and reads results from a temp file
- `windef.h` must be included before `aced.h` on Mac (provides Windows type compatibility)

### MCP Server (`mcp-server/`)

A Rust MCP server using [rmcp](https://crates.io/crates/rmcp). It:

1. Runs as a stdio MCP server
2. Connects to the ObjectARX plugin via Unix Domain Socket
3. Translates MCP tool calls to bridge commands
4. Returns results to the LLM

## License

MIT
