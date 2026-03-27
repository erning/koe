//! # koe-asr
//!
//! Streaming ASR (Automatic Speech Recognition) providers.

pub mod config;
pub mod doubao;
pub mod error;
pub mod event;
#[cfg(feature = "mlx")]
pub mod mlx;
pub mod provider;
pub mod transcript;

pub use config::DoubaoWsConfig;
pub use doubao::DoubaoWsProvider;
pub use error::AsrError;
pub use event::AsrEvent;
#[cfg(feature = "mlx")]
pub use mlx::{MlxConfig, MlxProvider};
pub use provider::AsrProvider;
pub use transcript::TranscriptAggregator;
