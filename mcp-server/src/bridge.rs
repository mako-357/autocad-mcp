use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::os::unix::net::UnixStream;
use std::io::{Read, Write};
use std::time::Duration;

const SOCKET_PATH: &str = "/tmp/gfp-arx-bridge.sock";

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
            id: uuid_short(),
            method: method.to_string(),
            params,
        };

        let mut msg = serde_json::to_string(&req)?;
        msg.push('\n');

        let mut stream_w = stream.try_clone()?;
        stream_w.write_all(msg.as_bytes())?;
        stream_w.flush()?;

        // バイト列で読んで lossy UTF-8 変換（日本語文字化け対策）
        let mut buf = vec![0u8; 65536];
        let n = stream.read(&mut buf)?;
        let line = String::from_utf8_lossy(&buf[..n]);

        let resp: BridgeResponse = serde_json::from_str(line.trim())
            .context("レスポンスのパースに失敗")?;

        Ok(resp)
    }

    /// 接続チェック
    pub fn is_connected() -> bool {
        Self::send("ping", serde_json::Value::Null).is_ok()
    }
}

fn uuid_short() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let t = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
    format!("{:x}", t.as_nanos() % 0xFFFFFFFF)
}
