#include "PluginHost.h"

namespace {

constexpr const char* kEnabledKey = "thirdPartyPluginsEnabled";

// Same folder as the shared settings file (see userSettingsOptions in
// ProcessorState.cpp): ~/Library/Application Support/TONE3000 on macOS,
// %APPDATA%/TONE3000 on Windows, the XDG config dir on Linux.
juce::File appDataDir() {
#if JUCE_MAC
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Application Support")
      .getChildFile("TONE3000");
#else
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("TONE3000");
#endif
}

juce::PropertiesFile::Options hostSettingsOptions() {
  juce::PropertiesFile::Options options;
  // Own file: two PropertiesFile instances must never share one (see
  // userSettingsOptions in ProcessorState.cpp).
  options.applicationName = "plugin-hosting";
  options.filenameSuffix = ".settings";
  options.folderName = appDataDir().getFullPathName();
  return options;
}

}  // namespace

PluginHost& PluginHost::get() {
  static PluginHost instance;
  return instance;
}

PluginHost::PluginHost() {
#if JUCE_PLUGINHOST_VST3 || JUCE_PLUGINHOST_AU
  juce::addDefaultFormatsToManager(formatManager);
#endif
  enabled.store(juce::PropertiesFile(hostSettingsOptions()).getBoolValue(kEnabledKey, false));
  loadListFromDisk();
}

bool PluginHost::isAvailable() const {
  return formatManager.getNumFormats() > 0;
}

void PluginHost::setEnabled(bool on, bool persist) {
  enabled.store(on);
  if (!persist)
    return;
  juce::PropertiesFile settings(hostSettingsOptions());
  settings.setValue(kEnabledKey, on);
  if (!settings.saveIfNeeded())
    juce::Logger::writeToLog("[PluginHost] Couldn't write " + settings.getFile().getFullPathName());
}

juce::File PluginHost::listFile() {
  return appDataDir().getChildFile("hosted-plugins.xml");
}

juce::File PluginHost::deadMansPedalFile() {
  return appDataDir().getChildFile("hosted-plugins-scanning.txt");
}

void PluginHost::loadListFromDisk() {
  if (auto xml = juce::XmlDocument::parse(listFile())) {
    const juce::ScopedLock lock(listLock);
    knownList.recreateFromXml(*xml);
  }
}

void PluginHost::saveListToDisk() const {
  std::unique_ptr<juce::XmlElement> xml;
  {
    const juce::ScopedLock lock(listLock);
    xml = knownList.createXml();
  }
  if (xml == nullptr)
    return;
  listFile().getParentDirectory().createDirectory();
  if (!xml->writeTo(listFile()))
    juce::Logger::writeToLog("[PluginHost] Couldn't write " + listFile().getFullPathName());
}

juce::Array<juce::PluginDescription> PluginHost::getKnownEffects() const {
  juce::Array<juce::PluginDescription> out;
  {
    const juce::ScopedLock lock(listLock);
    for (const auto& d : knownList.getTypes())
      if (!d.isInstrument)
        out.add(d);
  }
  for (const auto& t : testPlugins)
    out.add(t.desc);
  std::sort(out.begin(), out.end(),
            [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
              return a.name.compareNatural(b.name) < 0;
            });
  return out;
}

bool PluginHost::findByIdentifier(const juce::String& identifier,
                                  juce::PluginDescription& out) const {
  for (const auto& t : testPlugins)
    if (t.desc.createIdentifierString() == identifier) {
      out = t.desc;
      return true;
    }
  const juce::ScopedLock lock(listLock);
  if (auto found = knownList.getTypeForIdentifierString(identifier)) {
    out = *found;
    return true;
  }
  return false;
}

void PluginHost::scanAsync(std::function<void(int)> onDone) {
  if (scanning.exchange(true))
    return;

  std::thread([this, onDone = std::move(onDone)] {
    juce::Logger::writeToLog("[PluginHost] Scan started");
    for (int i = 0; i < formatManager.getNumFormats(); ++i) {
      auto* format = formatManager.getFormat(i);
      juce::KnownPluginList scratch;
      {
        // Seed with what we already know so unchanged files aren't reloaded.
        const juce::ScopedLock lock(listLock);
        for (const auto& d : knownList.getTypes())
          scratch.addType(d);
        for (const auto& f : knownList.getBlacklistedFiles())
          scratch.addToBlacklist(f);
      }
      juce::PluginDirectoryScanner scanner(scratch, *format, format->getDefaultLocationsToSearch(),
                                           true, deadMansPedalFile(), true);
      juce::String name;
      while (scanner.scanNextFile(true, name)) {
      }
      for (const auto& failed : scanner.getFailedFiles())
        juce::Logger::writeToLog("[PluginHost] Scan failed: " + failed);

      const juce::ScopedLock lock(listLock);
      for (const auto& d : scratch.getTypes())
        knownList.addType(d);
      for (const auto& f : scratch.getBlacklistedFiles())
        knownList.addToBlacklist(f);
    }
    saveListToDisk();
    const int count = getKnownEffects().size();
    juce::Logger::writeToLog("[PluginHost] Scan done: " + juce::String(count) + " effects");
    scanning.store(false);
    if (onDone)
      juce::MessageManager::callAsync([onDone, count] { onDone(count); });
  }).detach();
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance(
    const juce::PluginDescription& desc, double sampleRate, int maxBlockSize, juce::String& error) {
  std::unique_ptr<juce::AudioPluginInstance> instance;

  for (const auto& t : testPlugins)
    if (t.desc.createIdentifierString() == desc.createIdentifierString())
      instance = t.factory();

  if (instance == nullptr) {
    if (!enabled.load()) {
      error = "Third-party plugins are turned off";
      return nullptr;
    }
    instance = formatManager.createPluginInstance(desc, sampleRate, maxBlockSize, error);
  }
  if (instance == nullptr) {
    if (error.isEmpty())
      error = "Plugin not found";
    return nullptr;
  }

  // Effects only: stereo in/out when the plugin takes it, else mono/mono.
  // The chain feeds 1 or 2 channels; the block adapts in the audio path.
  using CS = juce::AudioChannelSet;
  juce::AudioProcessor::BusesLayout stereo, mono;
  stereo.inputBuses.add(CS::stereo());
  stereo.outputBuses.add(CS::stereo());
  mono.inputBuses.add(CS::mono());
  mono.outputBuses.add(CS::mono());
  if (!instance->setBusesLayout(stereo) && !instance->setBusesLayout(mono)) {
    // Keep whatever main layout it defaults to; the audio path clamps the
    // channel count to what the instance actually exposes.
    juce::Logger::writeToLog("[PluginHost] " + desc.name +
                             ": no stereo/mono layout accepted, using its default");
  }
  instance->setNonRealtime(false);
  instance->prepareToPlay(sampleRate, maxBlockSize);
  return instance;
}

juce::String PluginHost::descriptionToString(const juce::PluginDescription& desc) {
  if (auto xml = desc.createXml())
    return xml->toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
  return {};
}

bool PluginHost::descriptionFromString(const juce::String& xml, juce::PluginDescription& out) {
  if (auto el = juce::parseXML(xml))
    return out.loadFromXml(*el);
  return false;
}

void PluginHost::registerTestPlugin(const juce::PluginDescription& desc, TestFactory factory) {
  testPlugins.push_back({desc, std::move(factory)});
}

void PluginHost::clearTestPlugins() {
  testPlugins.clear();
}
