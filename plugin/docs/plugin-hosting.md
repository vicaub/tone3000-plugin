# Hosted plugin blocks (VST3 / AU)

Tracking issue: #208. A chain block of type `plugin` hosts a third-party
effect installed on the machine, next to NAM and IR blocks. Desktop only
(VST3 on macOS / Windows / Linux, AU on macOS); compiled out on iOS and
whenever `T3K_PLUGIN_HOSTING=OFF`.

## What the user sees

- Plugin Settings: "Third-party plugins" toggle (off by default, machine-wide)
  and, under it, "Scan plugins" with the count of plugins found.
- An insert slot's action sheet gains "Add Plugin…" (only while the toggle is
  on), which opens a picker of the scanned effects (name, vendor, format).
- The tile shows the plugin's name; the detail card has the usual In / Out /
  Mix, EQ and meters, plus "Open Editor", which shows the plugin's own UI in a
  separate native window.
- A preset or project naming a plugin that isn't installed (or the toggle is
  off) shows the block as failed ("Plugin not found: <name>"), passes audio
  through, and keeps the plugin's state untouched so saving round-trips it.

## Data model

A hosted plugin block reuses the tone plumbing instead of growing a second
identity path: `toneJson` is a small synthetic tone

```json
{ "id": 0, "title": "<plugin name>", "format": "plugin",
  "plugin": { "name": "...", "vendor": "...", "format": "VST3",
              "uid": "<PluginDescription::createIdentifierString()>",
              "description": "<PluginDescription XML>" } }
```

so `getChainState`, presets, undo, duplicate and clipboard carry it with no
special casing, and the UI reads name/format from the same tone summary.
`toneId` is 0 and `activeModelId` is 0 for every plugin block.

The plugin's opaque state (`AudioPluginInstance::getStateInformation`) lives
in `ChainBlock::pluginState` and is serialized as a `pluginState` binary
property on the block's tree. It is captured from the live instance on the
message thread whenever a snapshot is taken (undo, preset, project), so
edits made in the plugin's own editor ride every save.

## Threads

- Scanning runs on a background thread (`PluginHost::scanAsync`); results
  persist as XML in the app-data folder (`hosted-plugins.xml`), with JUCE's
  dead-man's-pedal file blacklisting a plugin that crashed mid-scan.
- Instantiation runs on the existing loader pool, like model prepares.
  `AudioPluginFormatManager::createPluginInstance` is called off the message
  thread; formats that need an unblocked message thread (AUv3) are honoured
  because the pool never blocks it.
- The prepared instance is spliced in under `chainMutex` through the existing
  swap-fade handshake (`requestSwapFadeAndWait` + `applyPreparedModel...`),
  and the old instance is destroyed after the lock is released.

## Audio path

The hosted plugin runs inside the block's base-rate island
(`ChainBlock::irBaseRateIsland`, now used by IR and plugin blocks), so it
always sees `kChainBaseSampleRate` (48 kHz) and at most
`chainBaseBlockSize()` frames per call, whatever the host rate or the
oversampling factor. It is prepared once with that spec at instantiation and
re-prepared by `prepareChain` when the base block size moves.

The instance is configured stereo-in / stereo-out (falling back to mono/mono)
and fed the lane's working channels; a mono lane duplicates channel 0 into a
scratch second channel so stereo-only plugins work. Mix, In / Out gain, EQ
and the wet fade are the block's usual stages around it.

Latency: the plugin's reported latency is not compensated in this first
version (the dry share of Mix would comb against a delayed wet path, and a
mid-session PDC change is what the chain domain was designed to avoid). Most
modulation / delay / reverb plugins report zero. The tail is folded into
`getTailLengthSeconds`.

## Out of scope (first version)

MIDI / sidechain routing, out-of-process sandboxing, AUv3 on iOS, plugin
parameter automation through the host, latency compensation.
