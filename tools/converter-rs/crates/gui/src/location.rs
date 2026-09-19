// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use tab5conv_ffmpeg::Tools;
use tauri::{AppHandle, Manager, Runtime};

const CONFIG_FILE: &str = "settings.json";
const SEARCH_DIRS: [&str; 7] = [
    "/opt/homebrew/bin",
    "/usr/local/bin",
    "/opt/local/bin",
    "/run/current-system/sw/bin",
    "/nix/var/nix/profiles/default/bin",
    "/usr/bin",
    "/snap/bin",
];

#[derive(Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Config {
    ffmpeg_dir: Option<PathBuf>,
}

fn search_dirs() -> Vec<PathBuf> {
    let mut dirs: Vec<PathBuf> = SEARCH_DIRS.iter().map(PathBuf::from).collect();
    if let Some(home) = env::var_os("HOME").map(PathBuf::from) {
        dirs.push(home.join(".nix-profile/bin"));
        dirs.push(home.join(".local/bin"));
    }
    if let Some(user) = env::var_os("USER") {
        dirs.push(Path::new("/etc/profiles/per-user").join(user).join("bin"));
    }
    dirs
}

pub fn find(configured: Option<&Path>) -> Option<Tools> {
    match configured {
        Some(dir) => Some(Tools::in_dir(dir)),
        None => Tools::locate(&search_dirs()),
    }
}

fn config_path<R: Runtime>(app: &AppHandle<R>) -> Option<PathBuf> {
    app.path()
        .app_config_dir()
        .ok()
        .map(|dir| dir.join(CONFIG_FILE))
}

pub fn load<R: Runtime>(app: &AppHandle<R>) -> Option<PathBuf> {
    let text = fs::read_to_string(config_path(app)?).ok()?;
    serde_json::from_str::<Config>(&text).ok()?.ffmpeg_dir
}

pub fn save<R: Runtime>(app: &AppHandle<R>, ffmpeg_dir: Option<&Path>) -> anyhow::Result<()> {
    let path = config_path(app).ok_or_else(|| anyhow::anyhow!("no config directory"))?;
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let config = Config {
        ffmpeg_dir: ffmpeg_dir.map(Path::to_path_buf),
    };
    fs::write(path, serde_json::to_string_pretty(&config)?)?;
    Ok(())
}
