// SPDX-License-Identifier: GPL-2.0-only
#pragma once

// What the client knows about the zone it is in: the spawns the server has
// told it about and the local player. Filled from zone packets by the app;
// read by the renderer and UI.

#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "core/ascii_case.h"
#include "core/ini_file.h"
#include "game/spawn_heights.h"


template <typename T>
inline bool IsNaN(T x) {
	return std::isnan(x);
}

namespace hailstone::game {

	// "a_rat01" -> "a rat", "#Guard_Bob00" -> "Guard Bob": leading '#' and trailing digits stripped, underscores to
// spaces.
	std::string display_name(const std::string& name);
	// Appearance::new_armor_bits for a spawn whose kind is not known: the player (8) or the NPC (0x10) bit of
	// ActorNewArmorFilter shows its hero's forge armour.
	inline constexpr std::uint8_t kNewArmorUnknownKind = 0x18;
	// What a spawn looks like (spawn packet, OP_WearChange, OP_Illusion). Slots
	// are the nine visible ones: 0 head, 1 chest, 2 arms, 3 wrists, 4 hands,
	// 5 legs, 6 feet, 7 primary, 8 secondary (game/appearance.h).
	struct Appearance {
		// TextureType: 0xff = per-slot armour (always for PC-model races, whose
		// TextureType the client folds into slots 1-6), else the NPC body texture.
		std::uint8_t texture = 0xff;
		std::uint8_t helm = 0; // HeadType: NPC head variant
		// Spawn bitfield bit 23: PC-model races show their head armour only
		// with it set (PlayerClient::SwapHead returns early otherwise).
		bool show_helm = true;
		std::uint8_t face = 0;
		std::uint8_t hair_style = 0, hair_color = 0, beard = 0, beard_color = 0;
		std::uint8_t eye_color1 = 0, eye_color2 = 0;
		std::uint32_t drakkin_heritage = 0, drakkin_tattoo = 0, drakkin_details = 0;
		// Per slot: armour material (slots 0-6) or item model number IT<n>
		// (slots 7, 8), and the tint 0xAARRGGBB (a zero high byte = untinted).
		std::array<std::uint32_t, 9> material{};
		// The rest of the five-word armour record (ArmorProperties: type, variation, material, newArmorID,
		// newArmorType; EQEmu's Texture_Struct Material, Unknown1, EliteMaterial, HeroForgeModel, Material2), kept
		// whole: RefreshEquipSlot @00594e50 hands the wire's record to PlayerClient_SetActorEquipmentSlot
		// @0042e6f0, which stores it as the ActorClient's ArmorProperties (+0x8c) in the same order.
		std::array<std::uint32_t, 9> material_variation{}; // variation (+4)
		std::array<std::uint32_t, 9> elite_material{};     // material (+8): game::ArmorRecord::material2
		std::array<std::uint32_t, 9> tint{};
		std::array<std::uint32_t, 9> hero_forge{};         // newArmorID (+0xc)
		std::array<std::uint32_t, 9> new_armor_type{};     // newArmorType (+0x10)
		// PlayerClient_ApplyNewArmorFilter @0x595de0: the ActorNewArmorFilter bits that show this spawn's hero's
		// forge armour (game::new_armor_bits). A spawn whose kind is not known (character select, previews)
		// has kNewArmorUnknownKind. The renderer collapses the bits to shown or hidden for the active filter
		// (game::resolve_new_armor_filter), so a change the filter does not see rebuilds nothing.
		std::uint8_t new_armor_bits = kNewArmorUnknownKind;
		// NpcTintIndex (spawn +0x51c; OP_SpawnAppearance 39): npct.ini Value<n>
		// tints new-style models (ActorClient_ApplyNpcTint); 0 none.
		std::uint32_t npc_tint = 0;
		// The class (ActorClient +0x14): SwapBodyMaterial's material rules for
		// monks (kind 7).
		std::uint8_t class_id = 0;

		bool operator==(const Appearance&) const = default;
	};
	// From here to the player races (EQR_*), and Spawn's IsNamed, GetSpawnType, GetTypeDesc and GetRaceIDName below:
	// copied from MacroQuest (GPL-2.0, Copyright (C) 2002-present MacroQuest Authors; THIRD_PARTY.md), for plugins
	// and TLOs that port MacroQuest logic. IsNamed was changed here (it mixes MacroQuest's older and newer tests).
	namespace Race {
		constexpr uint16_t Doug = 0;
		constexpr uint16_t Human = 1;
		constexpr uint16_t Barbarian = 2;
		constexpr uint16_t Erudite = 3;
		constexpr uint16_t WoodElf = 4;
		constexpr uint16_t HighElf = 5;
		constexpr uint16_t DarkElf = 6;
		constexpr uint16_t HalfElf = 7;
		constexpr uint16_t Dwarf = 8;
		constexpr uint16_t Troll = 9;
		constexpr uint16_t Ogre = 10;
		constexpr uint16_t Halfling = 11;
		constexpr uint16_t Gnome = 12;
		constexpr uint16_t Aviak = 13;
		constexpr uint16_t Werewolf = 14;
		constexpr uint16_t Brownie = 15;
		constexpr uint16_t Centaur = 16;
		constexpr uint16_t Golem = 17;
		constexpr uint16_t Giant = 18;
		constexpr uint16_t Trakanon = 19;
		constexpr uint16_t VenrilSathir = 20;
		constexpr uint16_t EvilEye = 21;
		constexpr uint16_t Beetle = 22;
		constexpr uint16_t Kerran = 23;
		constexpr uint16_t Fish = 24;
		constexpr uint16_t Fairy = 25;
		constexpr uint16_t Froglok = 26;
		constexpr uint16_t FroglokGhoul = 27;
		constexpr uint16_t Fungusman = 28;
		constexpr uint16_t Gargoyle = 29;
		constexpr uint16_t Gasbag = 30;
		constexpr uint16_t GelatinousCube = 31;
		constexpr uint16_t Ghost = 32;
		constexpr uint16_t Ghoul = 33;
		constexpr uint16_t GiantBat = 34;
		constexpr uint16_t GiantEel = 35;
		constexpr uint16_t GiantRat = 36;
		constexpr uint16_t GiantSnake = 37;
		constexpr uint16_t GiantSpider = 38;
		constexpr uint16_t Gnoll = 39;
		constexpr uint16_t Goblin = 40;
		constexpr uint16_t Gorilla = 41;
		constexpr uint16_t Wolf = 42;
		constexpr uint16_t Bear = 43;
		constexpr uint16_t FreeportGuard = 44;
		constexpr uint16_t DemiLich = 45;
		constexpr uint16_t Imp = 46;
		constexpr uint16_t Griffin = 47;
		constexpr uint16_t Kobold = 48;
		constexpr uint16_t LavaDragon = 49;
		constexpr uint16_t Lion = 50;
		constexpr uint16_t LizardMan = 51;
		constexpr uint16_t Mimic = 52;
		constexpr uint16_t Minotaur = 53;
		constexpr uint16_t Orc = 54;
		constexpr uint16_t HumanBeggar = 55;
		constexpr uint16_t Pixie = 56;
		constexpr uint16_t Drachnid = 57;
		constexpr uint16_t SolusekRo = 58;
		constexpr uint16_t Bloodgill = 59;
		constexpr uint16_t Skeleton = 60;
		constexpr uint16_t Shark = 61;
		constexpr uint16_t Tunare = 62;
		constexpr uint16_t Tiger = 63;
		constexpr uint16_t Treant = 64;
		constexpr uint16_t Vampire = 65;
		constexpr uint16_t StatueOfRallosZek = 66;
		constexpr uint16_t HighpassCitizen = 67;
		constexpr uint16_t TentacleTerror = 68;
		constexpr uint16_t Wisp = 69;
		constexpr uint16_t Zombie = 70;
		constexpr uint16_t QeynosCitizen = 71;
		constexpr uint16_t Ship = 72;
		constexpr uint16_t Launch = 73;
		constexpr uint16_t Piranha = 74;
		constexpr uint16_t Elemental = 75;
		constexpr uint16_t Puma = 76;
		constexpr uint16_t NeriakCitizen = 77;
		constexpr uint16_t EruditeCitizen = 78;
		constexpr uint16_t Bixie = 79;
		constexpr uint16_t ReanimatedHand = 80;
		constexpr uint16_t RivervaleCitizen = 81;
		constexpr uint16_t Scarecrow = 82;
		constexpr uint16_t Skunk = 83;
		constexpr uint16_t SnakeElemental = 84;
		constexpr uint16_t Spectre = 85;
		constexpr uint16_t Sphinx = 86;
		constexpr uint16_t Armadillo = 87;
		constexpr uint16_t ClockworkGnome = 88;
		constexpr uint16_t Drake = 89;
		constexpr uint16_t HalasCitizen = 90;
		constexpr uint16_t Alligator = 91;
		constexpr uint16_t GrobbCitizen = 92;
		constexpr uint16_t OggokCitizen = 93;
		constexpr uint16_t KaladimCitizen = 94;
		constexpr uint16_t CazicThule = 95;
		constexpr uint16_t Cockatrice = 96;
		constexpr uint16_t DaisyMan = 97;
		constexpr uint16_t ElfVampire = 98;
		constexpr uint16_t Denizen = 99;
		constexpr uint16_t Dervish = 100;
		constexpr uint16_t Efreeti = 101;
		constexpr uint16_t FroglokTadpole = 102;
		constexpr uint16_t PhinigelAutropos = 103;
		constexpr uint16_t Leech = 104;
		constexpr uint16_t Swordfish = 105;
		constexpr uint16_t Felguard = 106;
		constexpr uint16_t Mammoth = 107;
		constexpr uint16_t EyeOfZomm = 108;
		constexpr uint16_t Wasp = 109;
		constexpr uint16_t Mermaid = 110;
		constexpr uint16_t Harpy = 111;
		constexpr uint16_t Fayguard = 112;
		constexpr uint16_t Drixie = 113;
		constexpr uint16_t GhostShip = 114;
		constexpr uint16_t Clam = 115;
		constexpr uint16_t SeaHorse = 116;
		constexpr uint16_t DwarfGhost = 117;
		constexpr uint16_t EruditeGhost = 118;
		constexpr uint16_t Sabertooth = 119;
		constexpr uint16_t WolfElemental = 120;
		constexpr uint16_t Gorgon = 121;
		constexpr uint16_t DragonSkeleton = 122;
		constexpr uint16_t Innoruuk = 123;
		constexpr uint16_t Unicorn = 124;
		constexpr uint16_t Pegasus = 125;
		constexpr uint16_t Djinn = 126;
		constexpr uint16_t InvisibleMan = 127;
		constexpr uint16_t Iksar = 128;
		constexpr uint16_t Scorpion = 129;
		constexpr uint16_t VahShir = 130;
		constexpr uint16_t Sarnak = 131;
		constexpr uint16_t Draglock = 132;
		constexpr uint16_t Drolvarg = 133;
		constexpr uint16_t Mosquito = 134;
		constexpr uint16_t Rhinoceros = 135;
		constexpr uint16_t Xalgoz = 136;
		constexpr uint16_t KunarkGoblin = 137;
		constexpr uint16_t Yeti = 138;
		constexpr uint16_t IksarCitizen = 139;
		constexpr uint16_t ForestGiant = 140;
		constexpr uint16_t Boat = 141;
		constexpr uint16_t MinorIllusion = 142;
		constexpr uint16_t Tree = 143;
		constexpr uint16_t Burynai = 144;
		constexpr uint16_t Goo = 145;
		constexpr uint16_t SarnakSpirit = 146;
		constexpr uint16_t IksarSpirit = 147;
		constexpr uint16_t KunarkFish = 148;
		constexpr uint16_t IksarScorpion = 149;
		constexpr uint16_t Erollisi = 150;
		constexpr uint16_t Tribunal = 151;
		constexpr uint16_t Bertoxxulous = 152;
		constexpr uint16_t Bristlebane = 153;
		constexpr uint16_t FayDrake = 154;
		constexpr uint16_t UndeadSarnak = 155;
		constexpr uint16_t Ratman = 156;
		constexpr uint16_t Wyvern = 157;
		constexpr uint16_t Wurm = 158;
		constexpr uint16_t Devourer = 159;
		constexpr uint16_t IksarGolem = 160;
		constexpr uint16_t UndeadIksar = 161;
		constexpr uint16_t ManEatingPlant = 162;
		constexpr uint16_t Raptor = 163;
		constexpr uint16_t SarnakGolem = 164;
		constexpr uint16_t WaterDragon = 165;
		constexpr uint16_t AnimatedHand = 166;
		constexpr uint16_t Succulent = 167;
		constexpr uint16_t Holgresh = 168;
		constexpr uint16_t Brontotherium = 169;
		constexpr uint16_t SnowDervish = 170;
		constexpr uint16_t DireWolf = 171;
		constexpr uint16_t Manticore = 172;
		constexpr uint16_t Totem = 173;
		constexpr uint16_t IceSpectre = 174;
		constexpr uint16_t EnchantedArmor = 175;
		constexpr uint16_t SnowRabbit = 176;
		constexpr uint16_t Walrus = 177;
		constexpr uint16_t Geonid = 178;
		constexpr uint16_t Unknown = 179;
		constexpr uint16_t Unknown2 = 180;
		constexpr uint16_t Yakkar = 181;
		constexpr uint16_t Faun = 182;
		constexpr uint16_t Coldain = 183;
		constexpr uint16_t VeliousDragon = 184;
		constexpr uint16_t Hag = 185;
		constexpr uint16_t Hippogriff = 186;
		constexpr uint16_t Siren = 187;
		constexpr uint16_t FrostGiant = 188;
		constexpr uint16_t StormGiant = 189;
		constexpr uint16_t Othmir = 190;
		constexpr uint16_t Ulthork = 191;
		constexpr uint16_t ClockworkDragon = 192;
		constexpr uint16_t Abhorrent = 193;
		constexpr uint16_t SeaTurtle = 194;
		constexpr uint16_t BlackAndWhiteDragon = 195;
		constexpr uint16_t GhostDragon = 196;
		constexpr uint16_t RonnieTest = 197;
		constexpr uint16_t PrismaticDragon = 198;
		constexpr uint16_t Shiknar = 199;
		constexpr uint16_t Rockhopper = 200;
		constexpr uint16_t Underbulk = 201;
		constexpr uint16_t Grimling = 202;
		constexpr uint16_t Worm = 203;
		constexpr uint16_t EvanTest = 204;
		constexpr uint16_t KhatiSha = 205;
		constexpr uint16_t Owlbear = 206;
		constexpr uint16_t RhinoBeetle = 207;
		constexpr uint16_t Vampire2 = 208;
		constexpr uint16_t EarthElemental = 209;
		constexpr uint16_t AirElemental = 210;
		constexpr uint16_t WaterElemental = 211;
		constexpr uint16_t FireElemental = 212;
		constexpr uint16_t WetfangMinnow = 213;
		constexpr uint16_t ThoughtHorror = 214;
		constexpr uint16_t Tegi = 215;
		constexpr uint16_t Horse = 216;
		constexpr uint16_t Shissar = 217;
		constexpr uint16_t FungalFiend = 218;
		constexpr uint16_t VampireVolatalis = 219;
		constexpr uint16_t Stonegrabber = 220;
		constexpr uint16_t ScarletCheetah = 221;
		constexpr uint16_t Zelniak = 222;
		constexpr uint16_t Lightcrawler = 223;
		constexpr uint16_t Shade = 224;
		constexpr uint16_t Sunflower = 225;
		constexpr uint16_t Shadel = 226;
		constexpr uint16_t Shrieker = 227;
		constexpr uint16_t Galorian = 228;
		constexpr uint16_t Netherbian = 229;
		constexpr uint16_t Akhevan = 230;
		constexpr uint16_t GriegVeneficus = 231;
		constexpr uint16_t SonicWolf = 232;
		constexpr uint16_t GroundShaker = 233;
		constexpr uint16_t VahShirSkeleton = 234;
		constexpr uint16_t Wretch = 235;
		constexpr uint16_t LordInquisitorSeru = 236;
		constexpr uint16_t Recuso = 237;
		constexpr uint16_t VahShirKing = 238;
		constexpr uint16_t VahShirGuard = 239;
		constexpr uint16_t TeleportMan = 240;
		constexpr uint16_t Werewolf2 = 241;
		constexpr uint16_t Nymph = 242;
		constexpr uint16_t Dryad = 243;
		constexpr uint16_t Treant2 = 244;
		constexpr uint16_t Fly = 245;
		constexpr uint16_t TarewMarr = 246;
		constexpr uint16_t SolusekRo2 = 247;
		constexpr uint16_t ClockworkGolem = 248;
		constexpr uint16_t ClockworkBrain = 249;
		constexpr uint16_t Banshee = 250;
		constexpr uint16_t GuardOfJustice = 251;
		constexpr uint16_t MiniPom = 252;
		constexpr uint16_t DiseasedFiend = 253;
		constexpr uint16_t SolusekRoGuard = 254;
		constexpr uint16_t BertoxxulousNew = 255;
		constexpr uint16_t TribunalNew = 256;
		constexpr uint16_t TerrisThule = 257;
		constexpr uint16_t Vegerog = 258;
		constexpr uint16_t Crocodile = 259;
		constexpr uint16_t Bat = 260;
		constexpr uint16_t Hraquis = 261;
		constexpr uint16_t Tranquilion = 262;
		constexpr uint16_t TinSoldier = 263;
		constexpr uint16_t NightmareWraith = 264;
		constexpr uint16_t Malarian = 265;
		constexpr uint16_t KnightOfPestilence = 266;
		constexpr uint16_t Lepertoloth = 267;
		constexpr uint16_t Bubonian = 268;
		constexpr uint16_t BubonianUnderling = 269;
		constexpr uint16_t Pusling = 270;
		constexpr uint16_t WaterMephit = 271;
		constexpr uint16_t Stormrider = 272;
		constexpr uint16_t JunkBeast = 273;
		constexpr uint16_t BrokenClockwork = 274;
		constexpr uint16_t GiantClockwork = 275;
		constexpr uint16_t ClockworkBeetle = 276;
		constexpr uint16_t NightmareGoblin = 277;
		constexpr uint16_t Karana = 278;
		constexpr uint16_t BloodRaven = 279;
		constexpr uint16_t NightmareGargoyle = 280;
		constexpr uint16_t MouthOfInsanity = 281;
		constexpr uint16_t SkeletalHorse = 282;
		constexpr uint16_t Saryrn = 283;
		constexpr uint16_t FenninRo = 284;
		constexpr uint16_t Tormentor = 285;
		constexpr uint16_t SoulDevourer = 286;
		constexpr uint16_t Nightmare = 287;
		constexpr uint16_t NewRallosZek = 288;
		constexpr uint16_t VallonZek = 289;
		constexpr uint16_t TallonZek = 290;
		constexpr uint16_t AirMephit = 291;
		constexpr uint16_t EarthMephit = 292;
		constexpr uint16_t FireMephit = 293;
		constexpr uint16_t NightmareMephit = 294;
		constexpr uint16_t Zebuxoruk = 295;
		constexpr uint16_t MithanielMarr = 296;
		constexpr uint16_t UndeadKnight = 297;
		constexpr uint16_t Rathe = 298;
		constexpr uint16_t Xegony = 299;
		constexpr uint16_t Fiend = 300;
		constexpr uint16_t TestObject = 301;
		constexpr uint16_t Crab = 302;
		constexpr uint16_t Phoenix = 303;
		constexpr uint16_t Quarm = 304;
		constexpr uint16_t Bear2 = 305;
		constexpr uint16_t EarthGolem = 306;
		constexpr uint16_t IronGolem = 307;
		constexpr uint16_t StormGolem = 308;
		constexpr uint16_t AirGolem = 309;
		constexpr uint16_t WoodGolem = 310;
		constexpr uint16_t FireGolem = 311;
		constexpr uint16_t WaterGolem = 312;
		constexpr uint16_t WarWraith = 313;
		constexpr uint16_t Wrulon = 314;
		constexpr uint16_t Kraken = 315;
		constexpr uint16_t PoisonFrog = 316;
		constexpr uint16_t Nilborien = 317;
		constexpr uint16_t Valorian = 318;
		constexpr uint16_t WarBoar = 319;
		constexpr uint16_t Efreeti2 = 320;
		constexpr uint16_t WarBoar2 = 321;
		constexpr uint16_t Valorian2 = 322;
		constexpr uint16_t AnimatedArmor = 323;
		constexpr uint16_t UndeadFootman = 324;
		constexpr uint16_t RallosOgre = 325;
		constexpr uint16_t Arachnid = 326;
		constexpr uint16_t CrystalSpider = 327;
		constexpr uint16_t ZebuxoruksCage = 328;
		constexpr uint16_t Portal = 329;
		constexpr uint16_t Froglok2 = 330;
		constexpr uint16_t TrollCrewMember = 331;
		constexpr uint16_t PirateDeckhand = 332;
		constexpr uint16_t BrokenSkullPirate = 333;
		constexpr uint16_t PirateGhost = 334;
		constexpr uint16_t OneArmedPirate = 335;
		constexpr uint16_t SpiritmasterNadox = 336;
		constexpr uint16_t BrokenSkullTaskmaster = 337;
		constexpr uint16_t GnomePirate = 338;
		constexpr uint16_t DarkElfPirate = 339;
		constexpr uint16_t OgrePirate = 340;
		constexpr uint16_t HumanPirate = 341;
		constexpr uint16_t EruditePirate = 342;
		constexpr uint16_t Frog = 343;
		constexpr uint16_t TrollZombie = 344;
		constexpr uint16_t Luggald = 345;
		constexpr uint16_t Luggald2 = 346;
		constexpr uint16_t Luggald3 = 347;
		constexpr uint16_t Drogmor = 348;
		constexpr uint16_t FroglokSkeleton = 349;
		constexpr uint16_t UndeadFroglok = 350;
		constexpr uint16_t KnightOfHate = 351;
		constexpr uint16_t ArcanistOfHate = 352;
		constexpr uint16_t Veksar = 353;
		constexpr uint16_t Veksar2 = 354;
		constexpr uint16_t Veksar3 = 355;
		constexpr uint16_t Chokidai = 356;
		constexpr uint16_t UndeadChokidai = 357;
		constexpr uint16_t UndeadVeksar = 358;
		constexpr uint16_t UndeadVampire = 359;
		constexpr uint16_t Vampire3 = 360;
		constexpr uint16_t RujarkianOrc = 361;
		constexpr uint16_t BoneGolem = 362;
		constexpr uint16_t Synarcana = 363;
		constexpr uint16_t SandElf = 364;
		constexpr uint16_t MasterVampire = 365;
		constexpr uint16_t MasterOrc = 366;
		constexpr uint16_t Skeleton2 = 367;
		constexpr uint16_t Mummy = 368;
		constexpr uint16_t NewGoblin = 369;
		constexpr uint16_t Insect = 370;
		constexpr uint16_t FroglokGhost = 371;
		constexpr uint16_t Dervish2 = 372;
		constexpr uint16_t Shade2 = 373;
		constexpr uint16_t Golem2 = 374;
		constexpr uint16_t EvilEye2 = 375;
		constexpr uint16_t Box = 376;
		constexpr uint16_t Barrel = 377;
		constexpr uint16_t Chest = 378;
		constexpr uint16_t Vase = 379;
		constexpr uint16_t Table = 380;
		constexpr uint16_t WeaponRack = 381;
		constexpr uint16_t Coffin = 382;
		constexpr uint16_t Bones = 383;
		constexpr uint16_t Jokester = 384;
		constexpr uint16_t Nihil = 385;
		constexpr uint16_t Trusik = 386;
		constexpr uint16_t StoneWorker = 387;
		constexpr uint16_t Hynid = 388;
		constexpr uint16_t Turepta = 389;
		constexpr uint16_t Cragbeast = 390;
		constexpr uint16_t Stonemite = 391;
		constexpr uint16_t Ukun = 392;
		constexpr uint16_t Ixt = 393;
		constexpr uint16_t Ikaav = 394;
		constexpr uint16_t Aneuk = 395;
		constexpr uint16_t Kyv = 396;
		constexpr uint16_t Noc = 397;
		constexpr uint16_t Ratuk = 398;
		constexpr uint16_t Taneth = 399;
		constexpr uint16_t Huvul = 400;
		constexpr uint16_t Mutna = 401;
		constexpr uint16_t Mastruq = 402;
		constexpr uint16_t Taelosian = 403;
		constexpr uint16_t DiscordShip = 404;
		constexpr uint16_t StoneWorker2 = 405;
		constexpr uint16_t MataMuram = 406;
		constexpr uint16_t LightingWarrior = 407;
		constexpr uint16_t Succubus = 408;
		constexpr uint16_t Bazu = 409;
		constexpr uint16_t Feran = 410;
		constexpr uint16_t Pyrilen = 411;
		constexpr uint16_t Chimera = 412;
		constexpr uint16_t Dragorn = 413;
		constexpr uint16_t Murkglider = 414;
		constexpr uint16_t Rat = 415;
		constexpr uint16_t Bat2 = 416;
		constexpr uint16_t Gelidran = 417;
		constexpr uint16_t Discordling = 418;
		constexpr uint16_t Girplan = 419;
		constexpr uint16_t Minotaur2 = 420;
		constexpr uint16_t DragornBox = 421;
		constexpr uint16_t RunedOrb = 422;
		constexpr uint16_t DragonBones = 423;
		constexpr uint16_t MuramiteArmorPile = 424;
		constexpr uint16_t CrystalShard = 425;
		constexpr uint16_t Portal2 = 426;
		constexpr uint16_t CoinPurse = 427;
		constexpr uint16_t RockPile = 428;
		constexpr uint16_t MurkgliderEggSack = 429;
		constexpr uint16_t Drake2 = 430;
		constexpr uint16_t Dervish3 = 431;
		constexpr uint16_t Drake3 = 432;
		constexpr uint16_t Goblin2 = 433;
		constexpr uint16_t Kirin = 434;
		constexpr uint16_t Dragon = 435;
		constexpr uint16_t Basilisk = 436;
		constexpr uint16_t Dragon2 = 437;
		constexpr uint16_t Dragon3 = 438;
		constexpr uint16_t Puma2 = 439;
		constexpr uint16_t Spider = 440;
		constexpr uint16_t SpiderQueen = 441;
		constexpr uint16_t AnimatedStatue = 442;
		constexpr uint16_t Unknown3 = 443;
		constexpr uint16_t Unknown4 = 444;
		constexpr uint16_t DragonEgg = 445;
		constexpr uint16_t DragonStatue = 446;
		constexpr uint16_t LavaRock = 447;
		constexpr uint16_t AnimatedStatue2 = 448;
		constexpr uint16_t SpiderEggSack = 449;
		constexpr uint16_t LavaSpider = 450;
		constexpr uint16_t LavaSpiderQueen = 451;
		constexpr uint16_t Dragon4 = 452;
		constexpr uint16_t Giant2 = 453;
		constexpr uint16_t Werewolf3 = 454;
		constexpr uint16_t Kobold2 = 455;
		constexpr uint16_t Sporali = 456;
		constexpr uint16_t Gnomework = 457;
		constexpr uint16_t Orc2 = 458;
		constexpr uint16_t Corathus = 459;
		constexpr uint16_t Coral = 460;
		constexpr uint16_t Drachnid2 = 461;
		constexpr uint16_t DrachnidCocoon = 462;
		constexpr uint16_t FungusPatch = 463;
		constexpr uint16_t Gargoyle2 = 464;
		constexpr uint16_t Witheran = 465;
		constexpr uint16_t DarkLord = 466;
		constexpr uint16_t Shiliskin = 467;
		constexpr uint16_t Snake = 468;
		constexpr uint16_t EvilEye3 = 469;
		constexpr uint16_t Minotaur3 = 470;
		constexpr uint16_t Zombie2 = 471;
		constexpr uint16_t ClockworkBoar = 472;
		constexpr uint16_t Fairy2 = 473;
		constexpr uint16_t Witheran2 = 474;
		constexpr uint16_t AirElemental2 = 475;
		constexpr uint16_t EarthElemental2 = 476;
		constexpr uint16_t FireElemental2 = 477;
		constexpr uint16_t WaterElemental2 = 478;
		constexpr uint16_t Alligator2 = 479;
		constexpr uint16_t Bear3 = 480;
		constexpr uint16_t ScaledWolf = 481;
		constexpr uint16_t Wolf2 = 482;
		constexpr uint16_t SpiritWolf = 483;
		constexpr uint16_t Skeleton3 = 484;
		constexpr uint16_t Spectre2 = 485;
		constexpr uint16_t Bolvirk = 486;
		constexpr uint16_t Banshee2 = 487;
		constexpr uint16_t Banshee3 = 488;
		constexpr uint16_t Elddar = 489;
		constexpr uint16_t ForestGiant2 = 490;
		constexpr uint16_t BoneGolem2 = 491;
		constexpr uint16_t Horse2 = 492;
		constexpr uint16_t Pegasus2 = 493;
		constexpr uint16_t ShamblingMound = 494;
		constexpr uint16_t Scrykin = 495;
		constexpr uint16_t Treant3 = 496;
		constexpr uint16_t Vampire4 = 497;
		constexpr uint16_t AyonaeRo = 498;
		constexpr uint16_t SullonZek = 499;
		constexpr uint16_t Banner = 500;
		constexpr uint16_t Flag = 501;
		constexpr uint16_t Rowboat = 502;
		constexpr uint16_t BearTrap = 503;
		constexpr uint16_t ClockworkBomb = 504;
		constexpr uint16_t DynamiteKeg = 505;
		constexpr uint16_t PressurePlate = 506;
		constexpr uint16_t PufferSpore = 507;
		constexpr uint16_t StoneRing = 508;
		constexpr uint16_t RootTentacle = 509;
		constexpr uint16_t RunicSymbol = 510;
		constexpr uint16_t SaltpetterBomb = 511;
		constexpr uint16_t FloatingSkull = 512;
		constexpr uint16_t SpikeTrap = 513;
		constexpr uint16_t Totem2 = 514;
		constexpr uint16_t Web = 515;
		constexpr uint16_t WickerBasket = 516;
		constexpr uint16_t Unicorn2 = 517;
		constexpr uint16_t Horse3 = 518;
		constexpr uint16_t Unicorn3 = 519;
		constexpr uint16_t Bixie2 = 520;
		constexpr uint16_t Centaur2 = 521;
		constexpr uint16_t Drakkin = 522;
		constexpr uint16_t Giant3 = 523;
		constexpr uint16_t Gnoll2 = 524;
		constexpr uint16_t Griffin2 = 525;
		constexpr uint16_t GiantShade = 526;
		constexpr uint16_t Harpy2 = 527;
		constexpr uint16_t Mammoth2 = 528;
		constexpr uint16_t Satyr = 529;
		constexpr uint16_t Dragon5 = 530;
		constexpr uint16_t Dragon6 = 531;
		constexpr uint16_t Dynleth = 532;
		constexpr uint16_t Boat2 = 533;
		constexpr uint16_t WeaponRack2 = 534;
		constexpr uint16_t ArmorRack = 535;
		constexpr uint16_t HoneyPot = 536;
		constexpr uint16_t JumJumBucket = 537;
		constexpr uint16_t Plant = 538;
		constexpr uint16_t StoneJug = 539;
		constexpr uint16_t Plant2 = 540;
		constexpr uint16_t Toolbox = 541;
		constexpr uint16_t WineCask = 542;
		constexpr uint16_t StoneJug2 = 543;
		constexpr uint16_t ElvenBoat = 544;
		constexpr uint16_t GnomishBoat = 545;
		constexpr uint16_t UndeadBoat = 546;
		constexpr uint16_t Goo2 = 547;
		constexpr uint16_t Goo3 = 548;
		constexpr uint16_t Goo4 = 549;
		constexpr uint16_t MerchantShip = 550;
		constexpr uint16_t PirateShip = 551;
		constexpr uint16_t GhostShip2 = 552;
		constexpr uint16_t Banner2 = 553;
		constexpr uint16_t Banner3 = 554;
		constexpr uint16_t Banner4 = 555;
		constexpr uint16_t Banner5 = 556;
		constexpr uint16_t Banner6 = 557;
		constexpr uint16_t Aviak2 = 558;
		constexpr uint16_t Beetle2 = 559;
		constexpr uint16_t Gorilla2 = 560;
		constexpr uint16_t Kedge = 561;
		constexpr uint16_t Kerran2 = 562;
		constexpr uint16_t Shissar2 = 563;
		constexpr uint16_t Siren2 = 564;
		constexpr uint16_t Sphinx2 = 565;
		constexpr uint16_t Human2 = 566;
		constexpr uint16_t Campfire = 567;
		constexpr uint16_t Brownie2 = 568;
		constexpr uint16_t Dragon7 = 569;
		constexpr uint16_t Exoskeleton = 570;
		constexpr uint16_t Ghoul2 = 571;
		constexpr uint16_t ClockworkGuardian = 572;
		constexpr uint16_t Unknown5 = 573;
		constexpr uint16_t Minotaur4 = 574;
		constexpr uint16_t Scarecrow2 = 575;
		constexpr uint16_t Shade3 = 576;
		constexpr uint16_t Rotocopter = 577;
		constexpr uint16_t TentacleTerror2 = 578;
		constexpr uint16_t Wereorc = 579;
		constexpr uint16_t Worg = 580;
		constexpr uint16_t Wyvern2 = 581;
		constexpr uint16_t Chimera2 = 582;
		constexpr uint16_t Kirin2 = 583;
		constexpr uint16_t Puma3 = 584;
		constexpr uint16_t Boulder = 585;
		constexpr uint16_t Banner7 = 586;
		constexpr uint16_t ElvenGhost = 587;
		constexpr uint16_t HumanGhost = 588;
		constexpr uint16_t Chest2 = 589;
		constexpr uint16_t Chest3 = 590;
		constexpr uint16_t Crystal = 591;
		constexpr uint16_t Coffin2 = 592;
		constexpr uint16_t GuardianCpu = 593;
		constexpr uint16_t Worg2 = 594;
		constexpr uint16_t Mansion = 595;
		constexpr uint16_t FloatingIsland = 596;
		constexpr uint16_t Cragslither = 597;
		constexpr uint16_t Wrulon2 = 598;
		constexpr uint16_t SpellParticle = 599;
		constexpr uint16_t InvisibleManOfZomm = 600;
		constexpr uint16_t RobocopterOfZomm = 601;
		constexpr uint16_t Burynai2 = 602;
		constexpr uint16_t Frog2 = 603;
		constexpr uint16_t Dracolich = 604;
		constexpr uint16_t IksarGhost = 605;
		constexpr uint16_t IksarSkeleton = 606;
		constexpr uint16_t Mephit = 607;
		constexpr uint16_t Muddite = 608;
		constexpr uint16_t Raptor2 = 609;
		constexpr uint16_t Sarnak2 = 610;
		constexpr uint16_t Scorpion2 = 611;
		constexpr uint16_t Tsetsian = 612;
		constexpr uint16_t Wurm2 = 613;
		constexpr uint16_t Nekhon = 614;
		constexpr uint16_t HydraCrystal = 615;
		constexpr uint16_t CrystalSphere = 616;
		constexpr uint16_t Gnoll3 = 617;
		constexpr uint16_t Sokokar = 618;
		constexpr uint16_t StonePylon = 619;
		constexpr uint16_t DemonVulture = 620;
		constexpr uint16_t Wagon = 621;
		constexpr uint16_t GodOfDiscord = 622;
		constexpr uint16_t FeranMount = 623;
		constexpr uint16_t Ogre2 = 624;
		constexpr uint16_t SokokarMount = 625;
		constexpr uint16_t Giant4 = 626;
		constexpr uint16_t SokokarMount2 = 627;
		constexpr uint16_t TenthAnniversaryBanner = 628;
		constexpr uint16_t TenthAnniversaryCake = 629;
		constexpr uint16_t WineCask2 = 630;
		constexpr uint16_t HydraMount = 631;
		constexpr uint16_t Hydra = 632;
		constexpr uint16_t WeddingFlowers = 633;
		constexpr uint16_t WeddingArbor = 634;
		constexpr uint16_t WeddingAltar = 635;
		constexpr uint16_t PowderKeg = 636;
		constexpr uint16_t Apexus = 637;
		constexpr uint16_t Bellikos = 638;
		constexpr uint16_t BrellsFirstCreation = 639;
		constexpr uint16_t Brell = 640;
		constexpr uint16_t CrystalskinAmbuloid = 641;
		constexpr uint16_t CliknarQueen = 642;
		constexpr uint16_t CliknarSoldier = 643;
		constexpr uint16_t CliknarWorker = 644;
		constexpr uint16_t Coldain2 = 645;
		constexpr uint16_t Coldain3 = 646;
		constexpr uint16_t CrystalskinSessiloid = 647;
		constexpr uint16_t Genari = 648;
		constexpr uint16_t Gigyn = 649;
		constexpr uint16_t GrekenYoungAdult = 650;
		constexpr uint16_t GrekenYoung = 651;
		constexpr uint16_t CliknarMount = 652;
		constexpr uint16_t Telmira = 653;
		constexpr uint16_t SpiderMount = 654;
		constexpr uint16_t BearMount = 655;
		constexpr uint16_t RatMount = 656;
		constexpr uint16_t SessiloidMount = 657;
		constexpr uint16_t MorellThule = 658;
		constexpr uint16_t Marionette = 659;
		constexpr uint16_t BookDervish = 660;
		constexpr uint16_t TopiaryLion = 661;
		constexpr uint16_t RotDog = 662;
		constexpr uint16_t Amygdalan = 663;
		constexpr uint16_t Sandman = 664;
		constexpr uint16_t GrandfatherClock = 665;
		constexpr uint16_t GingerbreadMan = 666;
		constexpr uint16_t RoyalGuard = 667;
		constexpr uint16_t Rabbit = 668;
		constexpr uint16_t BlindDreamer = 669;
		constexpr uint16_t CazicThule2 = 670;
		constexpr uint16_t TopiaryLionMount = 671;
		constexpr uint16_t RotDogMount = 672;
		constexpr uint16_t GoralMount = 673;
		constexpr uint16_t SelyrahMount = 674;
		constexpr uint16_t ScleraMount = 675;
		constexpr uint16_t BraxiMount = 676;
		constexpr uint16_t KangonMount = 677;
		constexpr uint16_t Erudite2 = 678;
		constexpr uint16_t WurmMount = 679;
		constexpr uint16_t RaptorMount = 680;
		constexpr uint16_t InvisibleMan2 = 681;
		constexpr uint16_t Whirligig = 682;
		constexpr uint16_t GnomishBalloon = 683;
		constexpr uint16_t GnomishRocketPack = 684;
		constexpr uint16_t GnomishHoveringTransport = 685;
		constexpr uint16_t Selyrah = 686;
		constexpr uint16_t Goral = 687;
		constexpr uint16_t Braxi = 688;
		constexpr uint16_t Kangon = 689;
		constexpr uint16_t InvisibleMan3 = 690;
		constexpr uint16_t FloatingTower = 691;
		constexpr uint16_t ExplosiveCart = 692;
		constexpr uint16_t BlimpShip = 693;
		constexpr uint16_t Tumbleweed = 694;
		constexpr uint16_t Alaran = 695;
		constexpr uint16_t Swinetor = 696;
		constexpr uint16_t Triumvirate = 697;
		constexpr uint16_t Hadal = 698;
		constexpr uint16_t HoveringPlatform = 699;
		constexpr uint16_t ParasiticScavenger = 700;
		constexpr uint16_t Grendlaen = 701;
		constexpr uint16_t ShipInABottle = 702;
		constexpr uint16_t AlaranSentryStone = 703;
		constexpr uint16_t Dervish4 = 704;
		constexpr uint16_t RegenerationPool = 705;
		constexpr uint16_t TeleportationStand = 706;
		constexpr uint16_t RelicCase = 707;
		constexpr uint16_t AlaranGhost = 708;
		constexpr uint16_t Skystrider = 709;
		constexpr uint16_t WaterSpout = 710;
		constexpr uint16_t AviakPullAlong = 711;
		constexpr uint16_t GelatinousCube2 = 712;
		constexpr uint16_t Cat = 713;
		constexpr uint16_t ElkHead = 714;
		constexpr uint16_t Holgresh2 = 715;
		constexpr uint16_t Beetle3 = 716;
		constexpr uint16_t VineMaw = 717;
		constexpr uint16_t Ratman2 = 718;
		constexpr uint16_t FallenKnight = 719;
		constexpr uint16_t FlyingCarpet = 720;
		constexpr uint16_t CarrierHand = 721;
		constexpr uint16_t Akheva = 722;
		constexpr uint16_t ServantOfShadow = 723;
		constexpr uint16_t Luclin = 724;
		constexpr uint16_t Xaric = 725;
		constexpr uint16_t Dervish5 = 726;
		constexpr uint16_t Dervish6 = 727;
		constexpr uint16_t Luclin2 = 728;
		constexpr uint16_t Luclin3 = 729;
		constexpr uint16_t Orb = 730;
		constexpr uint16_t Luclin4 = 731;
		constexpr uint16_t Pegasus3 = 732;
		constexpr uint16_t InteractiveObject = 2250;
		constexpr uint16_t Node = 2254;
	}

	//needed for plugins for backward mq functionality
	#define CONCOLOR_GREY                            0x06
	#define CONCOLOR_GREEN                           0x02
	#define CONCOLOR_LIGHTBLUE                       0x12
	#define CONCOLOR_BLUE                            0x04
	#define CONCOLOR_BLACK                           0x14
	#define CONCOLOR_WHITE                           0x0a
	#define CONCOLOR_YELLOW                          0x0f
	#define CONCOLOR_RED                             0x0d
	#define STANDSTATE_STAND                         0x64
	#define STANDSTATE_CASTING                       0x66
	#define STANDSTATE_BIND                          0x69
	#define STANDSTATE_SIT                           0x6E
	#define STANDSTATE_DUCK                          0x6F
	#define STANDSTATE_FEIGN                         0x73
	#define STANDSTATE_DEAD                          0x78
	#define SPAWN_PLAYER                             0
	#define SPAWN_NPC                                1
	#define SPAWN_CORPSE                             2
	//also needed to get the string body type that MQ provided
	constexpr std::string_view szBodyType[] = {
		"Object",                //  0    not a real body type. (catapults, tents, practice dummies, etc)
		"Humanoid",              //  1
		"Lycanthrope",           //  2
		"Undead",                //  3
		"Giant",                 //  4
		"Construct",             //  5
		"Extraplanar",           //  6
		"Magical",               //  7
		"Undead Pet",            //  8
		"Bane Giant",            //  9
		"Dain",                  // 10
		"Untargetable",          // 11
		"Vampyre",               // 12
		"Atenha Ra",             // 13
		"Greater Akheva",        // 14
		"Khati Sha",             // 15
		"Seru",                  // 16
		"Greig",                 // 17
		"Draz Nurakk",           // 18
		"Zek",                   // 19
		"Luggald",               // 20
		"Animal",                // 21
		"Insect",                // 22
		"Monster",               // 23
		"Elemental",             // 24
		"Plant",                 // 25
		"Dragon",                // 26
		"Elemental",             // 27
		"Summoned Creature",     // 28
		"Puff Dragon",           // 29
		"Bane Dragon",           // 30
		"Familiar",              // 31
		"No Corpse",             // 32
		"Cursed",                // 33
		"Muramite",              // 34
		"*UNKNOWN BODYTYPE 35",
		"*UNKNOWN BODYTYPE 36",
		"*UNKNOWN BODYTYPE 37",
		"*UNKNOWN BODYTYPE 38",
		"*UNKNOWN BODYTYPE 39",
		"*UNKNOWN BODYTYPE 40",
		"*UNKNOWN BODYTYPE 41",
		"*UNKNOWN BODYTYPE 42",
		"*UNKNOWN BODYTYPE 43",
		"*UNKNOWN BODYTYPE 44",
		"*UNKNOWN BODYTYPE 45",
		"*UNKNOWN BODYTYPE 46",
		"*UNKNOWN BODYTYPE 47",
		"*UNKNOWN BODYTYPE 48",
		"*UNKNOWN BODYTYPE 49",
		"*UNKNOWN BODYTYPE 50",
		"*UNKNOWN BODYTYPE 51",
		"*UNKNOWN BODYTYPE 52",
		"*UNKNOWN BODYTYPE 53",
		"*UNKNOWN BODYTYPE 54",
		"*UNKNOWN BODYTYPE 55",
		"*UNKNOWN BODYTYPE 56",
		"*UNKNOWN BODYTYPE 57",
		"*UNKNOWN BODYTYPE 58",
		"*UNKNOWN BODYTYPE 59",
		"*UNKNOWN BODYTYPE 60",
		"*UNKNOWN BODYTYPE 61",
		"*UNKNOWN BODYTYPE 62",
		"*UNKNOWN BODYTYPE 63",
		"Monster Summoning",     // 64
		"*UNKNOWN BODYTYPE 65",
		"*UNKNOWN BODYTYPE 66",
		"*UNKNOWN BODYTYPE 67",
		"*UNKNOWN BODYTYPE 68",
		"*UNKNOWN BODYTYPE 69",
		"*UNKNOWN BODYTYPE 70",
		"*UNKNOWN BODYTYPE 71",
		"*UNKNOWN BODYTYPE 72",
		"*UNKNOWN BODYTYPE 73",
		"*UNKNOWN BODYTYPE 74",
		"*UNKNOWN BODYTYPE 75",
		"*UNKNOWN BODYTYPE 76",
		"*UNKNOWN BODYTYPE 77",
		"*UNKNOWN BODYTYPE 78",
		"*UNKNOWN BODYTYPE 79",
		"*UNKNOWN BODYTYPE 80",
		"*UNKNOWN BODYTYPE 81",
		"*UNKNOWN BODYTYPE 82",
		"*UNKNOWN BODYTYPE 83",
		"*UNKNOWN BODYTYPE 84",
		"*UNKNOWN BODYTYPE 85",
		"*UNKNOWN BODYTYPE 86",
		"*UNKNOWN BODYTYPE 87",
		"*UNKNOWN BODYTYPE 88",
		"*UNKNOWN BODYTYPE 89",
		"*UNKNOWN BODYTYPE 90",
		"*UNKNOWN BODYTYPE 91",
		"*UNKNOWN BODYTYPE 92",
		"*UNKNOWN BODYTYPE 93",
		"*UNKNOWN BODYTYPE 94",
		"*UNKNOWN BODYTYPE 95",
		"*UNKNOWN BODYTYPE 96",
		"*UNKNOWN BODYTYPE 97",
		"*UNKNOWN BODYTYPE 98",
		"*UNKNOWN BODYTYPE 99",  // Seen in characterpropertysets.txt
		"Utility",               // 100
		"Property Trap",         // 101
		"Property Companion",    // 102
		"Property Suicide",      // 103
		"*UNKNOWN BODYTYPE 104", // Seen on player pets
	};
	enum eSpawnType
	{
		NONE = 0,
		PC,
		MOUNT,
		PET,
		PCPET,
		NPCPET,
		XTARHATER,
		NPC,
		CORPSE,
		TRIGGER,
		TRAP,
		TIMER,
		UNTARGETABLE,
		CHEST,
		ITEM,
		AURA,
		OBJECT,
		BANNER,
		CAMPFIRE,
		MERCENARY,
		FLYER,
		NPCCORPSE = 2000,
		PCCORPSE,
	};
	enum CharacterProperty
	{
		CharacterProperty_None = 0,
		CharacterProperty_Humanoid = 1,
		CharacterProperty_Lycanthrope = 2, // Deprecated
		CharacterProperty_Undead = 3,
		CharacterProperty_Giant = 4,
		CharacterProperty_Construct = 5,
		CharacterProperty_Extraplanar = 6,
		CharacterProperty_Magical = 7,
		CharacterProperty_SummonedUndead = 8,  // now: Summoned Pet
		CharacterProperty_BaneGiant = 9, // Deprecated
		CharacterProperty_BaneColdain = 10, // Deprecated
		CharacterProperty_Untargetable = 11,
		CharacterProperty_Vampire = 12, // Deprecated
		CharacterProperty_AtenhaRa = 13, // Deprecated
		CharacterProperty_GreaterAkheva = 14,
		CharacterProperty_KhatiSha = 15, // Deprecated
		CharacterProperty_Seru = 16, // Deprecated
		CharacterProperty_Greig = 17, // Deprecated
		CharacterProperty_DrazNurakk = 18,
		CharacterProperty_Zek = 19,
		CharacterProperty_Luggald = 20, // Deprecated
		CharacterProperty_Animal = 21,
		CharacterProperty_Insect = 22, // Deprecated
		CharacterProperty_Monster = 23, // Deprecated
		CharacterProperty_Elemental = 24, // Deprecated
		CharacterProperty_Plant = 25,
		CharacterProperty_Dragon = 26,
		CharacterProperty_SummonedElemental = 27, // Deprecated
		CharacterProperty_SummonedCreature = 28, // Deprecated
		CharacterProperty_PuffDragon = 29,
		CharacterProperty_BaneDragon = 30, // Deprecated
		CharacterProperty_Familiar = 31,
		CharacterProperty_NoCorpse = 32,
		CharacterProperty_Cursed = 33,
		CharacterProperty_Muramite = 34, // Deprecated
		CharacterProperty_MonsterSummoning = 64,

		CharacterProperty_Utility = 100,
		CharacterProperty_Trap,
		CharacterProperty_Companion,
		CharacterProperty_Suicide,

		CharacterProperty_Last,
	};
	enum PlayerClass
	{
		Unknown = 0,
		Warrior = 1,
		Cleric = 2,
		Paladin = 3,
		Ranger = 4,
		Shadowknight = 5,
		Druid = 6,
		Monk = 7,
		Bard = 8,
		Rogue = 9,
		Shaman = 10,
		Necromancer = 11,
		Wizard = 12,
		Mage = 13,
		Magician = 13,
		Enchanter = 14,
		Beastlord = 15,
		Berserker = 16,

		Mercenary = 17,

		GM_Warrior = 20,
		GM_Cleric = 21,
		GM_Paladin = 22,
		GM_Ranger = 23,
		GM_Shadowknight = 24,
		GM_Druid = 25,
		GM_Monk = 26,
		GM_Bard = 27,
		GM_Rogue = 28,
		GM_Shaman = 29,
		GM_Necromancer = 30,
		GM_Wizard = 31,
		GM_Magician = 32,
		GM_Enchanter = 33,
		GM_Beastlord = 34,
		GM_Berserker = 35,
		GM_First = GM_Warrior,
		GM_Last = GM_Berserker,

		Class_Banker = 40,
		Class_Merchant = 41,
		Class_TaskMerchant = 58,
		Class_PvPMerchant = 59,
		Class_Adventure = 60,
		Class_AdventureMerchant = 61,
		Class_Object = 62,
		Class_TributeMaster = 63,
		Class_GuildTributeMaster = 64,

		Class_GuildBanker = 66,
		Class_GoodPointMerchant = 67,
		Class_EvilPointMerchant = 68,
		Class_FellowshipMaster = 69,
		Class_PointMerchant = 70,
		Class_MercenaryMerchant = 71,
		Class_RealEstateMerchant = 72,
		Class_LoyaltyMerchant = 73,
		Class_TributeMaster2 = 74,

		TotalPlayerClasses = Berserker,
		TotalClasses = TotalPlayerClasses + 1,

		// Special MQ-only values
		MQClass_CampFire = 253,
		MQClass_Banner = 254,
		MQClass_Aura = 255,
	};
	// player races
	constexpr int EQR_HUMAN = 1;
	constexpr int EQR_BARBARIAN = 2;
	constexpr int EQR_ERUDITE = 3;
	constexpr int EQR_ELF = 4;
	constexpr int EQR_HIGH_ELF = 5;
	constexpr int EQR_DARK_ELF = 6;
	constexpr int EQR_HALF_ELF = 7;
	constexpr int EQR_DWARF = 8;
	constexpr int EQR_TROLL = 9;
	constexpr int EQR_OGRE = 10;
	constexpr int EQR_HALFLING = 11;
	constexpr int EQR_GNOME = 12;
	constexpr int EQR_SKELETON = 60;
	constexpr int EQR_INVISIBLE_MAN = 127;
	constexpr int EQR_IKSAR = 128;
	constexpr int EQR_VAHSHIR = 130;
	constexpr int EQR_FROGLOCK = 330;
	constexpr int EQR_SKELETON_NEW = 367;
	constexpr int EQR_BANNER = 500;
	constexpr int EQR_SPIKE_TRAP = 513;
	constexpr int EQR_TOTEM = 514;
	constexpr int EQR_DRAKKIN = 522;
	constexpr int EQR_BANNER0 = 553;
	constexpr int EQR_BANNER1 = 554;
	constexpr int EQR_BANNER2 = 555;
	constexpr int EQR_BANNER3 = 556;
	constexpr int EQR_BANNER4 = 557;

	constexpr int EQR_CAMPSITE = 567;
	constexpr int EQR_TCGBANNER = 586;
	constexpr int EQR_BANNER_10TH_ANNIV = 628;
	constexpr int EQR_OEQ_SKELETON = 802;
	constexpr int EQR_SOL_SKELETON = 803;
	struct Spawn {
		std::uint32_t id = 0;
		std::uint64_t incarnation = 0; // connection-issued identity of the full spawn record, independent of its numeric id
		std::string name;      // as sent (e.g. "a_rat01"); see display_name()
		std::string last_name;
		std::string title, suffix; // the spawn record's AA title prefix and suffix (OP_SetTitleReply changes them)
		std::uint16_t race = 0;
		std::uint8_t class_id = 0;
		std::uint8_t level = 0;
		std::uint8_t gender = 0;
		bool is_npc = false;
		bool is_corpse = false;
		float size = 0.0f;
		float melee_radius = 0.0f; // the spawn record's MeleeRadius (PlayerClient +0x105c; EQEmu: size - 0.7)
		glm::vec3 position{ 0.0f };  // world (zone file) coordinates; see game/coords.h
		float heading = 0.0f;      // EQ heading units, 0..512 (game/coords.h)
		// The original's physics block between updates: an update snaps position
		// and heading and sets these; WorldState::smooth() steps them each frame
		// (mechanics::step_remote_spawn) for 12 s after the update.
		glm::vec3 velocity{ 0.0f };    // world units per physics tick (50 ms), from the update's deltas
		float speed_run = 0.0f;      // the update's animation field x 0.025
		// Last completed StepClock time since app start + 1; absent until this spawn incarnation steps.
		std::optional<std::uint32_t> physics_step_ms;
		float speed_heading = 0.0f;  // heading units per tick
		double update_time = 0.0;    // when position was last set (seconds, steady clock)
		float snap_error = 0.0f;     // how far the last update moved the stepped position (diagnostics)
		// The spawn record's flymode, then SpawnAppearance 19 (PlayerPhysics_SetFlyMode): 0 ground, 1 flying,
		// 2 levitating, 3 water, 4 floating, 5 levitating while running. The local player's own mode is worked out
		// by PlayerMovement (mech::local_fly_mode); `fly_mode_sets` counts the type-19 updates so that one setting
		// the same value again is still seen.
		std::uint8_t fly_mode = 0;
		std::uint32_t fly_mode_sets = 0;
		// The region types under it (+0xa1 FeetWet, +0xa2 BodyWet: 0 dry, 5 water, ...), probed with each step
		// (WorldState::smooth's `region`); the local player's come from its own controller.
		std::uint8_t feet_wet = 0, body_wet = 0;
		std::int32_t colliding_type = 0; // movement sweep's actor type (+0x24); 6 enables climbing animation
		// EQPlayer::SetHeights' results (game/spawn_heights.h): heights.avatar_height is the spawn's z above its
		// feet, heights.model_scale its actor's scale. WorldState::set_heights keeps them.
		SpawnHeights heights;
		bool actor_made = false; // CreatePlayerActor ran (its height fix-up; WorldState::make_actors)
		// What the actor was made with (WorldState::ActorModel): a tunic point (the definition's +0x34), and the
		// HUM / HUF fallback model. Riding asks both (game/mounts.h).
		bool tunic_point = false;
		bool human_actor = false;
		// Riding (game/mounts.h): EQPlayer::GetMount (+0x154) and PlayerBase_GetRider (+0x158) as spawn ids, 0 none.
		std::uint32_t mount_id = 0;
		std::uint32_t rider_id = 0;
		std::uint32_t form_mount_id = 0; // re-seat after a form change rebuilds this actor
		float walk_speed = 0.0f, run_speed = 0.0f; // the spawn record's walkspeed / runspeed (+0x330 / +0x350)
		std::uint8_t hp_percent = 100;
		std::uint8_t stand_state = 100; // SpawnAppearance animation: 100 stand, 110 sit, 111 crouch, 115 lying
		bool targetable = true;         // spawn bitfield bit 27
		bool cyclable = true;           // bit 28 (targetable_with_hotkey; PlayerZoneClient +0x161 bTargetCyclable)
		bool show_name = true;          // bit 29 (PlayerClient +0x1228 bDisplayNameSprite)
		bool click_through = false;     // bit 31: bClickThrough (+0x162), mask bit 8 (game/clicks.h)
		bool mercenary = false;         // the spawn record's IsMercenary (+0x208)
		std::uint32_t body_type = 0;
		// The spawn record's properties (PlayerBase +0x128, PlayerBase::HasProperty's list; [0] is body_type).
		std::vector<std::uint32_t> properties;
		bool summoned = false;          // bSummoned (+0x2e0): the spawn bitfield's bit 2 (EQEmu is_pet)
		std::uint32_t pet_owner_id = 0; // MasterID: the spawn record's petOwnerId, SpawnAppearance 25 (pet)
		// Social flags: the spawn record, then SpawnAppearance 21 (anonymous: 1
		// anonymous, 2 roleplaying), 22 / 23 (guild id / rank), 24 (AFK), 18 (linkdead).
		std::uint32_t guild_id = 0xffffffff;
		std::uint32_t guild_rank = 0;
		// bGuildShowAnim (+0x1f04): the spawn record's guild_show, then SpawnAppearance 52 (EQEmu: the rank's permission
		// 4, Display Guild Name). The name sprite shows the guild only with it.
		bool guild_show = false;
		std::uint8_t anon = 0;
		bool afk = false, lfg = false, linkdead = false, gm = false;
        std::int32_t invis_rank = 0; // spawn HideMode, then SpawnAppearance 3
        std::uint8_t light_type = 0; // active light from the spawn record and SpawnAppearance 5
		std::uint8_t pvp = 0;      // PvPFlag +0x349 (the spawn record)
		std::uint32_t deity = 0;   // +0x518 (the spawn record)
		// Bazaar flags: the spawn record's trader bit and OP_BecomeTrader (PlayerClient +800), the
		// buyer bit (other_data bit 0) and OP_Barter's buyer appearance.
		bool trader = false, buyer = false;
		// bOfflineMode (+0x194): the spawn record's other_data bit 1, then SpawnAppearance 53 (an offline trader or buyer).
		bool offline = false;
		// PlayerClient +0x14c: the spawn record's PlayerState, changed by OP_PlayerStateAdd / Remove (bit 0x2 sheathed,
		// /sheathe; 0x8 aggressive, /aggressive).
		std::uint32_t player_state = 0;
		Appearance appearance;

		// The last one-shot animation played on it (OP_Animation, or one the
		// client starts): an old-style number (game/combat.h animation_code) or
		// a new-style code; action_seq counts up with every packet, so a repeat
		// restarts the clip. action_speed is the packet's speed byte (the clip
		// plays at speed / 10).
		std::uint8_t action = 0;
		std::uint8_t action_speed = 10;
		std::uint32_t action_seq = 0;

		// Where to draw it: eases toward position/heading so the corrections an
		// update makes do not show as jumps. Set by WorldState::smooth().
		glm::vec3 display_position{ 0.0f };
		float display_heading = 0.0f;
		bool display_valid = false;
		// Hailstone (WorldState::ground_npcs): how far below its position the spawn is drawn, so one the server left
		// standing in the air is drawn on the floor; and the position that was measured at (NaN: not yet).
		float ground_drop = 0.0f;
		glm::vec3 ground_probe_at{ std::numeric_limits<float>::quiet_NaN() };
		// A vehicle (game/boats.h): how far the drawn pose is from the stepped one; it fades after each update.
		glm::vec3 display_error{ 0.0f };
		float display_heading_error = 0.0f;
		// A spawn standing on a vehicle (game/boats.h riders): the vehicle, its stepped and drawn place and heading in
		// the vehicle's frame, and the update the deck test last ran for.
		std::uint32_t on_vehicle = 0;
		glm::vec3 vehicle_local{ 0.0f }, vehicle_display_local{ 0.0f };
		float vehicle_local_heading = 0.0f, vehicle_display_local_heading = 0.0f;
		double vehicle_checked = -1.0;
		// Copied from MacroQuest, with the race tables above (THIRD_PARTY.md).
		static bool IsNamed(const game::Spawn* pSpawn)
		{
			if (pSpawn)
			{
				if (game::Spawn::GetSpawnType(pSpawn) != hailstone::game::eSpawnType::NPC)
					return false;
				if (!pSpawn->targetable)
					return false;
				if (pSpawn->class_id >= hailstone::game::PlayerClass::GM_First && pSpawn->class_id <= hailstone::game::PlayerClass::GM_Last)
					return false;

				switch (pSpawn->class_id)
				{
				case hailstone::game::PlayerClass::Class_Banker:
				case hailstone::game::PlayerClass::Class_Merchant:
				case hailstone::game::PlayerClass::Class_PointMerchant:
				case hailstone::game::PlayerClass::Class_Adventure:
				case hailstone::game::PlayerClass::Class_AdventureMerchant:
				case hailstone::game::PlayerClass::Class_Object:
				case hailstone::game::PlayerClass::Class_TributeMaster:
				case hailstone::game::PlayerClass::Class_GuildTributeMaster:
				case hailstone::game::PlayerClass::Class_TributeMaster2:
				case hailstone::game::PlayerClass::Class_GuildBanker:
				case hailstone::game::PlayerClass::Class_GoodPointMerchant:
				case hailstone::game::PlayerClass::Class_EvilPointMerchant:
				case hailstone::game::PlayerClass::Class_FellowshipMaster:
				case hailstone::game::PlayerClass::Class_MercenaryMerchant:
				case hailstone::game::PlayerClass::Class_LoyaltyMerchant:
				case hailstone::game::PlayerClass::Class_RealEstateMerchant:
				case hailstone::game::PlayerClass::Class_TaskMerchant:
				case hailstone::game::PlayerClass::Class_PvPMerchant:
					return false;
				}

				// The name's first space-separated word (spawn names use '_' for spaces, so usually all of it).
				const std::string_view name = pSpawn->name;
				const std::size_t first = name.find_first_not_of(' ');
				if (first != std::string_view::npos)
				{
					const std::string_view Cmd = name.substr(first, name.find(' ', first) - first);
					// Checking for mobs that have 'A' or 'An' as their first name
					if (Cmd[0] == 'A')
					{
						if (Cmd.size() > 1 && Cmd[1] == '_')
							return false;
						else if (Cmd.size() > 2 && Cmd[1] == 'n' && Cmd[2] == '_')
							return false;
					}

					if (hailstone::istarts_with(Cmd, "Guard")
						|| hailstone::istarts_with(Cmd, "Defender")
						|| hailstone::istarts_with(Cmd, "Soulbinder")
						|| hailstone::istarts_with(Cmd, "Aura")
						|| hailstone::istarts_with(Cmd, "Sage")
						//|| hailstone::istarts_with(Cmd, "High_Priest")
						|| hailstone::istarts_with(Cmd, "Ward")
						//|| hailstone::istarts_with(Cmd, "Shroudkeeper")
						|| hailstone::istarts_with(Cmd, "Eye of")
						|| hailstone::istarts_with(Cmd, "Imperial_Crypt")
						|| hailstone::istarts_with(Cmd, "Diaku"))
					{
						return false;
					}


					if (Cmd[0] == '#' && pSpawn->last_name.empty())
						return true;

					if (std::isupper(static_cast<unsigned char>(Cmd[0])) && pSpawn->last_name.empty())
						return true;
				}
			}

			return false;
		}
		static game::eSpawnType GetSpawnType(const game::Spawn* pSpawn)
		{
			if (pSpawn == nullptr)
				return hailstone::game::eSpawnType::NONE;
			const char* tName = pSpawn->name.c_str();
			int Type;
			if (pSpawn->is_npc) {
				Type = SPAWN_NPC;
			}
			else
			{
				Type = SPAWN_PLAYER;
			}
			switch (Type)
			{
			case SPAWN_PLAYER:
				return hailstone::game::eSpawnType::PC;

			case SPAWN_NPC:
				// Sometimes mounts can be spawned without a rider. Sometimes a user can have
				// multiple mounts due to bugs! Check the display name which will have the same
				// suffix for all mounts.

				if (pSpawn->rider_id > 0 || display_name(pSpawn->name).ends_with("`s Mount"))
					return hailstone::game::eSpawnType::MOUNT;

				if (pSpawn->pet_owner_id)
					return hailstone::game::eSpawnType::PET;
				if (pSpawn->mercenary)
					return hailstone::game::eSpawnType::MERCENARY;

				// some type of controller spawn for flying mobs - locations, speed, heading, all NaN
				if (IsNaN(pSpawn->position.y) && IsNaN(pSpawn->position.x) && IsNaN(pSpawn->position.z))
					return hailstone::game::eSpawnType::FLYER;

				switch (pSpawn->body_type)
				{
				case hailstone::game::CharacterProperty::CharacterProperty_None: // Object
					if (pSpawn->class_id == hailstone::game::PlayerClass::Class_Object)
						return hailstone::game::eSpawnType::OBJECT;
					return hailstone::game::eSpawnType::NPC;

				case hailstone::game::CharacterProperty::CharacterProperty_Construct:
					// "Invisible Man" Race containing "Aura" / "Circle_of" / "Guardian_Circle" / "Earthen_Strength" / "Pact_of_the_Wolf" in the Name

					//TODO
					//this may need to be updated for custom content? bad form to have hard coded values like this.
					if ((pSpawn->race == hailstone::game::EQR_INVISIBLE_MAN) &&
						(strstr(tName, "Aura") || strstr(tName, "Circle_of") || strstr(tName, "Guardian_Circle") || strstr(tName, "Earthen_Strength") || strstr(tName, "Pact_of_the_Wolf")))
						return hailstone::game::eSpawnType::AURA;
					// "Spike Trap" Race containing "Poison" in the Name
					if ((pSpawn->race == hailstone::game::EQR_SPIKE_TRAP) && (strstr(tName, "poison") || strstr(tName, "Poison")))
						return hailstone::game::eSpawnType::AURA;
					// Contains "Rune" in the Name
					if (strstr(tName, "Rune"))
						return hailstone::game::eSpawnType::AURA;
					// Object Class
					if (pSpawn->class_id == hailstone::game::PlayerClass::Class_Object)
						return hailstone::game::eSpawnType::OBJECT;
					return hailstone::game::eSpawnType::NPC;

				case hailstone::game::CharacterProperty::CharacterProperty_Magical:
					// "Campfire" Race
					if (pSpawn->race == hailstone::game::EQR_CAMPSITE)
						return hailstone::game::eSpawnType::CAMPFIRE;
					// "Banner" Race(s)
					if (pSpawn->race == hailstone::game::EQR_BANNER || (pSpawn->race >= hailstone::game::EQR_BANNER0 && pSpawn->race <= hailstone::game::EQR_BANNER4) || pSpawn->race == hailstone::game::EQR_TCGBANNER)
						return hailstone::game::eSpawnType::BANNER;
					// "Totem" Race containing "Idol" in the Name
					if ((pSpawn->race == hailstone::game::EQR_TOTEM) && strstr(tName, "Idol"))
						return hailstone::game::eSpawnType::AURA;
					// Object Class
					if (pSpawn->class_id == hailstone::game::PlayerClass::Class_Object)
						return hailstone::game::eSpawnType::OBJECT;
					return hailstone::game::eSpawnType::NPC;

				case hailstone::game::CharacterProperty::CharacterProperty_Untargetable:
					return hailstone::game::eSpawnType::UNTARGETABLE;

				case hailstone::game::CharacterProperty::CharacterProperty_Cursed:
					return hailstone::game::eSpawnType::CHEST;

				case hailstone::game::CharacterProperty::CharacterProperty_Utility:
					return hailstone::game::eSpawnType::UNTARGETABLE;

				case hailstone::game::CharacterProperty::CharacterProperty_Trap:
					return hailstone::game::eSpawnType::TRAP;

				case hailstone::game::CharacterProperty::CharacterProperty_Companion:
					return hailstone::game::eSpawnType::TIMER;

				case hailstone::game::CharacterProperty::CharacterProperty_Suicide:
					return hailstone::game::eSpawnType::TRIGGER;

				default: break;
				}
				return hailstone::game::eSpawnType::NPC;

			case SPAWN_CORPSE:
				return hailstone::game::eSpawnType::CORPSE;

			default: break;
			}

			return hailstone::game::eSpawnType::ITEM;
		}
		static const char* GetTypeDesc(eSpawnType TypeID)
		{
			switch (TypeID)
			{
			case MOUNT:
				return "Mount";
			case UNTARGETABLE:
				return "Untargetable";
			case NPC:
				return "NPC";
			case PC:
				return "PC";
			case CHEST:
				return "Chest";
			case TRAP:
				return "Trap";
			case TRIGGER:
				return "Trigger";
			case TIMER:
				return "Timer";
			case PET:
				return "Pet";
			case ITEM:
				return "Item";
			case CORPSE:
				return "Corpse";
			case AURA:
				return "Aura";
			case OBJECT:
				return "Object";
			case BANNER:
				return "Banner";
			case CAMPFIRE:
				return "Campfire";
			case MERCENARY:
				return "Mercenary";
			case FLYER:
				return "Flyer";
			default:
				return "Unknown Type";
			}
		}
		static const char* GetRaceIDName(uint16_t race_id)
		{
			switch (race_id) {
			case game::Race::Abhorrent:
				return "Abhorrent";
			case game::Race::AirElemental:
			case game::Race::AirElemental2:
				return "Air Elemental";
			case game::Race::AirMephit:
				return "Air Mephit";
			case game::Race::Akhevan:
			case game::Race::Akheva:
				return "Akheva";
			case game::Race::Alaran:
				return "Alaran";
			case game::Race::AlaranGhost:
				return "Alaran Ghost";
			case game::Race::AlaranSentryStone:
				return "Alaran Sentry Stone";
			case game::Race::Alligator:
			case game::Race::Alligator2:
				return "Alligator";
			case game::Race::Denizen:
			case game::Race::Amygdalan:
				return "Amygdalan";
			case game::Race::Aneuk:
				return "Aneuk";
			case game::Race::AnimatedArmor:
				return "Animated Armor";
			case game::Race::AnimatedHand:
				return "Animated Hand";
			case game::Race::AnimatedStatue:
			case game::Race::AnimatedStatue2:
				return "Animated Statue";
			case game::Race::Apexus:
				return "Apexus";
			case game::Race::Arachnid:
				return "Arachnid";
			case game::Race::ArcanistOfHate:
				return "Arcanist of Hate";
			case game::Race::Armadillo:
				return "Armadillo";
			case game::Race::ArmorRack:
				return "Armor Rack";
			case game::Race::Aviak:
			case game::Race::Aviak2:
				return "Aviak";
			case game::Race::AviakPullAlong:
				return "Aviak Pull Along";
			case game::Race::AyonaeRo:
				return "Ayonae Ro";
			case game::Race::Banner:
			case game::Race::Banner2:
			case game::Race::Banner3:
			case game::Race::Banner4:
			case game::Race::Banner5:
			case game::Race::Banner6:
			case game::Race::Banner7:
				return "Banner";
			case game::Race::TenthAnniversaryBanner:
				return "10th Anniversary Banner";
			case game::Race::Banshee:
			case game::Race::Banshee2:
			case game::Race::Banshee3:
				return "Banshee";
			case game::Race::Barbarian:
			case game::Race::HalasCitizen:
				return "Barbarian";
			case game::Race::Barrel:
				return "Barrel";
			case game::Race::UndeadBoat:
				return "Barrel Barge Ship";
			case game::Race::Basilisk:
				return "Basilisk";
			case game::Race::GiantBat:
			case game::Race::Bat:
			case game::Race::Bat2:
				return "Bat";
			case game::Race::Bazu:
				return "Bazu";
			case game::Race::Bear:
			case game::Race::Bear2:
			case game::Race::Bear3:
				return "Bear";
			case game::Race::BearMount:
				return "Bear Mount";
			case game::Race::BearTrap:
				return "Bear Trap";
			case game::Race::Beetle:
			case game::Race::Beetle2:
			case game::Race::Beetle3:
				return "Beetle";
			case game::Race::HumanBeggar:
				return "Beggar";
			case game::Race::Bellikos:
				return "Bellikos";
			case game::Race::Bertoxxulous:
			case game::Race::BertoxxulousNew:
				return "Bertoxxulous";
			case game::Race::Bixie:
			case game::Race::Bixie2:
				return "Bixie";
			case game::Race::BlimpShip:
				return "Blimp Ship";
			case game::Race::BlindDreamer:
				return "Blind Dreamer";
			case game::Race::BloodRaven:
				return "Blood Raven";
			case game::Race::Boat:
			case game::Race::Boat2:
				return "Boat";
			case game::Race::Bolvirk:
				return "Bolvirk";
			case game::Race::BoneGolem:
			case game::Race::BoneGolem2:
				return "Bone Golem";
			case game::Race::Bones:
				return "Bones";
			case game::Race::BookDervish:
				return "Book Dervish";
			case game::Race::Portal:
				return "BoT Portal";
			case game::Race::Boulder:
				return "Boulder";
			case game::Race::Box:
				return "Box";
			case game::Race::Braxi:
				return "Braxi";
			case game::Race::BraxiMount:
				return "Braxi Mount";
			case game::Race::Brell:
				return "Brell";
			case game::Race::BrellsFirstCreation:
				return "Brell's First Creation";
			case game::Race::Bristlebane:
				return "Bristlebane";
			case game::Race::BrokenClockwork:
				return "Broken Clockwork";
			case game::Race::Brontotherium:
				return "Brontotherium";
			case game::Race::Brownie:
			case game::Race::Brownie2:
				return "Brownie";
			case game::Race::Bubonian:
				return "Bubonian";
			case game::Race::BubonianUnderling:
				return "Bubonian Underling";
			case game::Race::Burynai:
			case game::Race::Burynai2:
				return "Burynai";
			case game::Race::TenthAnniversaryCake:
				return "10th Anniversary Cake";
			case game::Race::Campfire:
				return "Campfire";
			case game::Race::CarrierHand:
				return "Carrier Hand";
			case game::Race::Cat:
				return "Cat";
			case game::Race::CazicThule:
			case game::Race::CazicThule2:
				return "Cazic Thule";
			case game::Race::Centaur:
			case game::Race::Centaur2:
				return "Centaur";
			case game::Race::Chest:
			case game::Race::Chest2:
			case game::Race::Chest3:
				return "Chest";
			case game::Race::Chimera:
			case game::Race::Chimera2:
				return "Chimera";
			case game::Race::Chokidai:
				return "Chokidai";
			case game::Race::Clam:
				return "Clam";
			case game::Race::CliknarMount:
				return "Cliknar Mount";
			case game::Race::CliknarQueen:
				return "Cliknar Queen";
			case game::Race::CliknarSoldier:
				return "Cliknar Soldier";
			case game::Race::CliknarWorker:
				return "Cliknar Worker";
			case game::Race::ClockworkBeetle:
				return "Clockwork Beetle";
			case game::Race::ClockworkBoar:
				return "Clockwork Boar";
			case game::Race::ClockworkBomb:
				return "Clockwork Bomb";
			case game::Race::ClockworkBrain:
				return "Clockwork Brain";
			case game::Race::ClockworkGnome:
				return "Clockwork Gnome";
			case game::Race::ClockworkGolem:
				return "Clockwork Golem";
			case game::Race::ClockworkGuardian:
				return "Clockwork Guardian";
			case game::Race::Cockatrice:
				return "Cockatrice";
			case game::Race::Coffin:
			case game::Race::Coffin2:
				return "Coffin";
			case game::Race::CoinPurse:
				return "Coin Purse";
			case game::Race::Coldain:
			case game::Race::Coldain2:
			case game::Race::Coldain3:
				return "Coldain";
			case game::Race::Coral:
				return "Coral";
			case game::Race::Corathus:
				return "Corathus";
			case game::Race::Crab:
				return "Crab";
			case game::Race::Cragbeast:
				return "Cragbeast";
			case game::Race::Cragslither:
				return "Cragslither";
			case game::Race::Crocodile:
				return "Crocodile";
			case game::Race::Crystal:
				return "Crystal";
			case game::Race::CrystalShard:
				return "Crystal Shard";
			case game::Race::CrystalSphere:
				return "Crystal Sphere";
			case game::Race::CrystalSpider:
				return "Crystal Spider";
			case game::Race::CrystalskinAmbuloid:
				return "Crystalskin Ambuloid";
			case game::Race::CrystalskinSessiloid:
				return "Crystalskin Sessiloid";
			case game::Race::DaisyMan:
				return "Daisy Man";
			case game::Race::DarkElf:
			case game::Race::NeriakCitizen:
				return "Dark Elf";
			case game::Race::DarkLord:
				return "Dark Lord";
			case game::Race::DemiLich:
				return "Demi Lich";
			case game::Race::DemonVulture:
				return "Demon Vulture";
			case game::Race::Dervish:
			case game::Race::Dervish2:
			case game::Race::Dervish3:
			case game::Race::Dervish4:
				return "Dervish";
			case game::Race::Dervish5:
				return "Dervish(Ver. 5)";
			case game::Race::Dervish6:
				return "Dervish(Ver. 6)";
			case game::Race::Devourer:
				return "Devourer";
			case game::Race::DireWolf:
				return "Dire Wolf";
			case game::Race::DiscordShip:
				return "Discord Ship";
			case game::Race::Discordling:
				return "Discordling";
			case game::Race::DiseasedFiend:
				return "Diseased Fiend";
			case game::Race::Djinn:
				return "Djinn";
			case game::Race::Drachnid:
			case game::Race::Drachnid2:
				return "Drachnid";
			case game::Race::DrachnidCocoon:
				return "Drachnid Cocoon";
			case game::Race::Dracolich:
				return "Dracolich";
			case game::Race::Draglock:
				return "Draglock";
			case game::Race::LavaDragon:
			case game::Race::DragonSkeleton:
			case game::Race::WaterDragon:
			case game::Race::VeliousDragon:
			case game::Race::ClockworkDragon:
			case game::Race::BlackAndWhiteDragon:
			case game::Race::GhostDragon:
			case game::Race::PrismaticDragon:
			case game::Race::Quarm:
			case game::Race::Dragon:
			case game::Race::Dragon2:
			case game::Race::Dragon3:
			case game::Race::Dragon4:
			case game::Race::Dragon5:
			case game::Race::Dragon6:
			case game::Race::Dragon7:
				return "Dragon";
			case game::Race::DragonBones:
				return "Dragon Bones";
			case game::Race::DragonEgg:
				return "Dragon Egg";
			case game::Race::DragonStatue:
				return "Dragon Statue";
			case game::Race::Dragorn:
				return "Dragorn";
			case game::Race::DragornBox:
				return "Dragorn Box";
			case game::Race::Drake:
			case game::Race::Drake2:
			case game::Race::Drake3:
				return "Drake";
			case game::Race::Drakkin:
				return "Drakkin";
			case game::Race::Drixie:
				return "Drixie";
			case game::Race::Drogmor:
				return "Drogmore";
			case game::Race::Drolvarg:
				return "Drolvarg";
			case game::Race::Dryad:
				return "Dryad";
			case game::Race::Dwarf:
			case game::Race::KaladimCitizen:
				return "Dwarf";
			case game::Race::DynamiteKeg:
				return "Dynamite Keg";
			case game::Race::Dynleth:
				return "Dyn'Leth";
			case game::Race::EarthElemental:
			case game::Race::EarthElemental2:
				return "Earth Elemental";
			case game::Race::EarthMephit:
				return "Earth Mephit";
			case game::Race::GiantEel:
				return "Eel";
			case game::Race::Efreeti:
			case game::Race::Efreeti2:
				return "Efreeti";
			case game::Race::Elddar:
				return "Elddar";
			case game::Race::Elemental:
				return "Elemental";
			case game::Race::ElkHead:
				return "Elk Head";
			case game::Race::ElvenBoat:
				return "Elven Boat";
			case game::Race::ElvenGhost:
				return "Elven Ghost";
			case game::Race::EnchantedArmor:
				return "Enchanted Armor";
			case game::Race::Erollisi:
				return "Erollisi";
			case game::Race::Erudite:
			case game::Race::EruditeCitizen:
			case game::Race::Erudite2:
				return "Erudite";
			case game::Race::EvanTest:
				return "Evan Test";
			case game::Race::EvilEye:
			case game::Race::EvilEye2:
			case game::Race::EvilEye3:
				return "Evil Eye";
			case game::Race::Exoskeleton:
				return "Exoskeleton";
			case game::Race::ExplosiveCart:
				return "Explosive Cart";
			case game::Race::EyeOfZomm:
				return "Eye";
			case game::Race::Fairy:
			case game::Race::Fairy2:
				return "Fairy";
			case game::Race::FallenKnight:
				return "Fallen Knight";
			case game::Race::Faun:
				return "Faun";
			case game::Race::FayDrake:
				return "Fay Drake";
			case game::Race::FenninRo:
				return "Fennin Ro";
			case game::Race::Feran:
				return "Feran";
			case game::Race::FeranMount:
				return "Feran Mount";
			case game::Race::Fiend:
				return "Fiend";
			case game::Race::FireElemental:
			case game::Race::FireElemental2:
				return "Fire Elemental";
			case game::Race::FireMephit:
				return "Fire Mephit";
			case game::Race::Fish:
			case game::Race::KunarkFish:
				return "Fish";
			case game::Race::Flag:
				return "Flag";
			case game::Race::FloatingIsland:
				return "Floating Island";
			case game::Race::FloatingSkull:
				return "Floating Skull";
			case game::Race::FloatingTower:
				return "Floating Tower";
			case game::Race::Fly:
				return "Fly";
			case game::Race::FlyingCarpet:
				return "Flying Carpet";
			case game::Race::ForestGiant2:
				return "Forest Giant";
			case game::Race::Frog:
			case game::Race::Frog2:
				return "Frog";
			case game::Race::Froglok:
			case game::Race::FroglokGhoul:
			case game::Race::Froglok2:
				return "Froglok";
			case game::Race::FroglokGhost:
				return "Froglok Ghost";
			case game::Race::FroglokSkeleton:
				return "Froglok Skeleton";
			case game::Race::FungalFiend:
				return "Fungal Fiend";
			case game::Race::FungusPatch:
				return "Fungus Patch";
			case game::Race::Fungusman:
				return "Fungusman";
			case game::Race::Galorian:
				return "Galorian";
			case game::Race::Gargoyle:
			case game::Race::Gargoyle2:
				return "Gargoyle";
			case game::Race::Gasbag:
				return "Gasbag";
			case game::Race::GelatinousCube:
			case game::Race::GelatinousCube2:
				return "Gelatinous Cube";
			case game::Race::Gelidran:
				return "Gelidran";
			case game::Race::Genari:
				return "Genari";
			case game::Race::Geonid:
				return "Geonid";
			case game::Race::Ghost:
			case game::Race::DwarfGhost:
			case game::Race::EruditeGhost:
			case game::Race::PirateGhost:
				return "Ghost";
			case game::Race::GhostShip:
			case game::Race::GhostShip2:
				return "Ghost Ship";
			case game::Race::Ghoul:
			case game::Race::Ghoul2:
				return "Ghoul";
			case game::Race::Giant:
			case game::Race::ForestGiant:
			case game::Race::FrostGiant:
			case game::Race::StormGiant:
			case game::Race::EarthGolem:
			case game::Race::IronGolem:
			case game::Race::StormGolem:
			case game::Race::AirGolem:
			case game::Race::WoodGolem:
			case game::Race::FireGolem:
			case game::Race::WaterGolem:
			case game::Race::Giant2:
			case game::Race::Giant3:
				return "Giant";
			case game::Race::GiantClockwork:
				return "Giant Clockwork";
			case game::Race::Giant4:
				return "Giant(Rallosian mats)";
			case game::Race::GiantShade:
				return "Giant Shade";
			case game::Race::Gigyn:
				return "Gigyn";
			case game::Race::GingerbreadMan:
				return "Gingerbread Man";
			case game::Race::Girplan:
				return "Girplan";
			case game::Race::Gnoll:
			case game::Race::Gnoll2:
			case game::Race::Gnoll3:
				return "Gnoll";
			case game::Race::Gnome:
				return "Gnome";
			case game::Race::Gnomework:
				return "Gnomework";
			case game::Race::GnomishBalloon:
				return "Gnomish Balloon";
			case game::Race::GnomishBoat:
				return "Gnomish Boat";
			case game::Race::GnomishHoveringTransport:
				return "Gnomish Hovering Transport";
			case game::Race::GnomishRocketPack:
				return "Gnomish Rocket Pack";
			case game::Race::Goblin:
			case game::Race::Bloodgill:
			case game::Race::KunarkGoblin:
			case game::Race::NewGoblin:
			case game::Race::Goblin2:
				return "Goblin";
			case game::Race::Luclin2:
				return "God - Luclin(Ver. 2)";
			case game::Race::Luclin3:
				return "God - Luclin(Ver. 3)";
			case game::Race::Luclin4:
				return "God - Luclin(Ver. 4)";
			case game::Race::GodOfDiscord:
				return "God of Discord";
			case game::Race::Golem:
			case game::Race::Golem2:
				return "Golem";
			case game::Race::Goo:
			case game::Race::Goo2:
			case game::Race::Goo3:
			case game::Race::Goo4:
				return "Goo";
			case game::Race::Goral:
				return "Goral";
			case game::Race::GoralMount:
				return "Goral Mount";
			case game::Race::Gorgon:
				return "Gorgon";
			case game::Race::Gorilla:
			case game::Race::Gorilla2:
				return "Gorilla";
			case game::Race::GrandfatherClock:
				return "Grandfather Clock";
			case game::Race::GrekenYoung:
				return "Greken - Young";
			case game::Race::GrekenYoungAdult:
				return "Greken - Young Adult";
			case game::Race::Grendlaen:
				return "Grendlaen";
			case game::Race::GriegVeneficus:
				return "Grieg Veneficus";
			case game::Race::Griffin:
			case game::Race::Griffin2:
				return "Griffin";
			case game::Race::Grimling:
				return "Grimling";
			case game::Race::GroundShaker:
				return "Ground Shaker";
			case game::Race::FreeportGuard:
			case game::Race::Felguard:
			case game::Race::Fayguard:
			case game::Race::VahShirGuard:
				return "Guard";
			case game::Race::GuardOfJustice:
				return "Guard of Justice";
			case game::Race::GuardianCpu:
				return "Guardian CPU";
			case game::Race::Hadal:
				return "Hadal";
			case game::Race::Hag:
				return "Hag";
			case game::Race::HalfElf:
				return "Half Elf";
			case game::Race::Halfling:
			case game::Race::RivervaleCitizen:
				return "Halfling";
			case game::Race::Harpy:
			case game::Race::Harpy2:
				return "Harpy";
			case game::Race::HighElf:
				return "High Elf";
			case game::Race::Hippogriff:
				return "Hippogriff";
			case game::Race::Holgresh:
			case game::Race::Holgresh2:
				return "Holgresh";
			case game::Race::HoneyPot:
				return "Honey Pot";
			case game::Race::Horse:
			case game::Race::Horse2:
			case game::Race::Horse3:
				return "Horse";
			case game::Race::HoveringPlatform:
				return "Hovering Platform";
			case game::Race::Hraquis:
				return "Hraquis";
			case game::Race::Human:
			case game::Race::HighpassCitizen:
			case game::Race::QeynosCitizen:
			case game::Race::Human2:
				return "Human";
			case game::Race::HumanGhost:
				return "Human Ghost";
			case game::Race::Huvul:
				return "Huvul";
			case game::Race::HydraCrystal:
				return "Hydra Crystal";
			case game::Race::HydraMount:
				return "Hydra Mount";
			case game::Race::Hydra:
				return "Hydra NPC";
			case game::Race::Hynid:
				return "Hynid";
			case game::Race::IceSpectre:
				return "Ice Spectre";
			case game::Race::Ikaav:
				return "Ikaav";
			case game::Race::Iksar:
			case game::Race::IksarCitizen:
				return "Iksar";
			case game::Race::IksarGhost:
				return "Iksar Ghost";
			case game::Race::IksarGolem:
				return "Iksar Golem";
			case game::Race::IksarSkeleton:
				return "Iksar Skeleton";
			case game::Race::IksarSpirit:
				return "Iksar Spirit";
			case game::Race::Imp:
				return "Imp";
			case game::Race::Innoruuk:
				return "Innoruuk";
			case game::Race::Insect:
				return "Insect";
			case game::Race::InteractiveObject:
				return "Interactive Object";
			case game::Race::InvisibleMan:
			case game::Race::InvisibleMan2:
			case game::Race::InvisibleMan3:
				return "Invisible Man";
			case game::Race::InvisibleManOfZomm:
				return "Invisible Man of Zomm";
			case game::Race::Ixt:
				return "Ixt";
			case game::Race::Jokester:
				return "Jokester";
			case game::Race::JumJumBucket:
				return "Jum Jum Bucket";
			case game::Race::JunkBeast:
				return "Junk Beast";
			case game::Race::Kangon:
				return "Kangon";
			case game::Race::KangonMount:
				return "Kangon Mount";
			case game::Race::Karana:
				return "Karana";
			case game::Race::PhinigelAutropos:
			case game::Race::Kedge:
				return "Kedge";
			case game::Race::Kerran:
			case game::Race::Kerran2:
				return "Kerran";
			case game::Race::Kirin:
			case game::Race::Kirin2:
				return "Kirin";
			case game::Race::KnightOfHate:
				return "Knight of Hate";
			case game::Race::KnightOfPestilence:
				return "Knight of Pestilence";
			case game::Race::Kobold:
			case game::Race::Kobold2:
				return "Kobold";
			case game::Race::Kraken:
				return "Kraken";
			case game::Race::Kyv:
				return "Kyv";
			case game::Race::Launch:
				return "Launch";
			case game::Race::LavaRock:
				return "Lava Rock";
			case game::Race::LavaSpider:
				return "Lava Spider";
			case game::Race::LavaSpiderQueen:
				return "Lava Spider Queen";
			case game::Race::Leech:
				return "Leech";
			case game::Race::Lepertoloth:
				return "Lepertoloth";
			case game::Race::Lightcrawler:
				return "Lightcrawler";
			case game::Race::LightingWarrior:
				return "Lightning Warrior";
			case game::Race::Lion:
				return "Lion";
			case game::Race::LizardMan:
				return "Lizard Man";
			case game::Race::Luclin:
				return "Luclin";
			case game::Race::Luggald:
			case game::Race::Luggald2:
				return "Luggald";
			case game::Race::Luggald3:
				return "Luggalds";
			case game::Race::Malarian:
				return "Malarian";
			case game::Race::Mammoth:
			case game::Race::Mammoth2:
				return "Mammoth";
			case game::Race::ManEatingPlant:
				return "Man - Eating Plant";
			case game::Race::Mansion:
				return "Mansion";
			case game::Race::Manticore:
				return "Manticore";
			case game::Race::Unknown5:
				return "Mantrap";
			case game::Race::Marionette:
				return "Marionette";
			case game::Race::Mastruq:
				return "Mastruq";
			case game::Race::MataMuram:
				return "Mata Muram";
			case game::Race::Toolbox:
				return "Medium Plant";
			case game::Race::Mephit:
				return "Mephit";
			case game::Race::MerchantShip:
				return "Merchant Ship";
			case game::Race::Mermaid:
				return "Mermaid";
			case game::Race::Mimic:
				return "Mimic";
			case game::Race::MiniPom:
				return "Mini POM";
			case game::Race::Minotaur:
			case game::Race::Minotaur2:
			case game::Race::Minotaur3:
			case game::Race::Minotaur4:
				return "Minotaur";
			case game::Race::MithanielMarr:
				return "Mithaniel Marr";
			case game::Race::MorellThule:
				return "Morell Thule";
			case game::Race::Mosquito:
				return "Mosquito";
			case game::Race::MouthOfInsanity:
				return "Mouth of Insanity";
			case game::Race::Muddite:
				return "Muddite";
			case game::Race::Mummy:
				return "Mummy";
			case game::Race::MuramiteArmorPile:
				return "Muramite Armor Pile";
			case game::Race::Murkglider:
				return "Murkglider";
			case game::Race::MurkgliderEggSack:
				return "Murkglider Egg Sac";
			case game::Race::Mutna:
				return "Mutna";
			case game::Race::Nekhon:
				return "Nekhon";
			case game::Race::Netherbian:
				return "Netherbian";
			case game::Race::Nightmare:
				return "Nightmare";
			case game::Race::NightmareGargoyle:
				return "Nightmare Gargoyle";
			case game::Race::NightmareGoblin:
				return "Nightmare Goblin";
			case game::Race::NightmareMephit:
				return "Nightmare Mephit";
			case game::Race::Unicorn2:
			case game::Race::Unicorn3:
				return "Nightmare / Unicorn";
			case game::Race::NightmareWraith:
				return "Nightmare Wraith";
			case game::Race::Nihil:
				return "Nihil";
			case game::Race::Nilborien:
				return "Nilborien";
			case game::Race::Noc:
				return "Noc";
			case game::Race::Nymph:
				return "Nymph";
			case game::Race::Ogre:
			case game::Race::OggokCitizen:
				return "Ogre";
			case game::Race::Ogre2:
				return "Ogre NPC - Male";
			case game::Race::Orb:
				return "Orb";
			case game::Race::Orc:
			case game::Race::Orc2:
				return "Orc";
			case game::Race::Othmir:
				return "Othmir";
			case game::Race::Owlbear:
				return "Owlbear";
			case game::Race::ParasiticScavenger:
				return "Parasitic Scavenger";
			case game::Race::Pegasus:
			case game::Race::Pegasus2:
			case game::Race::Pegasus3:
				return "Pegasus";
			case game::Race::Phoenix:
				return "Phoenix";
			case game::Race::Piranha:
				return "Piranha";
			case game::Race::OneArmedPirate:
			case game::Race::SpiritmasterNadox:
			case game::Race::BrokenSkullTaskmaster:
			case game::Race::GnomePirate:
			case game::Race::DarkElfPirate:
			case game::Race::OgrePirate:
			case game::Race::HumanPirate:
			case game::Race::EruditePirate:
				return "Pirate";
			case game::Race::PirateShip:
				return "Pirate Ship";
			case game::Race::Pixie:
				return "Pixie";
			case game::Race::PoisonFrog:
				return "Poison Frog";
			case game::Race::Portal2:
				return "Portal";
			case game::Race::PowderKeg:
				return "Powder Keg";
			case game::Race::PressurePlate:
				return "Pressure Plate";
			case game::Race::PufferSpore:
				return "Puffer Spore";
			case game::Race::Puma:
			case game::Race::Puma2:
			case game::Race::Puma3:
				return "Puma";
			case game::Race::Pusling:
				return "Pusling";
			case game::Race::Pyrilen:
				return "Pyrilen";
			case game::Race::Ratuk:
				return "Ra`tuk";
			case game::Race::Rabbit:
				return "Rabbit";
			case game::Race::StatueOfRallosZek:
			case game::Race::NewRallosZek:
				return "Rallos Zek";
			case game::Race::RallosOgre:
				return "Rallos Zek Minion";
			case game::Race::Raptor:
			case game::Race::Raptor2:
				return "Raptor";
			case game::Race::RaptorMount:
				return "Raptor Mount";
			case game::Race::GiantRat:
			case game::Race::Rat:
				return "Rat";
			case game::Race::RatMount:
				return "Rat Mount";
			case game::Race::Ratman:
			case game::Race::Ratman2:
				return "Ratman";
			case game::Race::ReanimatedHand:
				return "Reanimated Hand";
			case game::Race::Recuso:
				return "Recuso";
			case game::Race::RegenerationPool:
				return "Regeneration Pool";
			case game::Race::RelicCase:
				return "Relic case ";
			case game::Race::RhinoBeetle:
				return "Rhino Beetle";
			case game::Race::Rhinoceros:
				return "Rhinoceros";
			case game::Race::RobocopterOfZomm:
				return "Robocopter of Zomm";
			case game::Race::RockPile:
				return "Rock Pile";
			case game::Race::Rockhopper:
				return "Rockhopper";
			case game::Race::RonnieTest:
				return "Ronnie Test";
			case game::Race::RootTentacle:
				return "Root Tentacle";
			case game::Race::RotDogMount:
				return "Rot Dog Mount";
			case game::Race::RotDog:
				return "Rotdog";
			case game::Race::Rotocopter:
				return "Rotocopter";
			case game::Race::Rowboat:
				return "Rowboat";
			case game::Race::RoyalGuard:
				return "Royal Guard";
			case game::Race::RujarkianOrc:
			case game::Race::MasterOrc:
				return "Rujarkian Orc";
			case game::Race::RunedOrb:
				return "Runed Orb";
			case game::Race::RunicSymbol:
				return "Runic Symbol";
			case game::Race::Sabertooth:
				return "Saber - toothed Cat";
			case game::Race::SaltpetterBomb:
				return "Saltpetter Bomb";
			case game::Race::SandElf:
				return "Sand Elf";
			case game::Race::Sandman:
				return "Sandman";
			case game::Race::Sarnak:
			case game::Race::Sarnak2:
				return "Sarnak";
			case game::Race::SarnakGolem:
				return "Sarnak Golem";
			case game::Race::SarnakSpirit:
				return "Sarnak Spirit";
			case game::Race::Saryrn:
				return "Saryrn";
			case game::Race::Satyr:
				return "Satyr";
			case game::Race::ScaledWolf:
				return "Scaled Wolf";
			case game::Race::Scarecrow:
			case game::Race::Scarecrow2:
				return "Scarecrow";
			case game::Race::ScarletCheetah:
				return "Scarlet Cheetah";
			case game::Race::ScleraMount:
				return "Sclera Mount";
			case game::Race::Scorpion:
			case game::Race::IksarScorpion:
			case game::Race::Scorpion2:
				return "Scorpion";
			case game::Race::Scrykin:
				return "Scrykin";
			case game::Race::SeaTurtle:
				return "Sea Turtle";
			case game::Race::SeaHorse:
				return "Seahorse";
			case game::Race::Selyrah:
				return "Selyrah";
			case game::Race::SelyrahMount:
				return "Selyrah Mount";
			case game::Race::LordInquisitorSeru:
				return "Seru";
			case game::Race::ServantOfShadow:
				return "Servant of Shadow";
			case game::Race::SessiloidMount:
				return "Sessiloid Mount";
			case game::Race::Shade:
			case game::Race::Shade2:
			case game::Race::Shade3:
				return "Shade";
			case game::Race::KhatiSha:
				return "Shadel";
			case game::Race::ShamblingMound:
				return "Shambling Mound";
			case game::Race::Shark:
				return "Shark";
			case game::Race::Shiknar:
				return "Shik'Nar";
			case game::Race::Shiliskin:
				return "Shiliskin";
			case game::Race::Ship:
				return "Ship";
			case game::Race::ShipInABottle:
				return "Ship in a Bottle";
			case game::Race::Shissar:
			case game::Race::Shissar2:
				return "Shissar";
			case game::Race::Shrieker:
				return "Shrieker";
			case game::Race::Siren:
			case game::Race::Siren2:
				return "Siren";
			case game::Race::SkeletalHorse:
				return "Skeletal Horse";
			case game::Race::Skeleton:
			case game::Race::Skeleton2:
			case game::Race::Skeleton3:
				return "Skeleton";
			case game::Race::Skunk:
				return "Skunk";
			case game::Race::Skystrider:
				return "Skystrider";
			case game::Race::Plant2:
				return "Small Plant";
			case game::Race::GiantSnake:
			case game::Race::Snake:
				return "Snake";
			case game::Race::SnakeElemental:
				return "Snake Elemental";
			case game::Race::SnowDervish:
				return "Snow Dervish";
			case game::Race::SnowRabbit:
				return "Snow Rabbit";
			case game::Race::Sokokar:
				return "Sokokar";
			case game::Race::SokokarMount:
				return "Sokokar Mount";
			case game::Race::SokokarMount2:
				return "Sokokar(w saddle)";
			case game::Race::SolusekRo:
			case game::Race::SolusekRo2:
				return "Solusek Ro";
			case game::Race::SolusekRoGuard:
				return "Solusek Ro Guard";
			case game::Race::SonicWolf:
				return "Sonic Wolf";
			case game::Race::SoulDevourer:
				return "Soul Devourer";
			case game::Race::Spectre:
			case game::Race::Spectre2:
				return "Spectre";
			case game::Race::SpellParticle:
				return "Spell Particle 1";
			case game::Race::Sphinx:
			case game::Race::Sphinx2:
				return "Sphinx";
			case game::Race::GiantSpider:
			case game::Race::Spider:
				return "Spider";
			case game::Race::SpiderEggSack:
				return "Spider Egg Sack";
			case game::Race::SpiderMount:
				return "Spider Mount";
			case game::Race::SpiderQueen:
				return "Spider Queen";
			case game::Race::SpikeTrap:
				return "Spike Trap";
			case game::Race::SpiritWolf:
				return "Spirit Wolf";
			case game::Race::Sporali:
				return "Sporali";
			case game::Race::StoneJug:
				return "Stone Jug";
			case game::Race::StonePylon:
				return "Stone Pylon";
			case game::Race::StoneRing:
				return "Stone Ring";
			case game::Race::StoneWorker:
			case game::Race::StoneWorker2:
				return "Stone Worker";
			case game::Race::Stonegrabber:
				return "Stonegrabber";
			case game::Race::Stonemite:
				return "Stonemite";
			case game::Race::Stormrider:
				return "Stormrider";
			case game::Race::Succubus:
				return "Succubus";
			case game::Race::Succulent:
				return "Succulent";
			case game::Race::SullonZek:
				return "Sullon Zek";
			case game::Race::Shadel:
				return "Sun Revenant";
			case game::Race::Sunflower:
				return "Sunflower";
			case game::Race::Swinetor:
				return "Swinetor";
			case game::Race::Swordfish:
				return "Swordfish";
			case game::Race::Synarcana:
				return "Synarcana";
			case game::Race::Table:
				return "Table";
			case game::Race::FroglokTadpole:
				return "Tadpole";
			case game::Race::Taelosian:
				return "Taelosian";
			case game::Race::WineCask:
				return "Tall Plant";
			case game::Race::TallonZek:
				return "Tallon Zek";
			case game::Race::Taneth:
				return "Taneth";
			case game::Race::TarewMarr:
				return "Tarew Marr";
			case game::Race::Tegi:
				return "Tegi";
			case game::Race::TeleportMan:
				return "Teleport Man";
			case game::Race::TeleportationStand:
				return "Teleportation Stand";
			case game::Race::Telmira:
				return "Telmira";
			case game::Race::TentacleTerror:
			case game::Race::TentacleTerror2:
				return "Tentacle Terror";
			case game::Race::TerrisThule:
				return "Terris Thule";
			case game::Race::TestObject:
				return "Test Object";
			case game::Race::Rathe:
				return "The Rathe";
			case game::Race::TribunalNew:
				return "The Tribunal";
			case game::Race::ThoughtHorror:
				return "Thought Horror";
			case game::Race::Tiger:
				return "Tiger";
			case game::Race::TinSoldier:
				return "Tin Soldier";
			case game::Race::Plant:
				return "Toolbox";
			case game::Race::TopiaryLion:
				return "Topiary Lion";
			case game::Race::TopiaryLionMount:
				return "Topiary Lion Mount";
			case game::Race::Tormentor:
				return "Tormentor";
			case game::Race::Totem:
			case game::Race::Totem2:
				return "Totem";
			case game::Race::Trakanon:
				return "Trakanon";
			case game::Race::Tranquilion:
				return "Tranquilion";
			case game::Race::Treant:
			case game::Race::Treant2:
			case game::Race::Treant3:
				return "Treant";
			case game::Race::Tribunal:
				return "Tribunal";
			case game::Race::Triumvirate:
				return "Triumvirate";
			case game::Race::Troll:
			case game::Race::GrobbCitizen:
			case game::Race::TrollCrewMember:
			case game::Race::PirateDeckhand:
			case game::Race::BrokenSkullPirate:
				return "Troll";
			case game::Race::TrollZombie:
				return "Troll Zombie";
			case game::Race::Trusik:
				return "Trusik";
			case game::Race::Tsetsian:
				return "Tsetsian";
			case game::Race::Tumbleweed:
				return "Tumbleweed";
			case game::Race::Tunare:
				return "Tunare";
			case game::Race::Turepta:
				return "Turepta";
			case game::Race::Ukun:
				return "Ukun";
			case game::Race::Ulthork:
				return "Ulthork";
			case game::Race::UndeadChokidai:
				return "Undead Chokidai";
			case game::Race::UndeadFootman:
				return "Undead Footman";
			case game::Race::UndeadFroglok:
				return "Undead Froglok";
			case game::Race::UndeadIksar:
				return "Undead Iksar";
			case game::Race::UndeadKnight:
				return "Undead Knight";
			case game::Race::UndeadSarnak:
				return "Undead Sarnak";
			case game::Race::UndeadVeksar:
				return "Undead Veksar";
			case game::Race::Underbulk:
				return "Underbulk";
			case game::Race::Unicorn:
				return "Unicorn";
			case game::Race::Doug:
			case game::Race::MinorIllusion:
			case game::Race::Tree:
			case game::Race::Unknown:
			case game::Race::Unknown2:
			case game::Race::Unknown3:
			case game::Race::Unknown4:
				return "UNKNOWN RACE";
			case game::Race::VahShir:
			case game::Race::VahShirKing:
				return "Vah Shir";
			case game::Race::VahShirSkeleton:
				return "Vah Shir Skeleton";
			case game::Race::VallonZek:
				return "Vallon Zek";
			case game::Race::Valorian:
			case game::Race::Valorian2:
				return "Valorian";
			case game::Race::Vampire:
			case game::Race::ElfVampire:
			case game::Race::Vampire2:
			case game::Race::VampireVolatalis:
			case game::Race::UndeadVampire:
			case game::Race::Vampire3:
			case game::Race::MasterVampire:
			case game::Race::Vampire4:
				return "Vampire";
			case game::Race::Vase:
				return "Vase";
			case game::Race::Vegerog:
				return "Vegerog";
			case game::Race::Veksar:
			case game::Race::Veksar2:
			case game::Race::Veksar3:
				return "Veksar";
			case game::Race::VenrilSathir:
				return "Venril Sathir";
			case game::Race::VineMaw:
				return "Vine Maw";
			case game::Race::Wagon:
				return "Wagon";
			case game::Race::Walrus:
				return "Walrus";
			case game::Race::WarBoar:
			case game::Race::WarBoar2:
				return "War Boar";
			case game::Race::WarWraith:
				return "War Wraith";
			case game::Race::Wasp:
				return "Wasp";
			case game::Race::WaterElemental:
			case game::Race::WaterElemental2:
				return "Water Elemental";
			case game::Race::WaterMephit:
				return "Water Mephit";
			case game::Race::WaterSpout:
				return "Water Spout";
			case game::Race::WeaponRack:
			case game::Race::WeaponRack2:
				return "Weapon Rack";
			case game::Race::Web:
				return "Web";
			case game::Race::WeddingAltar:
				return "Wedding Altar";
			case game::Race::WeddingArbor:
				return "Wedding Arbor";
			case game::Race::WeddingFlowers:
				return "Wedding Flowers";
			case game::Race::Wereorc:
				return "Wereorc";
			case game::Race::Werewolf:
			case game::Race::Werewolf2:
			case game::Race::Werewolf3:
				return "Werewolf";
			case game::Race::WetfangMinnow:
				return "Wetfang Minnow";
			case game::Race::Whirligig:
				return "Whirligig";
			case game::Race::WickerBasket:
				return "Wicker Basket";
			case game::Race::Wisp:
				return "Will - O - Wisp";
			case game::Race::StoneJug2:
			case game::Race::WineCask2:
				return "Wine Cask";
			case game::Race::Witheran:
			case game::Race::Witheran2:
				return "Witheran";
			case game::Race::Wolf:
			case game::Race::WolfElemental:
			case game::Race::Wolf2:
				return "Wolf";
			case game::Race::WoodElf:
				return "Wood Elf";
			case game::Race::Worg:
			case game::Race::Worg2:
				return "Worg";
			case game::Race::Worm:
				return "Worm";
			case game::Race::Wretch:
				return "Wretch";
			case game::Race::Wrulon:
			case game::Race::Wrulon2:
				return "Wrulon";
			case game::Race::Wurm:
			case game::Race::Wurm2:
				return "Wurm";
			case game::Race::WurmMount:
				return "Wurm Mount";
			case game::Race::Wyvern:
			case game::Race::Wyvern2:
				return "Wyvern";
			case game::Race::Xalgoz:
				return "Xalgoz";
			case game::Race::Xaric:
				return "Xaric the Unspoken";
			case game::Race::Xegony:
				return "Xegony";
			case game::Race::Yakkar:
				return "Yakkar";
			case game::Race::Yeti:
				return "Yeti";
			case game::Race::Zebuxoruk:
				return "Zebuxoruk";
			case game::Race::ZebuxoruksCage:
				return "Zebuxoruk's Cage";
			case game::Race::Zelniak:
				return "Zelniak";
			case game::Race::Zombie:
			case game::Race::Zombie2:
				return "Zombie";
			default:
				return "UNKNOWN RACE";
			}
		}
	};





	// CDisplay::CreatePlayerActor's elemental swap (0x48f5bc..0x48f771, before the model lookup), keyed on the body
	// texture (TextureType). With UseLuclinElementals on, race 75 (ELE, one body with four skins) becomes the typed
	// Luclin elemental: texture 0 earth 209, 1 fire 212, 2 water 211, any other air 210; the typed models have one skin,
	// so the texture and HeadType become 0 (only once that model is found: CreatePlayerActor keeps race 75 and the
	// texture otherwise). Off, 209 / 212 / 211 / 210 become race 75 with texture 0 / 1 / 2 / 3. nullopt: no swap.
	struct ElementalSwap {
		std::uint16_t race = 0;
		std::uint8_t texture = 0;
	};
	std::optional<ElementalSwap> luclin_elemental_swap(std::uint16_t race, std::uint8_t texture, bool luclin_elementals);
	using hailstone::eqclient_ini_bool; // core/ini_file.h

	struct WorldState {
		std::string zone_short_name;
		std::string zone_long_name;
		std::uint32_t player_id = 0; // our own spawn id, once known
		std::uint32_t target_id = 0; // the player's current target, 0 = none
		// pinstControlledPlayer: the spawn the local player moves when it is not itself (its mount; game/mounts.h).
		std::uint32_t controlled_id = 0;
		std::uint32_t eye_id = 0; // SPA67 remote control, distinct from a rider's mount
		std::uint32_t view_id = 0; // SetViewActor's spawn, 0: the local player
		// The small boat the local player has the helm of (OP_ControlBoat, game/boats.h steer_boat): the player moves it,
		// so it is not stepped.
		std::uint32_t driven_id = 0;
		std::unordered_map<std::uint32_t, Spawn> spawns;
		// Grows each time a spawn is added to `spawns` or taken out (not when one changes): a plugin's next_spawn walk
		// keeps a sorted copy of the ids and makes it again when this moves. Whatever adds or removes one bumps it.
		std::uint64_t spawn_set_serial = 0;
		// racedata.txt and moddat.ini for SetHeights (null: the defaults, as without a RaceManager).
		std::shared_ptr<const HeightTables> height_tables;
		// Dword 0xddfba4 (NewZone +0x2b4): 1 from the startup defaults until the first OP_NewZone, which copies it.
		bool old_model_scale = true;
		// CDisplay +0x129: eqclient.ini [Defaults] UseLuclinElementals (read @0x49276f, default TRUE). On, make_actors
		// draws a race-75 elemental as the typed Luclin one (209..212); off, those as race 75 (luclin_elemental_swap).
		bool luclin_elementals = true;

		// EQPlayer::SetHeights(view_offset, anim_scale) for a spawn, from s.heights.height.
		void set_heights(Spawn& s, float view_offset = 0.0f, float anim_scale = 1.0f) const;
		// PlayerZoneClient::ChangeHeight @0x5a4ec0 (OP_ChangeSize): the spawn, then each spawn of its mount chain (its
		// mount, that one's mount ...) with the same size and SetHeights(0, 1.0).
		void change_height(Spawn& s, float size, float view_offset, float anim_scale);
		// OP_NewZone's 0xddfba4: when it changes, every spawn gets SetHeights(0, 1.0).
		void set_old_model_scale(bool on);
		// What CreatePlayerActor finds for a spawn's actor.
		struct ActorModel {
			bool tunic_point = false;    // the model has a tunic point (the actor definition's +0x34)
			bool human_fallback = false; // the race's model is not loaded: the actor is HUM / HUF instead
		};
		// CreatePlayerActor for the spawns whose actor can be made now (`actor_model` nullopt while none can, as
		// before the zone is in). First the elemental swap (luclin_elemental_swap; with UseLuclinElementals on and the
		// typed model not loaded, the spawn stays race 75). A spawn whose model is not loaded becomes a Human
		// (SetRace(1), not for a mount race; its heights stay). With a tunic point, the height fix-up: ChangeHeight(new, 0, 1.0) for the exact
		// sizes, and the ViewHeight from before put back. Runs once per spawn (and again after an illusion makes a
		// new actor).
		using ActorModelFn = std::function<std::optional<ActorModel>(const Spawn&)>;
		// Returns the spawns whose actor was made now, in id order (EQPlayer's constructor and CreateInitialActors then
		// run MakeRiderMountUp on them).
		std::vector<std::uint32_t> make_actors(const ActorModelFn& actor_model);

		// Moves every spawn's display position toward where it is predicted to be
		// now: its last position advanced by its movement since the update. `floor`
		// (optional) gives the floor height under a point, to keep spawns on the
		// ground (the landing check).
		using FloorFn = std::function<std::optional<float>(const glm::vec3&)>;
		// `region` (optional) gives the region type at a point (the low byte of its flags; 0 outside any).
		using RegionFn = std::function<std::uint8_t(const glm::vec3&)>;
		// `vehicle_under` (optional) gives the vehicle whose deck is under a point (0: none): a spawn an update puts
		// there rides it (game/boats.h).
		using VehicleFn = std::function<std::uint32_t(const glm::vec3&)>;
		// The moving remote spawn's sweep, for CollisionHit_SetCollidingType (+0x24). This observes actor hits;
		// complete remote CollisionPass positioning remains separate from the interpolation below.
		using RemoteCollisionFn = std::function<std::int32_t(const Spawn&, const glm::vec3&, const glm::vec3&)>;
		// The local player and the mount it rides (controlled_id) are not eased: they are drawn where set_local_drawn put
		// them this frame, else at their position.
		// `ground_floor` (optional): the floor ground_npcs measures against, which has the doors too (a portal stone's
		// base, a platform); unset, `floor`.
		void smooth(float dt, double now, const FloorFn& floor = {}, const RegionFn& region = {},
			const VehicleFn& vehicle_under = {}, const FloorFn& ground_floor = {}, const RemoteCollisionFn& collision = {});
		// Where the local player is drawn this frame: where the camera is, between the game clock's last two steps
		// (app/client_app_player.cpp). Sets the drawn place and heading (0..512) of the player and of the mount it rides at
		// once, so the eye on a mount follows it in the same frame; the next smooth keeps them and forgets the call.
		// Easing them as other spawns are would draw the body behind the camera.
		void set_local_drawn(const glm::vec3& position, float heading);
		bool local_drawn = false;
		// The vehicles' part of smooth (game/boats.h), run before the local player moves so it is carried by, and
		// collides with, the boat where it is drawn this frame; smooth then leaves them alone.
		void smooth_vehicles(float dt, double now);
		bool vehicles_smoothed = false;
		// The game clock's steps this frame (core/step_clock.h, docs/mechanics.md "Frame rate"), in ms: the other spawns'
		// and the vehicles' physics (AdvanceSpawn's per-step friction) runs once for each. Unset: one step of the frame's
		// dt (the original's code as written). The drawn place eases over the frame's dt either way.
		std::optional<std::vector<float>> physics_steps;
		std::optional<std::uint32_t> physics_step_time_ms; // current frame last completed StepClock time + 1
		// Hailstone: how far from the local player another moving spawn has smooth's contact probe (the
		// RemoteCollisionFn, a full movement sweep per step). It only picks the climbing animation, so a spawn
		// farther than this, which is not drawn, is not probed and has no contact. The client sets it to the
		// camera's actor clip (game::actor_clip_distance: 550 at the default ActorClipPlane 50). 0: no limit.
		float remote_probe_range = 550.0f;
		// Hailstone (option ground_npcs, a deliberate difference; docs/mechanics.md "Grounding NPCs"): an NPC on the
		// ground (flymode 0, or EQEmu's 3 out of water) that stands over the floor (by more than 0.25, at most 50), as EQEmu servers with a poor map
		// leave some, is drawn on the floor. Only where it is drawn changes (display_position, so picking and the NPC
		// walls too); its position, which range and the other rules use, stays the server's.
		bool ground_npcs = false;
		// Hailstone (option illusion_mounts, a deliberate difference; docs/mechanics.md "Riding while illusioned"): the
		// rider's own model does not keep it off its mount. The original's PlayerClient_AttachRiderToMount refuses a rider
		// whose model has no tunic point (0x58dde0) or is a mount race drawn as the human fallback, which in practice
		// means one under an illusion: the summon then fails with 5524. The mount's own dag is still needed to seat it.
		bool illusion_mounts = false;
		// The floor smooth is given changed (a zone's collision loaded, a door came or moved): every spawn is measured for
		// ground_npcs again.
		// Spawns can come before the zone's floor (character select's scene is still loaded), and one that stands
		// still would keep what it was measured against then.
		void reground();

		const Spawn* player() const {
			auto it = spawns.find(player_id);
			return it == spawns.end() ? nullptr : &it->second;
		}
	};

} // namespace hailstone::game
