// Registry-backed settings, translated into an engine Config on demand.
// The engine knows nothing about persistence; this is the only place that
// does. HKCU\Software\ac3spdif is the Windows spelling of UserDefaults.

#pragma once

#include "../engine/Config.h"

#include <string>

namespace ac3spdif {
namespace Settings {

// Empty string means "the default output device" for input, and "pick the
// best bitstream-capable device automatically" for output. Devices are stored
// by endpoint ID, which survives renames and replugs; the menus display names.
std::string inputDevice();
void setInputDevice(const std::string& value);
std::string outputDevice();
void setOutputDevice(const std::string& value);

// 0 = match the mix format.
int channelCount();
void setChannelCount(int value);

std::string bitrate();
void setBitrate(const std::string& value);

OutputCodec codec();
void setCodec(OutputCodec value);

CarrierMode carrier();
void setCarrier(CarrierMode value);

double sampleRate();
void setSampleRate(double value);

// PCM margin ahead of the encoder, in milliseconds. Stored as the number
// rather than as one of a handful of named presets: the useful range depends
// on the hardware, so there is no set of names right for every device. A new
// value name, not the old prebufferBursts one, so a burst count left by an
// earlier version is never read as milliseconds.
int bufferMs();
void setBufferMs(int value);

bool selfTest();
void setSelfTest(bool value);

bool autoStartOnLaunch();
void setAutoStartOnLaunch(bool value);

Config makeConfig();

}  // namespace Settings
}  // namespace ac3spdif
