# AGENT.md — install, wire and verify this frontend

Written for an AI agent with a terminal on the target machine. If you are a human,
`./setup.sh` does the same thing deterministically and you can skip to README.md.

This document is the distilled result of building this stack on an 8 GB Turing card
(RTX 2060 SUPER, driver 595, Ubuntu 24.04). The traps in §8 each cost real time once;
they are here so they cost you none.

---

## 0. Non-negotiables

* **Never `sudo` without asking the user.** Everything here is user-local.
* **Never delete or restructure an existing `~/git/ComfyUI` or model vault.** Reuse it.
  If a file already exists and verifies, leave it alone.
* **Do not vendor or re-upload the model weights.** They are downloaded from Hugging
  Face under the Qwen Research License (research/evaluation only). Do not push them
  into a repository, and do not strip the licence notice.
* **Stop at the first red gate.** Do not improvise past a failed check; report the
  symptom, the command output, and what you tried.
* **Ask before any download over ~5 GB** if the user has not already agreed to the
  ~14 GB weight set.

---

## 1. Acceptance gates

You are done when all five pass. Report each one with the command you ran.

| Gate | Check | Pass looks like |
|---|---|---|
| G1 | environment | `python3 -c "import torch"` works in the ComfyUI venv, `torch.cuda.is_available()` is True, device capability printed |
| G2 | engine | ComfyUI answers on `127.0.0.1:8188` and `/object_info/UnetLoaderGGUF` lists the Qwen GGUF |
| G3 | weights | every entry in `models.json` either verifies by sha256 or is marked optional and skipped |
| G4 | UI | `harness/Build-Linux/QwenImageHarness --version` prints a version, and a headless run writes a PNG |
| G5 | mask correctness | outside `mask + 14 px` the output is byte-identical to the input |

Do not claim success on "it looks right". G5 is numeric on purpose.

---

## 2. Phase 0 — probe before touching anything

```bash
nvidia-smi --query-gpu=name,memory.total,driver_version,compute_cap --format=csv,noheader
df -h "$HOME" | tail -1
python3 -V; python3 -c 'import venv' && echo venv-ok
command -v qmake qmake6 aria2c git
free -g | head -2
```

Pick the torch wheel from the **driver**, not from the CUDA version in the docs:

| Driver | Wheel index | Note |
|---|---|---|
| ≥ 580 | `cu130` | what this build was validated with |
| 570–579 | `cu128` | fine, no gguf-relevant difference |
| 550–569 | `cu126` | fine |
| < 550 | — | stop: ask the user to update the driver |

VRAM budget on this model: 8 GB works with GGUF Q4 + W4A8 encoder at 1024². 6 GB does
not. 12 GB+ can use larger quants but nothing here depends on it.

Disk: ~18 GB without weights, ~40 GB with.

---

## 3. Phase 1 — Python environment

```bash
COMFY_DIR=$HOME/git/ComfyUI
git clone https://github.com/comfyanonymous/ComfyUI.git "$COMFY_DIR"
git -C "$COMFY_DIR" checkout v0.39.2      # >= v0.37 is the hard floor for Qwen-Image-2.1
python3 -m venv "$COMFY_DIR/venv"
"$COMFY_DIR/venv/bin/pip" install -U pip wheel
```

**Requirements trick.** `requirements.txt` pins
`comfyui-workflow-templates-media-assets-02==0.1.11`, a package that does not exist on
PyPI. Filter that line out, then install the rest:

```bash
grep -v '^comfyui-workflow-templates' "$COMFY_DIR/requirements.txt" > /tmp/req.txt
"$COMFY_DIR/venv/bin/pip" install -r /tmp/req.txt
"$COMFY_DIR/venv/bin/pip" install --force-reinstall \
    --index-url https://download.pytorch.org/whl/cu130 torch torchvision torchaudio
```

**cuDNN pin.** `pip` will happily install a cuDNN that differs from the one torch was
built against; VAE decode then dies with `CUDNN_STATUS_SUBLIBRARY_VERSION_MISMATCH`.
Query torch's own requirement and pin exactly that:

```bash
"$COMFY_DIR/venv/bin/python" -c "import importlib.metadata as m;print([d for d in m.requires('torch') if 'cudnn' in d][0])"
# e.g. nvidia-cudnn-cu13==9.20.0.48
"$COMFY_DIR/venv/bin/pip" install "nvidia-cudnn-cu13==9.20.0.48"
```

Verify (G1):

```bash
"$COMFY_DIR/venv/bin/python" -c "import torch;print(torch.__version__, torch.version.cuda, torch.cuda.is_available(), torch.cuda.get_device_capability(0))"
```

If `is_available()` is False the driver is older than the wheel — go back to the table in
§2, do not patch around it.

---

## 4. Phase 2 — ComfyUI-GGUF and the architecture patch

```bash
git clone https://github.com/city96/ComfyUI-GGUF "$COMFY_DIR/custom_nodes/ComfyUI-GGUF"
"$COMFY_DIR/venv/bin/pip" install -r "$COMFY_DIR/custom_nodes/ComfyUI-GGUF/requirements.txt"
git -C "$COMFY_DIR/custom_nodes/ComfyUI-GGUF" apply patches/comfyui-gguf-qwen-image-2.1.patch
```

Why: community Qwen-Image-2.1 GGUFs are converted without `general.architecture`, so
`UnetLoaderGGUF` raises `Unknown model architecture`. The patch teaches the node the
tensor signature and reports arch `qwen_image`, after which ComfyUI's own
`model_detection.py` picks the 2.1 transformer from the keys.

If the patch no longer applies because the node moved on, the equivalent change is a
`ModelQwenImage21` template in `tools/convert.py` with
`keys_detect = [("txt_in.text_norm.weight", "modulation.1.weight", "transformer_blocks.0.attn.norm_q.weight")]`
and arch `"qwen_image"`, appended to `arch_list`.

---

## 5. Phase 3 — weights

```bash
python3 patches/fetch_models.py --manifest models.json --vault "$HOME/Models"
python3 patches/fetch_models.py --manifest models.json --vault "$HOME/Models" --link-into "$COMFY_DIR/models"
```

14.3 GB required, 5.9 GB optional (the abliterated encoder). Every file is checked
against the sha256 in `models.json`; failures are deleted and reported so a re-run
resumes cleanly.

**Transport, measured on a hostile network:** `hf download` stalls at ~0.5 MB/s (Xet),
a single curl is throttled to ~3 MB/s and then collapses, `aria2c -x16` against
`hf-mirror.com` reaches 30–90 MB/s. Hence the default mirror and the 16 connections.
If the user's network prefers the Hub, pass `--mirror https://huggingface.co`.

The vault keeps the only real copy; ComfyUI gets file-level symlinks. Do not copy.

---

## 6. Phase 4 — build the UI

```bash
sudo apt install -y build-essential qtbase5-dev qttools5-dev-tools   # ask first
cd harness && ./build-linux.sh && ./install.sh --no-build
```

On Windows the frontend builds with the same `.pro` file: install a Qt 5.12+ MinGW
64-bit kit, then

```powershell
powershell -ExecutionPolicy Bypass -File harness\build-windows.ps1
# -> harness\dist\QwenImageHarness\QwenImageHarness.exe  (windeployqt output, standalone)
```

`harness/build-windows.ps1 -Version 1.2.3 -ZipPath dist\out.zip` produces the release
archive; `.github/workflows/build.yml` does this for both OSes on every push. The engine
phases in this document (venv, ComfyUI, weights) are Linux-only — on Windows install
ComfyUI separately and point Settings → Server at it.

Qt 5.4 API floor, Qt 5.15 in practice (Qt 6 also builds). The binary is ~850 KB; the
launcher, icon and `.desktop` entry go into `~/.local` — no root.

Headless mode is part of the same binary and is what you verify with:

```bash
qwen-image-harness --run "a brass key on dark slate" --steps 8 --size 1024 --out /tmp/t
```

---

## 7. Phase 5 — verify (G4, G5)

G4, generation:

```bash
qwen-image-harness --run "a red ceramic mug on a wooden table" --steps 8 --size 1024 --out /tmp/g4
# prints the saved path, exits 0.  ~45 s on a 2060 SUPER.
```

G5, mask correctness — this is the one that catches a dishonest inpaint:

```bash
qwen-image-harness --run "add a small red flower here" --mode mask \
    --image examples/pole.png --mask examples/pole_mask.png --steps 8 --out /tmp/g5
```

Then assert numerically (numpy/PIL):

```
outside mask grown by 14 px : 0 changed pixels, max delta 0
pole edge columns           : max delta 0 above and below the edit
inside the mask             : changed (that is the edit)
```

If the edges move, the crop/context step or the noise mask is not wired — see §9.

Expected timings at 1024² on this class of card, with `--fp16-unet` (which the app
always passes): turbo 8 steps ≈ 42 s, base 25 steps ≈ 88 s, one-reference edit ≈ 56 s.
Without `--fp16-unet` every number is ~3.8× worse.

---

## 8. Failure table

| Symptom | Cause | Fix |
|---|---|---|
| `Unknown model architecture` from UnetLoaderGGUF | GGUF has no arch metadata | apply the patch (§4) |
| `CUDNN_STATUS_SUBLIBRARY_VERSION_MISMATCH` at VAEDecode | cuDNN ≠ torch's pin | pin cuDNN (§3) |
| `CUDA driver version is insufficient` / `is_available()` False | wheel newer than driver | pick the index from §2 |
| VAE decode OOM at 1024² | TE still resident, or bf16 weights | restart engine, keep `--fp16-unet`, tiled VAE |
| 11–12 s/it instead of ~3 | running bf16 with fp32 cast on Turing | add `--fp16-unet` |
| `normalized_shape=[4096] … got [1,512,12288]` | GGUF text encoder with fused QKV | use the W4A8 safetensors encoder, not the GGUF one |
| `pip` cannot find `comfyui-workflow-templates-media-assets-02` | package does not exist | filter that line out (§3) |
| downloads crawl or stall | Hub/Xet throttling | aria2c + mirror (§5) |
| node `TextEncodeQwenImage21` rejects `image_1` | autogrow keys are `images.image_N` | use `images.image_1` |
| composite pastes the whole frame | `ImageCompositeMasked.mask` is optional and defaults to none | wire it |
| edit shifted / speckled | reference grid mismatch | pass the crop's own size as `resolution` |
| 2048² OOM | VAE decode needs ~7 GB | stay at 1024², upscale afterwards |

---

## 9. How the mask pipeline is supposed to work

Not a cut-and-paste. In order:

1. crop the image around the mask bbox expanded by ~45 % (min 64 px), snapped to 16 px;
2. upscale that crop to ~1024 px on the long side (clamped 0.5–3×);
3. upload the crop as `image_1` and the crop's mask, then
   `VAEEncode` → `SetLatentNoiseMask` → sample: only masked latents are re-sampled, so
   the sampler must adapt to the pixels that are already there (this is the part that
   stops a pole from moving);
4. decode, downscale to the crop, composite into the original at the crop offset with a
   3–12 px feather **only along the painted edge**.

Any implementation that skips (1) gives the model no context; one that skips (3) lets it
redraw the whole crop; one that feathers widely smears untouched geometry. G5 catches
all three.

---

## 10. Wiring a different backend

`harness/BackendFactory.cpp` is a one-line seam: return your own `Backend` subclass.
The contract is in `harness/docs/BACKEND.md` — four virtuals
(`startServer`, `stopServer`, `submit`, `cancel`) and four signals. `ComfyBackend.cpp`
is the reference client (HTTP to a local ComfyUI); `MockBackend.cpp` is a working
simulator you can develop the UI against with no GPU at all.

If you have your own model service, implement `submit()` against its API, emit
`progress(jobId, percent, stage)` as you go, and write results into
`AppSettings::instance().outputDir()` — that directory is what the gallery watches.

---

## 11. Before you report back

* leave nothing running: stop the engine you started, close any X server you opened;
* list the versions you installed and the gates you passed;
* if you changed a path or port away from the defaults, write it down — the next person
  will not guess.
