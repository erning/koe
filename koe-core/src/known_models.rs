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

/// Parse known-models.yaml and return JSON string for Obj-C consumption.
pub fn load_known_models_json(yaml_path: &str) -> Option<String> {
    let content = std::fs::read_to_string(yaml_path).ok()?;
    let data: BTreeMap<String, ProviderModels> = serde_yaml::from_str(&content).ok()?;
    serde_json::to_string(&data).ok()
}
