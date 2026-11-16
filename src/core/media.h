// Recognises the audio files Cairn will embed, from their first bytes (never from the file name).
#pragma once

#include <string>

namespace cairn {

struct AudioKind {
  std::string ext;   // "wav", "mp3", "m4a", "wma", "mid": safe to use in a file name
  std::string mime;  // "audio/wav", ...
};

// True if `bytes` starts like a WAV, MP3, M4A, WMA or MIDI file. Anything else (including an empty or
// tiny buffer) is refused, so a renamed text file or executable is never stored as audio.
bool sniffAudio(const std::string& bytes, AudioKind& out);

}  // namespace cairn
