#pragma once

#include <cstdint>
#include <string>

// Item names from the game's own message archive, English as the fallback.
namespace twili::ui {

// Game: the disc's message; Mod: our name for a randomizer item or, on English discs, one the game
// names poorly; Fallback: our English table (no usable name on the disc); Unknown: an id nothing
// uses.
enum class ItemNameSource : uint8_t { Game, Mod, Fallback, Unknown };

// UTF-8, never empty. `saveTbl` picks the stage-specific names (the Snowpeak Ruins map).
const std::string& itemDisplayName(uint8_t itemNo, int saveTbl = -1);
ItemNameSource itemNameSource(uint8_t itemNo, int saveTbl = -1);
// The game mounted English messages (US or UK); our name corrections apply only then.
bool itemNamesEnglish();
// Autotest: 1 English, 0 another language, -1 the disc's own.
void forceItemNamesEnglishForTest(int english);
// "Forest Temple" for dStage_SaveTbl_LV1, "" for tables that are not a dungeon.
const char* dungeonNameForSaveTbl(int saveTbl);

}  // namespace twili::ui
