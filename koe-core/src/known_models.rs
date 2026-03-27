use futures_util::StreamExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashMap};
use std::io::Write;
use std::path::PathBuf;
use std::sync::Mutex;
use tokio_util::sync::CancellationToken;

// ─── Data Structures ────────────────────────────────────────────────

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct ProviderModels {
    pub display_name: String,
    #[serde(default)]
    pub required_files: Vec<String>,
    #[serde(default)]
    pub models: Vec<Model>,
}

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct Model {
    pub id: String,
    pub short_id: String,
    pub repo: String,
    pub size: String,
    pub description: String,
}

#[derive(Debug, Deserialize, Serialize)]
struct HfTreeEntry {
    #[serde(rename = "type")]
    entry_type: String,
    path: String,
    size: Option<u64>,
    lfs: Option<HfLfsInfo>,
}

#[derive(Debug, Deserialize, Serialize)]
struct HfLfsInfo {
    oid: String,
    size: u64,
}

impl HfTreeEntry {
    fn file_size(&self) -> u64 {
        self.lfs.as_ref().map(|l| l.size).or(self.size).unwrap_or(0)
    }
    fn sha256(&self) -> Option<&str> {
        self.lfs.as_ref().map(|l| l.oid.as_str())
    }
}

#[derive(Debug, Serialize, Deserialize)]
struct ManifestEntry {
    size: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    sha256: Option<String>,
}

const MANIFEST_FILE: &str = ".koe-manifest.json";

// ─── Model Status ───────────────────────────────────────────────────

/// 0 = not installed, 1 = incomplete, 2 = installed
pub fn check_model_status(provider: &str, model_id: &str) -> i32 {
    let model_dir = model_dir(provider, model_id);
    if !model_dir.exists() {
        return 0;
    }

    let manifest_path = model_dir.join(MANIFEST_FILE);
    if !manifest_path.exists() {
        // Directory exists but no manifest — incomplete
        return 1;
    }

    let manifest: HashMap<String, ManifestEntry> = match std::fs::read_to_string(&manifest_path)
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
    {
        Some(m) => m,
        None => return 1,
    };

    // Check all manifest entries exist with correct size
    for (filename, entry) in &manifest {
        let file_path = model_dir.join(filename);
        if !file_path.exists() {
            return 1;
        }
        if let Ok(meta) = std::fs::metadata(&file_path) {
            if meta.len() != entry.size {
                return 1;
            }
        }
    }

    2 // installed
}

// ─── Download State Management ──────────────────────────────────────

struct DownloadState {
    cancel_token: CancellationToken,
}

static DOWNLOADS: Mutex<Option<HashMap<String, DownloadState>>> = Mutex::new(None);

fn downloads_map() -> &'static Mutex<Option<HashMap<String, DownloadState>>> {
    &DOWNLOADS
}

fn download_key(provider: &str, model_id: &str) -> String {
    format!("{}/{}", provider, model_id)
}

fn is_downloading(key: &str) -> bool {
    let guard = downloads_map().lock().unwrap();
    guard.as_ref().map(|m| m.contains_key(key)).unwrap_or(false)
}

fn register_download(key: String) -> Option<CancellationToken> {
    let mut guard = downloads_map().lock().unwrap();
    let map = guard.get_or_insert_with(HashMap::new);
    if map.contains_key(&key) {
        return None; // already downloading
    }
    let token = CancellationToken::new();
    let clone = token.clone();
    map.insert(key, DownloadState { cancel_token: token });
    Some(clone)
}

fn unregister_download(key: &str) {
    let mut guard = downloads_map().lock().unwrap();
    if let Some(map) = guard.as_mut() {
        map.remove(key);
    }
}

fn cancel_download_by_key(key: &str) -> bool {
    let guard = downloads_map().lock().unwrap();
    if let Some(map) = guard.as_ref() {
        if let Some(state) = map.get(key) {
            state.cancel_token.cancel();
            return true;
        }
    }
    false
}

// ─── Download Logic ─────────────────────────────────────────────────

/// C callback types for progress and status reporting.
pub type ProgressCallback =
    extern "C" fn(ctx: *mut std::ffi::c_void, downloaded: u64, total: u64, file: *const std::ffi::c_char);
pub type StatusCallback =
    extern "C" fn(ctx: *mut std::ffi::c_void, status: i32, message: *const std::ffi::c_char);

/// Start downloading a model. Returns 0 on success (started), -1 if already downloading, -2 on error.
pub fn start_download(
    provider: &str,
    model_id: &str,
    progress_cb: ProgressCallback,
    status_cb: StatusCallback,
    ctx: *mut std::ffi::c_void,
    runtime: &tokio::runtime::Runtime,
) -> i32 {
    let key = download_key(provider, model_id);

    let cancel_token = match register_download(key.clone()) {
        Some(t) => t,
        None => return -1, // already downloading
    };

    // Find model info from known-models.yaml
    let known = match load_known_models() {
        Some(k) => k,
        None => {
            unregister_download(&key);
            return -2;
        }
    };

    let provider_info = match known.get(provider) {
        Some(p) => p.clone(),
        None => {
            unregister_download(&key);
            return -2;
        }
    };

    let model = match provider_info.models.iter().find(|m| m.id == model_id) {
        Some(m) => m.clone(),
        None => {
            unregister_download(&key);
            return -2;
        }
    };

    let required_patterns = provider_info.required_files.clone();
    let dest_dir = model_dir(provider, model_id);
    let key_clone = key.clone();
    let provider_str = provider.to_string();

    let cb = CallbackCtx { ctx, progress_cb, status_cb };

    runtime.spawn(async move {
        invoke_status(cb.status_cb, cb.ctx, 0, "started");

        let result = download_model_async(
            &model.repo,
            &required_patterns,
            &dest_dir,
            &cancel_token,
            &cb,
        )
        .await;

        unregister_download(&key_clone);

        match result {
            Ok(()) => invoke_status(cb.status_cb, cb.ctx, 1, "completed"),
            Err(e) if e.contains("cancelled") => invoke_status(cb.status_cb, cb.ctx, 3, "cancelled"),
            Err(e) => invoke_status(cb.status_cb, cb.ctx, 2, &e),
        }

        // Force a status check log
        let status = check_model_status(&provider_str, &model.id);
        log::info!(
            "download finished: {}/{} status={}",
            provider_str,
            model.id,
            status
        );
    });

    0
}

/// Cancel an active download.
pub fn cancel_download(provider: &str, model_id: &str) -> bool {
    let key = download_key(provider, model_id);
    cancel_download_by_key(&key)
}

/// Delete a downloaded model.
pub fn delete_model(provider: &str, model_id: &str) -> bool {
    let dir = model_dir(provider, model_id);
    if dir.exists() {
        std::fs::remove_dir_all(&dir).is_ok()
    } else {
        true
    }
}

async fn download_model_async(
    repo: &str,
    required_patterns: &[String],
    dest_dir: &PathBuf,
    cancel_token: &CancellationToken,
    cb: &CallbackCtx,
) -> Result<(), String> {
    let client = reqwest::Client::builder()
        .user_agent("koe/1.0")
        .build()
        .map_err(|e| format!("http client: {e}"))?;

    // Fetch file tree from HuggingFace
    let tree_url = format!("https://huggingface.co/api/models/{}/tree/main", repo);
    let entries: Vec<HfTreeEntry> = client
        .get(&tree_url)
        .send()
        .await
        .map_err(|e| format!("fetch file tree: {e}"))?
        .json()
        .await
        .map_err(|e| format!("parse file tree: {e}"))?;

    // Filter files matching required patterns
    let files: Vec<&HfTreeEntry> = entries
        .iter()
        .filter(|e| {
            e.entry_type == "file"
                && required_patterns
                    .iter()
                    .any(|pat| glob_match::glob_match(pat, &e.path))
        })
        .collect();

    if files.is_empty() {
        return Err("no matching files in repository".into());
    }

    std::fs::create_dir_all(dest_dir).map_err(|e| format!("create dir: {e}"))?;

    let mut manifest: HashMap<String, ManifestEntry> = HashMap::new();

    for f in &files {
        if cancel_token.is_cancelled() {
            return Err("cancelled".into());
        }

        let url = format!(
            "https://huggingface.co/{}/resolve/main/{}",
            repo, f.path
        );
        let dest = dest_dir.join(&f.path);

        download_file(
            &client,
            &url,
            &dest,
            f.file_size(),
            f.sha256(),
            cancel_token,
            cb,
        )
        .await?;

        manifest.insert(
            f.path.clone(),
            ManifestEntry {
                size: f.file_size(),
                sha256: f.sha256().map(|s| s.to_string()),
            },
        );
    }

    // Write manifest
    let manifest_json =
        serde_json::to_string_pretty(&manifest).map_err(|e| format!("serialize manifest: {e}"))?;
    std::fs::write(dest_dir.join(MANIFEST_FILE), manifest_json)
        .map_err(|e| format!("write manifest: {e}"))?;

    Ok(())
}

async fn download_file(
    client: &reqwest::Client,
    url: &str,
    dest: &PathBuf,
    total_size: u64,
    expected_sha256: Option<&str>,
    cancel_token: &CancellationToken,
    cb: &CallbackCtx,
) -> Result<(), String> {
    let file_name = dest
        .file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .to_string();

    // Already complete?
    if dest.exists() {
        if let Ok(meta) = std::fs::metadata(dest) {
            if meta.len() == total_size || total_size == 0 {
                if let Some(expected) = expected_sha256 {
                    let actual = sha256_file(dest)?;
                    if actual == expected {
                        log::info!("{} already exists and verified", file_name);
                        invoke_progress(cb, total_size, total_size, &file_name);
                        return Ok(());
                    }
                    log::info!("{} SHA256 mismatch, re-downloading", file_name);
                } else {
                    log::info!("{} already exists, skipping", file_name);
                    invoke_progress(cb, total_size, total_size, &file_name);
                    return Ok(());
                }
            }
        }
    }

    // Resume from .part file
    let part_path = dest.with_extension(format!(
        "{}.part",
        dest.extension().unwrap_or_default().to_string_lossy()
    ));

    let existing_size = if part_path.exists() {
        std::fs::metadata(&part_path).map(|m| m.len()).unwrap_or(0)
    } else {
        0
    };

    let mut request = client.get(url);
    if existing_size > 0 {
        request = request.header("Range", format!("bytes={}-", existing_size));
    }

    let response = request
        .send()
        .await
        .map_err(|e| format!("download {}: {e}", file_name))?
        .error_for_status()
        .map_err(|e| format!("download {}: {e}", file_name))?;

    let resume = response.status() == reqwest::StatusCode::PARTIAL_CONTENT;

    let mut file = if resume && existing_size > 0 {
        invoke_progress(cb, existing_size, total_size, &file_name);
        std::fs::OpenOptions::new()
            .append(true)
            .open(&part_path)
            .map_err(|e| format!("open part file: {e}"))?
    } else {
        if let Some(parent) = part_path.parent() {
            std::fs::create_dir_all(parent).map_err(|e| format!("create dir: {e}"))?;
        }
        std::fs::File::create(&part_path).map_err(|e| format!("create part file: {e}"))?
    };

    let mut downloaded = if resume { existing_size } else { 0 };
    let mut stream = response.bytes_stream();

    loop {
        tokio::select! {
            _ = cancel_token.cancelled() => {
                return Err("cancelled".into());
            }
            chunk = stream.next() => {
                match chunk {
                    Some(Ok(chunk)) => {
                        let bytes: &[u8] = &chunk;
                        file.write_all(bytes).map_err(|e| format!("write: {e}"))?;
                        downloaded += bytes.len() as u64;
                        invoke_progress(cb, downloaded, total_size, &file_name);
                    }
                    Some(Err(e)) => return Err(format!("download {}: {e}", file_name)),
                    None => break,
                }
            }
        }
    }

    // Rename .part → final
    std::fs::rename(&part_path, dest).map_err(|e| format!("rename: {e}"))?;

    // Verify SHA256
    if let Some(expected) = expected_sha256 {
        let actual = sha256_file(dest)?;
        if actual != expected {
            std::fs::remove_file(dest).ok();
            return Err(format!(
                "SHA256 mismatch for {}: expected {}, got {}",
                file_name, expected, actual
            ));
        }
        log::info!("{} SHA256 verified", file_name);
    }

    Ok(())
}

// ─── Helpers ────────────────────────────────────────────────────────

fn sha256_file(path: &PathBuf) -> Result<String, String> {
    use std::io::{BufReader, Read};
    let file = std::fs::File::open(path).map_err(|e| format!("open for sha256: {e}"))?;
    let mut reader = BufReader::with_capacity(1024 * 1024, file);
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 1024 * 1024];
    loop {
        let n = reader.read(&mut buf).map_err(|e| format!("read for sha256: {e}"))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

fn model_dir(provider: &str, model_id: &str) -> PathBuf {
    crate::config::config_dir()
        .join("models")
        .join(provider)
        .join(model_id)
}

fn invoke_progress(cb: &CallbackCtx, downloaded: u64, total: u64, file_name: &str) {
    if let Ok(cstr) = std::ffi::CString::new(file_name) {
        (cb.progress_cb)(cb.ctx, downloaded, total, cstr.as_ptr());
    }
}

fn invoke_status(cb_status: StatusCallback, ctx: *mut std::ffi::c_void, status: i32, message: &str) {
    if let Ok(cstr) = std::ffi::CString::new(message) {
        cb_status(ctx, status, cstr.as_ptr());
    }
}

/// Send-safe wrapper for callback context passed across thread boundary.
/// Safety: The Obj-C side ensures the ctx pointer remains valid for the
/// lifetime of the download (the SetupWizard window controller is retained).
struct CallbackCtx {
    ctx: *mut std::ffi::c_void,
    progress_cb: ProgressCallback,
    status_cb: StatusCallback,
}
unsafe impl Send for CallbackCtx {}
unsafe impl Sync for CallbackCtx {}

// ─── YAML Loading ───────────────────────────────────────────────────

pub fn load_known_models() -> Option<BTreeMap<String, ProviderModels>> {
    let path = crate::config::known_models_path();
    let content = std::fs::read_to_string(path).ok()?;
    serde_yaml::from_str(&content).ok()
}

/// Load known-models.yaml from ~/.koe/ and return as JSON string.
pub fn load_known_models_json() -> Option<String> {
    let data = load_known_models()?;
    serde_json::to_string(&data).ok()
}
