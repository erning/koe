/// Configuration for the Doubao (豆包) WebSocket ASR provider.
#[derive(Debug, Clone)]
pub struct DoubaoWsConfig {
    /// WebSocket endpoint URL
    pub url: String,
    /// X-Api-App-Key (App ID from Volcengine console)
    pub app_key: String,
    /// X-Api-Access-Key (Access Token from Volcengine console)
    pub access_key: String,
    /// X-Api-Resource-Id (e.g. "volc.bigasr.sauc.duration")
    pub resource_id: String,
    /// Audio sample rate in Hz (default: 16000)
    pub sample_rate_hz: u32,
    /// Connection timeout in milliseconds (default: 3000)
    pub connect_timeout_ms: u64,
    /// Timeout waiting for final ASR result after finish signal (default: 5000)
    pub final_wait_timeout_ms: u64,
    /// Enable DDC (disfluency removal / smoothing)
    pub enable_ddc: bool,
    /// Enable ITN (inverse text normalization)
    pub enable_itn: bool,
    /// Enable automatic punctuation
    pub enable_punc: bool,
    /// Enable two-pass recognition (streaming + non-streaming re-recognition)
    pub enable_nonstream: bool,
    /// Hotwords for improved recognition accuracy
    pub hotwords: Vec<String>,
}

impl Default for DoubaoWsConfig {
    fn default() -> Self {
        Self {
            url: "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async".into(),
            app_key: String::new(),
            access_key: String::new(),
            resource_id: "volc.seedasr.sauc.duration".into(),
            sample_rate_hz: 16000,
            connect_timeout_ms: 3000,
            final_wait_timeout_ms: 5000,
            enable_ddc: true,
            enable_itn: true,
            enable_punc: true,
            enable_nonstream: true,
            hotwords: Vec::new(),
        }
    }
}
