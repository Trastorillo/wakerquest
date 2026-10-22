package io.github.wakerquest;

import dev.encounter.borealis.BorealisActivity;

// SDL's activity through Borealis (immersive mode, surface lifecycle, launch arguments): it loads
// libmain.so and runs the game's SDL_main. The data (GZLE01.iso, user/, logs/) lives in
// getExternalFilesDir(null), which the native side asks SDL for (pc_harness.cpp).
public class WakerQuestActivity extends BorealisActivity {
}
