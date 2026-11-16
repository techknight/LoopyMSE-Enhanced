#pragma once

// Autodrive: a cooperating cartridge publishes its mouse cursor (and where it
// wants it to go) through spare BG registers, and the emulator moves the
// emulated XN-110 mouse there frame by frame. The protocol is documented in
// docs/homebrew-debug-interface.md.
namespace Autodrive
{

void set_enabled(bool enabled);
bool is_enabled();
void run_frame();
void reset();

// Chase this game-pixel position instead of the cartridge's own target,
// until the cursor gets there. Returns false while no cartridge is
// publishing its cursor.
bool set_target(int x, int y);

}  // namespace Autodrive
