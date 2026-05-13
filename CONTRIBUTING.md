# Contributing to The Wave of Hormuz

## Building locally

Prerequisites: [VCV Rack 2 SDK](https://vcvrack.com/downloads), MSYS2 with the MINGW64 toolchain.
See [README.md](README.md#prerequisites) for the full setup table.

```bash
# Build plugin.dll
RACK_DIR="$HOME/Documents/Rack-SDK" ./build.sh

# Build + install to the local Rack plugins folder
RACK_DIR="$HOME/Documents/Rack-SDK" ./build.sh install

# Package a distributable .vcvplugin
RACK_DIR="$HOME/Documents/Rack-SDK" ./build.sh dist
```

The Linux SDK builds cleanly via the CI workflow without the Windows CRT
workaround — see `.github/workflows/build.yml`.

---

## Releasing a new version

1. **Update the version number in two places:**

   - [`plugin.json`](plugin.json) — `"version"` field
   - [`Makefile`](Makefile) — `VERSION` variable

   Both must match exactly (e.g. `2.3.0`).

2. **Add a changelog entry** in [`CHANGELOG.md`](CHANGELOG.md) under a new
   `## [x.y.z] — YYYY-MM-DD` heading. Summarise what changed.

3. **Commit and push to `main`:**

   ```bash
   git add plugin.json Makefile CHANGELOG.md
   git commit -m "v2.3.0 — brief description"
   git push
   ```

4. **Note the commit hash:**

   ```bash
   git rev-parse HEAD
   ```

5. **Notify the VCV Library maintainers** by commenting on the plugin's issue
   thread at <https://github.com/VCVRack/library/issues> (search for
   `WaveOfHormuz`). Include:
   - The new version number (e.g. `2.3.0`)
   - The full commit hash from step 4
   - Do **not** reference branch names

   The maintainers will rebuild the plugin and close/re-open the issue when
   done.

---

## Submitting to the VCV Library (first time)

1. Open one issue at <https://github.com/VCVRack/library/issues>.
2. Set the title to the exact plugin slug: `WaveOfHormuz`.
3. In the body, include the source URL:
   `https://github.com/quaternionmedia/waveofhormuz`
4. Wait for a maintainer to review and post a confirmation comment.

---

## Automated workflows

| Workflow | File | Trigger | What it does |
|---|---|---|---|
| CI build | [`.github/workflows/build.yml`](.github/workflows/build.yml) | Every push to `main`, every PR | Builds against Rack SDK 2.6.6 (Linux), packages `.vcvplugin`, uploads as a run artifact |
| GitHub Pages | [`.github/workflows/pages.yml`](.github/workflows/pages.yml) | Every push to `main` | Deploys README as the plugin website at <https://quaternionmedia.github.io/waveofhormuz> |

**One-time setup for GitHub Pages:** In the repo Settings → Pages → Source,
select **"GitHub Actions"** (not "Deploy from a branch").

---

## Updating the Rack SDK version in CI

Edit the `RACK_SDK_VERSION` env var at the top of
[`.github/workflows/build.yml`](.github/workflows/build.yml). Available
versions are listed at <https://vcvrack.com/downloads>.
