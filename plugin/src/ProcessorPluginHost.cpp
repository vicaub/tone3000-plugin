// Hosted third-party plugin blocks. Design: plugin/docs/plugin-hosting.md.
#include "Processor.h"
#include "PluginHost.h"

namespace {

juce::var makePluginTone(const juce::PluginDescription& desc) {
  juce::DynamicObject::Ptr plugin = new juce::DynamicObject();
  plugin->setProperty("name", desc.name);
  plugin->setProperty("vendor", desc.manufacturerName);
  plugin->setProperty("format", desc.pluginFormatName);
  plugin->setProperty("uid", desc.createIdentifierString());
  plugin->setProperty("description", PluginHost::descriptionToString(desc));

  juce::DynamicObject::Ptr tone = new juce::DynamicObject();
  tone->setProperty("id", 0);
  tone->setProperty("title", desc.name);
  tone->setProperty("format", "plugin");
  tone->setProperty("plugin", juce::var(plugin.get()));
  return juce::var(tone.get());
}

// The description stored with the block (survives a rescan or another
// machine), else whatever the current scan knows under the same identifier.
bool resolveDescription(const juce::var& toneVar, juce::PluginDescription& out) {
  const juce::var plugin = toneVar["plugin"];
  if (PluginHost::get().findByIdentifier(plugin["uid"].toString(), out))
    return true;
  return PluginHost::descriptionFromString(plugin["description"].toString(), out);
}

}  // namespace

std::string TONE3000Processor::loadPlugin(const juce::String& identifier,
                                          const std::string& targetInsertId) {
  juce::PluginDescription desc;
  if (!PluginHost::get().findByIdentifier(identifier, desc)) {
    juce::Logger::writeToLog("[PluginHost] loadPlugin: unknown plugin " + identifier);
    return {};
  }

  juce::ScopedLock lock(chainMutex);
  pushChainHistory();

  const std::string blockId = juce::Uuid().toString().toStdString();
  auto block = std::make_unique<ChainBlock>(blockId, ChainBlockType::PLUGIN);
  const juce::var tone = makePluginTone(desc);
  setToneOnBlock(*block, 0, juce::JSON::toString(tone), tone);
  block->loaded = false;
  block->modelLoading = true;

  placeInInsertSlot(std::move(block), targetInsertId);
  bumpChainRevision();
  queuePluginLoad(blockId);
  return blockId;
}

juce::AudioPluginInstance* TONE3000Processor::getHostedPlugin(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::PLUGIN)
    return nullptr;
  return block->pluginInstance.get();
}

void TONE3000Processor::queuePluginLoad(const std::string& blockId) {
  loadingThreadPool.addJob(
      std::function<void()>([this, blockId] { loadPluginInBackground(blockId); }));
}

juce::MemoryBlock TONE3000Processor::currentPluginState(const ChainBlock& block) {
  if (block.pluginInstance == nullptr || !block.loaded)
    return block.pluginState;
  juce::MemoryBlock state;
  block.pluginInstance->getStateInformation(state);
  return state;
}

void TONE3000Processor::loadPluginInBackground(const std::string& blockId) {
  juce::var toneVar;
  juce::MemoryBlock state;
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || block->type != ChainBlockType::PLUGIN)
      return;
    toneVar = block->toneVar;
    state = block->pluginState;
  }

  juce::PluginDescription desc;
  juce::String error;
  std::unique_ptr<juce::AudioPluginInstance> instance;
  const int maxFrames = juce::jmax(1, chainBaseBlockSize());
  if (!resolveDescription(toneVar, desc))
    error = "Plugin description missing";
  else
    instance = PluginHost::get().createInstance(desc, kChainBaseSampleRate, maxFrames, error);

  if (instance != nullptr && state.getSize() > 0)
    instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));

  if (instance == nullptr)
    juce::Logger::writeToLog("[PluginHost] Block " + juce::String(blockId) + ": " +
                             toneVar["title"].toString() + " not loaded (" + error + ")");

  requestSwapFadeAndWait(blockId, instance != nullptr);

  std::unique_ptr<juce::AudioPluginInstance> retired;
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      retired = std::move(instance);
    } else {
      bumpChainRevision();
      block->modelLoading = false;
      block->swapFadePending.store(false);
      block->swapFadeDone.store(false);
      block->swapMuteWet.store(false);

      if (instance == nullptr) {
        block->loaded = false;
        block->loadFailed = true;
      } else {
        const int channels = juce::jmax(2, instance->getTotalNumInputChannels(),
                                        instance->getTotalNumOutputChannels());
        block->pluginScratch.setSize(channels, maxFrames, false, true, false);
        block->pluginTailSeconds = instance->getTailLengthSeconds();
        block->irBaseRateIsland.prepare(chainOversampleFactor.load(), maxFrames);
        retired = std::move(block->pluginInstance);
        block->pluginInstance = std::move(instance);
        block->loaded = true;
        block->loadFailed = false;
      }
    }
    refreshIrTailLength();
  }
  // `retired` (the previous instance, if any) is destroyed here, unlocked.
}
