use crate::error::Result;
use crate::event::AsrEvent;

/// Trait for streaming ASR providers.
/// Each session creates a new provider instance.
#[allow(async_fn_in_trait)]
pub trait AsrProvider: Send {
    /// Connect to the ASR service and start the session.
    async fn connect(&mut self) -> Result<()>;
    /// Push a chunk of raw audio data (PCM 16-bit, mono, 16kHz).
    async fn send_audio(&mut self, frame: &[u8]) -> Result<()>;
    /// Signal that no more audio will be sent.
    async fn finish_input(&mut self) -> Result<()>;
    /// Wait for the next event from the ASR provider.
    async fn next_event(&mut self) -> Result<AsrEvent>;
    /// Close the connection and release resources.
    async fn close(&mut self) -> Result<()>;
}
