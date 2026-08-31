/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SPECIES_H
#define SPECIES_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    RACE_HUMAN,
    RACE_ELF,
    RACE_DWARF,
    RACE_GNOME,
    RACE_GOLIATH,
    RACE_HALFLING,
    RACE_ORC,
    RACE_TIEFLING,
    RACE_AASIMAR,
    RACE_DRAGONBORN,
    RACE_HALF_ELF,
    RACE_HALF_ORC,
    RACE_TABAXI,
    RACE_AARAKOCRA,
    RACE_KENKU,
    RACE_TORTLE,
    RACE_GOBLIN,
    RACE_HOBGOBLIN,
    RACE_BUGBEAR,
    RACE_KOBOLD,
    RACE_MINOTAUR,
    RACE_CENTAUR,
    RACE_YUAN_TI,
    RACE_GENASI,
    RACE_TRITON,
    RACE_FIRBOLG,
    RACE_GITH,
    RACE_CHANGELING,
    RACE_FAIRY,
    RACE_WARFORGED,
    RACE_LOXODON,
    RACE_VEDALKEN,
    RACE_PLASMOID,
    RACE_THRI_KREEN,
    RACE_AUTOGNOME,
    RACE_LEONIN,
    RACE_SATYR,
    RACE_HARENGON,
    RACE_OWLIN,
    RACE_GRUNG,
    RACE_LOCATHAH,
    RACE_SHIFTER,
    RACE_KALASHTAR,
    RACE_SIMIC_HYBRID,
    RACE_VERDAN,
    RACE_GIFF,
    RACE_HADOZEE,
    RACE_ASTRAL_ELF,
    RACE_KENDER,
    RACE_DHAMPIR,
    RACE_HEXBLOOD,
    RACE_REBORN,
    RACE_COUNT
} RaceType;

typedef enum {
    SUBRACE_NONE = -1,
    SUBRACE_HUMAN_STANDARD,
    SUBRACE_HUMAN_VARIANT,
    SUBRACE_ELF_HIGH,
    SUBRACE_ELF_WOOD,
    SUBRACE_ELF_DROW,
    SUBRACE_ELF_ELADRIN,
    SUBRACE_ELF_SEA,
    SUBRACE_ELF_SHADAR_KAI,
    SUBRACE_ELF_PALLID,
    SUBRACE_DWARF_HILL,
    SUBRACE_DWARF_MOUNTAIN,
    SUBRACE_DWARF_DUERGAR,
    SUBRACE_GNOME_FOREST,
    SUBRACE_GNOME_ROCK,
    SUBRACE_GNOME_DEEP,
    SUBRACE_HALFLING_LIGHTFOOT,
    SUBRACE_HALFLING_STOUT,
    SUBRACE_HALFLING_GHOSTWISE,
    SUBRACE_HALFLING_LOTUSDEN,
    SUBRACE_TIEFLING_PIT_LORD, // was Asmodeus
    SUBRACE_TIEFLING_BAALZEBUL,
    SUBRACE_TIEFLING_DISPATER,
    SUBRACE_TIEFLING_FIERNA,
    SUBRACE_TIEFLING_GLASYA,
    SUBRACE_TIEFLING_LEVISTUS,
    SUBRACE_TIEFLING_MAMMON,
    SUBRACE_TIEFLING_COLD_LORD, // was Mephistopheles
    SUBRACE_TIEFLING_ZARIEL,
    SUBRACE_TIEFLING_WINGED,
    SUBRACE_AASIMAR_PROTECTOR,
    SUBRACE_AASIMAR_SCOURGE,
    SUBRACE_AASIMAR_FALLEN,
    SUBRACE_DRAGONBORN_CHROMATIC,
    SUBRACE_DRAGONBORN_METALLIC,
    SUBRACE_DRAGONBORN_GEM,
    SUBRACE_HALFELF_BASE,
    SUBRACE_HALFELF_HIGH,
    SUBRACE_HALFELF_WOOD,
    SUBRACE_HALFELF_DROW,
    SUBRACE_HALFELF_SEA,
    SUBRACE_GENASI_AIR,
    SUBRACE_GENASI_WATER,
    SUBRACE_GENASI_FIRE,
    SUBRACE_GENASI_EARTH,
    SUBRACE_GITH_YANKI,
    SUBRACE_GITH_ZERAI,
    SUBRACE_SHIFTER_BEASTHIDE,
    SUBRACE_SHIFTER_LONGTOOTH,
    SUBRACE_SHIFTER_SWIFTSTRIDE,
    SUBRACE_SHIFTER_WILDHUNT,
    SUBRACE_COUNT
} SubraceType;

typedef struct {
    const char* name;
    const char* description;
    int str_bonus;
    int dex_bonus;
    int con_bonus;
    int int_bonus;
    int wis_bonus;
    int cha_bonus;
    const char* traits;
    bool is_flexible;
} RaceTemplate;

typedef struct {
    SubraceType type;
    RaceType parent_race;
    const char* name;
    const char* description;
    int str_bonus;
    int dex_bonus;
    int con_bonus;
    int int_bonus;
    int wis_bonus;
    int cha_bonus;
    const char* traits;
    bool is_flexible;
} SubraceTemplate;

static const RaceTemplate RACES[RACE_COUNT] = {
    [RACE_HUMAN] = { "Human", "Versatile and ambitious, humans are the most adaptable of the common races.", 0, 0, 0, 0, 0, 0, "Resourceful, Skillful, Versatile", false },
    [RACE_ELF] = { "Elf", "Graceful and long-lived, elves are deeply connected to the natural and magical worlds.", 0, 2, 0, 0, 0, 0, "Darkvision, Fey Ancestry, Keen Senses, Trance", false },
    [RACE_DWARF] = { "Dwarf", "Bold and hardy, dwarves are known for their skill in metalwork and their enduring traditions.", 0, 0, 2, 0, 0, 0, "Darkvision, Dwarven Resilience, Stonecunning", false },
    [RACE_GNOME] = { "Gnome", "Small and inventive, gnomes possess a boundless curiosity and a love for tinkering.", 0, 0, 0, 2, 0, 0, "Darkvision, Gnomish Cunning", false },
    [RACE_GOLIATH] = { "Giant-kin", "Powerful and competitive, these mountain-dwellers possess the strength of giants.", 2, 0, 1, 0, 0, 0, "Giant Ancestry, Large Form, Powerful Build", false },
    [RACE_HALFLING] = { "Halfling", "Small and cheerful, halflings value peace, comfort, and have a knack for avoiding trouble.", 0, 2, 0, 0, 0, 0, "Brave, Halfling Nimbleness, Luck", false },
    [RACE_ORC] = { "Orc", "Proud and fierce, orcs are formidable warriors who value strength and ancestral honor.", 2, 0, 1, 0, 0, 0, "Adrenaline Rush, Darkvision, Relentless Endurance", false },
    [RACE_TIEFLING] = { "Fiend-blood", "Marked by an ancient lineage, they navigate the world with a blend of charm and resilience.", 0, 0, 0, 0, 0, 2, "Darkvision, Fiendish Legacy", false },
    [RACE_AASIMAR] = { "Celestial-blood", "Infused with heavenly grace, they are often driven by a sense of duty or destiny.", 0, 0, 0, 0, 0, 2, "Celestial Resistance, Darkvision, Healing Hands, Light Bearer", false },
    [RACE_DRAGONBORN] = { "Dragon-kin", "Noble and strong, dragon-kin carry the legacy of ancient dragons in their blood and breath.", 2, 0, 0, 0, 0, 1, "Dragon Ancestry, Breath Weapon, Damage Resistance", false },
    [RACE_HALF_ELF] = { "Half-Elf", "Walking between two worlds, they combine the versatility of humans with the grace of elves.", 0, 0, 0, 0, 0, 2, "Human Versatility, Elven Grace", false },
    [RACE_HALF_ORC] = { "Half-Orc", "Resilient and strong, they possess a fierce determination born of two distinct heritages.", 2, 0, 1, 0, 0, 0, "Strong and Resilient", false },
    [RACE_TABAXI] = { "Feline-folk", "Agile and curious, these feline wanderers are driven by a love for discovery and secrets.", 0, 0, 0, 0, 0, 0, "Feline Agility, Cat's Claws, Cat's Talent", true },
    [RACE_AARAKOCRA] = { "Avian-folk", "Swift and free, these bird-like people originate from the vast reaches of the open sky.", 0, 0, 0, 0, 0, 0, "Flight, Talons, Wind Caller", true },
    [RACE_KENKU] = { "Raven-folk", "Clever mimics, they seek to regain their lost flight through ingenuity and shared knowledge.", 0, 0, 0, 0, 0, 0, "Expert Duplication, Kenku Recall, Mimicry", true },
    [RACE_TORTLE] = { "Turtle-folk", "Sturdy and patient, they carry their homes on their backs and live a life of wandering.", 0, 0, 0, 0, 0, 0, "Natural Armor, Hold Breath, Shell Defense", true },
    [RACE_GOBLIN] = { "Goblin", "Small and cunning, goblins rely on their wits and agility to thrive in a world of larger creatures.", 0, 0, 0, 0, 0, 0, "Fey Ancestry, Nimble Escape, Fury of the Small", true },
    [RACE_HOBGOBLIN] = { "Hobgoblin", "Disciplined and strategic, they value order and martial prowess above all else.", 0, 0, 0, 0, 0, 0, "Fey Ancestry, Fortune from the Many, Martial Training", true },
    [RACE_BUGBEAR] = { "Bugbear", "Large and stealthy, they possess a surprising grace for their size and a formidable strength.", 0, 0, 0, 0, 0, 0, "Fey Ancestry, Long-Limbed, Powerful Build, Sneaky", true },
    [RACE_KOBOLD] = { "Kobold", "Small and resourceful, they use their cleverness and numbers to overcome much larger foes.", 0, 0, 0, 0, 0, 0, "Draconic Cry, Kobold Knowledge", true },
    [RACE_MINOTAUR] = { "Minotaur", "Powerful and imposing, they are formidable warriors with a strong sense of direction and honor.", 0, 0, 0, 0, 0, 0, "Horns, Goring Rush, Hammering Horns", true },
    [RACE_CENTAUR] = { "Centaur", "Swift and noble, they combine the strength of a horse with the wisdom of humanoid kind.", 0, 0, 0, 0, 0, 0, "Charge, Hooves, Equine Build", true },
    [RACE_YUAN_TI] = { "Serpent-folk", "Graceful and enigmatic, they possess an ancient connection to serpents and subtle magic.", 0, 0, 0, 0, 0, 0, "Magic Resistance, Poison Resilience, Serpentine Spellcasting", true },
    [RACE_GENASI] = { "Elemental-kin", "Born of the elements, their physical form and temperament reflect the raw power of nature.", 0, 0, 0, 0, 0, 0, "Elemental Nature", true },
    [RACE_TRITON] = { "Sea-folk", "Noble guardians of the depths, they protect the surface and the sea from ancient threats.", 0, 0, 0, 0, 0, 0, "Amphibious, Control Air and Water, Emissary of the Sea", true },
    [RACE_FIRBOLG] = { "Forest-giant", "Gentle and wise, these reclusive giants act as protectors of the natural world.", 0, 0, 0, 0, 0, 0, "Firbolg Magic, Hidden Step, Speech of Beast and Leaf", true },
    [RACE_GITH] = { "Astral-strider", "Ancient survivors of the void, they possess formidable mental powers and a disciplined spirit.", 0, 0, 0, 0, 0, 0, "astral-strider/void-monk Psionics", true },
    [RACE_CHANGELING] = { "Shapeshifter", "Masters of identity, they can alter their appearance at will to blend into any crowd.", 0, 0, 0, 0, 0, 0, "Shapechanger, Changeling Instincts", true },
    [RACE_FAIRY] = { "Sprite", "Tiny and magical, they are spirited beings who flutter through the world with playful intent.", 0, 0, 0, 0, 0, 0, "Flight, Fairy Magic", true },
    [RACE_WARFORGED] = { "Iron-forged", "Constructed for battle, these living machines seek a new purpose beyond their original design.", 0, 0, 0, 0, 0, 0, "Constructed Resilience, Sentry's Rest, Integrated Protection", true },
    [RACE_LOXODON] = { "Elephant-folk", "Large and serene, they are known for their wisdom, strength, and calm demeanor.", 0, 0, 0, 0, 0, 0, "Powerful Build, Loxodon Serenity, Natural Armor, Trunk", true },
    [RACE_VEDALKEN] = { "Logic-kin", "Highly analytical and focused, they seek perfection through logic and tireless study.", 0, 0, 0, 0, 0, 0, "Vedalken Dispassion, Tireless Precision", true },
    [RACE_PLASMOID] = { "Slime-kin", "Amorphous and adaptable, these gelatinous beings can shift their form to suit their needs.", 0, 0, 0, 0, 0, 0, "Amorphous, Shape Self, Plasmoid Resilience", true },
    [RACE_THRI_KREEN] = { "Insect-folk", "Hardy and multi-armed, they are master hunters from the arid wastes.", 0, 0, 0, 0, 0, 0, "Chameleon Carapace, Multiple Arms, Sleepless", true },
    [RACE_AUTOGNOME] = { "Clockwork-kin", "Mechanical and precise, these small constructs were built to assist and endure.", 0, 0, 0, 0, 0, 0, "Armored Casing, Built for Success, Mechanical Nature", true },
    [RACE_LEONIN] = { "Lion-folk", "Proud and noble, these feline warriors value courage and their close-knit prides.", 0, 0, 0, 0, 0, 0, "Daunting Roar, Claws, Hunter's Instincts", true },
    [RACE_SATYR] = { "Faun", "Free-spirited and jovial, they celebrate the joys of life with music, dance, and revelry.", 0, 0, 0, 0, 0, 0, "Mirthful Leaps, Reveler, Magic Resistance", true },
    [RACE_HARENGON] = { "Rabbit-folk", "Quick-footed and lucky, they possess a boundless energy and a keen sense of danger.", 0, 0, 0, 0, 0, 0, "Hare-Trigger, Leporine Senses, Lucky Footwork", true },
    [RACE_OWLIN] = { "Owl-folk", "Graceful and silent, these avian beings have a natural connection to the night sky and magic.", 0, 0, 0, 0, 0, 0, "Flight, Silent Feathers", true },
    [RACE_GRUNG] = { "Frog-folk", "Small and vibrant, they live in complex societies and possess a natural toxicity.", 0, 0, 0, 0, 0, 0, "Amphibious, Poisonous Skin, Standing Leap", true },
    [RACE_LOCATHAH] = { "Fish-folk", "Sturdy and resilient, they are aquatic people who have adapted to life both above and below.", 0, 0, 0, 0, 0, 0, "Natural Armor, Leviathan Will, Limited Amphibiousness", true },
    [RACE_SHIFTER] = { "Skin-changer", "Primal and fierce, they can tap into their bestial heritage to enhance their physical abilities.", 0, 0, 0, 0, 0, 0, "Shifting", true },
    [RACE_KALASHTAR] = { "Dream-touched", "Bound to spirits of light, they possess a serene presence and formidable mental defenses.", 0, 0, 0, 0, 0, 0, "Dual Mind, Mental Discipline, Psychic Resilience, Severed from Dreams", true },
    [RACE_SIMIC_HYBRID] = { "Bio-mutant", "Subjected to magical augmentation, they possess physical traits from various aquatic and avian life.", 0, 0, 0, 0, 0, 0, "Animal Enhancement", true },
    [RACE_VERDAN] = { "Chaos-born", "Ever-changing and curious, they are a young race born from the touch of raw magic.", 0, 0, 0, 0, 0, 0, "Black Blood Healing, Limited Telepathy, Persuasive", true },
    [RACE_GIFF] = { "Hippo-folk", "Large and imposing, they possess a natural affinity for discipline and powerful weapons.", 0, 0, 0, 0, 0, 0, "Firearms Knowledge, Hippo Build", true },
    [RACE_HADOZEE] = { "Simian-glider", "Nimble and adventurous, they are expert sailors who can glide through the air.", 0, 0, 0, 0, 0, 0, "Dexterous Feet, Hadozee Dodge, Glide", true },
    [RACE_ASTRAL_ELF] = { "Void-elf", "Longevity and cosmic grace define these elves who have lived among the stars.", 0, 0, 0, 0, 0, 0, "Astral Fire, Starlight Step, Astral Trance", true },
    [RACE_KENDER] = { "Trickster-kin", "Fearless and inquisitive, they possess a natural curiosity that often leads them into trouble.", 0, 0, 0, 0, 0, 0, "Brave, Kender Aptitude, Taunt", true },
    [RACE_DHAMPIR] = { "Half-vampire", "Caught between life and undeath, they possess a hunger and grace born of a dark lineage.", 0, 0, 0, 0, 0, 0, "Ancestral Legacy, Deathless Nature, Spider Climb, Vampiric Bite", true },
    [RACE_HEXBLOOD] = { "Witch-blood", "Marked by hag magic, they possess eerie abilities and an enduring connection to the occult.", 0, 0, 0, 0, 0, 0, "Ancestral Legacy, Hex Magic, Magic Token", true },
    [RACE_REBORN] = { "Awakened-dead", "Having returned from the brink of death, they seek to rediscover their lost identity.", 0, 0, 0, 0, 0, 0, "Ancestral Legacy, Deathless Nature, Knowledge from a Past Life", true }
};

static const SubraceTemplate SUBRACES[SUBRACE_COUNT] = {
    { SUBRACE_HUMAN_STANDARD, RACE_HUMAN, "Standard", "Well-rounded and capable, they represent the common adaptability of humankind.", 1, 1, 1, 1, 1, 1, "Standard Bonuses", false },
    { SUBRACE_HUMAN_VARIANT, RACE_HUMAN, "Variant", "Highly specialized, they focus on specific talents and skills to excel in their chosen path.", 0, 0, 0, 0, 0, 0, "Extra Feat, Extra Skill", true },
    { SUBRACE_ELF_HIGH, RACE_ELF, "High Elf", "Scholarly and refined, they have an innate talent for arcane magic.", 0, 0, 0, 1, 0, 0, "Elf Cantrip, Extra Language", false },
    { SUBRACE_ELF_WOOD, RACE_ELF, "Wood Elf", "Swift and stealthy, they are at home in the deepest forests and wild places.", 0, 0, 0, 0, 1, 0, "Mask of the Wild, Fleet of Foot", false },
    { SUBRACE_ELF_DROW, RACE_ELF, "Dark Elf", "Born of the depths, they possess a natural talent for magic and a keen sight in the dark.", 0, 0, 0, 0, 0, 1, "Superior Darkvision, Dark Elf Magic, Sunlight Sensitivity", false },
    { SUBRACE_ELF_ELADRIN, RACE_ELF, "Seasonal Elf", "Infused with the raw emotions of the fey realms, their form shifts with the seasons.", 0, 0, 0, 0, 0, 0, "Fey Step (Seasonal)", true },
    { SUBRACE_ELF_SEA, RACE_ELF, "Sea Elf", "Graceful swimmers, they have made their homes in the vibrant reefs and deep trenches.", 0, 0, 1, 0, 0, 0, "Child of the Sea, Friend of the Sea", false },
    { SUBRACE_ELF_SHADAR_KAI, RACE_ELF, "Shadow Elf", "Bound to the realm of shadows, they possess a cold resilience and a ghostly step.", 0, 0, 1, 0, 0, 0, "Shadow Step, Necrotic Resistance", false },
    { SUBRACE_ELF_PALLID, RACE_ELF, "Pallid Elf", "Reclusive and insightful, they possess a deep connection to the mysteries of the moon.", 0, 0, 0, 0, 1, 0, "Incisive Sense, Blessing of the Weaver", false },
    { SUBRACE_DWARF_HILL, RACE_DWARF, "Hill Dwarf", "Hardy and wise, they possess an extra measure of resilience and a deep intuition.", 0, 0, 0, 0, 1, 0, "Dwarven Toughness", false },
    { SUBRACE_DWARF_MOUNTAIN, RACE_DWARF, "Mountain Dwarf", "Strong and capable, they are accustomed to the weight of armor and the rigors of mountain life.", 2, 0, 0, 0, 0, 0, "Dwarven Armor Training", false },
    { SUBRACE_DWARF_DUERGAR, RACE_DWARF, "Deep Dwarf", "Grim and powerful, they have adapted to the harsh and dark environment of the deep earth.", 1, 0, 0, 0, 0, 0, "Deep Dwarf Magic, Sunlight Sensitivity", false },
    { SUBRACE_GNOME_FOREST, RACE_GNOME, "Forest Gnome", "Small and elusive, they possess a natural talent for illusions and a bond with small forest beasts.", 0, 1, 0, 0, 0, 0, "Natural Illusionist, Speak with Small Beasts", false },
    { SUBRACE_GNOME_ROCK, RACE_GNOME, "Rock Gnome", "Inquisitive and clever, they have a natural talent for engineering and mechanical invention.", 0, 0, 1, 0, 0, 0, "Artificer's Lore, Tinker", false },
    { SUBRACE_GNOME_DEEP, RACE_GNOME, "Deep Gnome", "Stealthy and resilient, they are the quiet masters of the deep underground tunnels.", 0, 1, 0, 0, 0, 0, "Superior Darkvision, Stone Camouflage", false },
    { SUBRACE_HALFLING_LIGHTFOOT, RACE_HALFLING, "Lightfoot", "Small and easily overlooked, they are masters of stealth and blending into crowds.", 0, 0, 0, 0, 0, 1, "Naturally Stealthy", false },
    { SUBRACE_HALFLING_STOUT, RACE_HALFLING, "Stout", "Hardy and resilient, they possess a toughness that rivals that of dwarves.", 0, 0, 1, 0, 0, 0, "Stout Resilience", false },
    { SUBRACE_HALFLING_GHOSTWISE, RACE_HALFLING, "Ghostwise", "Reclusive and telepathic, they maintain a deep connection to their ancestral clans.", 0, 0, 0, 0, 1, 0, "Silent Speech", false },
    { SUBRACE_HALFLING_LOTUSDEN, RACE_HALFLING, "Lotusden", "Infused with the magic of the wild, they possess a natural talent for manipulating plants.", 0, 0, 0, 0, 0, 0, "Druid Magic, Timberwalk", true },
    { SUBRACE_TIEFLING_PIT_LORD, RACE_TIEFLING, "Infernal Legacy", "Bearing the mark of ancient pacts, they possess a natural talent for fire and deception.", 0, 0, 0, 1, 0, 0, "Infernal Legacy", false },
    { SUBRACE_TIEFLING_BAALZEBUL, RACE_TIEFLING, "Legacy of Corruption", "Their lineage is marked by a subtle rot that grants them power over decay.", 0, 0, 0, 1, 0, 0, "Legacy of Corruption", false },
    { SUBRACE_TIEFLING_DISPATER, RACE_TIEFLING, "Legacy of Deception", "Masters of shadows and lies, they can weave illusions as easily as breathing.", 0, 1, 0, 0, 0, 0, "Legacy of Deception", false },
    { SUBRACE_TIEFLING_FIERNA, RACE_TIEFLING, "Legacy of Emotion", "They possess an uncanny ability to influence the feelings and desires of others.", 0, 0, 0, 0, 1, 0, "Legacy of Emotion", false },
    { SUBRACE_TIEFLING_GLASYA, RACE_TIEFLING, "Legacy of Intrigue", "Natural spies and manipulators, they can move through society with ease and grace.", 0, 1, 0, 0, 0, 0, "Legacy of Intrigue", false },
    { SUBRACE_TIEFLING_LEVISTUS, RACE_TIEFLING, "Legacy of Ice", "Their blood runs cold, granting them a natural resilience and power over frost.", 0, 0, 1, 0, 0, 0, "Legacy of Ice", false },
    { SUBRACE_TIEFLING_MAMMON, RACE_TIEFLING, "Legacy of Greed", "Driven by an ancient avarice, they have a knack for finding and securing wealth.", 0, 0, 0, 1, 0, 0, "Legacy of Greed", false },
    { SUBRACE_TIEFLING_COLD_LORD, RACE_TIEFLING, "Legacy of Magic", "Their lineage is steeped in arcane power, granting them a natural talent for the mystic arts.", 0, 0, 0, 1, 0, 0, "Legacy of Magic", false },
    { SUBRACE_TIEFLING_ZARIEL, RACE_TIEFLING, "Martial Legacy", "Forged in the fires of conflict, they possess a natural strength and tactical mind.", 1, 0, 0, 0, 0, 0, "Martial Legacy", false },
    { SUBRACE_TIEFLING_WINGED, RACE_TIEFLING, "Winged", "Gifted with leathery wings, they can take to the skies and strike from above.", 0, 0, 0, 0, 0, 0, "Winged (Flight)", false },
    { SUBRACE_AASIMAR_PROTECTOR, RACE_AASIMAR, "Radiant", "Infused with celestial light, they can manifest wings of radiance to inspire and protect.", 0, 0, 0, 0, 1, 0, "Radiant Soul", false },
    { SUBRACE_AASIMAR_SCOURGE, RACE_AASIMAR, "Burning", "They carry a searing divine energy within them that can be unleashed to consume their foes.", 0, 0, 1, 0, 0, 0, "Radiant Consumption", false },
    { SUBRACE_AASIMAR_FALLEN, RACE_AASIMAR, "Corrupted", "Their celestial light has been dimmed or twisted, granting them power over fear and shadows.", 1, 0, 0, 0, 0, 0, "Necrotic Shroud", false },
    { SUBRACE_DRAGONBORN_CHROMATIC, RACE_DRAGONBORN, "Chromatic", "Linked to the elemental dragons, they possess a breath and resilience of fire, ice, or acid.", 0, 0, 0, 0, 0, 0, "Chromatic Warding", false },
    { SUBRACE_DRAGONBORN_METALLIC, RACE_DRAGONBORN, "Metallic", "Carrying the legacy of noble dragons, they possess a powerful breath and a protective aura.", 0, 0, 0, 0, 0, 0, "Metallic Breath Weapon", false },
    { SUBRACE_DRAGONBORN_GEM, RACE_DRAGONBORN, "Gem", "Infused with psionic energy, they possess a shimmering form and the power of the mind.", 0, 0, 0, 0, 0, 0, "Psionic Mind, Gem Flight", false },
    { SUBRACE_HALFELF_BASE, RACE_HALF_ELF, "Base", "Combining the best of both heritages, they are versatile and skilled in many areas.", 0, 0, 0, 0, 0, 0, "Skill Versatility", true },
    { SUBRACE_HALFELF_HIGH, RACE_HALF_ELF, "High Elf", "Inheriting the arcane tradition of their elven kin, they possess a natural talent for magic.", 0, 0, 0, 0, 0, 0, "Elf Cantrip", true },
    { SUBRACE_HALFELF_WOOD, RACE_HALF_ELF, "Wood Elf", "Inheriting the swiftness of their forest-dwelling kin, they are at home in the wilds.", 0, 0, 0, 0, 0, 0, "Fleet of Foot/Mask of the Wild", true },
    { SUBRACE_HALFELF_DROW, RACE_HALF_ELF, "Dark Elf", "Inheriting the subtle magic of their drow kin, they can weave shadows to their advantage.", 0, 0, 0, 0, 0, 0, "Dark Elf Magic", true },
    { SUBRACE_HALFELF_SEA, RACE_HALF_ELF, "Sea Elf", "Inheriting a connection to the oceans, they are as comfortable in the water as on land.", 0, 0, 0, 0, 0, 0, "Swimming Speed", true },
    { SUBRACE_GENASI_AIR, RACE_GENASI, "Air", "Light on their feet and quick-witted, they carry the essence of the shifting winds.", 0, 0, 0, 0, 0, 0, "Unending Breath, Mingle with Wind", true },
    { SUBRACE_GENASI_WATER, RACE_GENASI, "Water", "Fluid and adaptable, they possess the power of the tides and the depth of the oceans.", 0, 0, 0, 0, 0, 0, "Amphibious, Call to the Wave", true },
    { SUBRACE_GENASI_FIRE, RACE_GENASI, "Fire", "Passionate and volatile, they carry the searing heat and flickering light of the eternal flame.", 0, 0, 0, 0, 0, 0, "Fire Resistance, Reach to the Blaze", true },
    { SUBRACE_GENASI_EARTH, RACE_GENASI, "Earth", "Sturdy and deliberate, they are as unyielding and enduring as the ancient mountains.", 0, 0, 0, 0, 0, 0, "Earth Walk, Merge with Stone", true },
    { SUBRACE_GITH_YANKI, RACE_GITH, "Astral Warrior", "Disciplined and strong, they are trained from birth to defend their people in the void.", 0, 0, 0, 0, 0, 0, "Warrior Psionics, Martial Training", true },
    { SUBRACE_GITH_ZERAI, RACE_GITH, "Astral Monk", "Serene and focused, they use their mental discipline to master their own spirits.", 0, 0, 0, 0, 0, 0, "Monk Psionics, Mental Discipline", true },
    { SUBRACE_SHIFTER_BEASTHIDE, RACE_SHIFTER, "Beasthide", "Their shifting manifests as a thick, protective hide and an indomitable toughness.", 0, 0, 0, 0, 0, 0, "Beasthide Shifting", true },
    { SUBRACE_SHIFTER_LONGTOOTH, RACE_SHIFTER, "Longtooth", "When they shift, they grow formidable fangs and tap into a primal ferocity.", 0, 0, 0, 0, 0, 0, "Longtooth Shifting", true },
    { SUBRACE_SHIFTER_SWIFTSTRIDE, RACE_SHIFTER, "Swiftstride", "Shifting grants them a preternatural speed and a feline grace on the battlefield.", 0, 0, 0, 0, 0, 0, "Swiftstride Shifting", true },
    { SUBRACE_SHIFTER_WILDHUNT, RACE_SHIFTER, "Wildhunt", "Their shifting sharpens their senses to an uncanny degree, making them peerless trackers.", 0, 0, 0, 0, 0, 0, "Wildhunt Shifting", true }
};

#endif // SPECIES_H
