#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>
#include <memory>

// Third-party plugin hosting for `plugin` chain blocks (see
// plugin/docs/plugin-hosting.md). One process-wide instance owns the format
// manager and the scanned plugin list; every TONE3000Processor in the process
// shares it, like the settings file.
//
// Compiled everywhere so the processor and the DSP tests build unchanged; the
// formats themselves only exist where JUCE_PLUGINHOST_VST3 / _AU are on
// (desktop builds with T3K_PLUGIN_HOSTING). Without them the list is empty
// and every instantiation fails into the block's "plugin not found" state.
class PluginHost {
public:
  static PluginHost& get();

  // True when at least one hosting format was compiled in.
  bool isAvailable() const;

  // Machine-wide opt-in ("Third-party plugins" in Plugin Settings). Off by
  // default. When off, no plugin is instantiated (blocks restore as
  // not-found, state preserved) and the UI hides "Add Plugin…".
  bool isEnabled() const { return enabled.load(); }
  void setEnabled(bool on, bool persist = true);

  // Scanned effects (instruments are filtered out), sorted by name.
  juce::Array<juce::PluginDescription> getKnownEffects() const;
  bool findByIdentifier(const juce::String& identifier, juce::PluginDescription& out) const;

  // Scan the formats' default locations on a background thread. `onDone`
  // runs on the message thread with the number of effects found. A scan
  // already running is not restarted. The list persists in the app-data
  // folder; a plugin that crashes the scan is blacklisted on the next run
  // (JUCE's dead-man's-pedal file).
  void scanAsync(std::function<void(int numEffects)> onDone);
  bool isScanning() const { return scanning.load(); }

  // Off the message thread (the loader pool). The instance comes back
  // prepared at `sampleRate` / `maxBlockSize` with a stereo (or mono)
  // in/out layout. nullptr + `error` on failure.
  std::unique_ptr<juce::AudioPluginInstance> createInstance(const juce::PluginDescription& desc,
                                                            double sampleRate, int maxBlockSize,
                                                            juce::String& error);

  // Description <-> string, for block state.
  static juce::String descriptionToString(const juce::PluginDescription& desc);
  static bool descriptionFromString(const juce::String& xml, juce::PluginDescription& out);

  // Tests: register an in-process plugin type the format manager can't find
  // on disk. The factory runs instead of the format manager when the
  // description's identifier matches.
  using TestFactory = std::function<std::unique_ptr<juce::AudioPluginInstance>()>;
  void registerTestPlugin(const juce::PluginDescription& desc, TestFactory factory);
  void clearTestPlugins();

private:
  PluginHost();
  void loadListFromDisk();
  void saveListToDisk() const;
  static juce::File listFile();
  static juce::File deadMansPedalFile();

  juce::AudioPluginFormatManager formatManager;
  mutable juce::CriticalSection listLock;
  juce::KnownPluginList knownList;
  std::atomic<bool> enabled{false};
  std::atomic<bool> scanning{false};

  struct TestPlugin {
    juce::PluginDescription desc;
    TestFactory factory;
  };
  std::vector<TestPlugin> testPlugins;
};
