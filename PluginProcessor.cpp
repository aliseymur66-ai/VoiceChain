#include "PluginProcessor.h"

using namespace juce;

AudioProcessorValueTreeState::ParameterLayout VoiceChainProcessor::createLayout()
{
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto pct = [&] (const char* id, const char* name, float def)
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                                                            NormalisableRange<float> (0.f, 100.f, 1.f), def));
    };
    auto db = [&] (const char* id, const char* name, float lo, float hi, float def)
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                                                            NormalisableRange<float> (lo, hi, 0.1f), def));
    };

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "voice", 1 }, "Voice",
                                                         StringArray { "Male", "Female" }, 0));
    pct ("warmth", "Warmth", 50.f);
    pct ("presence", "Presence", 50.f);
    pct ("air", "Air", 50.f);
    pct ("comp", "Comp", 50.f);
    pct ("deesser", "De-Esser", 30.f);
    pct ("drive", "Drive", 15.f);

    db ("revMix", "Reverb Mix (dB)", -60.f, 0.f, -12.f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "revSize", 1 }, "Reverb Size",
                                                        NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "revDecay", 1 }, "Reverb Decay (s)",
                                                        NormalisableRange<float> (0.3f, 6.f, 0.1f), 2.0f));

    db ("dlyMix", "Delay Mix (dB)", -60.f, 0.f, -30.f);
    pct ("dlyFb", "Delay Feedback", 40.f);
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "dlyTime", 1 }, "Delay Time",
                                                         StringArray { "1/4", "1/8", "1/16" }, 0));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "pingpong", 1 }, "Ping-pong", true));

    pct ("mix", "Mix", 100.f);
    db ("out", "Out (dB)", -12.f, 12.f, 0.f);

    return { p.begin(), p.end() };
}

VoiceChainProcessor::VoiceChainProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

bool VoiceChainProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == AudioChannelSet::stereo()
        && l.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

void VoiceChainProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    dsp::ProcessSpec spec { sampleRate, (uint32) samplesPerBlock, 2 };

    deHP.prepare (spec);
    deHP.setType (dsp::StateVariableTPTFilterType::highpass);
    deEnv[0] = deEnv[1] = 0.f;

    comp.prepare (spec);
    comp.setAttack (8.f);
    comp.setRelease (120.f);

    lowShelf.prepare (spec);
    presence.prepare (spec);
    airShelf.prepare (spec);
    reverb.prepare (spec);

    rvBuf.setSize (2, samplesPerBlock);
    dryBuf.setSize (2, samplesPerBlock);
    dlyBuf.setSize (2, (int) (4.0 * sampleRate) + 2);
    dlyBuf.clear();
    delayWritePos = 0;
}

void VoiceChainProcessor::processBlock (AudioBuffer<float>& buf, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.getNumSamples();
    if (buf.getNumChannels() < 2 || n > dryBuf.getNumSamples())
        return;

    for (int c = 0; c < 2; ++c)
        dryBuf.copyFrom (c, 0, buf, c, 0, n);

    const bool female = get ("voice") > 0.5f;
    float* ch[2] = { buf.getWritePointer (0), buf.getWritePointer (1) };

    // ---------- 1) De-esser ----------
    const float dePct = get ("deesser");
    if (dePct > 1.f)
    {
        deHP.setCutoffFrequency (female ? 7500.f : 6000.f);
        const float thr = Decibels::decibelsToGain (-10.f - dePct * 0.3f);
        const float rel = std::exp (-1.f / (0.005f * (float) sr));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                const float x = ch[c][i];
                const float hp = deHP.processSample (c, x);
                deEnv[c] = jmax (std::abs (hp), deEnv[c] * rel);
                const float g = deEnv[c] > thr ? thr / deEnv[c] : 1.f;
                ch[c][i] = x - hp * (1.f - g);
            }
    }

    // ---------- 2) Compressor ----------
    {
        const float cp = get ("comp");
        comp.setThreshold (-6.f - cp * 0.3f);
        comp.setRatio (1.5f + cp * 0.04f);
        dsp::AudioBlock<float> blk (buf);
        dsp::ProcessContextReplacing<float> ctx (blk);
        comp.process (ctx);
        buf.applyGain (Decibels::decibelsToGain (cp * 0.06f)); // simple makeup gain
    }

    // ---------- 3) EQ: Warmth / Presence / Air ----------
    {
        *lowShelf.state = *dsp::IIR::Coefficients<float>::makeLowShelf (
            sr, female ? 250.f : 150.f, 0.7f, Decibels::decibelsToGain (get ("warmth") / 100.f * 6.f));
        *presence.state = *dsp::IIR::Coefficients<float>::makePeakFilter (
            sr, female ? 4000.f : 3000.f, 0.8f, Decibels::decibelsToGain (get ("presence") / 100.f * 6.f));
        *airShelf.state = *dsp::IIR::Coefficients<float>::makeHighShelf (
            sr, female ? 12000.f : 10000.f, 0.7f, Decibels::decibelsToGain (get ("air") / 100.f * 8.f));

        dsp::AudioBlock<float> blk (buf);
        dsp::ProcessContextReplacing<float> ctx (blk);
        lowShelf.process (ctx);
        presence.process (ctx);
        airShelf.process (ctx);
    }

    // ---------- 4) Drive (soft saturation) ----------
    const float drv = get ("drive") / 100.f;
    if (drv > 0.001f)
    {
        const float pre = 1.f + drv * 6.f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                ch[c][i] = std::tanh (ch[c][i] * pre) / pre;
    }

    // ---------- 5) Reverb (parallel) ----------
    const float revDb = get ("revMix");
    const float revGain = revDb <= -59.f ? 0.f : Decibels::decibelsToGain (revDb);
    if (revGain > 0.f)
    {
        for (int c = 0; c < 2; ++c)
            rvBuf.copyFrom (c, 0, buf, c, 0, n);

        Reverb::Parameters rp;
        const float size = get ("revSize");
        const float decay = get ("revDecay");
        rp.roomSize = jlimit (0.f, 0.98f, 0.25f * size + 0.75f * (decay - 0.3f) / 5.7f);
        rp.damping = female ? 0.4f : 0.5f;
        rp.wetLevel = 1.f;
        rp.dryLevel = 0.f;
        rp.width = 1.f;
        reverb.setParameters (rp);

        dsp::AudioBlock<float> blk (rvBuf);
        auto sub = blk.getSubBlock (0, (size_t) n);
        dsp::ProcessContextReplacing<float> ctx (sub);
        reverb.process (ctx);
    }

    // ---------- 6) Delay (tempo-synced, ping-pong) + sum ----------
    double bpm = 120.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                bpm = jmax (30.0, *b);

    const int divIdx = (int) get ("dlyTime");
    const double div = divIdx == 0 ? 1.0 : (divIdx == 1 ? 0.5 : 0.25);
    const int size = dlyBuf.getNumSamples();
    const int dSamp = jlimit (1, size - 1, (int) ((60.0 / bpm) * div * sr));
    const float fb = jmin (0.95f, get ("dlyFb") / 100.f);
    const float dlyDb = get ("dlyMix");
    const float dg = dlyDb <= -59.f ? 0.f : Decibels::decibelsToGain (dlyDb);
    const bool ping = get ("pingpong") > 0.5f;

    float* dL = dlyBuf.getWritePointer (0);
    float* dR = dlyBuf.getWritePointer (1);
    const float* rvL = rvBuf.getReadPointer (0);
    const float* rvR = rvBuf.getReadPointer (1);

    for (int i = 0; i < n; ++i)
    {
        const int rp = (delayWritePos - dSamp + size) % size;
        const float yl = dL[rp], yr = dR[rp];
        const float inL = ch[0][i], inR = ch[1][i];

        if (ping)
        {
            dL[delayWritePos] = 0.5f * (inL + inR) + fb * yr;
            dR[delayWritePos] = fb * yl;
        }
        else
        {
            dL[delayWritePos] = inL + fb * yl;
            dR[delayWritePos] = inR + fb * yr;
        }
        delayWritePos = (delayWritePos + 1) % size;

        ch[0][i] = inL + yl * dg + (revGain > 0.f ? rvL[i] * revGain : 0.f);
        ch[1][i] = inR + yr * dg + (revGain > 0.f ? rvR[i] * revGain : 0.f);
    }

    // ---------- 7) Dry/Wet + Output ----------
    const float m = get ("mix") / 100.f;
    const float outG = Decibels::decibelsToGain (get ("out"));
    for (int c = 0; c < 2; ++c)
    {
        const float* d = dryBuf.getReadPointer (c);
        for (int i = 0; i < n; ++i)
            ch[c][i] = (d[i] * (1.f - m) + ch[c][i] * m) * outG;
    }
}

void VoiceChainProcessor::getStateInformation (MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void VoiceChainProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoiceChainProcessor();
}
