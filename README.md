![SimpleLore](docs/Banner.jpg)

# SimpleLore — Lore Quick Commit

An Unreal Engine **editor plugin** that adds a small **Lore** section to the Level Editor
toolbar for one-click status, commit, and sync against a
[Lore](https://epicgames.github.io/lore) version-control server — without ever leaving the
editor.

> Status: **beta (v0.1.0)**. Built and tested against **Unreal Engine 5.7**.

---

## What it does

A colored status dot sits in the toolbar and tells you, at a glance, where your project
stands relative to the Lore server:

| Indicator | State | Meaning |
|-----------|-------|---------|
| 🔴 Red | Disconnected | No connection to the server, or the project isn't initialized with Lore yet. |
| 🟢 Green | Synced | Initialized and fully in sync with the server. |
| 🟡 Yellow | Local changes | You have local, uncommitted changes. |
| 🟠 Orange | Behind | The server has newer commits than your local project. |

Next to the dot are two buttons:

- **⚙ Gear** — Connect / settings. The first time you click it, you enter your **server
  address** and (if needed) initialize the project. Your address is stored **per-user** and
  is **never committed**, so every developer points at their own server.
- **💾 Disk** — the main action button. Opens a commit dialog and sends your changes
  (stage → commit → push). When you're **Behind** (orange) it instead opens a diff panel
  with an **Accept Changes** option that pulls the latest from the server.

The indicator updates automatically: it polls the server periodically, and turns **yellow
the moment you save** an asset or level.

---

## Requirements

- **Unreal Engine 5.7** (other 5.x versions may work but are untested — you may need to
  rebuild).
- The **Lore CLI** (`lore.exe`) installed and on your `PATH`, *or* an absolute path set in
  the plugin settings. Get it from the [Lore project](https://epicgames.github.io/lore).
- A reachable **Lore server** to push to.
- A C++ toolchain (Visual Studio) — this is a source plugin and is compiled by the editor
  on first load.

---

## Installation

1. Copy the **`LoreQuickCommit`** folder from this repo into your project's `Plugins`
   folder:

   ```
   YourProject/
     Plugins/
       LoreQuickCommit/        <-- this folder
         LoreQuickCommit.uplugin
         Source/
   ```

2. Right-click your `.uproject` → **Generate Visual Studio project files** (optional but
   recommended).
3. Open the project. The editor will offer to **rebuild** the missing module — accept.
   (Or build the editor target manually before launching.)
4. Once loaded, enable it under **Edit → Plugins → Version Control → Lore Quick Commit** if
   it isn't already, and restart if prompted.

---

## First-time setup

1. Click the **⚙ gear** in the toolbar's Lore section.
2. Enter your **server host** — just the IP address or hostname (e.g. `192.168.1.50` or
   `lore.mystudio.net`). **Do not** include a port or `lore://` scheme; the plugin adds
   those for you.
3. If the project isn't a Lore repo yet, choose **Initialize** — this creates the
   repository on the server, writes a default `.loreignore`, makes the first commit, and
   pushes.
4. The dot turns **green** once you're connected and in sync.

That's it. From then on, **save** your work and click the **💾 disk** to commit & push.

---

## Configuration

All settings live under **Project Settings → Plugins → Lore Quick Commit** (and are stored
per-user, not in source control):

| Setting | Default | Notes |
|---------|---------|-------|
| **Enabled** | `true` | Master on/off switch. |
| **Server Host** | *(empty)* | Your server IP/hostname. Set via the gear button. |
| **Lore Executable** | `lore.exe` | Resolved from `PATH`, or set an absolute path. |
| **Server Port** | `41337` | gRPC/QUIC port used in the `lore://` URL. |
| **Health Check Port** | `41339` | HTTP port for the connectivity probe. |
| **Poll Interval (s)** | `45` | How often to check the server for newer commits (30–120). |

The plugin also writes a default **`.loreignore`** at your project root on initialize,
excluding Unreal's generated folders (`Intermediate/`, `Saved/`, `DerivedDataCache/`,
`Binaries/`, …) so build artifacts never get committed.

---

## How it works

The plugin is a thin editor-side wrapper around the `lore` command-line tool. Every command
it runs is centralized in `FLoreClient` (`Source/LoreQuickCommit/Public/LoreClient.h` +
`Private/LoreClient.cpp`), so adapting to future CLI changes is a one-file job. The toolbar,
panels, and status polling live in `FLoreQuickCommitModule`.

Roughly, the commands used are:

- `lore status` — derive synced / dirty / behind state
- `lore repository create …` → `lore stage . --scan` → `lore commit "…"` → `lore push` — initialize
- `lore stage . --scan` → `lore commit "…"` → `lore push` — commit & publish
- `lore sync --reset` — accept the server version (destructive pull)

---

## Project layout

```
LoreQuickCommit/
  LoreQuickCommit.uplugin            # plugin descriptor
  Source/LoreQuickCommit/
    LoreQuickCommit.Build.cs         # module build rules / dependencies
    Public/
      LoreClient.h                   # lore CLI wrapper + status types
      LoreQuickCommitModule.h        # toolbar / panels / polling
      LoreQuickCommitSettings.h      # per-user settings object
    Private/
      LoreClient.cpp
      LoreQuickCommitModule.cpp
      LoreQuickCommitSettings.cpp
```

---

## License

See [LICENSE](LICENSE).
