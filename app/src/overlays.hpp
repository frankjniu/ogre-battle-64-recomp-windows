#pragma once

#include "recomp.h"

namespace ogre {

// Registers the base code sections (entry + main) from recomp_overlays.inl.
// Must be called before recomp::start().
void register_base_overlays();

// Registers the recompiled boot streamed overlays (A/B/C) at their fixed load
// addresses. Must be called after init_overlays() has run, i.e. from the
// GameEntry on_init_callback.
void register_streamed_overlays();

// Visits every function of the main ELF's code sections with its RAM address,
// ROM offset, ROM size and body (from recomp_overlays.inl).
void for_each_main_function(void (*visit)(uint32_t ram, uint32_t rom, uint32_t size,
                                          recomp_func_t* func));

}  // namespace ogre
