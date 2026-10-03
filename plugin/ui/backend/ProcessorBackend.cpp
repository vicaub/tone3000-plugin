#include "ProcessorBackend.h"

#include "PluginHost.h"
#include "WindowKeyEvents.h"

namespace t3k::ui {

ProcessorBackend::ProcessorBackend(TONE3000Processor& processor, juce::Component& peerHost)
    : processor_(processor), peerHost_(peerHost) {
  // Bespoke audio settings (standalone only): the controller listens to the
  // device manager and pushes changes; stores re-pull state on the event.
  if (StandaloneAudioSettings::isAvailable())
    audioSettings_ = std::make_unique<StandaloneAudioSettings>(processor_, [this] {
      listeners.call([](Listener& l) { l.audioDeviceChanged(); });
    });

  // MIDI map push (all builds): learn commits, removals and state restores
  // land as one event so the settings UI re-pulls instead of polling.
  processor_.midiMapper.onChanged = [this] {
    listeners.call([](Listener& l) { l.midiMapChanged(); });
  };
}

ProcessorBackend::~ProcessorBackend() {
  // Plugin editors reference live instances; close them before anything else.
  pluginWindows_.clear();
  // The mapper outlives the editor (it's the processor's): detach our hook.
  processor_.midiMapper.onChanged = nullptr;
  // Tear down the audio settings controller before the listeners it reports to.
  audioSettings_.reset();
  // The tuner and block spectrum analyzers are only useful while the UI is
  // visible; stop feeding them when the editor goes away.
  processor_.setTunerEnabled(false);
  processor_.disableAllBlockSpectrums();
}

juce::RangedAudioParameter* ProcessorBackend::parameter(const juce::String& id) {
  return processor_.parameters.getParameter(id);
}

// Chain state / history
juce::var ProcessorBackend::getChainState(int knownRevision) {
  return processor_.getChainState(knownRevision);
}
juce::uint32 ProcessorBackend::chainRevision() { return processor_.getCurrentChainRevision(); }
bool ProcessorBackend::undoChain() { return processor_.undoChain(); }
bool ProcessorBackend::redoChain() { return processor_.redoChain(); }
bool ProcessorBackend::resetToDefault() { return processor_.resetToDefault(); }

// Chain mutations
std::string ProcessorBackend::loadTone(const juce::String& toneJson,
                                       const std::string& targetInsertId) {
  return processor_.loadTone(toneJson, targetInsertId);
}
juce::var ProcessorBackend::loadLocalTonePath(const juce::File& source,
                                              const std::string& targetInsertId) {
  return processor_.loadLocalTonePath(source, targetInsertId);
}
juce::var ProcessorBackend::loadLocalToneUrls(const juce::Array<juce::URL>& sources,
                                              const std::string& targetInsertId) {
  return processor_.loadLocalToneUrls(sources, targetInsertId);
}
bool ProcessorBackend::swapTone(const std::string& blockId, const juce::String& toneJson) {
  return processor_.swapTone(blockId, toneJson);
}
bool ProcessorBackend::refreshToneMetadata(const juce::String& toneJson) {
  return processor_.refreshToneMetadata(toneJson);
}
bool ProcessorBackend::switchModel(const std::string& blockId, int modelId, const juce::var& model) {
  return processor_.switchModel(blockId, modelId, model);
}
bool ProcessorBackend::retryModelLoad(const std::string& blockId) {
  return processor_.retryModelLoad(blockId);
}
bool ProcessorBackend::removeChainBlock(const std::string& blockId) {
  return processor_.removeChainBlock(blockId);
}
bool ProcessorBackend::reorderChainBlocks(const std::vector<std::string>& newOrder) {
  return processor_.reorderChainBlocks(newOrder);
}
bool ProcessorBackend::moveBlockToChain(const std::string& blockId, const juce::String& side,
                                        int index) {
  return processor_.moveBlockToChain(blockId, side, index);
}
std::string ProcessorBackend::duplicateChainBlock(const std::string& blockId,
                                                  const juce::String& side, int index) {
  return processor_.duplicateChainBlock(blockId, side, index);
}
bool ProcessorBackend::copyChainBlock(const std::string& blockId) {
  return processor_.copyChainBlock(blockId);
}
std::string ProcessorBackend::pasteChainBlock(const juce::String& side, int index) {
  return processor_.pasteChainBlock(side, index);
}
bool ProcessorBackend::swapChains() { return processor_.swapChains(); }
bool ProcessorBackend::setChainBranch(const juce::String& side, const std::string& afterBlockId) {
  return processor_.setChainBranch(side, afterBlockId);
}
bool ProcessorBackend::clearChainBranch() { return processor_.clearChainBranch(); }
void ProcessorBackend::setStereoMode(bool enabled) { processor_.setStereoMode(enabled); }
void ProcessorBackend::setInputMode(const juce::String& mode) {
  processor_.setInputMode(TONE3000Processor::inputModeFromString(mode));
}
void ProcessorBackend::setActiveEditChain(const juce::String& side) {
  processor_.setActiveEditChain(side);
}
void ProcessorBackend::setNamSlimSizeDefault(double slimSize) {
  processor_.setNamSlimSizeDefault(slimSize);
}
void ProcessorBackend::setMultiCore(bool enabled) { processor_.setMultiCoreEnabled(enabled); }
void ProcessorBackend::persistParamAsMachineDefault(const juce::String& id) {
  processor_.persistParameterAsMachineDefault(id);
}

// Per-block params / EQ / spectrum
bool ProcessorBackend::setBlockParam(const std::string& blockId, const juce::String& param,
                                     double value) {
  return processor_.setBlockParam(blockId, param, value);
}
bool ProcessorBackend::setBlockSlimSize(const std::string& blockId, double slimSize) {
  return processor_.setBlockSlimSize(blockId, slimSize);
}
bool ProcessorBackend::setBlockEqBand(const std::string& blockId, int bandIndex,
                                      const juce::var& band) {
  return band.isObject() && processor_.setBlockEqBand(blockId, bandIndex, band);
}
bool ProcessorBackend::setBlockEqEnabled(const std::string& blockId, bool enabled) {
  return processor_.setBlockEqEnabled(blockId, enabled);
}
bool ProcessorBackend::setBlockEqPre(const std::string& blockId, bool pre) {
  return processor_.setBlockEqPre(blockId, pre);
}
bool ProcessorBackend::resetBlockEq(const std::string& blockId) {
  return processor_.resetBlockEq(blockId);
}
bool ProcessorBackend::setBlockSpectrumEnabled(const std::string& blockId, bool enabled) {
  return processor_.setBlockSpectrumEnabled(blockId, enabled);
}
juce::var ProcessorBackend::getBlockSpectrum(const std::string& blockId) {
  return processor_.getBlockSpectrum(blockId);
}

// Presets
juce::var ProcessorBackend::getPresetList() { return processor_.getPresetList(); }
juce::var ProcessorBackend::savePreset(const juce::String& name) {
  return processor_.savePreset(name);
}
bool ProcessorBackend::loadPreset(const juce::String& presetId) {
  return processor_.loadPreset(presetId);
}
bool ProcessorBackend::renamePreset(const juce::String& presetId, const juce::String& newName) {
  return processor_.renamePreset(presetId, newName);
}
bool ProcessorBackend::deletePreset(const juce::String& presetId) {
  return processor_.deletePreset(presetId);
}
bool ProcessorBackend::movePreset(const juce::String& presetId, int delta) {
  return processor_.movePreset(presetId, delta);
}

// Audio device settings (standalone only)
juce::var ProcessorBackend::getAudioDeviceState() {
  return withAudioSettings([](auto& s) { return s.getState(); });
}
juce::var ProcessorBackend::setAudioDeviceType(const juce::String& typeName) {
  return withAudioSettings([&](auto& s) { return s.setDeviceType(typeName); });
}
juce::var ProcessorBackend::setAudioDevice(const juce::String& kind, const juce::String& name) {
  return withAudioSettings([&](auto& s) { return s.setDevice(kind, name); });
}
juce::var ProcessorBackend::setAudioInputChannels(const juce::Array<juce::var>& indices) {
  return withAudioSettings([&](auto& s) { return s.setInputChannels(indices); });
}
juce::var ProcessorBackend::setAudioOutputPair(int pairIndex) {
  return withAudioSettings([&](auto& s) { return s.setOutputPair(pairIndex); });
}
juce::var ProcessorBackend::setAudioSampleRate(double rate) {
  return withAudioSettings([&](auto& s) { return s.setSampleRate(rate); });
}
juce::var ProcessorBackend::setAudioBufferSize(int samples) {
  return withAudioSettings([&](auto& s) { return s.setBufferSize(samples); });
}
juce::var ProcessorBackend::setHearYourself(bool hear) {
  return withAudioSettings([&](auto& s) { return s.setHearYourself(hear); });
}
juce::var ProcessorBackend::playTestTone() {
  return withAudioSettings([](auto& s) { return s.playTestTone(); });
}
juce::var ProcessorBackend::openAudioControlPanel() {
  return withAudioSettings([](auto& s) { return s.openControlPanel(); });
}
juce::var ProcessorBackend::restartAudioDevice() {
  return withAudioSettings([](auto& s) { return s.restartDevice(); });
}
juce::var ProcessorBackend::openMicSettings() {
  return withAudioSettings([](auto& s) { return s.openMicSettings(); });
}
juce::var ProcessorBackend::setMidiInputEnabled(const juce::String& id, bool enabled) {
  return withAudioSettings([&](auto& s) { return s.setMidiInputEnabled(id, enabled); });
}
juce::var ProcessorBackend::openBluetoothMidiPairing() {
  return withAudioSettings([](auto& s) { return s.openBluetoothMidiPairing(); });
}
void ProcessorBackend::setAudioInputMetering(bool enabled) {
  if (audioSettings_ != nullptr) audioSettings_->setInputMetering(enabled);
}
juce::var ProcessorBackend::getAudioInputLevels() {
  return withAudioSettings([](auto& s) { return s.getInputLevels(); });
}

// MIDI mapping
juce::var ProcessorBackend::getMidiMapState() { return processor_.midiMapper.getState(); }
void ProcessorBackend::setMidiChannelFilter(int channel) {
  processor_.midiMapper.setChannelFilter(channel);
}
void ProcessorBackend::startMidiLearn(const juce::String& targetId) {
  processor_.midiMapper.startLearn(targetId);
}
void ProcessorBackend::cancelMidiLearn() { processor_.midiMapper.cancelLearn(); }
bool ProcessorBackend::removeMidiMapping(const juce::String& targetId) {
  return processor_.midiMapper.removeMapping(targetId);
}
bool ProcessorBackend::setMidiCcMapping(const juce::String& targetId, int cc) {
  return processor_.midiMapper.setCcMapping(targetId, cc);
}

// Meters / tuner / auto-measure
juce::var ProcessorBackend::getMeterLevels() { return processor_.getMeterLevels(); }
void ProcessorBackend::setTunerEnabled(bool enabled) { processor_.setTunerEnabled(enabled); }
juce::var ProcessorBackend::getTunerReading() { return processor_.getTunerReading(); }
void ProcessorBackend::startAutoBalance() { processor_.startAutoBalance(); }
void ProcessorBackend::cancelAutoBalance() { processor_.cancelAutoBalance(); }
juce::var ProcessorBackend::pollAutoBalance() { return processor_.pollAutoBalance(); }
void ProcessorBackend::startAutoOffset() { processor_.startAutoOffset(); }
void ProcessorBackend::cancelAutoOffset() { processor_.cancelAutoOffset(); }
juce::var ProcessorBackend::pollAutoOffset() { return processor_.pollAutoOffset(); }

// Misc
juce::String ProcessorBackend::pluginVersion() { return JucePlugin_VersionString; }
juce::String ProcessorBackend::uniqueDeviceId() { return juce::SystemStats::getUniqueDeviceID(); }
void ProcessorBackend::setAccessToken(const juce::String& token) {
  processor_.setAccessToken(token);
}
void ProcessorBackend::copyToClipboard(const juce::String& text) {
  juce::SystemClipboard::copyTextToClipboard(text);
}

bool ProcessorBackend::copyLogs() {
  const juce::File logFile = TONE3000Processor::getLogFile();
  if (!logFile.existsAsFile()) return false;
  // Ship only the tail so we never dump a multi-MB file onto the clipboard.
  juce::String text = logFile.loadFileAsString();
  constexpr int maxChars = 200000;
  if (text.length() > maxChars) text = text.getLastCharacters(maxChars);
  juce::SystemClipboard::copyTextToClipboard(text);
  return true;
}

juce::String ProcessorBackend::revealLogs() {
  const juce::File logFile = TONE3000Processor::getLogFile();
  if (!logFile.existsAsFile()) return {};
  logFile.revealToUser();
  return logFile.getFullPathName();
}

bool ProcessorBackend::canOpenPresetsFolder() {
#if JUCE_IOS
  return false;
#else
  return true;
#endif
}

bool ProcessorBackend::openPresetsFolder() {
  const juce::File dir = processor_.getUserPresetsDir();
  // A fresh install has no folder until the first save; make it so the
  // user lands somewhere they can drop files into.
  if (!dir.isDirectory() && !dir.createDirectory()) return false;
  // startAsProcess opens the folder itself (Finder / Explorer / xdg-open);
  // revealToUser would select it inside its parent instead.
  return dir.startAsProcess();
}

bool ProcessorBackend::canOpenDateTimeSettings() {
#if JUCE_MAC || JUCE_WINDOWS
  return true;
#else
  // No reliable settings URI across Linux desktops; the UI hides the button.
  return false;
#endif
}

bool ProcessorBackend::openDateTimeSettings() {
#if JUCE_MAC
  // Ventura+ pane id; older systems fall back to the settings root.
  return juce::Process::openDocument(
      "x-apple.systempreferences:com.apple.Date-Time-Settings.extension", {});
#elif JUCE_WINDOWS
  return juce::Process::openDocument("ms-settings:dateandtime", {});
#else
  return false;
#endif
}

bool ProcessorBackend::forwardKeyToHost(HostKey key) {
  if (juce::JUCEApplicationBase::isStandaloneApp()) return false;
  if (auto* peer = peerHost_.getPeer())
    HostKeys::forwardKeyToHost(peer->getNativeHandle(),
                               key == HostKey::enter ? HostKeys::HostKey::enter : HostKeys::HostKey::space);
  return true;
}

// Hosted plugins
namespace {

// A native window around a hosted plugin's own editor. It watches the
// processor and closes itself when the block's instance is no longer the one
// it was opened on (block removed, plugin reloaded by undo/preset), so the
// editor never outlives the instance it draws.
class PluginEditorWindow : public juce::DocumentWindow, private juce::Timer {
public:
  PluginEditorWindow(TONE3000Processor& processor, std::string blockId,
                     juce::AudioPluginInstance& instance, std::function<void()> onClose)
      : DocumentWindow(instance.getName(), juce::Colours::black,
                       DocumentWindow::closeButton | DocumentWindow::minimiseButton),
        processor_(processor),
        blockId_(std::move(blockId)),
        instance_(&instance),
        onClose_(std::move(onClose)) {
    setUsingNativeTitleBar(true);
    auto* editor = instance.createEditorIfNeeded();
    if (editor == nullptr)
      editor = new juce::GenericAudioProcessorEditor(instance);
    setContentOwned(editor, true);
    setResizable(editor->isResizable(), false);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
    startTimerHz(4);
  }

  ~PluginEditorWindow() override { clearContentComponent(); }

  void closeButtonPressed() override { close(); }

private:
  void timerCallback() override {
    if (processor_.getHostedPlugin(blockId_) != instance_)
      close();
  }
  void close() {
    stopTimer();
    // Destroys this window; nothing may touch members afterwards.
    juce::MessageManager::callAsync([cb = onClose_] { cb(); });
  }

  TONE3000Processor& processor_;
  std::string blockId_;
  juce::AudioPluginInstance* instance_;
  std::function<void()> onClose_;
};

}  // namespace

bool ProcessorBackend::canHostPlugins() { return PluginHost::get().isAvailable(); }
bool ProcessorBackend::pluginHostingEnabled() { return PluginHost::get().isEnabled(); }
void ProcessorBackend::setPluginHostingEnabled(bool enabled) {
  PluginHost::get().setEnabled(enabled);
}

std::vector<Backend::HostedPluginInfo> ProcessorBackend::hostedPlugins() {
  std::vector<HostedPluginInfo> out;
  for (const auto& d : PluginHost::get().getKnownEffects())
    out.push_back({d.createIdentifierString(), d.name, d.manufacturerName, d.pluginFormatName});
  return out;
}

void ProcessorBackend::scanPlugins(std::function<void(int)> onDone) {
  PluginHost::get().scanAsync(std::move(onDone));
}
bool ProcessorBackend::isScanningPlugins() { return PluginHost::get().isScanning(); }

std::string ProcessorBackend::loadPlugin(const juce::String& identifier,
                                         const std::string& targetInsertId) {
  return processor_.loadPlugin(identifier, targetInsertId);
}

bool ProcessorBackend::openPluginEditor(const std::string& blockId) {
  if (auto it = pluginWindows_.find(blockId); it != pluginWindows_.end()) {
    it->second->toFront(true);
    return true;
  }
  auto* instance = processor_.getHostedPlugin(blockId);
  if (instance == nullptr)
    return false;
  juce::Component::SafePointer<juce::Component> alive(&peerHost_);
  pluginWindows_[blockId] = std::make_unique<PluginEditorWindow>(
      processor_, blockId, *instance, [this, alive, blockId] {
        if (alive != nullptr)
          pluginWindows_.erase(blockId);
      });
  return true;
}

}  // namespace t3k::ui
