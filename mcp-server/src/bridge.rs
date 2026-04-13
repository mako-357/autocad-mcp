use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::os::unix::net::UnixStream;
use std::io::{BufRead, BufReader, Write};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

const SOCKET_PATH: &str = "/tmp/gfp-arx-bridge.sock";

static REQUEST_COUNTER: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Serialize)]
struct BridgeRequest {
    id: String,
    method: String,
    params: serde_json::Value,
}

#[derive(Debug, Deserialize)]
pub struct BridgeResponse {
    pub id: String,
    pub success: bool,
    pub data: serde_json::Value,
}

/// ObjectARX プラグインと Unix Socket で通信するクライアント
pub struct AcadBridge;

impl AcadBridge {
    /// コマンドを送信して結果を受信
    pub fn send(method: &str, params: serde_json::Value) -> Result<BridgeResponse> {
        let mut stream = UnixStream::connect(SOCKET_PATH)
            .context("AutoCAD に接続できません。プラグインがロードされているか確認してください")?;
        stream.set_read_timeout(Some(Duration::from_secs(15)))?;
        stream.set_write_timeout(Some(Duration::from_secs(5)))?;

        let req = BridgeRequest {
            id: next_id(),
            method: method.to_string(),
            params,
        };

        let mut msg = serde_json::to_string(&req)?;
        msg.push('\n');

        stream.write_all(msg.as_bytes())?;
        stream.flush()?;

        // C++ 側の auto_to_utf8() で Shift-JIS→UTF-8 変換済みなので BufReader で安全に読める
        let mut reader = BufReader::new(&stream);
        let mut line = String::new();
        reader.read_line(&mut line)?;

        let resp: BridgeResponse = serde_json::from_str(line.trim())
            .context("レスポンスのパースに失敗")?;

        Ok(resp)
    }

    /// 接続チェック
    pub fn is_connected() -> bool {
        Self::send("ping", serde_json::Value::Null).is_ok()
    }
}

fn next_id() -> String {
    format!("req_{}", REQUEST_COUNTER.fetch_add(1, Ordering::Relaxed))
}
