use crate::doubao::DoubaoWsProvider;
use crate::error::Result;
use crate::event::AsrEvent;
#[cfg(feature = "mlx")]
use crate::mlx::MlxProvider;

/// Unified ASR provider enum.
/// Each session creates a new provider instance with its own configuration.
pub enum AsrProvider {
    Doubao(DoubaoWsProvider),
    #[cfg(feature = "mlx")]
    Mlx(MlxProvider),
}

macro_rules! dispatch {
    ($self:expr, $method:ident $(, $arg:expr)*) => {
        match $self {
            AsrProvider::Doubao(p) => p.$method($($arg),*).await,
            #[cfg(feature = "mlx")]
            AsrProvider::Mlx(p) => p.$method($($arg),*).await,
        }
    };
}

impl AsrProvider {
    /// Connect to the ASR service and start the session.
    pub async fn connect(&mut self) -> Result<()> {
        dispatch!(self, connect)
    }

    /// Push a chunk of raw audio data (PCM 16-bit, mono, 16kHz).
    pub async fn send_audio(&mut self, frame: &[u8]) -> Result<()> {
        dispatch!(self, send_audio, frame)
    }

    /// Signal that no more audio will be sent.
    pub async fn finish_input(&mut self) -> Result<()> {
        dispatch!(self, finish_input)
    }

    /// Wait for the next event from the ASR provider.
    pub async fn next_event(&mut self) -> Result<AsrEvent> {
        dispatch!(self, next_event)
    }

    /// Close the connection and release resources.
    pub async fn close(&mut self) -> Result<()> {
        dispatch!(self, close)
    }
}
