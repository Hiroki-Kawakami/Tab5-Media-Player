// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod location;

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use tab5conv_core::container::Container;
use tab5conv_core::video::VideoCodec;
use tab5conv_core::{Specs, audio, preset, video};
use tab5conv_ffmpeg::{Cancelled, Conversion, Monitor, Tools, batch};
use tauri::{AppHandle, Emitter, Manager, Runtime, State};

const PROGRESS_INTERVAL: Duration = Duration::from_millis(100);

struct App {
    configured: Mutex<Option<PathBuf>>,
    tools: Mutex<Option<Tools>>,
    running: Mutex<HashMap<String, Arc<AtomicBool>>>,
}

impl App {
    fn new(configured: Option<PathBuf>, tools: Option<Tools>) -> Self {
        Self {
            configured: Mutex::new(configured),
            tools: Mutex::new(tools),
            running: Mutex::new(HashMap::new()),
        }
    }

    fn tools(&self) -> Result<Tools, String> {
        self.tools
            .lock()
            .expect("tools lock")
            .clone()
            .ok_or_else(|| "ffmpeg and ffprobe were not found".to_string())
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Settings {
    preset: String,
    video: String,
    audio: String,
    outdir: Option<String>,
}

fn optional(text: &str) -> Option<&str> {
    let text = text.trim();
    (!text.is_empty()).then_some(text)
}

impl Settings {
    fn specs(&self) -> anyhow::Result<Specs> {
        Specs::resolve(&self.preset, optional(&self.video), optional(&self.audio))
    }
}

#[derive(Serialize)]
struct PresetInfo {
    name: &'static str,
    video: &'static str,
    audio: &'static str,
}

#[tauri::command]
fn presets() -> Vec<PresetInfo> {
    preset::all()
        .iter()
        .map(|p| PresetInfo {
            name: p.name,
            video: p.video,
            audio: p.audio,
        })
        .collect()
}

#[derive(Serialize)]
struct Help {
    preset: String,
    video: &'static str,
    audio: &'static str,
}

#[tauri::command]
fn help() -> Help {
    Help {
        preset: preset::help(),
        video: video::HELP,
        audio: audio::HELP,
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ToolsStatus {
    configured: Option<String>,
    path: Option<String>,
    version: Option<String>,
    error: Option<String>,
}

fn status(state: &App) -> ToolsStatus {
    let configured = state
        .configured
        .lock()
        .expect("configured lock")
        .as_ref()
        .map(|p| p.display().to_string());
    let tools = state.tools.lock().expect("tools lock").clone();
    let Some(tools) = tools else {
        return ToolsStatus {
            configured,
            path: None,
            version: None,
            error: Some("ffmpeg and ffprobe were not found".into()),
        };
    };
    let path = Some(tools.ffmpeg().display().to_string());
    match tools.version() {
        Ok(version) => ToolsStatus {
            configured,
            path,
            version: Some(version),
            error: None,
        },
        Err(err) => ToolsStatus {
            configured,
            path,
            version: None,
            error: Some(format!("{err:#}")),
        },
    }
}

#[tauri::command]
async fn tools_status(state: State<'_, App>) -> Result<ToolsStatus, String> {
    Ok(status(&state))
}

#[tauri::command]
async fn set_ffmpeg_dir<R: Runtime>(
    app: AppHandle<R>,
    state: State<'_, App>,
    dir: Option<String>,
) -> Result<ToolsStatus, String> {
    let dir = dir.map(PathBuf::from);
    location::save(&app, dir.as_deref()).map_err(|e| format!("{e:#}"))?;
    *state.tools.lock().expect("tools lock") = location::find(dir.as_deref());
    *state.configured.lock().expect("configured lock") = dir;
    Ok(status(&state))
}

#[derive(Serialize, Default)]
#[serde(rename_all = "camelCase")]
struct PlanItem {
    input: String,
    output: Option<String>,
    skip: Option<String>,
    video: Option<String>,
    audio: Option<String>,
    error: Option<String>,
    exists: bool,
    duration: Option<f64>,
}

#[derive(Serialize)]
struct Applied {
    preset: &'static str,
    video: String,
    audio: String,
}

#[derive(Serialize)]
struct Plan {
    applied: Applied,
    items: Vec<PlanItem>,
}

fn text(path: &Path) -> String {
    path.display().to_string()
}

fn plan_blocking(tools: &Tools, inputs: &[PathBuf], settings: &Settings) -> anyhow::Result<Plan> {
    let specs = settings.specs()?;
    let outdir = settings.outdir.as_deref().and_then(optional).map(Path::new);
    let targets = batch::plan(inputs, None, outdir)?;
    let items = targets
        .iter()
        .map(|target| match target {
            batch::Target::Skip { input, reason } => PlanItem {
                input: text(input),
                skip: Some(reason.to_string()),
                ..Default::default()
            },
            batch::Target::Convert {
                input,
                output,
                container,
            } => {
                let item = PlanItem {
                    input: text(input),
                    output: Some(text(output)),
                    exists: output.exists(),
                    ..Default::default()
                };
                match Conversion::prepare(tools, input, output, *container, &specs) {
                    Ok(conversion) => {
                        let source = &conversion.info.video;
                        PlanItem {
                            video: Some(format!(
                                "{}x{} -> {}",
                                source.display_width.round(),
                                source.display_height.round(),
                                conversion.video.label
                            )),
                            audio: Some(conversion.audio.label.clone()),
                            duration: conversion.info.duration,
                            ..item
                        }
                    }
                    Err(err) => PlanItem {
                        error: Some(format!("{err:#}")),
                        ..item
                    },
                }
            }
        })
        .collect();
    Ok(Plan {
        applied: Applied {
            preset: specs.preset,
            video: specs.video_text,
            audio: specs.audio_text,
        },
        items,
    })
}

#[tauri::command]
async fn plan(
    state: State<'_, App>,
    inputs: Vec<String>,
    settings: Settings,
) -> Result<Plan, String> {
    let tools = state.tools()?;
    let inputs: Vec<PathBuf> = inputs.into_iter().map(PathBuf::from).collect();
    tauri::async_runtime::spawn_blocking(move || plan_blocking(&tools, &inputs, &settings))
        .await
        .map_err(|e| e.to_string())?
        .map_err(|e| format!("{e:#}"))
}

#[derive(Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
enum Outcome {
    Done { report: Vec<String> },
    Cancelled,
    Failed { message: String },
}

#[derive(Clone, Serialize)]
struct Progress<'a> {
    key: &'a str,
    seconds: f64,
}

fn convert_blocking<R: Runtime>(
    app: &AppHandle<R>,
    tools: &Tools,
    key: &str,
    input: &Path,
    output: &Path,
    settings: &Settings,
    cancel: &AtomicBool,
) -> anyhow::Result<Vec<String>> {
    let specs = settings.specs()?;
    let container = Container::from_path(output)?;
    if let Some(dir) = output.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let conversion = Conversion::prepare(tools, input, output, container, &specs)?;
    let last = Mutex::new(None::<Instant>);
    let progress = |seconds: f64| {
        let mut last = last.lock().expect("progress lock");
        if last.is_none_or(|t| t.elapsed() >= PROGRESS_INTERVAL) {
            *last = Some(Instant::now());
            let _ = app.emit("progress", Progress { key, seconds });
        }
    };
    let monitor = Monitor {
        progress: &progress,
        cancel,
    };
    let stats = conversion.run(Some(&monitor))?;
    Ok(match (stats, &conversion.video.codec) {
        (Some(stats), VideoCodec::Mjpeg(settings)) => {
            stats.report(settings, conversion.video.picture.rate.as_f64())
        }
        _ => Vec::new(),
    })
}

#[tauri::command]
async fn convert<R: Runtime>(
    app: AppHandle<R>,
    state: State<'_, App>,
    key: String,
    input: String,
    output: String,
    settings: Settings,
) -> Result<Outcome, String> {
    let tools = state.tools()?;
    let cancel = Arc::new(AtomicBool::new(false));
    state
        .running
        .lock()
        .expect("running lock")
        .insert(key.clone(), Arc::clone(&cancel));
    let task_key = key.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        convert_blocking(
            &app,
            &tools,
            &task_key,
            Path::new(&input),
            Path::new(&output),
            &settings,
            &cancel,
        )
    })
    .await;
    state.running.lock().expect("running lock").remove(&key);
    Ok(match result.map_err(|e| e.to_string())? {
        Ok(report) => Outcome::Done { report },
        Err(err) if err.is::<Cancelled>() => Outcome::Cancelled,
        Err(err) => Outcome::Failed {
            message: format!("{err:#}"),
        },
    })
}

#[tauri::command]
fn cancel(state: State<'_, App>, key: String) {
    if let Some(flag) = state.running.lock().expect("running lock").get(&key) {
        flag.store(true, Ordering::Relaxed);
    }
}

fn commands<R: Runtime>(builder: tauri::Builder<R>) -> tauri::Builder<R> {
    builder.invoke_handler(tauri::generate_handler![
        presets,
        help,
        tools_status,
        set_ffmpeg_dir,
        plan,
        convert,
        cancel
    ])
}

fn context<R: Runtime>() -> tauri::Context<R> {
    tauri::generate_context!()
}

fn main() {
    commands(tauri::Builder::default())
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let configured = location::load(app.handle());
            let tools = location::find(configured.as_deref());
            app.manage(App::new(configured, tools));
            Ok(())
        })
        .run(context())
        .expect("error while running the app");
}

#[cfg(test)]
mod tests {
    use std::process::Command;
    use std::sync::mpsc;

    use serde_json::{Value, json};
    use tauri::Listener;
    use tauri::ipc::{CallbackFn, InvokeBody};
    use tauri::test::{INVOKE_KEY, MockRuntime, get_ipc_response, mock_builder};
    use tauri::webview::InvokeRequest;
    use tauri::{WebviewWindow, WebviewWindowBuilder};

    use super::*;

    #[cfg(any(windows, target_os = "android"))]
    const LOCAL_URL: &str = "http://tauri.localhost";
    #[cfg(not(any(windows, target_os = "android")))]
    const LOCAL_URL: &str = "tauri://localhost";

    fn app() -> (tauri::App<MockRuntime>, WebviewWindow<MockRuntime>) {
        let app = commands(mock_builder())
            .manage(App::new(None, Some(Tools::default())))
            .build(context())
            .unwrap();
        let webview = WebviewWindowBuilder::new(&app, "main", Default::default())
            .build()
            .unwrap();
        (app, webview)
    }

    fn invoke(
        webview: &WebviewWindow<MockRuntime>,
        cmd: &str,
        body: Value,
    ) -> Result<Value, Value> {
        get_ipc_response(
            webview,
            InvokeRequest {
                cmd: cmd.into(),
                callback: CallbackFn(0),
                error: CallbackFn(1),
                url: LOCAL_URL.parse().unwrap(),
                body: InvokeBody::Json(body),
                headers: Default::default(),
                invoke_key: INVOKE_KEY.to_string(),
            },
        )
        .map(|body| body.deserialize().unwrap())
    }

    fn source(dir: &Path, seconds: u32) -> String {
        let path = dir.join("clip.mov");
        let status = Command::new("ffmpeg")
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
            ])
            .arg(format!("testsrc2=s=320x240:r=30:d={seconds}"))
            .args(["-c:v", "libx264", "-preset", "ultrafast"])
            .arg(&path)
            .status()
            .expect("ffmpeg must be in PATH");
        assert!(status.success());
        path.display().to_string()
    }

    fn settings(preset: &str, video: &str) -> Value {
        json!({ "preset": preset, "video": video, "audio": "", "outdir": null })
    }

    #[test]
    fn presets_and_help() {
        let (_app, webview) = app();
        let presets = invoke(&webview, "presets", json!({})).unwrap();
        let names: Vec<&str> = presets
            .as_array()
            .unwrap()
            .iter()
            .map(|p| p["name"].as_str().unwrap())
            .collect();
        assert_eq!(names, ["tiny", "small", "default", "quality"]);
        let help = invoke(&webview, "help", json!({})).unwrap();
        assert!(help["video"].as_str().unwrap().contains("--video"));
    }

    #[test]
    fn tools_status_names_the_version() {
        let (_app, webview) = app();
        let status = invoke(&webview, "tools_status", json!({})).unwrap();
        let version = status["version"].as_str().unwrap();
        assert!(
            version.starts_with("ffmpeg ") && !version.contains("Copyright"),
            "{status}"
        );
        assert_eq!(status["path"], "ffmpeg");
    }

    #[test]
    fn plan_describes_each_input() {
        let dir = tempfile::tempdir().unwrap();
        let input = source(dir.path(), 1);
        let missing = dir.path().join("missing.mp4").display().to_string();
        let (_app, webview) = app();
        let plan = invoke(
            &webview,
            "plan",
            json!({ "inputs": [input, missing], "settings": settings("tiny", "h264,crf=20") }),
        )
        .unwrap();
        assert_eq!(
            plan["applied"]["video"],
            "h264,long=640,short=360,maxfps=30,crf=20"
        );
        let items = plan["items"].as_array().unwrap();
        assert!(
            items[0]["output"]
                .as_str()
                .unwrap()
                .ends_with("clip.tab5.mp4")
        );
        assert!(
            items[0]["video"]
                .as_str()
                .unwrap()
                .contains("H.264 main 320x240"),
            "{items:?}"
        );
        assert_eq!(items[0]["duration"], 1.0);
        assert_eq!(items[0]["exists"], false);
        assert!(
            items[1]["error"]
                .as_str()
                .unwrap()
                .contains("ffprobe failed"),
            "{items:?}"
        );
    }

    #[test]
    fn plan_rejects_bad_settings() {
        let (_app, webview) = app();
        let err = invoke(
            &webview,
            "plan",
            json!({ "inputs": [], "settings": settings("default", "mjpeg,crf=20") }),
        )
        .unwrap_err();
        assert!(err.as_str().unwrap().contains("unknown key 'crf'"), "{err}");
    }

    #[test]
    fn convert_reports_progress_and_finishes() {
        let dir = tempfile::tempdir().unwrap();
        let input = source(dir.path(), 2);
        let output = dir.path().join("out").join("clip.mp4");
        let (app, webview) = app();
        let (tx, rx) = mpsc::channel();
        app.listen_any("progress", move |event| {
            let _ = tx.send(event.payload().to_string());
        });
        let outcome = invoke(
            &webview,
            "convert",
            json!({
                "key": input,
                "input": input,
                "output": output.display().to_string(),
                "settings": settings("default", ""),
            }),
        )
        .unwrap();
        assert_eq!(outcome["kind"], "done", "{outcome}");
        assert!(
            outcome["report"][0]
                .as_str()
                .unwrap()
                .starts_with("mjpeg: 60 frames")
        );
        assert!(output.is_file());
        let first: Value = serde_json::from_str(&rx.try_recv().unwrap()).unwrap();
        assert_eq!(first["key"], input.as_str());
    }

    #[test]
    fn convert_can_be_cancelled() {
        let dir = tempfile::tempdir().unwrap();
        let input = source(dir.path(), 60);
        let output = dir.path().join("clip.mp4");
        let (app, webview) = app();
        let (tx, rx) = mpsc::channel();
        app.listen_any("progress", move |_| {
            let _ = tx.send(());
        });
        let canceller = {
            let webview = webview.clone();
            let key = input.clone();
            std::thread::spawn(move || {
                rx.recv().unwrap();
                invoke(&webview, "cancel", json!({ "key": key })).unwrap();
            })
        };
        let outcome = invoke(
            &webview,
            "convert",
            json!({
                "key": input,
                "input": input,
                "output": output.display().to_string(),
                "settings": settings("tiny", ""),
            }),
        )
        .unwrap();
        canceller.join().unwrap();
        assert_eq!(outcome["kind"], "cancelled", "{outcome}");
        assert!(!output.exists());
    }
}
