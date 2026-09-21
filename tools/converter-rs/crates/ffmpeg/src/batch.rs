// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hiroki Kawakami

use std::collections::HashMap;
use std::ffi::OsString;
use std::path::{Path, PathBuf};

use anyhow::{Result, bail};

use tab5conv_core::container::Container;

const CONVERTED_MARK: &str = ".tab5.";

pub enum Target {
    Convert {
        input: PathBuf,
        output: PathBuf,
        container: Container,
    },
    Skip {
        input: PathBuf,
        reason: &'static str,
    },
}

fn stem(input: &Path) -> OsString {
    input
        .file_stem()
        .unwrap_or(input.as_os_str())
        .to_os_string()
}

fn with_suffix(mut name: OsString, suffix: &str) -> OsString {
    name.push(suffix);
    name
}

fn is_converted(input: &Path) -> bool {
    input
        .file_name()
        .is_some_and(|name| name.to_string_lossy().contains(CONVERTED_MARK))
}

fn same_file(a: &Path, b: &Path) -> bool {
    let absolute = |p: &Path| std::path::absolute(p).ok();
    if absolute(a).is_some_and(|x| Some(x) == absolute(b)) {
        return true;
    }
    matches!((a.canonicalize(), b.canonicalize()), (Ok(x), Ok(y)) if x == y)
}

pub fn plan(
    inputs: &[PathBuf],
    output: Option<&Path>,
    outdir: Option<&Path>,
) -> Result<Vec<Target>> {
    if output.is_some() && inputs.len() > 1 {
        bail!("-o takes a single input; use --outdir for several");
    }
    let several = inputs.len() > 1;
    let mut targets = Vec::new();
    let mut problems = Vec::new();
    let mut seen: HashMap<PathBuf, &Path> = HashMap::new();
    for input in inputs {
        if several && is_converted(input) {
            targets.push(Target::Skip {
                input: input.clone(),
                reason: "converted output",
            });
            continue;
        }
        let out = match (output, outdir) {
            (Some(path), _) => path.to_path_buf(),
            (None, Some(dir)) => dir.join(with_suffix(stem(input), ".mp4")),
            (None, None) => input.with_file_name(with_suffix(stem(input), ".tab5.mp4")),
        };
        if same_file(input, &out) {
            problems.push(format!(
                "{}: the output would overwrite the input",
                input.display()
            ));
            continue;
        }
        let key = std::path::absolute(&out).unwrap_or_else(|_| out.clone());
        if let Some(first) = seen.insert(key, input) {
            problems.push(format!(
                "{} and {} would both write {}",
                first.display(),
                input.display(),
                out.display()
            ));
            continue;
        }
        let container = match Container::from_path(&out) {
            Ok(container) => container,
            Err(err) => {
                problems.push(format!("{err:#}"));
                continue;
            }
        };
        targets.push(Target::Convert {
            input: input.clone(),
            output: out,
            container,
        });
    }
    if !problems.is_empty() {
        bail!("{}", problems.join("\n       "));
    }
    Ok(targets)
}

pub fn existing(targets: &[Target]) -> Vec<&Path> {
    targets
        .iter()
        .filter_map(|t| match t {
            Target::Convert { output, .. } if output.exists() => Some(output.as_path()),
            _ => None,
        })
        .collect()
}

pub fn part_path(output: &Path) -> PathBuf {
    let mut name = stem(output);
    name.push(".part");
    if let Some(ext) = output.extension() {
        name.push(".");
        name.push(ext);
    }
    output.with_file_name(name)
}

/// Cover art for the runs ffmpeg muxes, which only takes it as a file. Deleted
/// with the part file, so a failed run leaves nothing behind either.
pub fn cover_path(output: &Path) -> PathBuf {
    let mut name = stem(output);
    name.push(".cover.jpg");
    output.with_file_name(name)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn paths(names: &[&str]) -> Vec<PathBuf> {
        names.iter().map(PathBuf::from).collect()
    }

    fn outputs(targets: &[Target]) -> Vec<String> {
        targets
            .iter()
            .map(|t| match t {
                Target::Convert { output, .. } => output.display().to_string(),
                Target::Skip { input, reason } => format!("skip {} ({reason})", input.display()),
            })
            .collect()
    }

    #[test]
    fn default_outputs_sit_next_to_the_inputs() {
        let t = plan(&paths(&["a/x.mkv", "y.mp4"]), None, None).unwrap();
        assert_eq!(outputs(&t), ["a/x.tab5.mp4", "y.tab5.mp4"]);
    }

    #[test]
    fn outdir_drops_the_tab5_mark() {
        let t = plan(
            &paths(&["a/x.mkv", "b/y.webm"]),
            None,
            Some(Path::new("out")),
        )
        .unwrap();
        assert_eq!(outputs(&t), ["out/x.mp4", "out/y.mp4"]);
    }

    #[test]
    fn explicit_output_needs_a_single_input() {
        let t = plan(&paths(&["x.mkv"]), Some(Path::new("o.mkv")), None).unwrap();
        assert_eq!(outputs(&t), ["o.mkv"]);
        assert!(plan(&paths(&["x.mkv", "y.mkv"]), Some(Path::new("o.mp4")), None).is_err());
        assert!(plan(&paths(&["x.mkv"]), Some(Path::new("o.avi")), None).is_err());
    }

    #[test]
    fn converted_outputs_are_skipped_only_in_batches() {
        let t = plan(
            &paths(&["x.mp4", "x.tab5.mp4", "y.tab5.part.mp4"]),
            None,
            None,
        )
        .unwrap();
        assert_eq!(
            outputs(&t),
            [
                "x.tab5.mp4",
                "skip x.tab5.mp4 (converted output)",
                "skip y.tab5.part.mp4 (converted output)"
            ]
        );
        let t = plan(&paths(&["x.tab5.mp4"]), None, None).unwrap();
        assert_eq!(outputs(&t), ["x.tab5.tab5.mp4"]);
    }

    #[test]
    fn collisions_and_self_overwrites_are_errors() {
        let err = plan(
            &paths(&["a/x.mp4", "b/x.mkv"]),
            None,
            Some(Path::new("out")),
        )
        .err()
        .unwrap()
        .to_string();
        assert!(err.contains("would both write out/x.mp4"), "{err}");
        let err = plan(&paths(&["out/x.mp4"]), None, Some(Path::new("out")))
            .err()
            .unwrap()
            .to_string();
        assert!(err.contains("would overwrite the input"), "{err}");
        let err = plan(&paths(&["x.mp4"]), Some(Path::new("./x.mp4")), None)
            .err()
            .unwrap()
            .to_string();
        assert!(err.contains("would overwrite the input"), "{err}");
    }

    #[test]
    fn part_files() {
        assert_eq!(
            part_path(Path::new("a/x.tab5.mp4")),
            Path::new("a/x.tab5.part.mp4")
        );
        assert_eq!(
            part_path(Path::new("out/x.mkv")),
            Path::new("out/x.part.mkv")
        );
    }
}
