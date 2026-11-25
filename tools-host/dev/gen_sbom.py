#!/usr/bin/env python3
"""ix68k の CycloneDX 1.6 SBOM (JSON) を生成する。

使い方: python3 tools-host/dev/gen_sbom.py [出力パス]
  既定の出力: build/ix68k.cdx.json

コンポーネントの構成:
  - git submodule (extern/cmc68000, extern/SDL_GameControllerDB): commit hash を git から取得
  - cmc68000 が内包する Berkeley SoftFloat 3e
  - ymfm 由来のソースコード (src/audio/ym2151*.c)
  - SDL3: ビルドに使った Homebrew の INSTALL_RECEIPT から取得 (無ければ unknown 扱いで省略)
"""

import datetime
import json
import subprocess
import sys
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = REPO_ROOT / "build" / "ix68k.cdx.json"
SPEC_VERSION = "1.6"
HOMEBREW_SDL3_RECEIPT = Path("/opt/homebrew/opt/sdl3/INSTALL_RECEIPT.json")
HOMEBREW_SDL3_CELLAR = Path("/opt/homebrew/Cellar/sdl3")

SUBMODULES = [
    {
        "path": "extern/cmc68000",
        "name": "cmc68000",
        "url": "https://github.com/TerukiYamaguchi/cmc68000",
        "license": None,
        "description": "MC68000 family CPU core (first-party submodule)",
    },
    {
        "path": "extern/SDL_GameControllerDB",
        "name": "SDL_GameControllerDB",
        "url": "https://github.com/mdqinc/SDL_GameControllerDB",
        "license": "Zlib",
        "description": "Game controller mapping database, bundled at install time",
    },
]


def run_git(*args, cwd=REPO_ROOT):
    return subprocess.run(
        ["git", *args], cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


def license_entry(spdx_id):
    return [{"license": {"id": spdx_id}}] if spdx_id else []


def external_ref(url):
    return [{"type": "vcs", "url": url}]


def submodule_component(entry):
    commit = run_git("rev-parse", "HEAD", cwd=REPO_ROOT / entry["path"])
    component = {
        "type": "library",
        "bom-ref": f"{entry['name']}@{commit}",
        "name": entry["name"],
        "version": commit,
        "description": entry["description"],
        "externalReferences": external_ref(entry["url"]),
    }
    licenses = license_entry(entry["license"])
    if licenses:
        component["licenses"] = licenses
    return component


def softfloat_component():
    return {
        "type": "library",
        "bom-ref": "berkeley-softfloat-3@3e",
        "name": "berkeley-softfloat-3",
        "version": "3e",
        "description": "Bundled in cmc68000 (FPU core)",
        "licenses": license_entry("BSD-3-Clause"),
        "externalReferences": external_ref(
            "https://github.com/ucb-bar/berkeley-softfloat-3"
        ),
    }


def ymfm_component():
    return {
        "type": "library",
        "bom-ref": "ymfm",
        "name": "ymfm",
        "description": "FM sound core (src/audio/ym2151*.c) ported via XEiJ; copied source, no pinned version",
        "licenses": license_entry("BSD-3-Clause"),
        "externalReferences": external_ref("https://github.com/aaronsgiles/ymfm"),
    }


def sdl3_version():
    """Homebrew の INSTALL_RECEIPT.json から SDL3 のバージョンを得る。無ければ None。"""
    if not HOMEBREW_SDL3_RECEIPT.is_file():
        return None
    receipt = json.loads(HOMEBREW_SDL3_RECEIPT.read_text())
    version = receipt.get("source", {}).get("versions", {}).get("stable")
    if version:
        return version
    versions = sorted(p.name for p in HOMEBREW_SDL3_CELLAR.glob("*"))
    return versions[-1] if versions else None


def sdl3_component():
    version = sdl3_version()
    if version is None:
        print("warning: SDL3 version not found, omitting SDL3", file=sys.stderr)
        return None
    return {
        "type": "library",
        "bom-ref": f"pkg:generic/SDL3@{version}",
        "name": "SDL3",
        "version": version,
        "purl": f"pkg:brew/sdl3@{version}",
        "licenses": license_entry("Zlib"),
        "externalReferences": external_ref("https://github.com/libsdl-org/SDL"),
    }


def build_sbom():
    root_commit = run_git("rev-parse", "HEAD")
    root = {
        "type": "application",
        "bom-ref": "ix68k",
        "name": "ix68k",
        "version": root_commit,
        "description": "Sharp X68000 emulator",
        "externalReferences": external_ref(
            "https://github.com/TerukiYamaguchi/ix68k"
        ),
    }
    components = [submodule_component(e) for e in SUBMODULES]
    components.append(softfloat_component())
    components.append(ymfm_component())
    sdl3 = sdl3_component()
    if sdl3:
        components.append(sdl3)

    dependencies = [
        {"ref": root["bom-ref"], "dependsOn": [c["bom-ref"] for c in components
                                                if c["name"] != "berkeley-softfloat-3"]},
        {"ref": f"cmc68000@{components[0]['version']}",
         "dependsOn": ["berkeley-softfloat-3@3e"]},
    ]
    return {
        "bomFormat": "CycloneDX",
        "specVersion": SPEC_VERSION,
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(
                timespec="seconds"
            ),
            "tools": {"components": [{
                "type": "application",
                "name": "gen_sbom.py",
                "description": "tools-host/dev/gen_sbom.py",
            }]},
            "component": root,
        },
        "components": components,
        "dependencies": dependencies,
    }


def main():
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUTPUT
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(build_sbom(), indent=2, ensure_ascii=False) + "\n")
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
