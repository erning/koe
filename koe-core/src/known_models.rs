use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Debug, Deserialize, Serialize)]
pub struct ProviderModels {
    pub display_name: String,
    #[serde(default)]
    pub required_files: Vec<String>,
    #[serde(default)]
    pub models: Vec<Model>,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct Model {
    pub id: String,
    pub short_id: String,
    pub repo: String,
    pub size: String,
    pub description: String,
}

/// Load known-models.yaml from ~/.koe/ and return as JSON string.
pub fn load_known_models_json() -> Option<String> {
    let path = crate::config::known_models_path();
    let content = std::fs::read_to_string(path).ok()?;
    let data: BTreeMap<String, ProviderModels> = serde_yaml::from_str(&content).ok()?;
    serde_json::to_string(&data).ok()
}
