#include "audioplayerbackend.h"

#include "audioutils.h"
#include "playerlogger.h"

AudioPlayerBackend::PlaybackState AudioPlayerBackend::playbackState() const
{
    return m_playbackState;
}

void AudioPlayerBackend::setPlaybackState(PlaybackState state)
{
    if (m_playbackState == state) {
        return;
    }

    const PlaybackState oldState = m_playbackState;
    m_playbackState = state;
    logPlaybackStateChange(oldState, state);
    emit playbackStateChanged(state);
}

void AudioPlayerBackend::emitAudioLevels(qreal leftLevel, qreal rightLevel)
{
    if (qFuzzyCompare(m_lastLeftLevel, leftLevel) &&
        qFuzzyCompare(m_lastRightLevel, rightLevel)) {
        return;
    }
    m_lastLeftLevel = leftLevel;
    m_lastRightLevel = rightLevel;
    emit audioLevelsChanged(leftLevel, rightLevel);
}

void AudioPlayerBackend::logPlaybackStateChange(PlaybackState from, PlaybackState to)
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("setPlaybackState %1 -> %2")
                          .arg(AudioUtils::playbackStateName(from))
                          .arg(AudioUtils::playbackStateName(to)));
}
