// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

namespace Libraries::AudioOut {

struct PortOut;

/// Speaker layout the game's 7.1 mix is sent to the sound device in (set from config.cfg `speakers`
/// before any port opens): 0 automatic (the device's channel count), 1 stereo, 2 5.1, 3 7.1.
void SetSpeakerMode(int mode);

/// The channel count sceAudioOutGetPortState reports for a port that carries `port_channels` channels. The game
/// mixes for what it is told the output is: a 7.1 port reported as stereo only gets front left and right.
/// Surround (5.1 or 7.1, set or the device's) is reported as 8, because the game's mix is 7.1 and is folded afterwards.
int ReportedOutputChannels(int port_channels);

class PortBackend {
public:
    virtual ~PortBackend() = default;

    /// Guaranteed to be called in intervals of at least port buffer time,
    /// with size equal to port buffer size.
    virtual void Output(void* ptr) = 0;

    virtual void SetVolume(const std::array<int, 8>& ch_volumes) = 0;
};

class AudioOutBackend {
public:
    AudioOutBackend() = default;
    virtual ~AudioOutBackend() = default;

    virtual std::unique_ptr<PortBackend> Open(PortOut& port) = 0;
};

class SDLAudioOut final : public AudioOutBackend {
public:
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
};

} // namespace Libraries::AudioOut
