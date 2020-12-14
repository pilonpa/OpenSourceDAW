/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion_engine
{


//==============================================================================
//==============================================================================
struct SpeedRampWaveNode::PerChannelState
{
    PerChannelState()    { resampler.reset(); }
    
    juce::LagrangeInterpolator resampler;
    float lastSample = 0;
};


//==============================================================================
SpeedRampWaveNode::SpeedRampWaveNode (const AudioFile& af,
                                      EditTimeRange editTime,
                                      double off,
                                      EditTimeRange loop,
                                      LiveClipLevel level,
                                      double speed,
                                      const juce::AudioChannelSet& channelSetToUse,
                                      const juce::AudioChannelSet& destChannelsToFill,
                                      ProcessState& ps,
                                      EditItemID itemIDToUse,
                                      bool isRendering,
                                      SpeedFadeDescription speedFadeDescriptionToUse)
   : TracktionEngineNode (ps),
     editPosition (editTime),
     loopSection (loop.getStart() * speed, loop.getEnd() * speed),
     offset (off),
     originalSpeedRatio (speed),
     editItemID (itemIDToUse),
     isOfflineRender (isRendering),
     speedFadeDescription (speedFadeDescriptionToUse),
     audioFile (af),
     clipLevel (level),
     channelsToUse (channelSetToUse),
     destChannels (destChannelsToFill)
{
    // Both ramp times should not be empty!
    assert ((! speedFadeDescription.inTimeRange.isEmpty())
            || (! speedFadeDescription.outTimeRange.isEmpty()));
}

tracktion_graph::NodeProperties SpeedRampWaveNode::getNodeProperties()
{
    tracktion_graph::NodeProperties props;
    props.hasAudio = true;
    props.hasMidi = false;
    props.numberOfChannels = destChannels.size();
    props.nodeID = (size_t) editItemID.getRawID();

    return props;
}

void SpeedRampWaveNode::prepareToPlay (const tracktion_graph::PlaybackInitialisationInfo& info)
{
    reader = audioFile.engine->getAudioFileManager().cache.createReader (audioFile);
    outputSampleRate = info.sampleRate;
    editPositionInSamples = tracktion_graph::timeToSample ({ editPosition.start, editPosition.end }, outputSampleRate);
    updateFileSampleRate();

    channelState.clear();

    if (reader != nullptr)
        for (int i = std::max (channelsToUse.size(), reader->getNumChannels()); --i >= 0;)
            channelState.add (new PerChannelState());
}

bool SpeedRampWaveNode::isReadyToProcess()
{
    // Only check this whilst rendering or it will block whilst the proxies are being created
    if (! isOfflineRender)
        return true;
    
    // If the hash is 0 it means an empty file path which means a missing file so
    // this will never return a valid reader and we should just bail
    if (audioFile.isNull())
        return true;

    if (reader == nullptr)
    {
        reader = audioFile.engine->getAudioFileManager().cache.createReader (audioFile);

        if (reader == nullptr)
            return false;
    }

    if (audioFileSampleRate == 0.0 && ! updateFileSampleRate())
        return false;

    return true;
}

void SpeedRampWaveNode::process (ProcessContext& pc)
{
    SCOPED_REALTIME_CHECK
    assert (outputSampleRate == getSampleRate());

    //TODO: Might get a performance boost by pre-setting the file position in prepareForNextBlock
    processSection (pc, getTimelineSampleRange());
}

//==============================================================================
int64_t SpeedRampWaveNode::editTimeToFileSample (double editTime) const noexcept
{
    editTime = juce::jlimit (speedFadeDescription.inTimeRange.getStart(),
                             speedFadeDescription.outTimeRange.getEnd(),
                             editTime);

    if (! speedFadeDescription.inTimeRange.isEmpty()
        && speedFadeDescription.inTimeRange.containsInclusive (editTime))
    {
        const double timeFromStart = editTime - speedFadeDescription.inTimeRange.getStart();
        const double proportionOfFade = timeFromStart / speedFadeDescription.inTimeRange.getLength();
        const double rescaledProportion = rescale (speedFadeDescription.fadeInType, proportionOfFade, true);

        editTime = speedFadeDescription.inTimeRange.getStart()
                    + (rescaledProportion * speedFadeDescription.inTimeRange.getLength());

        jassert (speedFadeDescription.inTimeRange.containsInclusive (editTime));
    }
    else if (! speedFadeDescription.outTimeRange.isEmpty()
             && speedFadeDescription.outTimeRange.containsInclusive (editTime))
    {
        const double timeFromStart = editTime - speedFadeDescription.outTimeRange.getStart();
        const double proportionOfFade = timeFromStart / speedFadeDescription.outTimeRange.getLength();
        const double rescaledProportion = rescale (speedFadeDescription.fadeOutType, proportionOfFade, false);

        editTime = speedFadeDescription.outTimeRange.getStart()
                    + (rescaledProportion * speedFadeDescription.outTimeRange.getLength());
        
        jassert (speedFadeDescription.outTimeRange.containsInclusive (editTime));
    }
        
    return (int64_t) ((editTime - (editPosition.getStart() - offset))
                       * originalSpeedRatio * audioFileSampleRate + 0.5);
}

bool SpeedRampWaveNode::updateFileSampleRate()
{
    using namespace tracktion_graph;
    
    if (reader == nullptr)
        return false;

    audioFileSampleRate = reader->getSampleRate();

    if (audioFileSampleRate <= 0)
        return false;
    
    if (! loopSection.isEmpty())
        reader->setLoopRange ({ timeToSample (loopSection.getStart(), audioFileSampleRate),
                                timeToSample (loopSection.getEnd(), audioFileSampleRate) });

    return true;
}

void SpeedRampWaveNode::processSection (ProcessContext& pc, juce::Range<int64_t> timelineRange)
{
    const auto sectionEditTime = tracktion_graph::sampleToTime (timelineRange, outputSampleRate);
    
    if (reader == nullptr
         || sectionEditTime.getEnd() <= editPosition.getStart()
         || sectionEditTime.getStart() >= editPosition.getEnd())
        return;

    SCOPED_REALTIME_CHECK

    if (audioFileSampleRate == 0.0 && ! updateFileSampleRate())
        return;

    const auto fileStart       = editTimeToFileSample (sectionEditTime.getStart());
    const auto fileEnd         = editTimeToFileSample (sectionEditTime.getEnd());
    const auto numFileSamples  = (int) (fileEnd - fileStart);
    
    if (numFileSamples <= 3)
    {
        playedLastBlock = false;
        return;
    }
    
    reader->setReadPosition (fileStart);

    auto destBuffer = pc.buffers.audio;
    const int numSamples = (int) destBuffer.getNumFrames();
    const auto destBufferChannels = juce::AudioChannelSet::canonicalChannelSet ((int) destBuffer.getNumChannels());
    const int numChannels = destBufferChannels.size();
    assert ((int) pc.buffers.audio.getNumChannels() == numChannels);

    AudioScratchBuffer fileData (numChannels, numFileSamples + 2);

    int lastSampleFadeLength = 0;

    {
        SCOPED_REALTIME_CHECK

        if (reader->readSamples (numFileSamples + 2, fileData.buffer, destBufferChannels, 0,
                                 channelsToUse,
                                 isOfflineRender ? 5000 : 3))
        {
            if (! getPlayHeadState().isContiguousWithPreviousBlock() && ! getPlayHeadState().isFirstBlockOfLoop())
                lastSampleFadeLength = std::min (numSamples, getPlayHead().isUserDragging() ? 40 : 10);
        }
        else
        {
            lastSampleFadeLength = std::min (numSamples, 40);
            fileData.buffer.clear();
        }
    }

    float gains[2];

    // For stereo, use the pan, otherwise ignore it
    if (numChannels == 2)
        clipLevel.getLeftAndRightGains (gains[0], gains[1]);
    else
        gains[0] = gains[1] = clipLevel.getGainIncludingMute();

    if (getPlayHead().isUserDragging())
    {
        gains[0] *= 0.4f;
        gains[1] *= 0.4f;
    }

    auto ratio = numFileSamples / (double) numSamples;

    if (ratio <= 0.0)
        return;
    
    jassert (numChannels <= channelState.size()); // this should always have been made big enough

    for (int channel = 0; channel < numChannels; ++channel)
    {
        if (channel < channelState.size())
        {
            const auto src = fileData.buffer.getReadPointer (channel);
            const auto dest = destBuffer.getIterator ((choc::buffer::ChannelCount) channel).sample;

            auto& state = *channelState.getUnchecked (channel);
            state.resampler.processAdding (ratio, src, dest, numSamples, gains[channel & 1]);

            if (lastSampleFadeLength > 0)
            {
                for (int i = 0; i < lastSampleFadeLength; ++i)
                {
                    auto alpha = i / (float) lastSampleFadeLength;
                    dest[i] = alpha * dest[i] + state.lastSample * (1.0f - alpha);
                }
            }

            state.lastSample = dest[numSamples - 1];
        }
        else
        {
            destBuffer.getChannelRange (tracktion_graph::channelRangeWithStartAndLength ((choc::buffer::ChannelCount) channel, 1)).clear();
        }
    }
    
    // If the ratio goes below 0.05, this will be too low to hear so fade out the block if it was played
    // If this is the first block, fade it in
    if (ratio < 0.05)
    {
        if (! playedLastBlock)
        {
            destBuffer.clear();
            return;
        }

        auto bufferRef = tracktion_graph::createAudioBuffer (destBuffer);
        bufferRef.applyGainRamp (0, bufferRef.getNumSamples(),
                                 1.0f, 0.0f);
        
        playedLastBlock = false;
        return;
    }
    else
    {
        if (! playedLastBlock)
        {
            auto bufferRef = tracktion_graph::createAudioBuffer (destBuffer);
            bufferRef.applyGainRamp (0, bufferRef.getNumSamples(),
                                     0.0f, 1.0f);
        }
        
        playedLastBlock = true;
    }

    
    // Silence any samples before or after our edit time range
    // N.B. this shouldn't happen when using a clip combiner as the times should be clipped correctly
    {
        const int64_t numSamplesToClearAtStart = editPositionInSamples.getStart() - timelineRange.getStart();
        const int64_t numSamplesToClearAtEnd = timelineRange.getEnd() - editPositionInSamples.getEnd();

        if (numSamplesToClearAtStart > 0)
            destBuffer.getStart ((choc::buffer::FrameCount) numSamplesToClearAtStart).clear();

        if (numSamplesToClearAtEnd > 0)
            destBuffer.getFrameRange (tracktion_graph::frameRangeWithStartAndLength ((choc::buffer::FrameCount) (numSamples - numSamplesToClearAtEnd), (choc::buffer::FrameCount) numSamplesToClearAtEnd)).clear();
    }
}


//==============================================================================
double SpeedRampWaveNode::rescale (AudioFadeCurve::Type t, double proportion, bool rampUp)
{
    switch (t)
    {
        case AudioFadeCurve::convex:
            return rampUp ? (-2.0 * std::cos ((juce::double_Pi * proportion) / 2.0)) / juce::double_Pi + 1.0
                          : 1.0 - ((-2.0 * std::cos ((juce::double_Pi * (proportion - 1.0)) / 2.0)) / juce::double_Pi + 1.0);

        case AudioFadeCurve::concave:
            return rampUp ? proportion - (2.0 * std::sin ((juce::double_Pi * proportion) / 2.0)) / juce::double_Pi + (2.0 / juce::double_Pi)
                          : ((2.0 * std::sin ((juce::double_Pi * (proportion + 1.0)) / 2.0)) / juce::double_Pi) + proportion - (2.0 / juce::double_Pi);

        case AudioFadeCurve::sCurve:
            return rampUp ? (proportion / 2.0) - (std::sin (juce::double_Pi * proportion) / (2.0 * juce::double_Pi)) + 0.5
                          : std::sin (juce::double_Pi * proportion) / (2.0 * juce::double_Pi) + (proportion / 2.0);

        case AudioFadeCurve::linear:
        default:
            return rampUp ? (juce::square (proportion) * 0.5) + 0.5
                          : ((-juce::square (proportion - 1.0)) * 0.5) + 0.5;
    }
}

}