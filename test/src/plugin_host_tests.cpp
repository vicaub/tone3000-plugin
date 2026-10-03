// Hosted plugin block tests (plugin/docs/plugin-hosting.md).
//
// A fake in-process AudioPluginInstance (registered through
// PluginHost::registerTestPlugin) stands in for a real VST3/AU so the
// contracts are pinned without anything installed:
//
//   - loadPlugin lands a `plugin` block on the clicked insert and the
//     instance processes the lane's audio,
//   - the instance always runs at the chain base rate (48 kHz), within
//     chainBaseBlockSize frames, at every host rate and oversampling factor,
//   - a mono-only plugin works in a stereo lane,
//   - the plugin's state round-trips through getStateInformation and survives
//     an undo of an edit made in the plugin itself,
//   - a block naming a plugin that isn't available restores as failed,
//     passes audio through, and keeps its stored state for the next save,
//   - duplicate and copy/paste carry the plugin's state to the new block.
#include "PluginHost.h"
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

namespace {

// Gain plugin whose gain is its whole state (one float). Records the rate
// and the largest block it was handed, to pin the island contract.
class FakeGainPlugin : public juce::AudioPluginInstance {
public:
  explicit FakeGainPlugin(bool monoOnly)
      : AudioPluginInstance(monoOnly ? BusesProperties()
                                           .withInput("In", juce::AudioChannelSet::mono())
                                           .withOutput("Out", juce::AudioChannelSet::mono())
                                     : BusesProperties()
                                           .withInput("In", juce::AudioChannelSet::stereo())
                                           .withOutput("Out", juce::AudioChannelSet::stereo())),
        monoOnly(monoOnly) {}

  static std::atomic<double> lastRate;
  static std::atomic<int> maxFramesSeen;
  static std::atomic<int> preparedMaxFrames;

  std::atomic<float> gain{0.5f};

  void fillInPluginDescription(juce::PluginDescription& d) const override {
    d = describe(monoOnly);
  }
  static juce::PluginDescription describe(bool monoOnly) {
    juce::PluginDescription d;
    d.name = monoOnly ? "Fake Mono Gain" : "Fake Gain";
    d.manufacturerName = "Test";
    d.pluginFormatName = "Test";
    d.fileOrIdentifier = monoOnly ? "fake-mono-gain" : "fake-gain";
    d.uniqueId = monoOnly ? 2 : 1;
    d.numInputChannels = d.numOutputChannels = monoOnly ? 1 : 2;
    return d;
  }

  bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
    const auto want = monoOnly ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    return layouts.getMainInputChannelSet() == want && layouts.getMainOutputChannelSet() == want;
  }

  const juce::String getName() const override { return describe(monoOnly).name; }
  void prepareToPlay(double rate, int maxFrames) override {
    lastRate.store(rate);
    preparedMaxFrames.store(maxFrames);
  }
  void releaseResources() override {}
  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
    maxFramesSeen.store(std::max(maxFramesSeen.load(), buffer.getNumSamples()));
    buffer.applyGain(gain.load());
  }
  double getTailLengthSeconds() const override { return 0.0; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  juce::AudioProcessorEditor* createEditor() override { return nullptr; }
  bool hasEditor() const override { return false; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}
  void getStateInformation(juce::MemoryBlock& dest) override {
    const float g = gain.load();
    dest.replaceAll(&g, sizeof(g));
  }
  void setStateInformation(const void* data, int size) override {
    if (size == sizeof(float))
      gain.store(*static_cast<const float*>(data));
  }

private:
  bool monoOnly;
};

std::atomic<double> FakeGainPlugin::lastRate{0.0};
std::atomic<int> FakeGainPlugin::maxFramesSeen{0};
std::atomic<int> FakeGainPlugin::preparedMaxFrames{0};

struct PluginHostFixture : ::testing::Test {
  void SetUp() override {
    for (bool mono : {false, true})
      PluginHost::get().registerTestPlugin(FakeGainPlugin::describe(mono), [mono] {
        return std::make_unique<FakeGainPlugin>(mono);
      });
    FakeGainPlugin::lastRate.store(0.0);
    FakeGainPlugin::maxFramesSeen.store(0);
  }
  void TearDown() override { PluginHost::get().clearTestPlugins(); }

  static juce::String uid(bool mono = false) {
    return FakeGainPlugin::describe(mono).createIdentifierString();
  }
};

void prepare(TONE3000Processor& proc, double rate = kFs) {
  proc.setPlayConfigDetails(2, 2, rate, 512);
  proc.prepareToPlay(rate, 512);
}

juce::String firstInsertId(TONE3000Processor& proc) {
  const juce::var chain = proc.getChainState(-1)["chain"];
  for (const auto& item : *chain.getArray())
    if (item["kind"].toString() == "insert")
      return item["blockId"].toString();
  return {};
}

juce::var findItem(TONE3000Processor& proc, const std::string& id) {
  const juce::var chain = proc.getChainState(-1)["chain"];
  for (const auto& item : *chain.getArray())
    if (item["blockId"].toString() == juce::String(id))
      return item;
  return {};
}

// Settled output / input RMS ratio for a sine through the processor.
float settledGain(TONE3000Processor& proc) {
  const auto in = makeSine(96000, 440.0, 0.25f);
  const auto [l, r] = processStereo(proc, in);
  double num = 0.0, den = 0.0;
  for (size_t i = 48000; i < in.size(); ++i) {
    num += static_cast<double>(l[i]) * l[i];
    den += static_cast<double>(in[i]) * in[i];
  }
  return static_cast<float>(std::sqrt(num / den));
}

TEST_F(PluginHostFixture, LoadPluginLandsOnInsertAndProcessesAudio) {
  ChainTestProcessor proc;
  prepare(proc);
  const auto insert = firstInsertId(proc).toStdString();

  const std::string id = proc.loadPlugin(uid(), insert);
  ASSERT_FALSE(id.empty());
  ASSERT_TRUE(waitForChainLoaded(proc));

  const juce::var item = findItem(proc, id);
  EXPECT_EQ(item["kind"].toString(), "tone");
  EXPECT_EQ(item["tone"]["format"].toString(), "plugin");
  EXPECT_EQ(item["tone"]["title"].toString(), "Fake Gain");
  EXPECT_EQ(item["blockId"].toString(), juce::String(id));
  ASSERT_NE(proc.getHostedPlugin(id), nullptr);

  EXPECT_NEAR(settledGain(proc), 0.5f, 0.02f);
}

TEST_F(PluginHostFixture, RunsAtBaseRateAtEveryHostRate) {
  for (double hostRate : {44100.0, 48000.0, 96000.0}) {
    FakeGainPlugin::maxFramesSeen.store(0);
    ChainTestProcessor proc;
    prepare(proc, hostRate);
    const std::string id = proc.loadPlugin(uid(), firstInsertId(proc).toStdString());
    ASSERT_TRUE(waitForChainLoaded(proc));
    settledGain(proc);
    EXPECT_DOUBLE_EQ(FakeGainPlugin::lastRate.load(), kChainBaseSampleRate) << hostRate;
    EXPECT_LE(FakeGainPlugin::maxFramesSeen.load(), FakeGainPlugin::preparedMaxFrames.load())
        << hostRate << " prepared " << FakeGainPlugin::preparedMaxFrames.load();
    EXPECT_GT(FakeGainPlugin::maxFramesSeen.load(), 0) << hostRate;
  }
}

TEST_F(PluginHostFixture, MonoOnlyPluginWorksInStereoLane) {
  ChainTestProcessor proc;
  prepare(proc);
  ASSERT_FALSE(proc.loadPlugin(uid(true), firstInsertId(proc).toStdString()).empty());
  ASSERT_TRUE(waitForChainLoaded(proc));
  EXPECT_NEAR(settledGain(proc), 0.5f, 0.02f);
}

TEST_F(PluginHostFixture, StateRoundTripsThroughProjectState) {
  juce::MemoryBlock saved;
  {
    ChainTestProcessor proc;
    prepare(proc);
    const std::string id = proc.loadPlugin(uid(), firstInsertId(proc).toStdString());
    ASSERT_TRUE(waitForChainLoaded(proc));
    // An edit made inside the plugin itself (its editor), never seen by us.
    static_cast<FakeGainPlugin*>(proc.getHostedPlugin(id))->gain.store(0.25f);
    proc.getStateInformation(saved);
  }

  ChainTestProcessor restored;
  prepare(restored);
  restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  ASSERT_TRUE(waitForChainLoaded(restored));
  EXPECT_NEAR(settledGain(restored), 0.25f, 0.02f);
}

TEST_F(PluginHostFixture, MissingPluginPassesThroughAndKeepsState) {
  juce::MemoryBlock saved;
  {
    ChainTestProcessor proc;
    prepare(proc);
    const std::string id = proc.loadPlugin(uid(), firstInsertId(proc).toStdString());
    ASSERT_TRUE(waitForChainLoaded(proc));
    static_cast<FakeGainPlugin*>(proc.getHostedPlugin(id))->gain.store(0.25f);
    proc.getStateInformation(saved);
  }

  // Another machine: the plugin isn't installed.
  PluginHost::get().clearTestPlugins();
  juce::MemoryBlock resaved;
  {
    ChainTestProcessor proc;
    prepare(proc);
    proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    const auto deadline = juce::Time::getMillisecondCounter() + 10000;
    juce::var item;
    while (juce::Time::getMillisecondCounter() < deadline) {
      item = proc.getChainState(-1)["chain"][0];
      if (static_cast<bool>(item["loadFailed"]))
        break;
      juce::Thread::sleep(20);
    }
    EXPECT_TRUE(static_cast<bool>(item["loadFailed"]));
    EXPECT_EQ(item["tone"]["title"].toString(), "Fake Gain");
    while (proc.isChainEditFadeHeld())
      juce::Thread::sleep(20);
    EXPECT_NEAR(settledGain(proc), 1.0f, 0.02f) << "a missing plugin must pass audio through";
    proc.getStateInformation(resaved);
  }

  // Back on a machine that has it: the original setting is still there.
  SetUp();
  ChainTestProcessor proc;
  prepare(proc);
  proc.setStateInformation(resaved.getData(), static_cast<int>(resaved.getSize()));
  ASSERT_TRUE(waitForChainLoaded(proc));
  EXPECT_NEAR(settledGain(proc), 0.25f, 0.02f);
}

TEST_F(PluginHostFixture, UndoRestoresPluginStateInPlace) {
  ChainTestProcessor proc;
  prepare(proc);
  const std::string id = proc.loadPlugin(uid(), firstInsertId(proc).toStdString());
  ASSERT_TRUE(waitForChainLoaded(proc));
  auto* instance = proc.getHostedPlugin(id);

  // Snapshot (gain 0.5), plugin-side edit, then a chain edit and its undo.
  letAudioGoIdle();
  ASSERT_TRUE(proc.setBlockParam(id, "mix", 0.9));
  static_cast<FakeGainPlugin*>(instance)->gain.store(0.125f);
  ASSERT_TRUE(proc.undoChain());

  EXPECT_EQ(proc.getHostedPlugin(id), instance) << "undo must reuse the live instance";
  EXPECT_FLOAT_EQ(static_cast<FakeGainPlugin*>(instance)->gain.load(), 0.5f);
}

TEST_F(PluginHostFixture, DuplicateAndPasteCarryPluginState) {
  ChainTestProcessor proc;
  prepare(proc);
  const std::string id = proc.loadPlugin(uid(), firstInsertId(proc).toStdString());
  ASSERT_TRUE(waitForChainLoaded(proc));
  static_cast<FakeGainPlugin*>(proc.getHostedPlugin(id))->gain.store(0.3f);

  letAudioGoIdle();
  const std::string dup = proc.duplicateChainBlock(id, "left", 1);
  ASSERT_FALSE(dup.empty());
  ASSERT_TRUE(proc.copyChainBlock(id));
  letAudioGoIdle();
  const std::string pasted = proc.pasteChainBlock("left", 2);
  ASSERT_FALSE(pasted.empty());
  ASSERT_TRUE(waitForChainLoaded(proc));

  for (const auto& clone : {dup, pasted}) {
    auto* instance = proc.getHostedPlugin(clone);
    ASSERT_NE(instance, nullptr) << clone;
    EXPECT_NE(instance, proc.getHostedPlugin(id)) << "clones get their own instance";
    EXPECT_FLOAT_EQ(static_cast<FakeGainPlugin*>(instance)->gain.load(), 0.3f) << clone;
  }
}

}  // namespace
