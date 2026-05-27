#!/usr/bin/env python3
# This file ships into oss-fuzz review-repo PRs via the prepare composite
# action; oss-fuzz's `infra/presubmit.py` check_license greps every Python
# file under non-projects/ paths for the Apache 2.0 LICENSE-2.0 URL, so we
# carry the standard header here even though it's our own infra code.
# Copyright 2026 fuzz-for-me contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Mode-specific prep for the fuzz-verify reusable workflow.

Resolves the project + variant SHA, performs baseline checkout/overlay,
clones oss-fuzz (upstream mode), snapshots a pristine source tree
(upstream mode, bug #2 fix), and emits normalized outputs the
mode-agnostic reusable workflow consumes.
"""

import os
import pathlib
import re
import shutil
import subprocess
import sys

_OSS_FUZZ_DIR = "/tmp/oss-fuzz"
_PRISTINE_DIR = "/tmp/fuzz-verify-pristine"


def _run(args, cwd=None, check=True):
  return subprocess.run(args,
                        cwd=cwd,
                        check=check,
                        capture_output=True,
                        text=True)


def detect_oss_fuzz_project(repo: pathlib.Path) -> str:
  out = _run(
      ["git", "-C",
       str(repo), "diff", "--name-only", "origin/master...HEAD"],
      check=False,
  ).stdout
  for line in out.splitlines():
    if line.startswith("projects/"):
      return line.split("/")[1]
  # Fallback for local test repos without an `origin` remote.
  out = _run(
      ["git", "-C",
       str(repo), "diff", "--name-only", "master...HEAD"],
      check=False,
  ).stdout
  for line in out.splitlines():
    if line.startswith("projects/"):
      return line.split("/")[1]
  return ""


def compute_outputs(mode: str, project: str) -> dict[str, str]:
  if mode == "oss-fuzz":
    return {
        "helper_dir": ".",
        "out_base": "build/out",
        "corpus_root": f"corpus/{project}",
        "seeds_dir": f"projects/{project}/seeds",
    }
  if mode == "upstream":
    return {
        "helper_dir": _OSS_FUZZ_DIR,
        "out_base": f"{_OSS_FUZZ_DIR}/build/out",
        "corpus_root": f"{_OSS_FUZZ_DIR}/corpus/{project}",
        "seeds_dir": ".github/fuzz/seeds",
    }
  raise ValueError(f"unknown mode: {mode}")


def _merge_base(repo: pathlib.Path, ref_a: str, ref_b: str) -> str:
  return _run(["git", "-C", str(repo), "merge-base", ref_a,
               ref_b]).stdout.strip()


def _default_remote_branch(repo: pathlib.Path) -> str:
  r = _run(
      [
          "git", "-C",
          str(repo), "symbolic-ref", "--short", "refs/remotes/origin/HEAD"
      ],
      check=False,
  ).stdout.strip()
  return r.split("origin/")[-1] if r else ""


def _compute_merge_base(repo: pathlib.Path) -> str:
  base_ref = "origin/master"
  if _run(["git", "-C",
           str(repo), "rev-parse", "--verify", "-q", base_ref],
          check=False).returncode != 0:
    base_ref = "master"
  sha = _merge_base(repo, base_ref, "HEAD")
  if not re.fullmatch(r"[0-9a-f]{7,40}", sha):
    raise RuntimeError(
        f"merge-base produced no usable SHA (base_ref={base_ref})")
  return sha


def resolve_variant(mode: str, variant: str, project: str,
                    repo: pathlib.Path) -> dict[str, str]:
  head = _run(["git", "-C", str(repo), "rev-parse", "HEAD"]).stdout.strip()
  if variant == "current":
    return {"sha": os.environ.get("GITHUB_SHA", head), "has_project": "true"}

  sha = _compute_merge_base(repo)

  if mode == "oss-fuzz":
    present = _run(
        [
            "git", "-C",
            str(repo), "cat-file", "-e", f"{sha}:projects/{project}/Dockerfile"
        ],
        check=False,
    ).returncode == 0
    if not present:
      return {"sha": sha, "has_project": "false"}
    shutil.rmtree(repo / "projects" / project, ignore_errors=True)
    _run(["git", "-C", str(repo), "checkout", sha, "--", f"projects/{project}"])
    return {"sha": sha, "has_project": "true"}

  # upstream baseline: stash overlay, hard-reset to merge-base, restore.
  fuzz = repo / ".github" / "fuzz"
  if not fuzz.is_dir():
    print("::error::.github/fuzz not present on PR — "
          "cannot overlay onto baseline")
    return {"sha": sha, "has_project": "false"}
  actions = repo / ".github" / "actions"
  tmp = pathlib.Path(_run(["mktemp", "-d"]).stdout.strip())
  shutil.copytree(fuzz, tmp / "fuzz")
  if actions.is_dir():
    shutil.copytree(actions, tmp / "actions")
  _run(["git", "-C", str(repo), "reset", "--hard", sha])
  _run(
      ["git", "-C",
       str(repo), "submodule", "update", "--init", "--recursive"],
      check=False,
  )
  shutil.rmtree(repo / ".github" / "fuzz", ignore_errors=True)
  shutil.rmtree(repo / ".github" / "actions", ignore_errors=True)
  (repo / ".github").mkdir(exist_ok=True)
  shutil.copytree(tmp / "fuzz", repo / ".github" / "fuzz")
  if (tmp / "actions").is_dir():
    shutil.copytree(tmp / "actions", repo / ".github" / "actions")
  return {"sha": sha, "has_project": "true"}


def _copy_tree(src: pathlib.Path, dst: pathlib.Path) -> str:
  if shutil.which("rsync"):
    dst.mkdir(parents=True, exist_ok=True)
    _run(["rsync", "-a", "--delete", f"{src}/", f"{dst}/"])
  else:
    if dst.exists():
      shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=True)
  return str(dst)


def snapshot_pristine(workspace: pathlib.Path | str,
                      dest_root: pathlib.Path | str) -> str:
  return _copy_tree(pathlib.Path(workspace), pathlib.Path(dest_root))


def materialize_working_copy(pristine: pathlib.Path | str,
                             dest: pathlib.Path | str) -> str:
  return _copy_tree(pathlib.Path(pristine), pathlib.Path(dest))


def _emit(outputs: dict[str, str]):
  path = os.environ["GITHUB_OUTPUT"]
  with open(path, "a") as fh:
    for k, v in outputs.items():
      fh.write(f"{k}={v}\n")


def main():
  mode = os.environ["MODE"]
  variant = os.environ["VARIANT"]
  ws = pathlib.Path(os.environ["GITHUB_WORKSPACE"])
  project = os.environ.get("PROJECT_NAME", "") or detect_oss_fuzz_project(ws)

  var = resolve_variant(mode, variant, project, ws)
  out = {"project": project, **var, "merge_base_sha": _compute_merge_base(ws)}
  is_upstream_project = var["has_project"] == "true" and mode == "upstream"

  if is_upstream_project:
    fuzz = ws / ".github" / "fuzz"
    if not fuzz.is_dir():
      print("::error::.github/fuzz not present — "
            "cannot overlay onto baseline")
      return 1
    _run([
        "git", "clone", "--depth", "1",
        "https://github.com/google/oss-fuzz.git", _OSS_FUZZ_DIR
    ],
         check=False)
    dest = pathlib.Path(f"{_OSS_FUZZ_DIR}/projects/{project}")
    dest.mkdir(parents=True, exist_ok=True)
    for item in fuzz.iterdir():
      tgt = dest / item.name
      (shutil.copytree if item.is_dir() else shutil.copy)(item, tgt)

  paths = compute_outputs(mode, project)
  if is_upstream_project:
    out["pristine_dir"] = snapshot_pristine(ws, _PRISTINE_DIR)
  else:
    out["pristine_dir"] = ""
  out.update(paths)
  _emit(out)
  return 0


if __name__ == "__main__":
  sys.exit(main())
