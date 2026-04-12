mod bridge;
mod server;

use anyhow::Result;
use rmcp::ServiceExt;
use tracing_subscriber::EnvFilter;

use crate::server::AutocadMcpServer;

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .with_env_filter(EnvFilter::from_default_env().add_directive("info".parse()?))
        .init();

    tracing::info!("AutoCAD MCP Server starting...");

    if bridge::AcadBridge::is_connected() {
        tracing::info!("AutoCAD bridge: connected");
    } else {
        tracing::warn!("AutoCAD bridge: not connected (plugin may not be loaded)");
    }

    let server = AutocadMcpServer::new();
    let service = server.serve(rmcp::transport::stdio()).await?;

    tracing::info!("AutoCAD MCP Server running on stdio");
    service.waiting().await?;

    Ok(())
}
