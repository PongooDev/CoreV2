#include "pch.h"
#include "Athena/AI/CoreAthenaBotManager.h"
#include "AIController.h"
#include "Athena/AI/AthenaNavSystem.h"
#include "Building/BuildingContainer.h"
#include "Athena/FortAthenaAircraft.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Athena/FortPoiVolume.h"
#include "Athena/FortSafeZoneIndicator.h"
#include "Configuration.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortTeamInfo.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameThread.h"
#include "GameplayTagContainer.h"
#include "Heroes/FortHeroType.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortCheatManager.h"
#include "Player/FortPlayerState.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

namespace
{
    std::unique_ptr<FCoreAthenaBotManager> Instance;

    // The bots each player eliminated (their match results add them), and the bot pawns the game itself sent kill quest
    // events for, with each event's type.
    std::vector<std::pair<TWeakObjectPtr<AFortPlayerStateAthena>, int32_t>> BotKills;
    std::vector<std::pair<TWeakObjectPtr<UObject>, uint8_t>> NativeKillTargets;

    // Core's bot names: player-like tags (8.30 names a bot from its customization loadout, else "Bot": AFortAthenaAIBotController's
    // 0xD49760; 21.40 picks from a name list, AssignDisplayName). Each match deals them out shuffled, none twice.
    const wchar_t* const BotNames[] = {
        L"Entirely_Eva", L"Leo_x", L"VelvetNinja_GG", L"Christopher_2008", L"vRogue_99", L"xThalassa_x", L"Tate_2009", L"zLegacy",
        L"SolarSurfer_77", L"zTether_GG", L"Christopher_12", L"qStatic_99", L"VelvetGale_X", L"Christopher_08", L"YoIts_Jonathan", L"zSol_99",
        L"xSentry_GG", L"zVibe_x0", L"iiOsprey_04", L"dZealot_", L"Kinsley_23", L"Ronan_2007", L"CosmicHeron_21", L"EchoingChaser_99",
        L"LunarHornet_", L"oStalker_x0", L"kZenith_GG", L"r Fervor", L"qInfinity_99", L"zFable_x", L"kSentry_GG", L"EmeraldRunner_42",
        L"Rowan_YT", L"dZion", L"DarkTitan_GG", L"TwistedChaser_07", L"qEden_x0", L"vBane_", L"HeyIts_Ezekiel", L"Nico_23",
        L"WildCheetah_21", L"Eliana_06", L"Cameron_14", L"DefNot_Kinsley", L"SwiftBandit", L"Ruby_03", L"Benjamin_14", L"LurkingRelic_42",
        L"Atticus_05", L"dSiphon_x", L"rReverie_07", L"vAsgard_07", L"rVortexx_x", L"zWarden_", L"HeyIts_Beckett", L"Jason_10",
        L"LuckyFinch", L"TheReal_Claire", L"DefNot_Preston", L"Highly_Harrison", L"Nova_2007", L"iiHarbinger_07", L"Declan_2003", L"Theodore_2001",
        L"YoIts_Eli", L"Quinn_05", L"kFringe_x", L"LurkingMarten_X", L"Wesley_FN", L"rPyro_99", L"Grayson_02", L"Callum_14",
        L"Blake_14", L"Eleanor_02", L"Olivia_04", L"DefNot_Christian", L"Violet_14", L"Kayden_12", L"xThalassa_04", L"SmokyVortex_99",
        L"Not_Isabella", L"dChasm", L"Isaac_15", L"SubtlePirate_99", L"Calmly_Camden", L"rTether_99", L"LazySamurai_X", L"GildedSentry_",
        L"Not_Tessa", L"kVertex_x", L"Rhett_23", L"ShiftyQuiver_42", L"Jaxon_02", L"q Nexus", L"Real_Penelope", L"SolarVoyager_99",
        L"zSanctuary_x0", L"Evelyn_99", L"Shane_08", L"GoldenSamurai_X", L"Clearly_Callum", L"VibrantBison_X", L"kPinnacle_GG", L"iiTorrent_GG",
        L"qTether_04", L"Evenly_Emily", L"oZenith_x0", L"John_12", L"Olivia_2007", L"RapidCheeto_X", L"Aria_06", L"CopperRunner_42",
        L"iiParadox_99", L"HeyIts_Eden", L"dChronos_x0", L"FuzzyPanda", L"iiSpecter_GG", L"Mila_15", L"sKairo_x", L"rCrest_99",
        L"Joseph_77", L"Roman_2001", L"Colton_11", L"zVandal_x", L"Isabella_Plays", L"zVortex_x0", L"ItsMe_Josiah", L"FuzzySparrow_42",
        L"ItsJustEverett", L"kSable_07", L"r Avalon", L"vStatic_GG", L"oGale", L"sOmen_", L"xMatrix_99", L"CobaltBeetle_42",
        L"kSentry_x", L"Naomi_2001", L"Zane_01", L"Solely_Silas", L"rBane", L"PolarCanyon_21", L"Brock_09", L"vSurge_04",
        L"Zoey_06", L"Solely_Sawyer", L"QuickNomad_21", L"sRelic_x", L"kZenon_99", L"rStrobe_x", L"zStalker_x0", L"DefNot_Gunner",
        L"rCinder_x", L"zVigil_07", L"Ruby_23", L"Dalton_2003", L"SilverRebel_77", L"Jayden_2009", L"iiEmber_99", L"FrostyFox_77",
        L"vHemera_GG", L"Santiago_FN", L"Ashton_77", L"Entirely_Elias", L"SilverNinja_77", L"LuckyReef_07", L"sEternity_GG", L"ChillCanyon_X",
        L"Merely_Michael", L"iiHaze_GG", L"rAether_x", L"Declan_2006", L"Jaxon_2006", L"BrazenScout_X", L"kUtopia", L"MightyWalker_07",
        L"Deftly_Dawson", L"iiTz_Trace", L"Nora_2006", L"CosmicBeacon_X", L"MysticBandit", L"zVandal_x0", L"ItsJustNova", L"WildGale_GG",
        L"Pierce_2009", L"oCosmo_99", L"Justly_Joshua", L"Nathaniel_15", L"DustyWaffle_07", L"Jacob_2002", L"Harrison_2004", L"SaltyDrifter_42",
        L"oChaos_99", L"Theodore_15", L"Clara_TTV", L"NobleGale_X", L"Atticus_08", L"kNyx_GG", L"Aubrey_02", L"SturdyHornet_21",
        L"Brooke_2002", L"VibrantMango_", L"vLegacy_x0", L"Isaiah_x", L"oNebula_04", L"sAstral_x", L"xCobalt_GG", L"vValhalla_x0",
        L"rMiracle_07", L"Aubrey_77", L"AmberHawk_77", L"Greatly_Gael", L"kAether_", L"sSentry_GG", L"Kingston_2001", L"xVector_99",
        L"Sophia_2002", L"qPhantom_04", L"kVertex_", L"ArcticBiscuit_42", L"ItsJustEleanor", L"Vance_2006", L"dOsprey_GG", L"SleekChaser_",
        L"Vincent_2004", L"Matthew_06", L"kStyx_GG", L"Eden_2002", L"sParadise_07", L"oEuphoria", L"Waylon_15", L"LiquidBeacon",
        L"DarkSurfer_", L"zArcadia_x0", L"Cole_07", L"qPinnacle_GG", L"Just_Abigail", L"Teagan_12", L"Evenly_Eliana", L"Cyrus_21",
        L"SturdySpark_42", L"DefNot_Kaiden", L"xInfinity_07", L"Aurora_TTV", L"Avery_14", L"oRune_07", L"Brooks_03", L"DefNot_Kyler",
        L"rAura_x0", L"iiTz_Kellan", L"SleekBeast_42", L"dAsgard_", L"Heath_TTV", L"Reid_2002", L"iiHelix_", L"Leo_10",
        L"vValkyrie_04", L"Reagan_04", L"CrankyCheetah_", L"TimberWarden_GG", L"dLethe_07", L"Not_Pierce", L"dSiphon_04", L"BrazenWarden_GG",
        L"Merely_Mia", L"vWraith_x", L"Ridge_2006", L"Jason_2006", L"Ezra_TTV", L"s Haze", L"vStyx_x0", L"Deeply_Dylan",
        L"dRavage_x0", L"IronAnvil_07", L"SleekRobin_21", L"rKrono_99", L"rTartarus_x0", L"ItsJustOwen", L"Real_Stella", L"BrazenSentry_77",
        L"kDawn_GG", L"IronGuardian", L"oMiracle_99", L"RapidWarden_99", L"Waylon_FN", L"Ezekiel_2001", L"Isabella_02", L"Jackson_x0",
        L"FierceVault_99", L"Entirely_Emery", L"Fiona_88", L"Jason_YT", L"VelvetRider_21", L"Grant_23", L"zFrost_99", L"sVerve_04",
        L"xPhase_x0", L"iiSanctuary_GG", L"Rarely_Rowan", L"iiAbyss_07", L"rReverie_", L"Serenity_2001", L"Ella_15", L"Ivy_2005",
        L"Hazel_23", L"vClaw_99", L"Delilah_99", L"Sadie_GG", L"Not_Rowan", L"Jaxon_12", L"dPhanes_04", L"GildedGunner_77",
        L"Gabriel_2010", L"dStrobe_04", L"Ryder_14", L"zRaptor_x0", L"xErebus_GG", L"kNova", L"qShard_x", L"QuietPanther",
        L"Jameson_10", L"xOrigin_99", L"Jaxon_15", L"NobleCheeto", L"Entirely_Ellie", L"xAvalon_GG", L"kZephyr_07", L"Kip_12",
        L"ScarletOtter", L"Joshua_22", L"qFable_GG", L"xRelic_07", L"x Prism", L"Vance_2005", L"oRaptor_99", L"FierceCookie_99",
        L"Camden_FN", L"iiKrono_x", L"kEcho_GG", L"qInfinity_GG", L"BrazenEcho_99", L"Ronan_Plays", L"NeonSamurai_07", L"SubtleSparrow_77",
        L"iiSnipez_07", L"Abigail_2007", L"Dylan_22", L"dOmen_07", L"Anthony_88", L"Garrett_Plays", L"FierceViper_X", L"ShiftyBiscuit_GG",
        L"Lily_YT", L"Eleanor_12", L"Madison_07", L"FadedRover_X", L"xKrono_99", L"Nolan_FN", L"PrimalRider_77", L"RowdyMoose",
        L"Hunter_05", L"ToxicCanyon_", L"Isabella_10", L"Ryan_Plays", L"EmeraldHawk", L"qHalo", L"dCortex_x", L"ItsJustSummer",
        L"rCortex_", L"dSable_x", L"Not_Ozzie", L"SneakyRhino_07", L"kAstral_", L"BleakMonk_42", L"dChronos_GG", L"Wildly_William",
        L"Only_Eleanor", L"Bennett_2007", L"Ryder_22", L"Brody_FN", L"dClaw", L"Julia_07", L"oTorrent_GG", L"kStalker_x",
        L"Trace_11", L"iiTz_Jackson", L"iiTartarus_99", L"Liam_2002", L"qRadiance_04", L"Flynn_x", L"LuckyOsprey_", L"xAether_GG",
        L"Autumn_2009", L"rOnyx_", L"Garrett_02", L"ii Zephyr", L"sHalcyon_99", L"CopperSpark_GG", L"zTether", L"TimberWarden_99",
        L"Elliott_2005", L"Daisy_11", L"kPhase_07", L"Christopher_GG", L"YoIts_Rhys", L"StormyMoose_77", L"sNirvana_04", L"Santiago_02",
        L"Hunter_06", L"Briggs_04", L"rAstral_99", L"iiVigil_04", L"dTether_99", L"kSol_x", L"s Sol", L"Bentley_09",
        L"QuickSentry_42", L"Hunter_2002", L"vTempest_x", L"iiStasis_07", L"rRadiance_x0", L"Only_Caleb", L"Atticus_YT", L"DriftingValley_X",
        L"CalmWaffle_99", L"rPulse", L"Micah_2004", L"Mostly_Madelyn", L"iiVandal_x0", L"xBlaze_GG", L"HeyIts_Jack", L"BleakColossus_42",
        L"rZephyr_07", L"Zachary_2009", L"Ava_05", L"LurkingVault_GG", L"Luna_2005", L"sPinnacle_x", L"vOurea", L"TimberPanther_21",
        L"Ryder_2006", L"Hudson_2009", L"vTriton_GG", L"Sadie_03", L"rAbyss_x0", L"CrankyRover_42", L"Brody_02", L"oDrift_x0",
        L"WildArrow_07", L"Evan_21", L"QuietCondor_42", L"Honestly_Holden", L"BriskPilot_21", L"Ellis_04", L"xHemera_x", L"GildedSparrow_99",
        L"kMystic_07", L"Real_Kaiden", L"xNexus_07", L"ii Ignite", L"CalmCobra_21", L"Logan_x0", L"ArcticSpark_07", L"ItsJustEzra",
        L"ItsJustGage", L"PaleHeron_07", L"Callie_2010", L"WildHornet_X", L"sSovereign_", L"xKestrel_", L"Evenly_Emilia", L"ArcticMammoth_21",
        L"TwistedMoth_21", L"Emmett_2002", L"Atticus_2003", L"SmokyVoyager_07", L"dOurea_99", L"Jack_2009", L"Vincent_2003", L"PolarGale_21",
        L"Purely_Paisley", L"Everly_x0", L"Wyatt_11", L"SolarBlizzard_21", L"August_11", L"Christian_04", L"qSurge_x", L"SleekRider_77",
        L"Gabriel_01", L"ItsJustElena", L"qSiphon_99", L"Aurora_07", L"xArcadia_99", L"dSolstice_", L"iiAero_04", L"Ian_09",
        L"LuckyPirate_GG", L"rChaos_07", L"sAvalon_x", L"Braxton_2002", L"rThalassa_GG", L"xExile_07", L"iiSpectre_99", L"Entirely_Elijah",
        L"dAvalon_", L"dBane", L"Jason_11", L"PrimalWalker_21", L"iiLoom_GG", L"VelvetHeron_21", L"Only_Cameron", L"VelvetWeasel_07",
        L"zCobalt", L"sKrono_x0", L"sAegis_x0", L"Kyler_15", L"rKrono_07", L"ArcticEmber_X", L"iiZest_07", L"Honestly_Hudson",
        L"qSol_04", L"xHarbinger_07", L"rGoliath_", L"qNomad_99", L"Wholly_Wesley", L"VividSniper_", L"kCosmos_04", L"Just_Isaiah",
        L"zStalker", L"PaleFlare_77", L"ObsidianAnvil", L"BrightBeacon_X", L"BrightRelic_", L"Lane_14", L"rEternity_07", L"sPhantom",
        L"rVigor_", L"Sienna_x", L"Wildly_Wyatt", L"GrizzlyMango_77", L"HeyIts_Zachary", L"Clearly_Colt", L"Cole_YT", L"rVanguard_07",
        L"Brock_TTV", L"BrazenDrifter_", L"Harrison_09", L"SubtleMonk_77", L"SubtleGolem", L"GildedMoose_GG", L"IronPirate_77", L"sCatalyst_x0",
        L"qErebus_x", L"CrankyRider_99", L"SleepyCrag_99", L"sPulse_99", L"qHavoc_04", L"xStalker_99", L"zCatalyst_99", L"Avery_GG",
        L"Leo_09", L"WickedCougar_", L"GrizzlyCactus", L"StaticWeasel_GG", L"iiSanctuary_99", L"sGlade_", L"xSol", L"xRenegade_",
        L"kOmen_", L"x Strafe", L"iiTz_Kip", L"Gael_05", L"Not_Ryder", L"Jasper_15", L"Clara_05", L"ObsidianRobin_",
        L"xCatalyst_x", L"ItsMe_Ashton", L"Brock_2003", L"Evan_2003", L"iiVortexx_x0", L"vSerenity_", L"Benjamin_x0", L"Not_Faith",
        L"dNyx", L"Rapidly_Roman", L"Hadley_2007", L"kRift_GG", L"iiGlade_99", L"NeonPilot", L"rMiracle_99", L"Silas_Plays",
        L"Not_Nova", L"ToxicSurfer_42", L"LunarGunner_", L"rVapor_x0", L"Reagan_2002", L"Mia_2010", L"ArcticSeeker_X", L"Swiftly_Santiago",
        L"sLoom", L"FadedMoth_X", L"Enzo_GG", L"GrizzlySentry_21", L"Crisply_Chase", L"dArcadia_x", L"Purely_Parker", L"SubtleRunner_77",
        L"SmokyHunter", L"iiSlate_x0", L"Wholly_Weston", L"YoIts_Hunter", L"oChronos_", L"Miles_2002", L"ItsMe_Wesley", L"SleepyBandit_21",
        L"David_01", L"xRelic", L"Milo_22", L"Emily_2001", L"xEden_x0", L"Gage_12", L"IronPirate_X", L"Maverick_11",
        L"x Dawn", L"Faith_06", L"Abigail_2004", L"Softly_Serenity", L"George_88", L"PrimalAnvil_77", L"oOurea_04", L"TimberMammoth_GG",
        L"dOutlaw_", L"zBane_GG", L"Mackenzie_2003", L"sTempest_x", L"rSerenity_04", L"Everly_06", L"dZealot_99", L"QuietBiscuit_07",
        L"iiTz_Leo", L"Knox_2001", L"WhoIsCamden", L"kZenith", L"rLyric_", L"LunarBiscuit", L"Cooper_YT", L"CrimsonRidge_21",
        L"rCipher_", L"Kellan_21", L"Vance_04", L"ShiftyCrag_99", L"rDawn_07", L"WildRunner_X", L"Lincoln_77", L"ItsJustOzzie",
        L"oTalon_x0", L"Not_Willow", L"Bentley_2008", L"BleakBiscuit_99", L"IronRebel_", L"sInfinity_99", L"Merely_Miles", L"Dante_GG",
        L"ItsJustBeckett", L"rProwl_x", L"FadedRider_77", L"ShiftyBandit_21", L"rJinx_x0", L"xLyric_07", L"Matteo_2003", L"dDawn_x0",
        L"NobleCobra_42", L"sScythe_x", L"George_15", L"BriskBeacon_21", L"Real_Melanie", L"oJinx_07", L"SolarValley_X", L"RapidRobin_GG",
        L"v Lyric", L"Holden_2009", L"Aurora_15", L"oApex_99", L"Calmly_Clara", L"iiTz_Max", L"Santiago_14", L"ShadowWarden_GG",
        L"rStatic_99", L"vSolstice_99", L"VividSurfer_GG", L"Dax_12", L"Greatly_Gavin", L"ObsidianSpark_", L"iiAnanke_GG", L"zNexus_x",
        L"Sebastian_2004", L"TwistedBadger_", L"kGoliath_", L"Aiden_TTV", L"LunarQuiver_X", L"Chloe_2001", L"Miles_77", L"Josiah_09",
        L"WickedMoth_77", L"Gladly_Garrett", L"xClaw_GG", L"Mostly_Mackenzie", L"Enzo_21", L"Mateo_23", L"iiLethe_04", L"YoIts_Sawyer",
        L"NobleMango_77", L"Maverick_08", L"BleakCobra_99", L"HollowHornet_21", L"Callie_TTV", L"oVapor_x", L"oDraco_07", L"Colby_06",
        L"Ryan_2002", L"YoIts_Lincoln", L"Ozzie_15", L"Kaden_x", L"rFrost_04", L"David_2006", L"Delilah_09", L"Noah_2001",
        L"x Drift", L"kEclipse_04", L"Just_Nico", L"Santiago_03", L"Just_Matthew", L"Brooks_2006", L"Hudson_2007", L"ShadowMonk_99",
        L"QuietMonk_GG", L"Fiona_Plays", L"Hayes_2002", L"QuietGuardian_42", L"Not_Tate", L"GildedHarbor_", L"xDestiny", L"WickedOsprey_21",
        L"Just_Zoey", L"Warmly_William", L"DustyCougar_07", L"ShiftyPickle_GG", L"zTranquil_07", L"oEcho_04", L"Jasper_07", L"vTitan_GG",
        L"iiTz_Lincoln", L"Kaden_22", L"Christian_21", L"dStatic_04", L"Tanner_2003", L"rNirvana_GG", L"Luke_04", L"GoldenCookie_",
        L"FrostyNomad_42", L"HeyIts_Thomas", L"Briskly_Beau", L"Gage_15", L"xPontus_", L"Luna_22", L"Spencer_2004", L"DarkVoyager_07",
        L"sTalon_04", L"Lydia_2005", L"zTorque_07", L"zDraco_99", L"Sadie_2002", L"Oliver_03", L"dSentry_07", L"CobaltWaffle_07",
        L"qVibe_x0", L"DefNot_Xavier", L"RowdyRhino_", L"oOdyssey_04", L"oOsprey_07", L"Rarely_Ronan", L"zCinder_GG", L"GrizzlyRebel_77",
        L"Softly_Sofia", L"sNyx_x", L"rTitan_04", L"iiVandal_x", L"GhostRanger_99", L"sTitan_GG", L"qBane", L"YoIts_Cameron",
        L"o Phase", L"MellowRidge_77", L"dGlade", L"Just_Roman", L"WhoIsNolan", L"dFringe_x0", L"Faith_2006", L"Trace_22",
        L"oSable_04", L"sAstro_07", L"Pierce_2003", L"Matthew_15", L"dDraco_04", L"YoIts_Delilah", L"FuzzyOsprey_07", L"Jett_08",
        L"vValkyrie_", L"iiCrest_x", L"Micah_04", L"Micah_22", L"vVertex_99", L"Vividly_Vivian", L"vStrobe_GG", L"Reagan_2007",
        L"Landon_04", L"r Vandal", L"dTorque_04", L"vRavage_x0", L"LazyDrifter_42", L"iiShard", L"vLoom_", L"TimberOtter_77",
        L"SmokyNinja_42", L"Kip_FN", L"BrightSniper", L"LiquidCanyon_77", L"Luna_2003", L"Santiago_07", L"zSentry_07", L"dDestiny_",
        L"zStrobe_x", L"IronSparrow_GG", L"z Siphon", L"TwistedRover_GG", L"iiTz_Tate", L"dAstro_x0", L"Faith_88", L"Autumn_05",
        L"qTempest_x0", L"ToxicRelic_42", L"QuickRover_", L"iiArcane", L"Jonathan_01", L"dGlade_x0", L"Ian_GG", L"Joshua_77",
        L"Brock_88", L"Hudson_99", L"dValhalla_x0", L"Micah_02", L"Just_Ellis", L"Callum_2009", L"SubtleHeron_GG", L"vStrafe_GG",
        L"Matteo_GG", L"Matteo_99", L"MightyPilot_GG", L"Sullivan_14", L"iiOrigin_04", L"David_2002", L"vSiphon_04", L"SubtleBeast_21",
        L"kSerenity_07", L"GoldenGolem", L"WildChaser_99", L"Angel_2004", L"vSol", L"Rowan_2003", L"Julian_05", L"DriftingNomad_77",
        L"qGlade_GG", L"Vividly_Violet", L"v Aether", L"Kingston_12", L"rSnipez_99", L"Christopher_99", L"sChasm_07", L"Only_Jackson",
        L"Parker_08", L"ItsJustSophia", L"oRelic_", L"BrightBlizzard_", L"SleepyHawk", L"xDynasty_07", L"Alice_2004", L"xSerenity",
        L"Spencer_Plays", L"d Vandal", L"HollowBadger", L"dHalo_99", L"kAegis_x", L"NobleWasp_", L"Aiden_YT", L"oQuake_04",
        L"YoIts_Robert", L"Only_Kinsley", L"sStyx_07", L"iiHemera_", L"vStasis_x0", L"iiHemera_99", L"r Pulse", L"GildedTyphoon_99",
        L"MellowRaven_GG", L"Jayden_GG", L"Waylon_05", L"PrimalRobin_21", L"kTempest", L"zPyro_", L"sTorque_04", L"Delilah_07",
        L"Evan_22", L"sClaw_", L"qThalassa_", L"Finley_GG", L"rWarden_99", L"zSpectral_GG", L"vOurea_x0", L"Landon_2010",
        L"iiNova", L"xHorizon_04", L"qZealot_", L"SmokyReef_07", L"vEchoes_04", L"Kip_14", L"Bentley_03", L"Easton_2003",
        L"rParadise_x", L"rVector_04", L"MightyPanda_GG", L"Elliott_15", L"rRune", L"dPyro_07", L"iiClinch_99", L"BrazenBiscuit_X",
        L"rHarmony_", L"Barely_Brooke", L"sVex_x0", L"Ruby_x", L"rTempest", L"dSnipez_", L"qShard_99", L"Kaiden_04",
        L"sSilhouette_GG", L"qClaw_04", L"rHaven_07", L"qOutlaw_", L"Deeply_Dante", L"dNexus_07", L"kAero_x0", L"EmeraldRanger_42",
        L"zPontus_07", L"IronBandit_99", L"Only_Aurora", L"Lane_2010", L"Kyler_07", L"rStatic_x", L"Nolan_09", L"SolarAnvil_",
        L"qPhanes_07", L"Santiago_22", L"s Eden", L"iiCortex", L"ShiftyTitan_42", L"Easton_14", L"Bennett_2002", L"Evenly_Elizabeth",
        L"Milo_21", L"dExile_x", L"TheReal_David", L"VividNinja_99", L"Layla_FN", L"kOlympus_x", L"HeyIts_Emily", L"xTranquil_",
        L"StormyNinja_21", L"Santiago_2005", L"s Snipez", L"DustyPanther_99", L"Drake_99", L"DriftingFox_07", L"Paisley_2008", L"CopperTrooper_42",
        L"Hadley_YT", L"TheReal_Joseph", L"qSurge_", L"z Drift", L"Michael_77", L"xHavoc_99", L"Colt_10", L"zPontus_GG",
        L"Orion_TTV", L"Madelyn_x0", L"xOnyx", L"RustyScout_99", L"dZest_", L"zSovereign_07", L"Elijah_14", L"rEclipse_x0",
        L"StaticRhino_77", L"Hadley_FN", L"Sullivan_07", L"Only_Matteo", L"xSlate_", L"Chase_99", L"Hadley_04", L"FuzzySpotter_21",
        L"Clearly_Caleb", L"Charles_02", L"Kai_14", L"Only_Evan", L"Isla_2008", L"DarkCrag_77", L"qThalassa_GG", L"Emily_TTV",
        L"TheReal_Nathan", L"Andrew_2001", L"vEclipse_x", L"Claire_08", L"Eden_04", L"Vivian_FN", L"qZealot_GG", L"Calmly_Chloe",
        L"Nathaniel_08", L"oGale_", L"Ella_x0", L"IroncladSamurai", L"sVex", L"Joshua_2007", L"Ian_02", L"ObsidianVoyager",
        L"ItsJustChase", L"Oliver_07", L"Finley_2006", L"dZealot_04", L"Spencer_GG", L"ArcticRidge", L"iiNirvana_GG", L"dEmber_x",
        L"qNomad_GG", L"iiCortex_", L"sVortex_99", L"RustyBlizzard_21", L"Aubrey_x0", L"qSolstice_99", L"Gabriel_14", L"Ridge_2003",
        L"Crisply_Clara", L"Gideon_04", L"StormyRobin_21", L"FierceSeeker_42", L"Joshua_02", L"Declan_11", L"x Zealot", L"iiDestiny_",
        L"rPyro_GG", L"LazyGale_21", L"Nathaniel_YT", L"vFringe", L"rRenegade_x0", L"Matthew_22", L"Kaylee_22", L"Elliott_07",
        L"HeyIts_Maya", L"GrizzlyWasp", L"iiTz_Camila", L"DefNot_Elijah", L"Nora_06", L"rOutlaw_07", L"Softly_Shane", L"ChillRover_07",
        L"FierceBison_77", L"zClaw_x", L"Aaron_FN", L"Just_Ezra", L"vOdyssey", L"kScythe_07", L"SavageGale_07", L"MellowCanyon_77",
        L"Only_Sullivan", L"Cooper_05", L"Not_Daniel", L"CrankySamurai_42", L"iiTz_Leonardo", L"xVapor", L"MysticNinja_07", L"zScythe_x",
        L"FadedHunter_X", L"HeyIts_Matteo", L"YoIts_Daisy", L"ShiftyFlare_X", L"Warmly_Wesley", L"DefNot_Drake", L"Brielle_2005", L"Greyson_12",
        L"xStalker_", L"CrimsonEcho_21", L"zDusk_", L"ScarletOsprey_21", L"SolarHarbor_", L"o Torque", L"vValor_99", L"Vastly_Vincent",
        L"dPrism_x", L"kAegis_04", L"sGlitch", L"GoldenHunter_99", L"zNirvana_", L"Easton_2009", L"Thomas_2003", L"dPrism_",
        L"Vivian_02", L"vValor", L"vSable_99", L"SilverBandit_21", L"Vivian_2003", L"PolarOsprey_42", L"Beckett_21", L"VibrantCheeto_42",
        L"qTether_x0", L"Colby_2002", L"WickedOsprey", L"Hudson_07", L"xJinx_x0", L"d Zenon", L"Neatly_Nathan", L"sSanctuary_GG",
        L"dNebula_", L"dGoliath_", L"MightyHeron_21", L"Carlos_Plays", L"Sadie_2007", L"Thomas_GG", L"vRogue_x", L"zHavoc_07",
        L"ScarletCrag", L"ItsJustAlice", L"Nolan_2010", L"CrankyMammoth_X", L"Crisply_Cora", L"FierceScout_42", L"Mostly_Maya", L"IroncladTitan_07",
        L"rMirage_x0", L"Isabella_2003", L"Just_Eli", L"NeonFalcon_X", L"Justly_Julian", L"Hudson_77", L"Ariana_77", L"ChillSummit_77",
        L"Ryan_x", L"sOrigin_GG", L"kMiracle_99", L"Jonathan_88", L"Isaiah_22", L"CosmicBeast_42", L"GildedRidge_X", L"SilverBeast_07",
        L"Colt_2010", L"Judah_TTV", L"YoIts_Ryan", L"dHavoc", L"Jameson_08", L"kValhalla_04", L"SmokyMoose_42", L"Ava_77",
        L"Vance_2001", L"Camila_GG", L"Isaiah_08", L"Greatly_Grayson", L"Dylan_2006", L"iiTz_Spencer", L"v Pontus", L"Aurora_99",
        L"BleakBadger_21", L"sStrafe_99", L"xHorizon_07", L"GildedWalker_99", L"Justly_Joseph", L"zVapor_99", L"Trace_2001", L"Sofia_77",
        L"SneakyCougar_", L"Declan_88", L"qPulse_07", L"MightyTyphoon_21", L"BrightGuardian", L"John_99", L"xHaze_04", L"iiCrest_x0",
        L"Briskly_Brock", L"Not_Kai", L"sStatic_07", L"QuietBlizzard_42", L"zHarmony_04", L"LazyMarten_07", L"Zoey_x", L"dBane_",
        L"vIgnite_", L"zZenith", L"kFrost", L"AmberCrane", L"zNova_", L"kHelix_07", L"ItsJustGarrett", L"rLegacy_x0",
        L"iiRaptor_x0", L"kVertex", L"kMatrix_04", L"Crisply_Chloe", L"Gael_04", L"sPhase_99", L"Ashton_07", L"Grant_11",
        L"Hadley_03", L"TheReal_Archer", L"Trace_99", L"MightyBlizzard_X", L"HeyIts_Ronan", L"r Shade", L"DefNot_Arlo", L"WhoIsHailey",
        L"Jackson_03", L"qCrest_GG", L"GildedAnvil", L"Cyrus_04", L"CopperFinch_07", L"rEmber_", L"Luna_03", L"xOmen_",
        L"oAegis_07", L"sEchoes_", L"BleakJaguar_X", L"Dallas_2008", L"Bennett_99", L"KeenCobra_07", L"TheReal_Quinn", L"sShard_GG",
        L"ItsMe_Eden", L"Sadie_FN", L"Zane_2005", L"Only_Bennett", L"Gael_23", L"Garrett_22", L"zUtopia_99", L"Only_Shane",
        L"Just_Aiden", L"LiquidArrow", L"Brayden_77", L"Aubrey_2002", L"Chloe_2009", L"SleekSentry_77", L"dTartarus_04", L"vSilhouette_x0",
        L"VividHawk_99", L"Elliott_2007", L"ItsJustIsla", L"SmokyDrifter_99", L"sEmpire_04", L"iiParadise_x0", L"rVandal_99", L"ShiftyBeacon_GG",
        L"dSpecter_GG", L"Wholly_Wyatt", L"z Echo", L"WickedBeetle_X", L"dTether_", L"Jonathan_15", L"Nora_x0", L"Elijah_22",
        L"RapidBadger_99", L"Evenly_Everett", L"Ryder_TTV", L"ItsJustRyder", L"oDrift_GG", L"ii Shard", L"vOlympus_07", L"oVandal_GG",
        L"Only_Penelope", L"Jonathan_06", L"vTalon_99", L"Easton_TTV", L"rAstral_x0", L"Evelyn_09", L"Brody_99", L"FierceCookie_GG",
        L"Pierce_09", L"LuckyBeast_77", L"Tobias_01", L"dValor_99", L"zKrono_04", L"RapidKnight", L"Emery_2002", L"PaleHarbor_07",
        L"xValhalla_07", L"xWraith_", L"Hank_TTV", L"Barely_Brooks", L"iiUtopia_x0", L"xAstro_", L"Boldly_Brody", L"ItsJustIsabella",
        L"rProwl", L"Delilah_77", L"Jaxon_x0", L"CrimsonSniper_42", L"qSol_GG", L"DustyArrow_77", L"xTether", L"dRhythm_04",
        L"Justly_Jett", L"Logan_09", L"Not_Gianna", L"xGlade_", L"Judah_88", L"Reid_12", L"dSlate_99", L"TheReal_Naomi",
        L"Isaiah_23", L"Nico_06", L"Boldly_Bentley", L"iiInfinity_x", L"Just_August", L"Gael_Plays", L"Ezra_11", L"iiTz_Mackenzie",
        L"GhostSurfer_", L"sZenith_", L"iiBliss_", L"Lily_09", L"Cameron_GG", L"Aaron_2005", L"kRavage_", L"GrizzlyRider_21",
        L"KeenViper_21", L"Mateo_2003", L"Faith_15", L"Grayson_10", L"Lane_77", L"Fiercely_Fiona", L"kFlux_x", L"Bennett_14",
        L"kVex_x", L"Barely_Bentley", L"Kaylee_04", L"Holden_x0", L"xWarden_04", L"EchoingAnvil_X", L"xHaze_", L"vCosmos",
        L"Mason_x", L"MellowLynx", L"zMatrix_07", L"Aiden_07", L"ItsJustDeclan", L"Swiftly_Sienna", L"Ezra_2001", L"Entirely_Emma",
        L"DustyRaven_21", L"Reid_2003", L"Lydia_11", L"LurkingGale_21", L"oMatrix_GG", L"Clara_GG", L"MightyGolem_99", L"HollowSparrow_21",
        L"rHorizon_GG", L"Sophia_08", L"Real_Austin", L"TheReal_Leonardo", L"GildedNomad_21", L"sChaos_07", L"LuckyRebel_X", L"CalmSpark",
        L"MysticGlacier_", L"dChronos_", L"Adrian_2002", L"Jason_x", L"Merely_Matteo", L"xSpectral_", L"Reagan_Plays", L"Zane_2007",
        L"Kayden_2005", L"Lucas_23", L"Scarlett_22", L"DefNot_Colby", L"SlickJaguar_21", L"zOdyssey_04", L"Gavin_YT", L"ArcticBandit_07",
        L"vAether_x0", L"oCortex_x", L"iiTz_Maverick", L"ShiftyQuiver_21", L"oZest_x0", L"oZest_", L"FuzzyBadger_", L"Fiona_09",
        L"Christian_YT", L"iiPulse", L"Real_Sawyer", L"Mostly_Matthew", L"q Drift", L"SubtleCanyon_X", L"Hadley_23", L"SturdyWaffle_99",
        L"SolarLynx_GG", L"SolarBeast", L"vAegis_GG", L"kPrism_GG", L"Kaylee_2010", L"zLoom", L"BrazenMonk_", L"sVex_",
        L"dTempest_x", L"iiCosmos_x", L"qHarbinger", L"oAether_GG", L"qHarmony", L"Easton_2010", L"kScythe_99", L"SavageGlacier_07",
        L"Evan_TTV", L"iiInfinity_99", L"ItsMe_Atticus", L"Eva_2010", L"kThalassa_x", L"rNesoi_99", L"Gladly_Gunner", L"vClinch_GG",
        L"xVigor_GG", L"Austin_23", L"d Phase", L"rHalo", L"xTorque_x", L"Briggs_2010", L"Drake_07", L"zCortex_x",
        L"Asher_06", L"kPulse_99", L"John_03", L"DefNot_Dawson", L"Gladly_Gianna", L"Matteo_2005", L"Merely_Mateo", L"StormyWeasel_99",
        L"LunarEmber_", L"Brielle_23", L"Nora_03", L"PrimalWeasel_77", L"iiSpectre_x0", L"oReign_x", L"Kayden_01", L"IronPanther_77",
        L"Emily_22", L"Sofia_03", L"MightyHawk_21", L"qDestiny_04", L"Eliana_x", L"Aaron_x", L"Madison_14", L"CopperSentry_21",
        L"SleekCobra_77", L"vHalo_07", L"ItsMe_Delilah", L"DefNot_Nathaniel", L"Leonardo_GG", L"Emmett_09", L"QuickWeasel", L"sTitan",
        L"Orion_01", L"Shane_23", L"Evenly_Evan", L"Faith_03", L"s Torque", L"Wesley_11", L"dNova_x", L"Just_Vance",
        L"Bentley_2001", L"Mia_05", L"xNesoi", L"sHelix_07", L"oDynasty_04", L"Gavin_2007", L"vVapor_99", L"vRift_",
        L"Tate_2004", L"John_11", L"SleepyHawk_99", L"rSpecter_04", L"rWraith_07", L"HeyIts_Damian", L"GrimCascade_77", L"rEnigma",
        L"iiTz_Ian", L"x Asgard", L"zAstro", L"Real_Scarlett", L"zCosmo_x0", L"ToxicCookie_", L"zTriton_99", L"Flynn_21",
        L"sZest_GG", L"xRhythm_", L"Greyson_02", L"LurkingOsprey", L"Hailey_03", L"sClaw_x", L"Bennett_03", L"dAnanke_04",
        L"Enzo_FN", L"Frankly_Flynn", L"Bryson_2006", L"IronCheeto_X", L"kOmen_04", L"DarkSniper", L"xDraco", L"Josiah_YT",
        L"rOlympus_GG", L"rSpectre_GG", L"iiDrift", L"SubtlePirate_07", L"Matthew_2005", L"sAero_99", L"s Pulse", L"sSnipez_x",
        L"oEden_x0", L"SmokyCobra_X", L"dAura_x", L"Only_Aaron", L"LunarBeast_X", L"ItsMe_Clara", L"VibrantBeetle_", L"John_10",
        L"ToxicCanyon_07", L"Alice_22", L"oPinnacle_x", L"ii Aura", L"sVertex_x0", L"Aaron_77", L"oJinx_99", L"CrispyRelic_21",
        L"Cole_2001", L"rRelic_99", L"Preston_09", L"William_14", L"Hudson_21", L"zVanguard", L"ItsJustRobert", L"FrostyKnight_99",
        L"Clearly_Colton", L"RapidValley", L"TheReal_Piper", L"FrostyCanyon_", L"TheReal_Maya", L"kRenegade_04", L"xVertex_x0", L"WickedBeetle_07",
        L"sPontus_99", L"CrankyFox_42", L"iiAstro_x", L"sStrata", L"YoIts_Stella", L"Callie_2005", L"Elias_2006", L"Kayden_2006",
        L"Only_Lucas", L"Briggs_2001", L"Ryan_07", L"Clearly_Cyrus", L"SmokyRunner_X", L"Lydia_21", L"Maya_05", L"SneakyVoyager_GG",
        L"qHavoc_99", L"BleakPirate_", L"Boldly_Bennett", L"Greyson_06", L"zVerve_99", L"Leonardo_06", L"SmokySentry_07", L"Briskly_Beckett",
        L"SleekMango", L"Hailey_07", L"PrimalRobin_77", L"sSovereign_99", L"Asher_2006", L"Hailey_YT", L"HeyIts_Isaiah", L"DefNot_Aurora",
        L"WhoIsCaleb", L"Clearly_Chloe", L"vZephyr_", L"Chloe_09", L"sNexus_99", L"Layla_01", L"v Vector", L"q Stasis",
        L"oDaze", L"kTalon_x0", L"Dax_2009", L"ToxicPanda_X", L"vRune_GG", L"Kaylee_x", L"ItsMe_Liam", L"MightyCheetah_X",
        L"iiRadiance_GG", L"sEclipse_x0", L"Andrew_TTV", L"x Prowl", L"Evelyn_22", L"dVortexx_04", L"GrizzlyFinch_X", L"FrostyBadger_99",
        L"Ozzie_21", L"dZealot_07", L"qScythe_x0", L"Only_Hunter", L"zTalon_07", L"kBlaze_04", L"iiGale_07", L"PaleEcho",
        L"Lucas_2008", L"Beau_2009", L"TwistedMoth_42", L"oHalo_99", L"qOrigin_GG", L"Greyson_2002", L"Colby_12", L"Gemma_GG",
        L"zRhythm_07", L"dVertex_99", L"DefNot_Parker", L"iiCipher_", L"z Prism", L"Nash_99", L"Elias_88", L"CobaltPanther_21",
        L"iiTartarus_x0", L"Lively_Luna", L"BrazenBison_21", L"iiTz_Sadie", L"vNomad", L"r Nesoi", L"Dallas_77", L"SmokyRidge_GG",
        L"Emily_06", L"Real_Ronan", L"kRelic_x0", L"ChillMango_99", L"Atticus_2010", L"Serenity_06", L"Beau_2002", L"Dalton_77",
        L"kCosmo_", L"IronRanger", L"Jace_03", L"Ridge_09", L"IronSeeker_42", L"Colby_2004", L"Dallas_2004", L"rVibe_04",
        L"SaltyMammoth", L"Joseph_11", L"Kellan_22", L"Owen_11", L"SubtleRebel_99", L"Matthew_14", L"ItsMe_Brock", L"EmeraldCanyon_99",
        L"Camden_07", L"Nathaniel_2010", L"oVortexx", L"Carter_08", L"Robert_GG", L"Emmett_x", L"HeyIts_Carter", L"GoldenGunner_GG",
        L"Levi_02", L"Just_Aria", L"qEclipse_x", L"xGale_07", L"oSpire", L"zDraco_04", L"Summer_03", L"ObsidianRebel_77",
        L"Declan_TTV", L"Gemma_TTV", L"Paisley_22", L"ii Omen", L"Rowan_77", L"Josiah_03", L"dTriton_04", L"SleepyBiscuit_GG",
        L"Only_Joseph", L"Tristan_21", L"Damian_14", L"sStrobe_", L"qErebus_", L"Only_Delilah", L"IronHornet_42", L"qSpectral_x0",
        L"Scarlett_2001", L"qTorrent_x0", L"sGlitch_", L"dValhalla_04", L"Declan_03", L"vPyro", L"xOmen_x", L"EchoingFalcon_GG",
        L"xAsgard_x0", L"xGale_99", L"TheReal_Oliver", L"SilentWalker_21", L"Quinn_2006", L"q Ourea", L"Willow_03", L"LunarRanger",
        L"vRaptor_", L"Scarlett_02", L"sCrest_04", L"xSpectre", L"TheReal_Wesley", L"Swiftly_Silas", L"Carter_07", L"SleepySpark_21",
        L"oTether_x", L"Cyrus_15", L"vChaos_07", L"Vance_88", L"ItsJustAubrey", L"qValkyrie_04", L"sOsprey_x0", L"Logan_05",
        L"SavageGale_", L"Jack_12", L"Benjamin_21", L"Promptly_Pierce", L"Rarely_Ryder", L"Aubrey_08", L"Kip_06", L"Tristan_22",
        L"GrizzlyCrane_X", L"dZephyr_GG", L"Real_Owen", L"GoldenRanger_07", L"zClaw_", L"vSol_GG", L"oValhalla_07", L"xHelix_04",
        L"vStyx_x", L"SaltyRidge_99", L"zSol_x0", L"QuietRunner_GG", L"Truly_Tyler", L"DarkJaguar_42", L"Kip_03", L"oHemera_",
        L"LurkingReef_99", L"ToxicRidge_", L"SneakyTrooper", L"GrimCactus_X", L"Clearly_Colby", L"Cooper_2009", L"LuckyCondor_GG", L"IronWalker_GG",
        L"Jaxon_2002", L"Isla_88", L"rStalker_04", L"SolarTrooper", L"Nova_x", L"iiTorrent_07", L"Kingston_23", L"Gavin_2001",
        L"o Shard", L"zSiphon_99", L"z Pontus", L"FadedViper_99", L"GrimHeron_07", L"oOsprey_x", L"Orion_77", L"Reagan_TTV",
        L"oJinx", L"Dalton_02", L"kSanctuary", L"Not_Brielle", L"Emilia_2010", L"Deftly_David", L"Skylar_04", L"EchoingSummit_99",
        L"zAstro_99", L"CalmBandit_GG", L"Henry_2005", L"CrimsonPanda_42", L"qVandal_", L"Micah_2010", L"oGoliath_07", L"r Pontus",
        L"dSol_07", L"iiTz_Dax", L"qSol_", L"ItsJustElizabeth", L"dTitan_07", L"Ava_2008", L"Colt_07", L"xStrobe_04",
        L"Luna_YT", L"Jason_2009", L"Nathaniel_x0", L"Sofia_99", L"DustyKnight_07", L"Ronan_07", L"Tate_05", L"Emilia_2001",
        L"q Frost", L"GrimBadger_99", L"Reagan_x0", L"ItsMe_Sebastian", L"WildFinch_77", L"SturdyGlacier", L"Naomi_05", L"dFlux_x0",
        L"SavagePilot_42", L"HeyIts_Grace", L"qAbyss_GG", L"Hudson_TTV", L"rValkyrie_", L"dNexus_x", L"Boldly_Beau", L"o Onyx",
        L"Nash_88", L"LurkingSpotter", L"sOsprey_x", L"Samuel_GG", L"Madelyn_2002", L"WildOtter_42", L"QuietCheetah_X", L"Adrian_2010",
        L"Emma_23", L"zGlitch_99", L"s Raptor", L"vStasis_x", L"Only_Colton", L"Neatly_Noah", L"Sofia_TTV", L"Keenly_Knox",
        L"Josiah_x", L"kSolace_07", L"Kingston_03", L"Evenly_Eden", L"sNova_x", L"Zane_14", L"LazyScout_42", L"Orion_11",
        L"Sloane_x", L"iiTz_Holden", L"Zoey_2005", L"Delilah_23", L"SlickEmber", L"sLyric_04", L"Evan_01", L"Real_Piper",
        L"zCipher_99", L"iiTz_Cameron", L"CrankyHawk_07", L"PrimalNomad_21", L"Asher_01", L"sFable_07", L"iiFrost_", L"TheReal_Zachary",
        L"Brayden_2008", L"Real_Sadie", L"ArcticSeeker_", L"xStyx_x0", L"dEden_", L"vGlitch", L"Real_Rowan", L"Nathaniel_10",
        L"Quinn_08", L"oVapor_GG", L"Cooper_77", L"oArcadia_x", L"William_11", L"Luke_07", L"Anthony_10", L"Brayden_FN",
        L"vThalassa", L"Emmett_88", L"zOlympus_99", L"kWraith_x", L"xVerve_x", L"Tate_14", L"LuckyBeetle_", L"CobaltPickle_99",
        L"GrimTrooper_X", L"xStrafe_99", L"ShadowBison", L"Real_Cameron", L"ShiftyCascade_", L"QuickMoth", L"Nora_14", L"Benjamin_06",
        L"WildOtter_07", L"Emily_05", L"v Wraith", L"k Ourea", L"Colt_Plays", L"vScythe_x0", L"Eli_88", L"k Abyss",
        L"sNomad_x0", L"rSolstice_07", L"Jackson_21", L"Sadie_2006", L"xGoliath", L"Santiago_TTV", L"Pierce_05", L"VelvetCheeto_GG",
        L"TheReal_Dax", L"xOdyssey_", L"vObsidian_04", L"vTartarus", L"Hadley_TTV", L"SleekPirate_X", L"Dante_08", L"sZephyr_GG",
        L"q Onyx", L"Beau_08", L"qLoom_x", L"Caleb_TTV", L"Daisy_23", L"Greatly_Grace", L"Keenly_Kaylee", L"rSovereign_07",
        L"xLegacy_GG", L"Asher_x0", L"dMatrix_07", L"Kellan_2009", L"RustyWizard_21", L"TheReal_Jason", L"vVandal_", L"Liam_TTV",
        L"xSpecter_GG", L"Flynn_99", L"iiTz_Felix", L"v Sol", L"ArcticCrane_42", L"dDaze_GG", L"Deeply_Declan", L"kFable_99",
        L"WhoIsGemma", L"zChasm_x", L"Vance_2003", L"rZenon_99", L"dGlade_07", L"xArcane_x0", L"SturdyBeast_", L"SolarCrane_77",
        L"Avery_2008", L"Elizabeth_x0", L"vStalker_x", L"CrankyBeacon", L"WhoIsPenelope", L"Kip_10", L"Elliott_21", L"MysticSurfer_GG",
        L"qSlate_04", L"Beckett_2001", L"Quinn_88", L"vCipher_x0", L"Claire_x", L"qIgnite_GG", L"Eden_2003", L"vWarden",
        L"q Dusk", L"ItsMe_Kai", L"Real_Mateo", L"Camden_21", L"xChaos_GG", L"ShiftyRhino", L"vEmpire_x", L"dBlaze_07",
        L"Oliver_FN", L"iiTz_Tristan", L"qKrono", L"iiDusk_GG", L"Lane_08", L"oSpecter_99", L"qPhantom_x", L"TheReal_Carlos",
        L"Simply_Sadie", L"WildVoyager_42", L"Briskly_Bodhi", L"oMystic_07", L"ItsJustElias", L"Carlos_2002", L"YoIts_Ruby", L"ShadowNomad_X",
        L"xSniperWolf", L"Kayzo_YT", L"NoScopeNate", L"ToxicTurtle77", L"GhostRider_99", L"Zyphr", L"Frostbyte_TTV", L"LilJimmy2009",
        L"SweatyMango", L"BuilderBob_42", L"Dr4gonSlay3r", L"CrankKing", L"NightOwlGG", L"Blitzkrieg_X", L"TacoTuesday88", L"PixelPanda",
        L"MrBushCamper", L"ShadowStrike21", L"Vortexx", L"JustAFloss", L"CaptainCrunchy", L"TheRealDefault", L"OmegaWolf_", L"HeadshotHarry",
        L"Sk8rBoi_14", L"CoolKid_Max", L"Lemonade_Jay", L"iiFlame", L"BananaBoat_7", L"KingKaiz", L"NoobMaster_69", L"Mystic_Ninja",
        L"Turbo_Tom", L"LlamaDrama22", L"xXSlayerXx", L"Raptor_GG", L"Ruby_Rose", L"EpicGamer2010", L"ChillVibes_", L"Moonwalker88",
        L"Crypt1c", L"ZeroGravity_", L"BigChungus_44", L"Fl1ckGod", L"SilentAssassin", L"SunnyDayz", L"Jaxon_Plays", L"OG_Tilted",
        L"Bacon_Bits99", L"WhyNoW1n", L"Sonic_Boom7", L"Kr4ken", L"FreshPrince_", L"GoldenGoose23", L"StormChaser_X", L"DabOnEm2018",
        L"Cactus_Jack11", L"Mav3rick_", L"PotatoAim", L"LuckyLoot", L"Nova_Strike", L"AquaMan_09", L"Glitchy_TV", L"DeltaForce_7",
        L"SpicyNugget", L"TiltedTom_", L"Wolfie_x0", L"ShotgunSally", L"Bl4zeRunner", L"FuzzyPickles", L"Vexxed", L"MrLoot_Llama",
        L"Cheeto_Dust4", L"Phantom_Ace", L"Ty_The_Guy", L"NinjaTurtl3", L"BlueBerry_Pie", L"Zapdos_99", L"ProBuilder_Z", L"SnipeyMcSnipe",
        L"Kenzo_YT", L"HypeTrain42", L"Rocket_Rex", L"IceColdKilla", L"Jumpman_23", L"Cosmic_Kitty", L"Dusty_Divot8", L"HungryHippo_",
        L"Echo_Six", L"Wildcat_Wes", L"Peely4Life", L"SlurpJuice_", L"Rookie_Ray", L"AimAssist_Andy", L"Groot_Gamer", L"Lazer_Tag88",
        L"CrispyChicken_", L"MidnightMoss", L"Viper_Venom", L"Daisy_Duke07", L"TheSaltyOne", L"Bolt_Action_Bri", L"Panda_Express", L"KiloWatt_",
        L"SweetChinMusic", L"Juno_x", L"RetailRowRat", L"xTrickShotx", L"Mango_Mike12", L"FlossBoss3000", L"Zeke_The_Freak", L"Pancake_Pete",
        L"Atomic_Annie", L"Shifty_Sam", L"CloudSurfer_", L"Tomatohead_Tim", L"BrickByBrick", L"Rage_Quit_Ray", L"Nebula_Nick", L"Kaylee_Plays",
        L"Brandon_2004", L"xKryptic_", L"ItsJustLucas", L"zVelo", L"Tyl3r_05", L"MasonPlays_", L"iiTz_Connor", L"Not_Dylan",
        L"logan_smith06", L"Aiden_x", L"Caleb_YT", L"xX_Hunter_Xx", L"Noah_2005", L"Ethan_GG", L"Liam_04", L"Jayden_99",
        L"Karter_x", L"Owen_Plays", L"Gavin_07", L"Carter_23", L"Wyatt_TTV", L"Luke_14", L"Jack_x0", L"Julian_03",
        L"Levi_GG", L"Isaac_2006", L"Gabriel_x", L"Anthony_11", L"Lincoln_08", L"Jaxon_05", L"Asher_YT", L"Christopher_98",
        L"Andrew_x", L"Theodore_07", L"Caleb_12", L"Ryan_2003", L"Nathan_GG", L"Adrian_04", L"Christian_x", L"Maverick_06",
        L"Colton_99", L"Elias_TTV", L"Aaron_05", L"Eli_Plays", L"Landon_21", L"Jonathan_02", L"Nolan_x", L"Hunter_08",
        L"Cameron_YT", L"Connor_15", L"Santiago_04", L"Jeremiah_x", L"Ezekiel_07", L"Angel_99", L"Roman_GG", L"Easton_06",
        L"Miles_x", L"Robert_2001", L"Jameson_05", L"Nicholas_19", L"Greyson_TTV", L"Cooper_03", L"Ian_Plays", L"Carson_x",
        L"Axel_08", L"Jaxson_10", L"Dominic_04", L"Leonardo_x", L"Luca_GG", L"Austin_2002", L"Jordan_23", L"Adam_06",
        L"Xavier_YT", L"Jose_99", L"Jace_x", L"Everett_07", L"Declan_05", L"Evan_Plays", L"Kayden_11", L"Parker_x",
        L"Wesley_04", L"Kai_TTV", L"Brayden_08", L"Bryson_99", L"Weston_x", L"Jason_2003", L"Emmett_GG", L"Sawyer_06",
        L"Silas_x", L"Bennett_05", L"Brooks_14", L"Micah_YT", L"Damian_02", L"Harrison_x", L"Waylon_07", L"Ayden_99",
        L"Vincent_GG", L"Ryder_04", L"Kingston_x", L"Rowan_08", L"George_2005", L"Luis_17", L"Chase_TTV", L"Cole_03",
        L"Nathaniel_x", L"Zachary_06", L"Ashton_Plays", L"Braxton_99", L"Gael_x", L"Tyler_2002", L"Bentley_07", L"Camden_GG",
        L"Beau_04", L"Gavin_x", L"Kaden_12", L"Max_YT", L"Matteo_05", L"Carlos_98", L"Kaiden_x", L"Elliott_06",
        L"Blake_TTV", L"Tristan_03", L"Grant_x", L"Tanner_08", L"vPulse", L"zStrafe", L"xClaw_", L"iiVortex",
        L"oSnipez", L"xVenom_99", L"zKarma", L"v Astro", L"xPhaze_", L"ii Frost", L"oHavoc", L"zRelic",
        L"vzephyr", L"x Apex", L"iiMirage", L"oCipher", L"zBane_", L"vKrono", L"xOnyx_", L"ii Jinx",
        L"oSpectre", L"zSurge", L"vMystic", L"xRogue_04", L"iiTitan", L"oWraith", L"zEnigma", L"vBlaze",
        L"xDusk_", L"iiZenith", L"oQuake", L"zDraco", L"vFlux", L"xNexus_", L"iiPyro", L"oTalon",
        L"zAether", L"vSolace", L"xReaper_99", L"iiValkyrie", L"oPhantom", L"zGoliath", L"vSynergy", L"xObsidian",
        L"iiTempest", L"oVanguard", L"zAbyss", L"vCatalyst", L"xEclipse_", L"iiHarbinger", L"oNomad", L"zOutlaw",
        L"vParadox", L"xRenegade", L"iiSovereign", L"oWarden", L"zZealot", L"vHorizon", L"xMiracle_", L"iiOdyssey",
        L"oPinnacle", L"zRadiance", L"vSanctuary", L"xValor_", L"iiVortex_", L"oLegacy", L"zEmpire", L"vDynasty",
        L"xTrinity", L"iiIllusion", L"oDestiny", L"zEternity", L"vInfinity", L"xHarmony", L"iiSerenity", L"oTranquil",
        L"zHalcyon", L"vEuphoria", L"xBliss_", L"iiNirvana", L"oParadise", L"zUtopia", L"vArcadia", L"xEden_",
        L"iiAvalon", L"oZion", L"zOlympus", L"vValhalla", L"xAsgard_", L"iiTartarus", L"oStyx", L"zLethe",
        L"vErebus", L"xNyx_", L"iiHemera", L"oAether", L"zChaos", L"vChronos", L"xAnanke_", L"iiPhanes",
        L"oNesoi", L"zOurea", L"vPontus", L"xThalassa", L"iiUranus", L"oGaia", L"zCoeus", L"vCrius",
        L"xHyperion", L"iiIapetus", L"oOceanus", L"zPhoebe", L"vRhea", L"xTethys", L"iiTheia", L"oThemis",
        L"ItsKev_", L"Not_Marcus", L"WhoIsDev", L"Just_Tariq", L"YoIts_Sam", L"Real_Dante", L"Im_Kobe", L"HeyIts_Trey",
        L"Only_Malik", L"TheReal_CJ", L"ItsMe_Zack", L"DefNot_Seth", L"Actually_Shane", L"Basically_Ben", L"Mostly_Matt", L"Simply_spencer",
        L"Purely_Paul", L"Truly_Travis", L"Honestly_Hank", L"Frankly_Frank", L"Clearly_Clark", L"Merely_mark", L"Solely_scott", L"Wholly_will",
        L"Fully_fred", L"Totally_todd", L"Entirely_eric", L"Utterly_uri", L"Deeply_dan", L"Greatly_greg", L"Highly_hal", L"Hugely_hugh",
        L"Vast_victor", L"Wide_wade", L"Broad_brad", L"Long_lance", L"Tall_trent", L"High_heath", L"Deep_drew", L"Low_lyle",
        L"Short_shawn", L"Small_saul", L"Tiny_timmy", L"Mini_mike", L"Micro_mack", L"Nano_ned", L"Pico_pete", L"Femto_finn",
        L"Atto_alec", L"Zepto_zane", L"Yocto_yuri", L"Kilo_kyle", L"Mega_miles", L"Giga_gabe", L"Tera_theo", L"Peta_phil",
        L"Exa_evan", L"Zetta_zack", L"Yotta_yale", L"Alpha_alex", L"Beta_blake", L"Gamma_grant", L"Delta_dean", L"Epsilon_eli",
        L"Zeta_zeke", L"Eta_ethan", L"Theta_theo", L"Iota_ian", L"Kappa_kurt", L"Lambda_luke", L"Mu_mark", L"Nu_nate",
        L"Xi_xander", L"Omicron_omar", L"Pi_peter", L"Rho_ryan", L"Sigma_seth", L"Tau_troy", L"Upsilon_uri", L"Phi_paul",
        L"Chi_chad", L"Psi_percy", L"Omega_owen", L"Ruby_04", L"Chloe_Plays", L"Mia_x0", L"Zoe_YT", L"Lily_2005",
        L"Hannah_GG", L"Emma_06", L"Olivia_x", L"Ava_TTV", L"Sophia_03", L"Isabella_99", L"Amelia_x", L"Evelyn_07",
        L"Abigail_Plays", L"Harper_04", L"Emily_x0", L"Elizabeth_YT", L"Avery_08", L"Sofia_GG", L"Ella_2006", L"Madison_x",
        L"Scarlett_05", L"Victoria_TTV", L"Aria_02", L"Grace_x", L"B4dger_01", L"W0lf_Pack99", L"F4lcon_Punch", L"Sn4ke_Eyes7",
        L"T1ger_Blood", L"Sh4rk_Bite", L"P4nther_x", L"C0bra_Kai88", L"V1per_Strike", L"H4wk_Eye04", L"E4gle_Soar", L"R4ven_Claw",
        L"Cr0w_Bar", L"Ow1_Night", L"D0ve_Peace", L"Sw4n_Song", L"G00se_Chase", L"D_u_c_k_y", L"Dr4ke_06", L"G4nder_x",
        L"H3n_House", L"R00ster_99", L"Ch1ck_Pea", L"P3acock_x", L"T_u_r_k_e_y", L"Qu4il_Man", L"Gr0use_05", L"Ph3asant_x",
        L"P4rtridge", L"P1geon_Toe", L"Sp4rrow_07", L"F1nch_x", L"R0bin_Hood", L"Bl_u_e_Jay", L"C4rdinal_99", L"C4nary_Yellow",
        L"P4rrot_Bay", L"M4caw_x", L"C0ckatoo_04", L"B_u_d_g_i_e", L"L0ve_Bird", L"L0rikeet_x", L"C0nure_06", L"C4ique_99",
        L"P1onus_x", L"P0icephalus", L"Am4zon_05", L"Gr3y_Parrot", L"Ec1ectus_x", L"K4kapo_08", L"K3a_Bird", L"K4ka_99",
        L"T0ucan_Sam", L"H0rnbill_x", L"W0odpecker", L"B4rbet_04", L"H0neyguide", L"J4camar_x", L"P_u_f_f_bird", L"Bee_E4ter",
        L"R0ller_06", L"K1ngfisher", L"M0mot_x", L"T0dy_99", L"H0opoe_05", L"Tr0gon_x", L"Qu3tzal_07", L"M0usebird",
        L"C_u_c_k_o_o", L"R0adrunner", L"An1_Bird", L"T_u_r_a_c_o", L"Ostr1ch_x", L"Rh3a_99", L"C4ssowary", L"Em_u_04",
        L"K1wi_Fruit", L"T1namou_x", L"P3nguin_06", L"A1batross", L"P3trel_99", L"Sh3arwater", L"St0rm_Petrel", L"D1ving_Petrel",
        L"Gr3be_x", L"Fl4mingo_05", L"Tr0picbird", L"P3lican_08", L"G4nnet_x", L"B0oby_Trap", L"C0rmorant", L"Sh4g_Carpet",
        L"Fr1gatebird", L"Anhinga_x", L"D4rter_99", L"H3ron_04", L"Egr3t_x", L"B1ttern_06", L"St0rk_Delivery", L"Ib1s_x",
        L"Sp0onbill", L"Shoeb1ll_99", L"H4mmerkop", L"Cr4ne_Game", L"L1mpkin_x", L"Tr_u_m_p_e_t", L"R4il_Road", L"Cr4ke_05",
        L"C0ot_x", L"M0orhen_07", L"Sw4mphen", L"F1nfoot_x", L"S_u_n_grebe", L"K4gu_99", L"S_u_n_bittern", L"S3riema_x",
        L"B_u_s_t_a_r_d", L"B_u_t_t_o_n", L"Pl4ins_Wander", L"S3ed_Snipe", L"Sh3athbill", L"St0ne_Curlew", L"Oysterc4tch", L"Ibisb1ll_x",
        L"St1lt_04", L"Av0cet_99", L"Pl0ver_x", L"L4pwing_06", L"P4inted_Snipe", L"J4cana_x", L"S4ndpiper", L"Sn1pe_Hunt",
        L"W0odcock_05", L"G0dwit_x", L"C_u_r_l_e_w", L"Wh1mbrel_99", L"Sh4nk_08", L"T4ttler_x", L"T_u_r_n_stone", L"K_n_o_t_04",
        L"S4nderling", L"D_u_n_l_i_n", L"St1nt_x", L"P_h_a_l_a_r", L"Cr4b_Plover", L"Pr4tincole", L"C0urser_99", L"Sk1mmer_x",
        L"G_u_l_l_06", L"K1ttiwake", L"T3rn_Around", L"N0ddy_x",
    };
    constexpr int32_t NumBotNames = static_cast<int32_t>(sizeof(BotNames) / sizeof(BotNames[0]));

    // Bots spawned in the warmup (CoreV2 #256): one at a time, at most one every MinSpawnInterval seconds and MinSpawnFrames
    // frames (a third of a second at the server's 30 Hz, longer when its frames run long). Each spawn is a controller, a
    // pawn and its loadout's cosmetics loaded from the paks, a heavy frame: one bot a frame, as the warmup's last 20 s
    // (or a countdown the game shortened to 15 s) had it, hung the server. They are spread over the warmup up to
    // SpawnDoneBeforeBus seconds before the bus, never closer, and the most MaxSpawnInterval apart. And how often the loot
    // list is refreshed.
    constexpr float MinSpawnInterval = 0.5f;
    constexpr int32_t MinSpawnFrames = 10;
    constexpr float MaxSpawnInterval = 6.0f;
    constexpr float SpawnDoneBeforeBus = 20.0f;
    constexpr float LootRefreshInterval = 3.0f;

    void WatchManager()
    {
        GameThread::Enqueue([]()
        {
            if (!Instance)
            {
                return;
            }
            if (!Instance->GetGameMode())
            {
                Log::Info(L"Battle Royale bots: the game mode is gone, the bot manager stops");
                Instance.reset();
                BotKills.clear();
                NativeKillTargets.clear();
                UAthenaNavSystem::ResetForNewMatch();
                return;
            }
            Instance->Tick();
            WatchManager();
        });
    }

    FVector LocationOf(const AActor* Actor)
    {
        return Actor ? Actor->GetTransform().GetLocation() : FVector();
    }

    float RandomRange(float Min, float Max)
    {
        return Min + (Max - Min) * FMath::FRand();
    }

    AFortGameStateAthena* AthenaGameStateOf(AFortGameModeAthena* GameMode)
    {
        UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
        return World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    }

    AFortPlayerStateAthena* AthenaStateOf(AController* Controller)
    {
        return Controller ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState))) : nullptr;
    }

    // Whether a player on Team (in Squad, when it isn't 0) is still in the match (the game mode's alive players).
    bool HasAlivePlayerOnTeam(AFortGameModeAthena* GameMode, uint8_t Team, uint8_t Squad = 0)
    {
        if (!GameMode)
        {
            return false;
        }
        const TArray<AFortPlayerControllerAthena*>& Alive = GameMode->AlivePlayers;
        for (int32_t Index = 0; Index < Alive.Num(); ++Index)
        {
            AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Alive[Index]));
            if (State && static_cast<uint8_t>(State->TeamIndex) == Team && (Squad == 0 || static_cast<uint8_t>(State->SquadId) == Squad))
            {
                return true;
            }
        }
        return false;
    }

    // An elimination for Team: each of its players' team eliminations (AFortPlayerStateAthena::TeamKillScore, the HUD's).
    void AddTeamKillScore(AFortGameModeAthena* GameMode, uint8_t Team)
    {
        UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
        AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
        if (!AFortPlayerStateAthena::HasTeamKillScore() || !GameState)
        {
            return;
        }
        for (APlayerState* Other : GameState->PlayerArray)
        {
            AFortPlayerStateAthena* Mate = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(Other));
            if (Mate && static_cast<uint8_t>(Mate->TeamIndex) == Team)
            {
                Mate->TeamKillScore = Mate->TeamKillScore + 1;
            }
        }
    }

    UClass* RangedWeaponClass()
    {
        static UClass* Class = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortWeaponRangedItemDefinition");
        return Class;
    }
}

void FCoreAthenaBotManager::Create(AFortGameModeAthena* GameMode)
{
#if WITH_SERVER_CODE
    if (!GameMode || (Instance && Instance->GetGameMode() == GameMode))
    {
        return;
    }
    const bool bWatching = Instance != nullptr;
    // A new game mode is a new world (the server reloads the map after a match): nothing of the last match's bots is
    // kept. Their controllers and pawns went with the old world; Core only ever holds them weakly.
    Instance.reset(new FCoreAthenaBotManager(GameMode));
    BotKills.clear();
    NativeKillTargets.clear();
    UAthenaNavSystem::ResetForNewMatch();
    FCoreAthenaBotController::ResetForNewMatch();
    if (!bWatching)
    {
        WatchManager();
    }
    static bool bReplicatesHealth = false;
    if (!bReplicatesHealth)
    {
        bReplicatesHealth = true;
        UNetDriver::AddPreReplicationCallback([]()
        {
            if (Instance)
            {
                Instance->ReplicateHealthAndShield();
            }
        });
    }
#endif
}

FCoreAthenaBotManager* FCoreAthenaBotManager::Get()
{
    return Instance.get();
}

FCoreAthenaBotManager::FCoreAthenaBotManager(AFortGameModeAthena* InGameMode)
    : GameMode(InGameMode)
{
}

bool FCoreAthenaBotManager::HasNativePlayerBots()
{
    // 8.x's own bot controller class (the backport's rename to Core names took this lookup with it: it looked for Core's
    // class name, which no build has, so a native-bot build would have run the backport too).
    static const bool bNative = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortAthenaAIBotController") != nullptr;
    return bNative;
}

AFortGameModeAthena* FCoreAthenaBotManager::GetGameMode() const
{
    return static_cast<AFortGameModeAthena*>(GameMode.Get());
}

void FCoreAthenaBotManager::SetSettings(bool bInEnabled, const FCoreAthenaBotSettings& InSettings)
{
    const bool bChanged = !bHasSettings || bInEnabled != bEnabled || InSettings.Count != Settings.Count || InSettings.Level != Settings.Level
        || InSettings.StartingHealth != Settings.StartingHealth || InSettings.StartingShield != Settings.StartingShield
        || InSettings.bEliminationsCountForQuests != Settings.bEliminationsCountForQuests || InSettings.BotFightChance != Settings.BotFightChance
        || InSettings.bFillPlayerTeams != Settings.bFillPlayerTeams || InSettings.NoFillAccounts != Settings.NoFillAccounts;
    bHasSettings = true;
    if (!bChanged)
    {
        return;
    }
    // The match keeps the bots it has once the bus has left; how they behave follows the panel.
    if (bAircraftStarted)
    {
        Settings.bEliminationsCountForQuests = InSettings.bEliminationsCountForQuests;
        Settings.BotFightChance = InSettings.BotFightChance;
        return;
    }
    bEnabled = bInEnabled;
    Settings = InSettings;
    if (bEnabled && HasNativePlayerBots())
    {
        // The boundary with 8.30's native bots: Force player AIs is the backport's alone, and players' teams on such a
        // build are filled by Epic's own system (its bot manager), which Core doesn't drive yet.
        Log::Info(L"Battle Royale bots: this build has Epic's own player bots, so \"Force player AIs\" (Core's backport) is ignored{}",
            Settings.bFillPlayerTeams ? L", and \"Fill players' teams with bots\" is for the native bots, which Core doesn't run yet" : L"");
        bEnabled = false;
        return;
    }
    if (bEnabled)
    {
        Log::Info(L"Battle Royale bots: Force player AIs is {}: up to {} bots; players' teams {}; level {}, {:.0f} health, {:.0f} shield, eliminations {} for quests, {}% bot fights",
            Settings.Count > 0 ? L"on" : L"off", Settings.Count,
            Settings.bFillPlayerTeams ? std::format(L"filled with bots ({} players without fill)", Settings.NoFillAccounts.size()) : std::wstring(L"not filled"), Settings.Level,
            Settings.StartingHealth, Settings.StartingShield, Settings.bEliminationsCountForQuests ? L"count" : L"don't count", Settings.BotFightChance);
    }
    else
    {
        Log::Info(L"Battle Royale bots: Force player AIs and Fill players' teams with bots are off");
    }
}

void FCoreAthenaBotManager::SetFillAnswered(const std::vector<std::wstring>& AccountIds)
{
    FillAnsweredAccounts.insert(AccountIds.begin(), AccountIds.end());
}

FCoreAIBotSkills FCoreAthenaBotManager::SkillsForLevel(int32_t Level)
{
    // Core's levels 🟡 (8.30's FAIBotSkills values and 21.40's aiming skill sets don't ship): level 1 reacts slowly,
    // shoots slowly and misses a lot; level 10 reacts in a quarter of a second and tracks closely. The default level 5
    // aims for a player's hit rate, a quarter to two fifths of its assault rifle shots at mid range (10-35 m; a
    // headless 3.1 match: 23%, 16 of 69, against strafing bots); testers found the old error, which shrank to nothing
    // up close and at high levels, an aimbot.
    const float T = std::clamp((static_cast<float>(Level) - 1.0f) / 9.0f, 0.0f, 1.0f);
    FCoreAIBotSkills Skills;
    Skills.MaxTimeBetweenShots = FMath::Lerp(0.9f, 0.25f, T);
    Skills.DeviationTimeBetweenShots = FMath::Lerp(0.5f, 0.1f, T);
    Skills.MaxTrackingReactionTime = FMath::Lerp(1.2f, 0.25f, T);
    Skills.MaxTrackingInterpTime = FMath::Lerp(0.25f, 0.05f, T);
    Skills.MaxTrackingPredictionError = FMath::Lerp(250.0f, 30.0f, T);
    Skills.SightRange = FMath::Lerp(4000.0f, 9000.0f, T);
    Skills.MaxTrackingOffsetError = FMath::Lerp(70.0f, 20.0f, T);
    Skills.MaxTrackingDistanceNearError = FMath::Lerp(35.0f, 8.0f, T);
    Skills.MaxTrackingDistanceFarError = FMath::Lerp(50.0f, 12.0f, T);
    // Building (Chapter 2's bots, 21.40's building skill set and bunker perk, scaled by level as their FScalableFloats
    // are): a better bot builds more often, faster, and boxes up more. Kept occasional: these bots fill matches.
    FCoreAthenaBotBuildingSkills& Building = Skills.Building;
    Building.DefensiveBuildingDelayBetweenBuilds = FMath::Lerp(25.0f, 10.0f, T);
    Building.DefensiveBuildingDelayDeviationBetweenBuilds = FMath::Lerp(6.0f, 3.0f, T);
    Building.DelayBetweenBuildPieces = FMath::Lerp(0.6f, 0.2f, T);
    Building.DefensiveBuildingOdds = FMath::Lerp(0.2f, 0.45f, T);
    Building.WallListWeight = FMath::Lerp(0.85f, 0.5f, T);
    Building.WallAndRampListWeight = FMath::Lerp(0.15f, 0.5f, T);
    Building.BunkerOddsToActivate = FMath::Lerp(0.2f, 0.6f, T);
    Building.BunkerCooldownDuration = FMath::Lerp(60.0f, 30.0f, T);
    Building.OddsToBeUsedAgainstUnknownThreat = FMath::Lerp(0.1f, 0.35f, T);
    Building.OddsToUseRoofBuilding = FMath::Lerp(0.3f, 0.7f, T);
    Building.OddsToBuildForwardRamp = FMath::Lerp(0.2f, 0.5f, T);
    Building.InitialBuildDelay = FMath::Lerp(0.7f, 0.2f, T);
    Building.SamePieceBuildDelay = FMath::Lerp(0.5f, 0.15f, T);
    Building.SamePieceBuildDelayRandomDeviation = FMath::Lerp(0.15f, 0.05f, T);
    Building.DifferentPieceBuildDelay = FMath::Lerp(0.7f, 0.25f, T);
    Building.DifferentPieceBuildDelayRandomDeviation = FMath::Lerp(0.2f, 0.08f, T);
    return Skills;
}

int32_t FCoreAthenaBotManager::GetAliveBotCount() const
{
    return static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [](const auto& Bot) { return Bot->IsAlive(); }));
}

FCoreAthenaBotController* FCoreAthenaBotManager::FindBot(const UObject* PawnOrPlayerState) const
{
    if (!PawnOrPlayerState)
    {
        return nullptr;
    }
    for (const auto& Bot : PlayerBots)
    {
        if (reinterpret_cast<const UObject*>(Bot->GetPawn()) == PawnOrPlayerState || reinterpret_cast<const UObject*>(Bot->GetPlayerState()) == PawnOrPlayerState
            || reinterpret_cast<const UObject*>(Bot->GetController()) == PawnOrPlayerState)
        {
            return Bot.get();
        }
    }
    return nullptr;
}

bool FCoreAthenaBotManager::IsBotPawn(const UObject* Object)
{
    return Instance && Instance->FindBot(Object) != nullptr;
}

int32_t FCoreAthenaBotManager::BotKillsOf(const AFortPlayerStateAthena* PlayerState)
{
    const auto Found = std::find_if(BotKills.begin(), BotKills.end(), [PlayerState](const auto& Entry) { return Entry.first.Get() == PlayerState; });
    return Found != BotKills.end() ? Found->second : 0;
}

bool FCoreAthenaBotManager::ShouldCountQuestEvent(const UObject* Target)
{
    return !Instance || Instance->Settings.bEliminationsCountForQuests;
}

void FCoreAthenaBotManager::NoteNativeKillEvent(const UObject* Target, uint8_t Type)
{
    std::erase_if(NativeKillTargets, [](const auto& Entry) { return !Entry.first.Get(); });
    if (!HasNativeKillEvent(Target, Type))
    {
        NativeKillTargets.emplace_back(TWeakObjectPtr<UObject>(const_cast<UObject*>(Target)), Type);
    }
}

bool FCoreAthenaBotManager::HasNativeKillEvent(const UObject* Target, uint8_t Type)
{
    return std::any_of(NativeKillTargets.begin(), NativeKillTargets.end(), [Target, Type](const auto& Entry) { return Entry.first.Get() == Target && Entry.second == Type; });
}

bool FCoreAthenaBotManager::IsWarmup()
{
    return Instance && !Instance->bAircraftStarted;
}

namespace
{
    // An actor's own gameplay tags (IGameplayTagAssetInterface::GetOwnedGameplayTags, through its reflected function,
    // whose exec finds the actor's interface).
    void AppendOwnedGameplayTags(UObject* Actor, FGameplayTagContainer& Tags)
    {
        static UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/GameplayTags.GameplayTagAssetInterface:GetOwnedGameplayTags");
        Reflection::FParams Params(Function, true);
        if (Actor && Function && Params.Call(Actor))
        {
            Tags.AppendTags(Params.Take<FGameplayTagContainer>(L"TagContainer"));
        }
    }
}

void FCoreAthenaBotManager::AddKillEventTags(const UObject* Target, AController* Killer, FGameplayTagContainer& TargetTags, FGameplayTagContainer& SourceTags)
{
#if WITH_SERVER_CODE
    // 3.1's AFortGameModeZone::Killed (0xB5E890) sends the killer's Kill event with the dead pawn's own tags as the
    // target's and the killer's (their player state's hero, UFortPlayerController 0xE466C0) with their pawn's as the
    // source's. Battle Royale's kill objectives read both (AthenaObjectiveStatTable: killingblow_athena_player asks for
    // Homebase.Class in each, killingblow_athena_player_shotgun for Weapon.Ranged.shotgun in the source's too). A bot's
    // pawn has no hero of a player's: its class is its outfit's hero's (HID_*: Homebase.Class.IsCommando...), and the
    // weapon is the one the killer holds (its item's GameplayTags).
    FCoreAthenaBotController* Bot = Instance ? Instance->FindBot(Target) : nullptr;
    if (!Bot)
    {
        return;
    }
    AppendOwnedGameplayTags(reinterpret_cast<UObject*>(Bot->GetPawn()), TargetTags);
    UAthenaCharacterItemDefinition* Character = Cast<UAthenaCharacterItemDefinition>(Bot->GetLoadoutCharacter());
    if (UFortHeroType* Hero = Character ? static_cast<UFortHeroType*>(Character->HeroDefinition) : nullptr)
    {
        TargetTags.AppendTags(static_cast<FGameplayTagContainer&>(reinterpret_cast<UFortItemDefinition*>(Hero)->GameplayTags));
    }
    AFortPawn* KillerPawn = Killer ? Cast<AFortPawn>(reinterpret_cast<UObject*>(static_cast<APawn*>(Killer->Pawn))) : nullptr;
    AFortWeapon* Weapon = KillerPawn ? static_cast<AFortWeapon*>(KillerPawn->CurrentWeapon) : nullptr;
    if (UFortWeaponItemDefinition* WeaponData = Weapon ? static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData) : nullptr)
    {
        SourceTags.AppendTags(static_cast<FGameplayTagContainer&>(reinterpret_cast<UFortItemDefinition*>(WeaponData)->GameplayTags));
    }
#endif
}

bool FCoreAthenaBotManager::ShouldHoldNativeWinCheck() const
{
    return HasAliveBots();
}

void FCoreAthenaBotManager::OnPlayerRemoved(AFortPlayerStateAthena* KillerState)
{
    if (FCoreAthenaBotController* Killer = FindBot(KillerState))
    {
        Killer->AddKill();
    }
    UpdateCounts();
    CheckWinConditionLastManStanding();
}

bool FCoreAthenaBotManager::GetSafeZone(FVector& OutCenter, float& OutRadius) const
{
    AFortGameModeAthena* Athena = GetGameMode();
    AFortSafeZoneIndicator* Indicator = Athena ? static_cast<AFortSafeZoneIndicator*>(Athena->SafeZoneIndicator) : nullptr;
    if (!Indicator || !AFortSafeZoneIndicator::HasNextCenter())
    {
        return false;
    }
    OutCenter = Indicator->NextCenter;
    OutRadius = Indicator->NextRadius;
    return OutRadius > 0.0f;
}

bool FCoreAthenaBotManager::GetCurrentSafeZone(FVector& OutCenter, float& OutRadius) const
{
    AFortGameModeAthena* Athena = GetGameMode();
    AFortSafeZoneIndicator* Indicator = Athena ? static_cast<AFortSafeZoneIndicator*>(Athena->SafeZoneIndicator) : nullptr;
    if (!Indicator || !AFortSafeZoneIndicator::FindStaticFunction_GetSafeZoneCenter())
    {
        return false;
    }
    OutCenter = Indicator->GetSafeZoneCenter();
    OutRadius = Indicator->Radius;
    return OutRadius > 0.0f;
}

float FCoreAthenaBotManager::GetSafeZoneFinishShrinkTime() const
{
    AFortGameModeAthena* Athena = GetGameMode();
    AFortSafeZoneIndicator* Indicator = Athena ? static_cast<AFortSafeZoneIndicator*>(Athena->SafeZoneIndicator) : nullptr;
    if (!Indicator || !AFortSafeZoneIndicator::HasShrinkTimes())
    {
        return -1.0f;
    }
    const float Finish = Indicator->SafeZoneFinishShrinkTime;
    return Finish > 0.0f ? Finish : -1.0f;
}

float FCoreAthenaBotManager::GetStormDamagePerSecond() const
{
    // Battle Royale's storm hurts 1 a second in its first circles, more later 🟡 (the game's GE_OutsideSafeZone
    // damage by phase isn't read here).
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t Phase = Athena ? static_cast<int32_t>(Athena->SafeZonePhase) : 0;
    return Phase <= 2 ? 1.0f : Phase <= 4 ? 2.0f : Phase <= 6 ? 5.0f : 10.0f;
}

bool FCoreAthenaBotManager::FindGroundNear(const FVector& Around, float Radius, FVector& OutLocation) const
{
    AFortGameModeAthena* Athena = GetGameMode();
    for (int32_t Attempt = 0; Athena && Attempt < 6; ++Attempt)
    {
        const float Angle = RandomRange(0.0f, 6.2831853f);
        const float Distance = Radius * std::sqrt(FMath::FRand());
        const FVector Top(Around.X + std::cos(Angle) * Distance, Around.Y + std::sin(Angle) * Distance, 30000.0f);
        FVector Hit;
        if (UKismetSystemLibrary::LineTraceSingle(Athena, Top, FVector(Top.X, Top.Y, -5000.0f), 0, false, false, Hit))
        {
            OutLocation = Hit;
            return true;
        }
    }
    return false;
}

bool FCoreAthenaBotManager::GetWarmupSpot(FVector& OutLocation) const
{
    if (WarmupSpots.empty())
    {
        return false;
    }
    OutLocation = WarmupSpots[static_cast<size_t>(FMath::Rand()) % WarmupSpots.size()];
    return true;
}

uint8_t FCoreAthenaBotManager::TeamOfPawn(const APawn* Pawn) const
{
    AFortPlayerStateAthena* State = Pawn ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Pawn->PlayerState))) : nullptr;
    return State ? static_cast<uint8_t>(State->TeamIndex) : 0xFF;
}

void FCoreAthenaBotManager::ForgetLoot(AFortPickup* Pickup)
{
    std::erase_if(Loot, [Pickup](const FCoreAthenaBotLoot& Entry) { return Entry.Pickup.Get() == Pickup; });
}

std::set<uint8_t> FCoreAthenaBotManager::HumanTeams() const
{
    std::set<uint8_t> Teams;
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (!GameState)
    {
        return Teams;
    }
    const TArray<APlayerState*>& PlayerArray = GameState->PlayerArray;
    for (APlayerState* PlayerState : PlayerArray)
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (AthenaState && !FindBot(AthenaState) && AthenaState->TeamIndex != EFortTeam::GetSpectator())
        {
            Teams.insert(static_cast<uint8_t>(AthenaState->TeamIndex));
        }
    }
    return Teams;
}

bool FCoreAthenaBotManager::HasPlayersConnected() const
{
    // A player's player state stays in the game state's PlayerArray, out of the match or not, until they leave
    // (AGameModeBase::Logout); a bot's has an AI controller for its owner.
    AFortGameStateAthena* GameState = AthenaGameStateOf(GetGameMode());
    if (!GameState)
    {
        return false;
    }
    const TArray<APlayerState*>& PlayerArray = GameState->PlayerArray;
    for (APlayerState* PlayerState : PlayerArray)
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (AthenaState && !FindBot(AthenaState) && Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(AthenaState->GetOwner())))
        {
            return true;
        }
    }
    return false;
}

std::set<uint8_t> FCoreAthenaBotManager::AliveTeams(bool bIncludeHumans) const
{
    std::set<uint8_t> Teams;
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive())
        {
            Teams.insert(Bot->GetTeam());
        }
    }
    AFortGameModeAthena* Athena = GetGameMode();
    if (bIncludeHumans && Athena)
    {
        const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
        for (int32_t Index = 0; Index < Alive.Num(); ++Index)
        {
            if (AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Alive[Index])))
            {
                Teams.insert(static_cast<uint8_t>(State->TeamIndex));
            }
        }
    }
    return Teams;
}

void FCoreAthenaBotManager::Tick()
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!GameState || !World)
    {
        return;
    }
    const float Now = World->GetTimeSeconds();
    const float DeltaSeconds = LastTickTime < 0.0f ? 0.0f : std::max(Now - LastTickTime, 0.0f);
    LastTickTime = Now;
    // The server's frame (wall clock, the 30 Hz cap included) and the bots' share of it, for the summary.
    const auto FrameStart = std::chrono::steady_clock::now();
    if (LastFrameStart.time_since_epoch().count() != 0)
    {
        const double FrameMs = std::chrono::duration<double, std::milli>(FrameStart - LastFrameStart).count();
        FrameMsSum += FrameMs;
        FrameMsMax = std::max(FrameMsMax, FrameMs);
        FrameSpikes += FrameMs >= 50.0 ? 1 : 0;
        ++FrameCount;
    }
    LastFrameStart = FrameStart;
    if (!bEnabled && PlayerBots.empty())
    {
        return;
    }

    const uint8_t Phase = GameState->GamePhase;
    if (!bAircraftStarted)
    {
        ReadPlaylistTeams();
    }
    StartBotMatchIfAsked();
    if (Phase != LastPhase)
    {
        LastPhase = Phase;
        Log::Info(L"Battle Royale bots: the match is in {} with {} bots", GameState->GamePhase.ToString(), GetAliveBotCount());
        if (Phase == EAthenaGamePhase::GetAircraft() && !bAircraftStarted)
        {
            OnAircraftPhaseStarted(Now);
        }
        if (Phase == EAthenaGamePhase::GetEndGame())
        {
            bMatchEnded = true;
        }
    }
    if (Phase == EAthenaGamePhase::GetWarmup())
    {
        TickWarmup(Now);
        UpdateCounts();
    }

    if (bHasNavigation)
    {
        UAthenaNavSystem::Tick(World);
    }
    RefreshEnemies(Now);
    // The warmup island's weapons too, once the bots are in (they pick them up for a fight there).
    if ((bAircraftStarted || bNavigationTried) && Now >= NextLootRefresh)
    {
        NextLootRefresh = Now + LootRefreshInterval;
        const auto LootStart = std::chrono::steady_clock::now();
        RefreshLoot(Now);
        LootRefreshMsMax = std::max(LootRefreshMsMax, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - LootStart).count());
    }
    if (bAircraftStarted && !bLandingAssigned && !bMatchEnded)
    {
        UpdateFlightPath(Now);
    }
    if (!bMatchEnded)
    {
        UpdatePlayerTeamBots(Now);
    }
    // Once the match has ended the bots stand still (the winners' screen is up).
    for (size_t Index = 0; Index < PlayerBots.size() && !bMatchEnded; ++Index)
    {
        const auto BotStart = std::chrono::steady_clock::now();
        PlayerBots[Index]->Tick(Now, DeltaSeconds);
        if (const double BotMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - BotStart).count(); BotMs > BotTickMsMax)
        {
            BotTickMsMax = BotMs;
            SlowestBotTick = PlayerBots[Index]->GetBotName();
        }
    }
    if (bAircraftStarted && !bMatchEnded)
    {
        UpdateCounts();
        CheckWinConditionLastManStanding();
    }
    const double BotsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - FrameStart).count();
    BotsMsSum += BotsMs;
    BotsMsMax = std::max(BotsMsMax, BotsMs);
    LogSummary(Now);
#endif
}

void FCoreAthenaBotManager::StartBotMatchIfAsked()
{
#if WITH_SERVER_CODE
    // Testing only (-CoreBotMatch): a match of bots alone. Nobody's coming, so nothing assigns the match or starts its
    // warmup (HandleMatchHasStarted, 3.0 0x9AFB60, runs StartWarmupPhase once the match state goes InProgress); and at
    // the warmup's end UpdateWarmupPhase (0x9CC140) restarts the session when GetNumPlayers is 0 ("All players
    // disconnected during warmup phase"), so the bus is started a second early.
    if (!Configuration::Get().bBotMatch || !bEnabled)
    {
        return;
    }
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!GameState || !World)
    {
        return;
    }
    if (!bBotMatchStarted && GameState->GamePhase <= EAthenaGamePhase::GetSetup() && World->GetTimeSeconds() > 5.0f)
    {
        bBotMatchStarted = true;
        Log::Info(L"Battle Royale bots: -CoreBotMatch: starting a match of bots only");
        // The match's playlist comes with its assignment, and nobody assigns this one. 3.5.1 to 4.1's InitGameState
        // (4.1 0xA2B880) gives the game state the game mode's playlist at the world's start anyway; 4.2's leaves it to
        // OnGivenMatchAssignment (0xA1B5F0), so the world had no playlist data: no aircraft paths, no safe zone
        // locations (OnPlaylistDataLoaded, 0xA1BE40). The assignment's own call runs here, on the world's playlist.
        if (AFortGameStateAthena::HasCurrentPlaylistData() && !static_cast<UFortPlaylistAthena*>(GameState->CurrentPlaylistData) && Athena->CurrentPlaylistId >= 0
            && Finder::ResolveSetCurrentPlaylistIdLive())
        {
            Log::Info(L"Battle Royale bots: -CoreBotMatch: the game state has no playlist, so it takes the world's playlist {}", static_cast<int32_t>(Athena->CurrentPlaylistId));
            Natives::AFortGameStateAthena_SetCurrentPlaylistId(GameState, Athena->CurrentPlaylistId);
        }
        reinterpret_cast<AGameMode*>(Athena)->StartMatch();
    }
    if (!bBotMatchAircraftStarted && GameState->GamePhase == EAthenaGamePhase::GetWarmup() && AFortGameStateAthena::HasWarmupCountdown()
        && GameState->WarmupCountdownEndTime > 0.0f && World->GetTimeSeconds() >= GameState->WarmupCountdownEndTime - 2.0f && Athena->GetNumConnectedPlayers() == 0)
    {
        bBotMatchAircraftStarted = true;
        if (Natives::AFortGameModeAthena_StartAircraftPhase)
        {
            // Nothing loads the match's playlist either, so OnPlaylistDataLoaded never builds the aircraft paths. 3.5.1
            // to 4.1's StartWarmupPhase and StartAircraftPhase build them anyway; 4.2's don't, and its StartAircraftPhase
            // spawns the aircraft on FlightInfos[0] unchecked: the game's own build runs first.
            if (Athena->InitializeFlightPath())
            {
                Log::Info(L"Battle Royale bots: -CoreBotMatch: no playlist loaded, so the aircraft paths are built before the bus leaves");
            }
            Log::Info(L"Battle Royale bots: -CoreBotMatch: the bus leaves with {} bots", GetAliveBotCount());
            Natives::AFortGameModeAthena_StartAircraftPhase(Athena);
        }
        else
        {
            Log::Error(L"Battle Royale bots: -CoreBotMatch: AFortGameModeAthena::StartAircraftPhase wasn't found, the bus can't leave");
        }
    }
#endif
}

void FCoreAthenaBotManager::ReadPlaylistTeams()
{
    // The reservation beacon's teams, while it has them: a match whose players never came (the bots-only test match)
    // loses its beacon when its match starts, and the game session's own room is Save the World's 4.
    AFortGameModeAthena* Athena = GetGameMode();
    int32_t Teams = Athena ? Athena->GetNumTeams() : 0;
    int32_t TeamSize = Athena ? Athena->GetPlayersPerTeam() : 0;
    if (const int32_t Forced = Configuration::Get().BotTeamSize; Forced > 0 && Configuration::Get().bBotMatch)
    {
        // -CoreBotTeamSize (testing only, with -CoreBotMatch): duos or squads in a bots-only match.
        TeamSize = Forced;
        Teams = 100 / Forced;
    }
    if (Teams > 0 && TeamSize > 0 && (Teams != PlaylistTeams || TeamSize != PlaylistTeamSize))
    {
        PlaylistTeams = Teams;
        PlaylistTeamSize = TeamSize;
        Log::Info(L"Battle Royale bots: the playlist has {} teams of {}", Teams, TeamSize);
    }
}

int32_t FCoreAthenaBotManager::MatchCapacity() const
{
    // The playlist's most players, not its team slots: the beacon's NumTeams x NumPlayersPerTeam is 100 x 2 in duos and
    // 100 x 4 in squads (GetMaxTeamInfoForPlaylist), while every Battle Royale playlist of these seasons takes 100
    // players (FortPlaylistAthena MaxPlayers: solo, duos, squads and 50v50). Duos and squads with bots went past 100.
    constexpr int32_t PlaylistMaxPlayers = 100;
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t Slots = PlaylistTeams > 0 ? PlaylistTeams * PlaylistTeamSize : Athena ? Athena->GetMatchCapacity() : 0;
    return Slots > 0 ? std::min(Slots, PlaylistMaxPlayers) : 0;
}

int32_t FCoreAthenaBotManager::WantedBotCount(int32_t PlayerTeamBots) const
{
    // bots = min(requested, MaxPlayers - connected players), re-checked until the bus leaves. Bots play on teams of
    // their own, filled to the playlist's team size: when the room is what limits them, whole teams only, and never
    // more than the teams no player is on can hold. The bots in players' teams' empty places ("Fill players' teams with
    // bots") are counted first: they take their room from these, so the match stays at its size. A large team game
    // (Teams of 20, HasSquadsInTeams) has only the playlist's few teams, players' teams too: the bots fill the room left
    // on them, in squads (GetNextBotTeamAndSquadId).
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena || !bEnabled || Settings.Count <= 0)
    {
        return 0;
    }
    const int32_t Capacity = MatchCapacity();
    int32_t Room = Capacity > 0 ? Capacity - Athena->GetNumConnectedPlayers() - PlayerTeamBots : Settings.Count;
    const int32_t TeamSize = GetTeamSize();
    if (HasSquadsInTeams())
    {
        return std::clamp(std::min(Settings.Count, Room), 0, 100);
    }
    if (Room < Settings.Count && TeamSize > 1)
    {
        Room = Room / TeamSize * TeamSize;
    }
    if (PlaylistTeams > 0)
    {
        Room = std::min(Room, (PlaylistTeams - static_cast<int32_t>(HumanTeams().size())) * TeamSize);
    }
    return std::clamp(std::min(Settings.Count, Room), 0, 100);
}

uint16_t FCoreAthenaBotManager::SquadKeyOf(const FCoreAthenaBotController* Bot) const
{
    return SquadKey(Bot->GetTeam(), HasSquadsInTeams() ? Bot->GetSquad() : 0);
}

std::map<uint16_t, int32_t> FCoreAthenaBotManager::PlayerTeamSlots() const
{
    // "Fill players' teams with bots", in duos and squads (teams of 2 to 4, not solo), and in a large team game (Teams of
    // 20) a player's squad of 4 (AFortPlayerStateAthena::SquadId, the match assignment's or Core's for a late
    // player; a player without one yet waits for it). A team (squad) gets bots once a player of it is in the match, for
    // the places its players don't take: those in the match and those its reservations still bring (a party's other
    // members, or another party that fills, loading in; a reservation has a team, not a squad, so only duos' and squads'
    // count). A team with a player whose party doesn't fill (the backend's noFill) gets none. The bots count in the
    // match's size: no more of them than its room leaves.
    //
    // The noFill list comes with the warmup answer to a report naming the player (every second in the warmup), and the
    // settings the server has before then are an earlier answer's: the one from before anyone joined has nobody in noFill
    // (CoreV2 #330: a no-fill squad's player got a bot the frame they reached the island, which the answer 0.1 s later
    // sent away). So a team gets bots only once an answer has covered each of its players in the match.
    std::map<uint16_t, int32_t> Slots;
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    const int32_t SquadSize = bSquads ? GetSquadSize() : PlaylistTeamSize;
    if (!bEnabled || !Settings.bFillPlayerTeams || !GameState || SquadSize < 2 || SquadSize > MaxSquadSize)
    {
        return Slots;
    }
    std::map<uint16_t, std::set<std::wstring>> Players;
    std::set<uint16_t> Unanswered;
    int32_t InMatch = 0;
    int32_t Unnamed = 0;
    for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (!AthenaState || FindBot(reinterpret_cast<const UObject*>(AthenaState)) || AthenaState->TeamIndex == EFortTeam::GetSpectator())
        {
            continue;
        }
        ++InMatch;
        const uint8_t Squad = bSquads ? static_cast<uint8_t>(AthenaState->SquadId) : 0;
        if (bSquads && Squad == 0)
        {
            continue;
        }
        const bool bHasId = AthenaState->UniqueId.IsValid();
        const std::wstring Id = bHasId ? AthenaState->UniqueId.ToString().ToStdString() : std::format(L"#{}", Unnamed++);
        const uint16_t Key = SquadKey(static_cast<uint8_t>(AthenaState->TeamIndex), Squad);
        Players[Key].insert(Id);
        if (bHasId && !FillAnsweredAccounts.contains(Id))
        {
            Unanswered.insert(Key);
        }
    }
    for (const auto& [Team, Id] : Athena->GetTeamReservations())
    {
        const auto Found = bSquads ? Players.end() : Players.find(SquadKey(Team, 0));
        if (Found != Players.end() && Found->second.insert(Id).second)
        {
            ++InMatch;
        }
    }
    const int32_t Capacity = MatchCapacity();
    int32_t Room = Capacity > 0 ? Capacity - std::max(InMatch, Athena->GetNumConnectedPlayers()) : 100;
    for (const auto& [Key, Ids] : Players)
    {
        const bool bNoFill = std::any_of(Ids.begin(), Ids.end(), [this](const std::wstring& Id)
        {
            return std::find(Settings.NoFillAccounts.begin(), Settings.NoFillAccounts.end(), Id) != Settings.NoFillAccounts.end();
        });
        const int32_t Free = std::min(SquadSize - static_cast<int32_t>(Ids.size()), Room);
        if (!bNoFill && !Unanswered.contains(Key) && Free > 0)
        {
            Slots[Key] = Free;
            Room -= Free;
        }
    }
    return Slots;
}

int32_t FCoreAthenaBotManager::CountAliveBots(bool bPlayersTeams) const
{
    return static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [bPlayersTeams](const auto& Bot) { return Bot->IsAlive() && Bot->IsOnPlayersTeam() == bPlayersTeams; }));
}

void FCoreAthenaBotManager::TickWarmup(float Now)
{
#if WITH_SERVER_CODE
    if (!Finder::ResolveBotSkydiveLive(AFortPlayerPawnAthena::IsSkydivingOffset()) || !AAIController::StaticClass())
    {
        if (!bWarnedUnsupported && bEnabled)
        {
            bWarnedUnsupported = true;
            Log::Warning(L"Battle Royale: bots aren't supported on this build (no AAIController or AFortPlayerPawnAthena::BeginSkydiving), the match runs without them");
        }
        return;
    }
    const std::map<uint16_t, int32_t> TeamSlots = PlayerTeamSlots();
    int32_t TeamBots = 0;
    for (const auto& [Key, Free] : TeamSlots)
    {
        TeamBots += Free;
    }
    const int32_t Wanted = WantedBotCount(TeamBots);
    if (Wanted != LastLoggedWanted || TeamBots != LastLoggedTeamBots)
    {
        AFortGameModeAthena* Athena = GetGameMode();
        Log::Info(L"Battle Royale bots: {} bots for the match (asked for {}, room for {} of {} with {} players connected){}", Wanted, Settings.Count,
            Athena ? MatchCapacity() - Athena->GetNumConnectedPlayers() : 0, MatchCapacity(), Athena ? Athena->GetNumConnectedPlayers() : 0,
            Settings.bFillPlayerTeams ? std::format(L", and {} in the empty places of {} players' teams", TeamBots, TeamSlots.size()) : std::wstring());
        LastLoggedWanted = Wanted;
        LastLoggedTeamBots = TeamBots;
    }
    RemoveSurplusBots(Wanted, TeamSlots);
    LastWantedTotal = Wanted + TeamBots;
    ++FramesSinceSpawn;
    const int32_t Missing = std::max(Wanted - CountAliveBots(false), 0) + std::max(TeamBots - CountAliveBots(true), 0);
    if (Missing > 0)
    {
        // Bots come in as players do (#233): none before the first player is on the island (a server nobody joined
        // spends nothing on them), then one at a time, a few sooner when another player joins, spread over the warmup
        // up to 20 s before the bus. Never more than one a frame, nor closer than MinSpawnInterval and MinSpawnFrames
        // (#256): when the warmup is too short for them all at that rate, fewer are in when the bus leaves, which
        // OnAircraftPhaseStarted logs. -CoreBotMatch (no players) brings them in at that rate from the start. A
        // player's team's bots come first.
        AFortGameModeAthena* Athena = GetGameMode();
        AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
        const int32_t Humans = Athena ? Athena->GetNumConnectedPlayers() : 0;
        const bool bBotMatch = Configuration::Get().bBotMatch;
        if (bBotMatch || HasHumanSpawned())
        {
            if (Humans > LastHumanCount && !bBotMatch)
            {
                // Another player joined: a few bots with them, the next one now and the others at the quickest rate.
                JoinBotsLeft = std::min(Missing, 1 + static_cast<int32_t>(FMath::Rand() % 3));
                NextPacedSpawn = std::min(NextPacedSpawn, Now);
            }
            LastHumanCount = Humans;
            if (Now >= NextPacedSpawn && Now - LastSpawnTime >= MinSpawnInterval && FramesSinceSpawn >= MinSpawnFrames)
            {
                const float EndTime = GameState && AFortGameStateAthena::HasWarmupCountdown() ? static_cast<float>(GameState->WarmupCountdownEndTime) : 0.0f;
                // Spread evenly over the time left to 20 s before the bus (the warmup's countdown); with no countdown
                // yet, or past that point, at the quickest rate.
                const float TimeLeft = EndTime > 0.0f ? EndTime - SpawnDoneBeforeBus - Now : 0.0f;
                const float Interval = std::clamp(TimeLeft / static_cast<float>(Missing), MinSpawnInterval, MaxSpawnInterval);
                if (!bLoggedSpawnStart)
                {
                    bLoggedSpawnStart = true;
                    Log::Info(L"Battle Royale bots: {} spawning {} bots, about one every {:.1f} s ({:.0f} s of warmup left)", bBotMatch ? L"-CoreBotMatch:" : L"a player is on the island:",
                        Missing, Interval, EndTime > 0.0f ? EndTime - Now : -1.0f);
                    if (const int32_t Fit = EndTime > 0.0f ? std::max(static_cast<int32_t>((EndTime - Now) / MinSpawnInterval), 0) : Missing; Fit < Missing)
                    {
                        Log::Warning(L"Battle Royale bots: the warmup has room for about {} of the {} bots at one every {:.1f} s at most, so about {} won't be in by the bus "
                            L"(a longer warmup on the admin panel's Matchmaking tab brings them all)", Fit, Missing, MinSpawnInterval, Missing - Fit);
                    }
                }
                const auto SpawnStart = std::chrono::steady_clock::now();
                SpawnNextBot(Wanted, TeamSlots, Now);
                SpawnMsMax = std::max(SpawnMsMax, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - SpawnStart).count());
                LastSpawnTime = Now;
                FramesSinceSpawn = 0;
                JoinBotsLeft = std::max(JoinBotsLeft - 1, 0);
                NextPacedSpawn = Now + (JoinBotsLeft > 0 ? MinSpawnInterval : std::max(Interval * RandomRange(0.6f, 1.4f), MinSpawnInterval));
            }
        }
    }
    // The navigation (its octree and first build take a quarter of a second in one frame) and the assets the bots load
    // on first use (emotes) are set up as soon as the first bots are in, not as the bus leaves with every player on
    // the server ("ping spikes" with bots on, #233).
    if (!bNavigationTried && Wanted + TeamBots > 0 && GetAliveBotCount() > 0)
    {
        bNavigationTried = true;
        FCoreAthenaBotController::PreloadAssets();
        RefreshLoot(Now);
        GatherContainers();
        InitializeNavigation();
    }
#endif
}

bool FCoreAthenaBotManager::HasHumanSpawned() const
{
    // A player on the island: a player state that isn't a bot's, whose controller has a pawn.
    AFortGameStateAthena* GameState = AthenaGameStateOf(GetGameMode());
    if (!GameState)
    {
        return false;
    }
    for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        AController* Owner = AthenaState && !FindBot(reinterpret_cast<const UObject*>(AthenaState)) ? Cast<AController>(reinterpret_cast<UObject*>(AthenaState->GetOwner())) : nullptr;
        if (Owner && static_cast<APawn*>(Owner->Pawn))
        {
            return true;
        }
    }
    return false;
}

bool FCoreAthenaBotManager::SpawnNextBot(int32_t Wanted, const std::map<uint16_t, int32_t>& TeamSlots, float Now)
{
    if (Now < NextRegisterTime)
    {
        return false;
    }
    // A player's team (squad) first: its bots are in with it, well before the bus.
    for (const auto& [Key, Free] : TeamSlots)
    {
        const int32_t OnTeam = static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [this, Key](const auto& Bot)
        {
            return Bot->IsAlive() && Bot->IsOnPlayersTeam() && SquadKeyOf(Bot.get()) == Key;
        }));
        if (OnTeam < Free)
        {
            if (!RegisterBot(Now, static_cast<uint8_t>(Key & 0xFF), static_cast<uint8_t>(Key >> 8), true))
            {
                // Try again in a while (a spawn failed).
                NextRegisterTime = Now + 2.0f;
                return false;
            }
            return true;
        }
    }
    if (CountAliveBots(false) >= Wanted)
    {
        return false;
    }
    uint8_t Team = 0xFF;
    uint8_t Squad = 0;
    if (!GetNextBotTeamAndSquadId(Team, Squad))
    {
        if (!bLoggedCap)
        {
            bLoggedCap = true;
            Log::Warning(L"Battle Royale bots: no team is free for another bot");
        }
        NextRegisterTime = Now + 2.0f;
        return false;
    }
    if (!RegisterBot(Now, Team, Squad, false))
    {
        NextRegisterTime = Now + 2.0f;
        return false;
    }
    return true;
}

void FCoreAthenaBotManager::RemoveSurplusBots(int32_t Wanted, const std::map<uint16_t, int32_t>& TeamSlots)
{
    // Bots on players' teams (squads): a team keeps as many as it has empty places (a party member loading in late takes
    // one back; the setting turned off, or a player without fill joining the team, takes them all). Bots on teams of their
    // own: the newest go first, a bot on a team a player has since joined (late joining) before them; in a large team
    // game, whose teams players and bots share, only the newest.
    std::map<uint16_t, int32_t> OnTeams;
    for (const auto& Bot : PlayerBots)
    {
        OnTeams[SquadKeyOf(Bot.get())] += Bot->IsAlive() && Bot->IsOnPlayersTeam() ? 1 : 0;
    }
    bool bSurplus = CountAliveBots(false) > Wanted;
    for (const auto& [Key, OnTeam] : OnTeams)
    {
        const auto Slot = TeamSlots.find(Key);
        bSurplus = bSurplus || OnTeam > (Slot != TeamSlots.end() ? Slot->second : 0);
    }
    if (!bSurplus)
    {
        return;
    }
    const std::set<uint8_t> Humans = HasSquadsInTeams() ? std::set<uint8_t>() : HumanTeams();
    std::stable_sort(PlayerBots.begin(), PlayerBots.end(), [&Humans](const auto& A, const auto& B)
    {
        const bool bAHuman = !A->IsOnPlayersTeam() && Humans.contains(A->GetTeam());
        const bool bBHuman = !B->IsOnPlayersTeam() && Humans.contains(B->GetTeam());
        if (bAHuman != bBHuman)
        {
            return !bAHuman;
        }
        return A->GetBotIndex() < B->GetBotIndex();
    });
    std::map<uint16_t, int32_t> Kept;
    int32_t KeptOwn = 0;
    for (auto It = PlayerBots.begin(); It != PlayerBots.end();)
    {
        FCoreAthenaBotController* Bot = It->get();
        bool bKeep = true;
        int32_t Room = Wanted;
        if (Bot->IsAlive() && Bot->IsOnPlayersTeam())
        {
            const uint16_t Key = SquadKeyOf(Bot);
            const auto Slot = TeamSlots.find(Key);
            Room = Slot != TeamSlots.end() ? Slot->second : 0;
            bKeep = Kept[Key] < Room;
            Kept[Key] += bKeep ? 1 : 0;
        }
        else if (Bot->IsAlive())
        {
            bKeep = KeptOwn < Wanted;
            KeptOwn += bKeep ? 1 : 0;
        }
        if (bKeep)
        {
            ++It;
            continue;
        }
        Log::Info(L"Battle Royale bots: {} leaves to make room ({})", Bot->GetBotName(), Bot->IsOnPlayersTeam()
            ? std::format(L"{} has {} empty places for bots", EFortTeam{ Bot->GetTeam() }.ToString(), Room)
            : std::format(L"the match wants {} bots on teams of their own", Wanted));
        Bot->Destroy();
        It = PlayerBots.erase(It);
    }
}

bool FCoreAthenaBotManager::GetNextBotTeamAndSquadId(uint8_t& OutTeam, uint8_t& OutSquadId) const
{
    // 21.40's GetNextBotTeamAndSquadId (8.30's RegisterBot fills the playlist's teams in order, cycling LastBotTeam).
    // Core's bots play on teams of their own (the owner's choice): a team no player is on, from the last (HumanPvP_Team100
    // on solo's 100 teams) down, while players' teams come from the first, filled to the playlist's team size.
    // A large team game (Teams of 20's 5 teams of 20) has no teams of their own to give them: they fill
    // the playlist's teams in squads of 4, as its players do. The team with the fewest players (those in the match or
    // reserved, and its bots) that has room, the first on a tie; in it, a bots' squad with room, else a new squad.
    OutTeam = 0xFF;
    OutSquadId = 0;
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    const int32_t TeamSize = GetTeamSize();
    const int32_t NumTeams = PlaylistTeams > 0 ? std::min(PlaylistTeams, 100) : 100;
    const uint8_t FirstTeam = EFortTeam::GetHumanPvP_Team1();
    if (!HasSquadsInTeams())
    {
        const std::set<uint8_t> Humans = HumanTeams();
        for (int32_t Index = NumTeams - 1; Index >= 0; --Index)
        {
            const uint8_t Team = static_cast<uint8_t>(FirstTeam + Index);
            if (Humans.contains(Team))
            {
                continue;
            }
            const int32_t OnTeam = static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [Team](const auto& Bot) { return Bot->IsAlive() && Bot->GetTeam() == Team; }));
            if (OnTeam < TeamSize)
            {
                OutTeam = Team;
                return true;
            }
        }
        return false;
    }
    if (!Athena || !GameState || PlaylistTeams <= 0)
    {
        return false;
    }

    std::map<uint8_t, std::set<std::wstring>> Players;
    for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (AthenaState && !FindBot(reinterpret_cast<const UObject*>(AthenaState)) && AthenaState->TeamIndex != EFortTeam::GetSpectator())
        {
            Players[static_cast<uint8_t>(AthenaState->TeamIndex)].insert(AthenaState->UniqueId.IsValid() ? AthenaState->UniqueId.ToString().ToStdString() : AthenaState->GetName().ToStdString());
        }
    }
    for (const auto& [Team, Id] : Athena->GetTeamReservations())
    {
        Players[Team].insert(Id);
    }
    int32_t Fewest = TeamSize;
    for (int32_t Index = 0; Index < NumTeams; ++Index)
    {
        const uint8_t Team = static_cast<uint8_t>(FirstTeam + Index);
        const auto Found = Players.find(Team);
        const int32_t OnTeam = (Found != Players.end() ? static_cast<int32_t>(Found->second.size()) : 0)
            + static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [Team](const auto& Bot) { return Bot->IsAlive() && Bot->GetTeam() == Team; }));
        if (OnTeam < Fewest)
        {
            Fewest = OnTeam;
            OutTeam = Team;
        }
    }
    if (OutTeam == 0xFF)
    {
        return false;
    }
    // The fullest of the team's bots' squads that has room, so they make whole squads.
    std::map<uint8_t, int32_t> BotSquads;
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive() && !Bot->IsOnPlayersTeam() && Bot->GetTeam() == OutTeam && Bot->GetSquad() != 0)
        {
            ++BotSquads[Bot->GetSquad()];
        }
    }
    int32_t Fullest = 0;
    for (const auto& [Squad, Members] : BotSquads)
    {
        if (Members < GetSquadSize() && Members > Fullest)
        {
            Fullest = Members;
            OutSquadId = Squad;
        }
    }
    if (OutSquadId == 0)
    {
        OutSquadId = GetNextUniqueSquadID();
    }
    return true;
}

uint8_t FCoreAthenaBotManager::GetNextUniqueSquadID() const
{
    // A squad no player state (a player's or a bot's) is in. The match assignment's squads count up from 1 and its players
    // may still be loading, so, as late players' do (PlaceLatePlayerInSquad), the bots' count down from 255, clear of them.
    std::set<uint8_t> Used;
    AFortGameStateAthena* GameState = AthenaGameStateOf(GetGameMode());
    if (GameState && AFortPlayerStateAthena::HasSquadId())
    {
        for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            if (AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState)))
            {
                Used.insert(static_cast<uint8_t>(AthenaState->SquadId));
            }
        }
    }
    for (const auto& Bot : PlayerBots)
    {
        Used.insert(Bot->GetSquad());
    }
    for (uint32_t Candidate = 255; Candidate > 0; --Candidate)
    {
        if (!Used.contains(static_cast<uint8_t>(Candidate)))
        {
            return static_cast<uint8_t>(Candidate);
        }
    }
    return 0;
}

void FCoreAthenaBotManager::PlaceBotOnTeam(FCoreAthenaBotController* Bot)
{
#if WITH_SERVER_CODE
    // 21.40's AFortGameModeAthena::PlaceBotOnTeam: the bot's controller joins its team as a joining player's does in
    // PlacePlayerOnTeam, with the game mode's AttemptChangeTeamInfoForController: the team's AFortTeamInfo takes the
    // controller (TeamMembers) and its player state the team (PlayerTeam and PlayerTeamPrivate, replicated, and
    // TeamIndex). Then its squad (SquadId: the squad bar's, and the squad's relevancy), as Battle Royale's
    // PlacePlayerOnTeam (3.2 0x9C6240) gives a player theirs. Without the finder's slot, or for a team without its
    // AFortTeamInfo, the bot keeps the old way: TeamIndex alone.
    AFortGameModeAthena* Athena = GetGameMode();
    AAIController* Controller = Bot->GetController();
    AFortPlayerStateAthena* State = Bot->GetPlayerState();
    if (!Athena || !Controller || !State)
    {
        return;
    }
    const uint8_t Team = Bot->GetTeam();
    if (Finder::ResolveTeamChangeLive())
    {
        // The team in a whole register: the game compares all of edx with Spectator (3.2 0xB526C0, "cmp r14d, 67h").
        CallVirtual<bool>(static_cast<AFortGameMode*>(Athena), Natives::AFortGameMode_AttemptChangeTeamInfoForControllerIndex, static_cast<uint32_t>(Team),
            reinterpret_cast<AController*>(Controller));
    }
    static bool bLoggedTeam = false;
    if (static_cast<uint8_t>(State->TeamIndex) != Team)
    {
        State->TeamIndex = EFortTeam{ Team };
        if (!bLoggedTeam)
        {
            bLoggedTeam = true;
            Log::Warning(L"Battle Royale bots: {} isn't in {}'s AFortTeamInfo, so only its TeamIndex is set", Bot->GetBotName(), EFortTeam{ Team }.ToString());
        }
    }
    else if (!bLoggedTeam)
    {
        bLoggedTeam = true;
        AFortTeamInfo* TeamInfo = static_cast<AFortTeamInfo*>(State->PlayerTeam);
        Log::Info(L"Battle Royale bots: {} joins {} as a player does ({})", Bot->GetBotName(), EFortTeam{ Team }.ToString(),
            TeamInfo ? TeamInfo->GetName().ToStdString() : std::wstring(L"no AFortTeamInfo"));
    }
    if (Bot->GetSquad() != 0 && AFortPlayerStateAthena::HasSquadId())
    {
        State->SquadId = Bot->GetSquad();
    }
    State->ForceNetUpdate();
#endif
}

bool FCoreAthenaBotManager::RegisterBot(float Now, uint8_t Team, uint8_t Squad, bool bPlayersTeam)
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!World || Team == 0xFF)
    {
        return false;
    }

    if (WarmupSpots.empty())
    {
        UClass* WarmupStartClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortPlayerStartWarmup");
        for (AActor* Start : UGameplayStatics::GetAllActorsOfClass(Athena, WarmupStartClass))
        {
            if (Start)
            {
                WarmupSpots.push_back(LocationOf(Start));
            }
        }
        Log::Info(L"Battle Royale bots: {} warmup spots", WarmupSpots.size());
        if (WarmupSpots.empty())
        {
            return false;
        }
    }

    // A stock AAIController with a player state of the game mode's class (bWantsPlayerState, read as the controller
    // finishes spawning: AAIController::PostInitializeComponents' InitPlayerState), as 8.30's bot controller has.
    FTransform SpawnTransform;
    SpawnTransform.Rotation.W = 1.0f;
    AAIController* Controller = Cast<AAIController>(UGameplayStatics::BeginDeferredActorSpawnFromClass(Athena, AAIController::StaticClass(), SpawnTransform,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn));
    if (!Controller)
    {
        Log::Error(L"Battle Royale bots: couldn't spawn an AIController");
        return false;
    }
    Controller->bWantsPlayerState = true;
    Controller->bStopAILogicOnUnposses = false;
    UGameplayStatics::FinishSpawningActor(Controller, SpawnTransform);
    AFortPlayerStateAthena* PlayerState = AthenaStateOf(Controller);
    if (!PlayerState)
    {
        Log::Error(L"Battle Royale bots: {} has no Athena player state ({}), so it can't play", Controller->GetName().ToStdString(),
            Controller->PlayerState ? static_cast<APlayerState*>(Controller->PlayerState)->GetClass()->GetName().ToStdString() : std::wstring(L"none"));
        Controller->K2_DestroyActor();
        return false;
    }

    const int32_t BotIndex = NextBotIndex++;
    auto Bot = std::make_unique<FCoreAthenaBotController>(this, Controller, BotIndex, Team);
    Bot->SetSquad(Squad);
    Bot->SetOnPlayersTeam(bPlayersTeam);
    if (NameOrder.empty())
    {
        for (int32_t Index = 0; Index < NumBotNames; ++Index)
        {
            NameOrder.push_back(Index);
        }
        for (int32_t Index = NumBotNames - 1; Index > 0; --Index)
        {
            std::swap(NameOrder[static_cast<size_t>(Index)], NameOrder[static_cast<size_t>(FMath::Rand()) % static_cast<size_t>(Index + 1)]);
        }
    }
    const wchar_t* const BaseName = BotNames[NameOrder[static_cast<size_t>(BotIndex % NumBotNames)]];
    const std::wstring Name = BotIndex < NumBotNames ? std::wstring(BaseName) : std::format(L"{}{}", BaseName, BotIndex / NumBotNames + 1);
    Bot->AssignDisplayName(Name);
    PlaceBotOnTeam(Bot.get());
    Bot->InitializeSkills(Settings.Level);
    FVector Spot = WarmupSpots[static_cast<size_t>(FMath::Rand()) % WarmupSpots.size()];
    // A bot on a player's team (squad) comes in by its player, when they have a pawn on the island.
    if (AFortGameStateAthena* GameState = bPlayersTeam ? AthenaGameStateOf(Athena) : nullptr)
    {
        const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
        for (APlayerState* Other : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            AFortPlayerStateAthena* OtherState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(Other));
            AController* Owner = OtherState && static_cast<uint8_t>(OtherState->TeamIndex) == Team && (!bSquads || static_cast<uint8_t>(OtherState->SquadId) == Squad)
                && !FindBot(reinterpret_cast<const UObject*>(OtherState)) ? Cast<AController>(reinterpret_cast<UObject*>(OtherState->GetOwner())) : nullptr;
            if (APawn* OtherPawn = Owner ? static_cast<APawn*>(Owner->Pawn) : nullptr)
            {
                const FVector Near = LocationOf(reinterpret_cast<const AActor*>(OtherPawn));
                Spot = FVector(Near.X + RandomRange(-250.0f, 250.0f), Near.Y + RandomRange(-250.0f, 250.0f), Near.Z);
                break;
            }
        }
    }
    if (!Bot->SpawnPawnAt(FVector(Spot.X + RandomRange(-150.0f, 150.0f), Spot.Y + RandomRange(-150.0f, 150.0f), Spot.Z + 100.0f), RandomRange(0.0f, 360.0f), false))
    {
        Bot->Destroy();
        return false;
    }
    AddToAliveBots(Bot.get());
    PlayerBots.push_back(std::move(Bot));
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotManager::AddToAliveBots(FCoreAthenaBotController* Bot)
{
    AFortPlayerStateAthena* State = Bot->GetPlayerState();
    Log::Info(L"Battle Royale bots: {} joins {}{} ({}, {})", Bot->GetBotName(), Bot->IsOnPlayersTeam() ? L"a player's team, " : L"on ",
        State ? static_cast<EFortTeam>(State->TeamIndex).ToString() : std::wstring(L"no team"), Bot->GetController() ? Bot->GetController()->GetName().ToStdString() : std::wstring(), Bot->GetPawn() ? Bot->GetPawn()->GetName().ToStdString() : std::wstring(L"no pawn"));
}

AFortAthenaAircraft* FCoreAthenaBotManager::AircraftOfTeam(AFortGameStateAthena* GameState, uint8_t Team, AFortAthenaAircraft* DefaultAircraft)
{
#if WITH_SERVER_CODE
    // 3.5's AFortGameStateAthena::GetAircraftIndex (3.5.1 0x9E5750) for a bot's team: with OpposingAirCraftForEachTeam
    // (50v50: one bus per team) the aircraft is Aircrafts[the team less HumanPvP_Team1], else the only one (index 0). The
    // index is unbounded during the setup and the warmup, and from the aircraft phase on it is 0 outside Aircrafts.
    if (!GameState || !AFortGameStateAthena::HasAircrafts() || GameState->AirCraftBehavior != 1)
    {
        return DefaultAircraft;
    }
    const TArray<AFortAthenaAircraft*>& Aircrafts = GameState->GetMember_Aircrafts();
    int32_t Index = static_cast<int32_t>(Team) - static_cast<int32_t>(EFortTeam::GetHumanPvP_Team1());
    if (Index < 0 || Index >= Aircrafts.Num())
    {
        Index = 0;
    }
    return Aircrafts.IsValidIndex(Index) && Aircrafts[Index] ? Aircrafts[Index] : DefaultAircraft;
#else
    return DefaultAircraft;
#endif
}

void FCoreAthenaBotManager::OnAircraftPhaseStarted(float Now)
{
#if WITH_SERVER_CODE
    bAircraftStarted = true;
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    AFortAthenaAircraft* Aircraft = nullptr;
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(Athena, AFortAthenaAircraft::StaticClass()))
    {
        Aircraft = Aircraft ? Aircraft : Cast<AFortAthenaAircraft>(Actor);
    }
    float DropStart = Now + 5.0f;
    float DropEnd = Now + 60.0f;
    if (Aircraft && AFortAthenaAircraft::HasDropTimes() && Aircraft->DropEndTime > Aircraft->DropStartTime)
    {
        DropStart = Aircraft->DropStartTime;
        DropEnd = Aircraft->DropEndTime;
    }
    DropStartTime = DropStart;
    DropEndTime = DropEnd;
    CurrentAircraft = FWeakObjectPtr(reinterpret_cast<UObject*>(Aircraft));
    if (!Aircraft)
    {
        Log::Warning(L"Battle Royale bots: no aircraft found, the bots drop from where they are");
    }
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive())
        {
            // 21.40's JumpOffBus picks its moment along the path; Core's spread the bots over the drop zone, most of them
            // in its first two thirds.
            // A bot on a player's team waits for its player (UpdatePlayerTeamBots), else goes at the drop zone's end.
            const float T = Bot->IsOnPlayersTeam() && HasAlivePlayerOnTeam(Athena, Bot->GetTeam(), HasSquadsInTeams() ? Bot->GetSquad() : 0) ? 1.0f : std::pow(FMath::FRand(), 1.4f);
            Bot->EnterAircraft(AircraftOfTeam(GameState, Bot->GetTeam(), Aircraft), DropStart + 1.0f + (DropEnd - DropStart - 3.0f) * T, DropEnd - 1.0f);
        }
    }
    BotsAtStart = GetAliveBotCount();
    HumansAtStart = Athena ? Athena->AlivePlayers.Num() : 0;
    MostTeamsAlive = static_cast<int32_t>(AliveTeams(true).size());
    // The match's starting players (the count the players outlived is worked out from, copied from PlayersLeft as the
    // aircraft starts) and its players and teams left count the bots.
    if (GameState && Natives::AFortGameStateAthena_StartingPlayers > 0)
    {
        int32_t& StartingPlayers = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(GameState) + Natives::AFortGameStateAthena_StartingPlayers);
        StartingPlayers = HumansAtStart + BotsAtStart;
    }
    UpdateCounts();
    Log::Info(L"Battle Royale bots: the bus leaves with {} players and {} bots (drop zone {:.0f} s to {:.0f} s from now)", HumansAtStart, BotsAtStart, DropStart - Now, DropEnd - Now);
    if (bLoggedSpawnStart && BotsAtStart < LastWantedTotal)
    {
        // Paced spawning (#256): the warmup ran out before every bot was in, and the rest don't come in a burst.
        Log::Warning(L"Battle Royale bots: {} bots short of the {} wanted: the warmup ran out at one bot every {:.1f} s at most (a longer warmup on the admin panel's Matchmaking tab brings them all)",
            LastWantedTotal - BotsAtStart, LastWantedTotal, MinSpawnInterval);
    }
    if (BotsAtStart > 0)
    {
        RefreshLoot(Now);
        GatherContainers();
        GatherLandingSpots();
        if (!bNavigationTried)
        {
            bNavigationTried = true;
            InitializeNavigation();
        }
    }
#endif
}

void FCoreAthenaBotManager::GatherLandingSpots()
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_FreeFalling lands at a POI (_SelectNextPOI). 3.x's named locations are its
    // AFortPoiVolumes tagged Athena.Location.POI.* (Core gives the shapeless ones a box, FortPoiVolume.cpp); the loot
    // elsewhere is in clusters of chests (houses and farms), which count as spots too.
    LandingSpots.clear();
    size_t Named = 0;
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(GetGameMode(), AFortPoiVolume::StaticClass()))
    {
        AFortPoiVolume* Volume = static_cast<AFortPoiVolume*>(Actor);
        std::wstring Name;
        for (const FGameplayTag& Tag : Volume->LocationTags.GameplayTags)
        {
            const std::wstring TagName = Tag.TagName.ToString().ToStdString();
            if (TagName.rfind(L"Athena.Location.POI.", 0) == 0)
            {
                Name = TagName.substr(20);
            }
        }
        if (Name.empty())
        {
            continue;
        }
        const FBox Box = Volume->GetFallbackBox();
        const FVector Center = Box.IsValid ? Box.GetCenter() : LocationOf(Volume);
        const float Radius = Box.IsValid ? std::clamp(std::max(Box.GetExtent().X, Box.GetExtent().Y), 4000.0f, 15000.0f) : 8000.0f;
        if (std::fabs(Center.X) < 1.0f && std::fabs(Center.Y) < 1.0f)
        {
            continue;
        }
        const float Value = LootAround(Center, Radius);
        if (Value >= 2.0f)
        {
            LandingSpots.push_back({ Center, Radius, Name, Value, 0 });
            ++Named;
        }
    }
    // Clusters of chests away from the named locations: a 60 m grid, three chests or more to a cell.
    std::map<std::pair<int32_t, int32_t>, std::vector<FVector>> Cells;
    for (const FCoreAthenaBotContainer& Container : Containers)
    {
        if (Container.bChest)
        {
            Cells[{ static_cast<int32_t>(std::floor(Container.Location.X / 6000.0f)), static_cast<int32_t>(std::floor(Container.Location.Y / 6000.0f)) }].push_back(Container.Location);
        }
    }
    for (const auto& [Cell, Chests] : Cells)
    {
        if (Chests.size() < 3)
        {
            continue;
        }
        FVector Center;
        for (const FVector& Chest : Chests)
        {
            Center = Center + Chest * (1.0f / static_cast<float>(Chests.size()));
        }
        bool bNearNamed = false;
        for (size_t Index = 0; Index < Named; ++Index)
        {
            const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Index];
            bNearNamed = bNearNamed || (Spot.Location.X - Center.X) * (Spot.Location.X - Center.X) + (Spot.Location.Y - Center.Y) * (Spot.Location.Y - Center.Y) < Spot.Radius * Spot.Radius;
        }
        if (!bNearNamed)
        {
            LandingSpots.push_back({ Center, 4500.0f, std::format(L"chests at ({:.0f}, {:.0f})", Center.X, Center.Y), LootAround(Center, 4500.0f), 0 });
        }
    }
    std::sort(LandingSpots.begin(), LandingSpots.end(), [](const FCoreAthenaBotLandingSpot& A, const FCoreAthenaBotLandingSpot& B) { return A.Loot > B.Loot; });
    std::wstring Top;
    for (size_t Index = 0; Index < LandingSpots.size() && Index < 8; ++Index)
    {
        Top += std::format(L"{}{} ({:.0f})", Index ? L", " : L"", LandingSpots[Index].Name, LandingSpots[Index].Loot);
    }
    Log::Info(L"Battle Royale bots: {} landing spots, {} of them named locations; the richest: {}", LandingSpots.size(), Named, Top);
#endif
}

float FCoreAthenaBotManager::LootAround(const FVector& Center, float Radius) const
{
    float Value = 0.0f;
    for (const FCoreAthenaBotContainer& Container : Containers)
    {
        const float Dx = Container.Location.X - Center.X;
        const float Dy = Container.Location.Y - Center.Y;
        Value += Dx * Dx + Dy * Dy < Radius * Radius ? (Container.bChest ? 1.0f : 0.3f) : 0.0f;
    }
    for (const FCoreAthenaBotLoot& Entry : Loot)
    {
        const float Dx = Entry.Location.X - Center.X;
        const float Dy = Entry.Location.Y - Center.Y;
        Value += Entry.bWeapon && Dx * Dx + Dy * Dy < Radius * Radius ? 0.3f : 0.0f;
    }
    return Value;
}

int32_t FCoreAthenaBotManager::FindNextBestPOI(const FVector& From, const std::vector<int32_t>& Visited) const
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_SelectNextPOI::FindNextBestPOI: the spots are the world's own (its AFortPoiVolumes'
    // Athena.Location.POI.* tags and its chest clusters, GatherLandingSpots), so every build's map has its own. A spot
    // counts with loot still in it (its containers unsearched), inside the next safe zone, and near (Core's 🟡: within
    // 400 m); the loot left weighs it, the distance (squared) and the bots already there count against it.
    constexpr float MaxDistance = 40000.0f;
    FVector ZoneCenter;
    float ZoneRadius = 0.0f;
    const bool bHasZone = GetSafeZone(ZoneCenter, ZoneRadius);
    int32_t Best = -1;
    float BestScore = 0.0f;
    for (size_t Index = 0; Index < LandingSpots.size(); ++Index)
    {
        const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Index];
        if (std::find(Visited.begin(), Visited.end(), static_cast<int32_t>(Index)) != Visited.end())
        {
            continue;
        }
        const float Dx = Spot.Location.X - From.X;
        const float Dy = Spot.Location.Y - From.Y;
        const float Distance = std::sqrt(Dx * Dx + Dy * Dy);
        if (Distance > MaxDistance || Distance < Spot.Radius * 0.5f)
        {
            continue;
        }
        if (bHasZone)
        {
            const float Zx = Spot.Location.X - ZoneCenter.X;
            const float Zy = Spot.Location.Y - ZoneCenter.Y;
            if (std::sqrt(Zx * Zx + Zy * Zy) > ZoneRadius * 0.85f)
            {
                continue;
            }
        }
        const float Value = LootAround(Spot.Location, Spot.Radius);
        if (Value < 1.0f)
        {
            continue;
        }
        int32_t BotsThere = 0;
        for (const auto& Bot : PlayerBots)
        {
            if (Bot->GetState() != ECoreAthenaBotState::Landed || !Bot->GetPawn())
            {
                continue;
            }
            const FVector Other = LocationOf(reinterpret_cast<const AActor*>(Bot->GetPawn()));
            const float Ox = Other.X - Spot.Location.X;
            const float Oy = Other.Y - Spot.Location.Y;
            BotsThere += Bot->GetNextPOI() == static_cast<int32_t>(Index) || Ox * Ox + Oy * Oy < Spot.Radius * Spot.Radius ? 1 : 0;
        }
        // The nearest come first (a big named location 300 m off outscored the chests next door): the weight falls with
        // the square of the distance in 100 m.
        const float Hundreds = Distance / 10000.0f;
        const float Score = Value * RandomRange(0.8f, 1.2f) / (1.0f + Hundreds * Hundreds) / (1.0f + 0.5f * static_cast<float>(BotsThere));
        if (Score > BestScore)
        {
            BestScore = Score;
            Best = static_cast<int32_t>(Index);
        }
    }
    return Best;
#else
    return -1;
#endif
}

int32_t FCoreAthenaBotManager::FindPOIAt(const FVector& Location) const
{
    int32_t Found = -1;
    float Nearest = 1e18f;
    for (size_t Index = 0; Index < LandingSpots.size(); ++Index)
    {
        const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Index];
        const float DistanceSquared = (Spot.Location.X - Location.X) * (Spot.Location.X - Location.X) + (Spot.Location.Y - Location.Y) * (Spot.Location.Y - Location.Y);
        if (DistanceSquared < Spot.Radius * Spot.Radius && DistanceSquared < Nearest)
        {
            Nearest = DistanceSquared;
            Found = static_cast<int32_t>(Index);
        }
    }
    return Found;
}

const FCoreAthenaBotLandingSpot* FCoreAthenaBotManager::GetPOI(int32_t Index) const
{
    return Index >= 0 && Index < static_cast<int32_t>(LandingSpots.size()) ? &LandingSpots[static_cast<size_t>(Index)] : nullptr;
}

int32_t FCoreAthenaBotManager::CountHuntersOf(const AFortPawn* Target, const FCoreAthenaBotController* Except) const
{
    return static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [Target, Except](const auto& Bot) { return Bot.get() != Except && Bot->IsAlive() && Bot->GetHuntTarget() == Target; }));
}

void FCoreAthenaBotManager::UpdateFlightPath(float Now)
{
#if WITH_SERVER_CODE
    // The bus flies a straight line at a steady speed: two of its locations a moment apart give its path.
    AActor* Aircraft = GetAircraft();
    if (!Aircraft || LandingSpots.empty())
    {
        bLandingAssigned = true;
        Log::Info(L"Battle Royale bots: no {}, the bots drop near the bus's path", Aircraft ? L"landing spots" : L"aircraft");
        return;
    }
    const FVector Location = LocationOf(Aircraft);
    if (!bHasFlightSample)
    {
        bHasFlightSample = true;
        FlightSample = Location;
        FlightSampleTime = Now;
        return;
    }
    const float Moved = std::sqrt((Location.X - FlightSample.X) * (Location.X - FlightSample.X) + (Location.Y - FlightSample.Y) * (Location.Y - FlightSample.Y));
    if (Moved < 2000.0f || Now - FlightSampleTime < 0.5f)
    {
        // The bus waiting on its start: sampled again from where it is.
        if (Now - FlightSampleTime > 2.0f && Moved < 100.0f)
        {
            FlightSample = Location;
            FlightSampleTime = Now;
        }
        return;
    }
    FlightSpeed = Moved / (Now - FlightSampleTime);
    FlightDirection = FVector((Location.X - FlightSample.X) / Moved, (Location.Y - FlightSample.Y) / Moved, 0.0f);
    FlightSample = Location;
    FlightSampleTime = Now;
    bLandingAssigned = true;
    Log::Info(L"Battle Royale bots: the bus flies at {:.0f} u/s heading ({:.2f}, {:.2f})", FlightSpeed, FlightDirection.X, FlightDirection.Y);
    AssignLandingSpots(Now);
#endif
}

void FCoreAthenaBotManager::AssignLandingSpots(float Now)
{
#if WITH_SERVER_CODE
    // Each spot's moment: when the bus passes closest to it, within the drop zone. It is reachable from there when a
    // skydive covers the distance (Core's 🟡: 700 m; a bot glides ~12 m/s through a 60-80 s fall).
    constexpr float MaxReach = 70000.0f;
    struct FCandidate
    {
        size_t Spot;
        float Time;
        float Reach;
    };
    std::vector<FCandidate> Candidates;
    const float Earliest = std::max(DropStartTime + 1.0f, Now + 0.5f);
    const float Latest = DropEndTime - 1.0f;
    for (size_t Index = 0; Index < LandingSpots.size() && Latest > Earliest; ++Index)
    {
        const FVector& Spot = LandingSpots[Index].Location;
        const float Along = (Spot.X - FlightSample.X) * FlightDirection.X + (Spot.Y - FlightSample.Y) * FlightDirection.Y;
        const float Time = std::clamp(FlightSampleTime + Along / FlightSpeed, Earliest, Latest);
        const float Travelled = (Time - FlightSampleTime) * FlightSpeed;
        const float JumpX = FlightSample.X + FlightDirection.X * Travelled;
        const float JumpY = FlightSample.Y + FlightDirection.Y * Travelled;
        const float Reach = std::sqrt((Spot.X - JumpX) * (Spot.X - JumpX) + (Spot.Y - JumpY) * (Spot.Y - JumpY));
        if (Reach <= MaxReach)
        {
            Candidates.push_back({ Index, Time, Reach });
        }
    }
    if (Candidates.empty())
    {
        Log::Info(L"Battle Royale bots: no landing spot in reach of the bus's path, the bots drop near it");
        return;
    }
    // Spread: a spot's loot is shared by the bots sent there (its weight falls with the square of their number), with a
    // little chance in it, and a far spot counts a little less. -CoreBotHotDrop sends everyone to the richest one.
    const bool bHotDrop = Configuration::Get().bBotHotDrop;
    int32_t Assigned = 0;
    // A squad lands together (duos and squads: the team): its first bot picks the spot and the jump, its squadmates follow
    // it out. A large team game's team is several squads (Teams of 20: five teams of 20 in squads of 4), which land apart:
    // each squad picks its own spot, a spot another squad of its team has taken scoring lower, and a bot without a squad
    // picks alone (CoreV2 #233: a whole team of 20 landed at Shifty Shafts).
    const bool bSquadsInTeams = HasSquadsInTeams();
    std::map<uint16_t, std::pair<const FCandidate*, float>> SquadSpots;
    std::map<std::pair<uint8_t, size_t>, int32_t> TeamSquadsAtSpot;
    for (const auto& Bot : PlayerBots)
    {
        if (!Bot->IsAlive() || Bot->GetState() != ECoreAthenaBotState::InAircraft)
        {
            continue;
        }
        // A bot on a player's team goes when its player does (UpdatePlayerTeamBots: FollowTeammateDrop): its spot and
        // jump are for a player who leaves it on the bus, at the drop zone's end.
        if (Bot->IsFollowingTeammateDrop())
        {
            ++Assigned;
            continue;
        }
        // The spots are timed along the first bus's path; a bot on another team's bus (50v50) drops near its own bus's.
        if (Bot->GetAircraft() != GetAircraft())
        {
            continue;
        }
        const bool bWaitsForPlayer = Bot->IsOnPlayersTeam() && HasAlivePlayerOnTeam(GetGameMode(), Bot->GetTeam());
        const bool bLandsWithSquad = PlaylistTeamSize > 1 && (!bSquadsInTeams || Bot->GetSquad() != 0);
        const uint16_t Squad = SquadKeyOf(Bot.get());
        if (const auto Chosen = bLandsWithSquad ? SquadSpots.find(Squad) : SquadSpots.end(); Chosen != SquadSpots.end())
        {
            const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Chosen->second.first->Spot];
            Bot->SetLandingSpot(Spot.Location, Spot.Radius, bWaitsForPlayer ? Latest : std::clamp(Chosen->second.second + RandomRange(0.1f, 0.6f), Earliest, Latest));
            ++Assigned;
            continue;
        }
        const FCandidate* Best = nullptr;
        float BestScore = -1.0f;
        for (const FCandidate& Candidate : Candidates)
        {
            const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Candidate.Spot];
            const float Shared = (1.0f + static_cast<float>(Spot.Bots)) * (1.0f + static_cast<float>(Spot.Bots));
            const auto TeamSquads = bSquadsInTeams ? TeamSquadsAtSpot.find({ Bot->GetTeam(), Candidate.Spot }) : TeamSquadsAtSpot.end();
            const float TeamShared = TeamSquads != TeamSquadsAtSpot.end() ? 1.0f + 3.0f * static_cast<float>(TeamSquads->second) : 1.0f;
            const float Score = bHotDrop ? Spot.Loot : Spot.Loot * RandomRange(0.7f, 1.3f) * (1.0f - 0.4f * Candidate.Reach / MaxReach) / (Shared * TeamShared);
            if (Score > BestScore)
            {
                BestScore = Score;
                Best = &Candidate;
            }
        }
        FCoreAthenaBotLandingSpot& Spot = LandingSpots[Best->Spot];
        ++Spot.Bots;
        const float JumpAt = std::clamp(Best->Time + RandomRange(-1.5f, 1.5f), Earliest, Latest);
        Bot->SetLandingSpot(Spot.Location, Spot.Radius, bWaitsForPlayer ? Latest : JumpAt);
        if (bLandsWithSquad)
        {
            SquadSpots[Squad] = { Best, JumpAt };
        }
        if (bSquadsInTeams)
        {
            ++TeamSquadsAtSpot[{ Bot->GetTeam(), Best->Spot }];
        }
        ++Assigned;
    }
    std::wstring Used;
    int32_t SpotsUsed = 0;
    for (const FCoreAthenaBotLandingSpot& Spot : LandingSpots)
    {
        if (Spot.Bots > 0)
        {
            Used += std::format(L"{}{} x{}", SpotsUsed++ ? L", " : L"", Spot.Name, Spot.Bots);
        }
    }
    Log::Info(L"Battle Royale bots: {} bots land at {} of {} reachable spots: {}", Assigned, SpotsUsed, Candidates.size(), Used);
#endif
}

void FCoreAthenaBotManager::GatherContainers()
{
#if WITH_SERVER_CODE
    // Battle Royale's chests and ammo boxes (the tiered containers a player searches; not the floor loot spawners).
    Containers.clear();
    int32_t Chests = 0;
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(GetGameMode(), ABuildingContainer::StaticClass()))
    {
        ABuildingContainer* Container = Cast<ABuildingContainer>(Actor);
        const std::wstring ClassName = Container ? reinterpret_cast<UObject*>(Container)->GetClass()->GetName().ToStdString() : std::wstring();
        const bool bChest = ClassName.find(L"Chest") != std::wstring::npos;
        const bool bAmmo = ClassName.find(L"Ammo") != std::wstring::npos;
        if (!Container || Container->IsPendingKill() || Container->bDestroyed || (!bChest && !bAmmo) || ClassName.find(L"FloorLoot") != std::wstring::npos)
        {
            continue;
        }
        Containers.push_back({ TWeakObjectPtr<ABuildingContainer>(Container), LocationOf(Container), bChest });
        Chests += bChest ? 1 : 0;
    }
    Log::Info(L"Battle Royale bots: {} chests and {} ammo boxes to search", Chests, Containers.size() - static_cast<size_t>(Chests));
#endif
}

void FCoreAthenaBotManager::OnContainerSearched(ABuildingContainer* Container)
{
    std::erase_if(Containers, [Container](const FCoreAthenaBotContainer& Entry) { return Entry.Container.Get() == Container; });
    // Its loot is on the floor now: the bots see it on the next tick.
    NextLootRefresh = 0.0f;
}

void FCoreAthenaBotManager::InitializeNavigation()
{
#if WITH_SERVER_CODE
    // The island: everything the bots go to (the loot and the containers), with room around it.
    FBox Island;
    for (const FCoreAthenaBotLoot& Entry : Loot)
    {
        Island += Entry.Location;
    }
    for (const FCoreAthenaBotContainer& Entry : Containers)
    {
        Island += Entry.Location;
    }
    if (!Island.IsValid)
    {
        return;
    }
    Island.Min = Island.Min - FVector(5000.0f, 5000.0f, 3000.0f);
    Island.Max = Island.Max + FVector(5000.0f, 5000.0f, 6000.0f);
    AFortGameModeAthena* Athena = GetGameMode();
    bHasNavigation = !Configuration::Get().bBotNoNavigation && UAthenaNavSystem::InitializeForBots(Athena ? Athena->GetWorld() : nullptr, Island);
#endif
}

void FCoreAthenaBotManager::UpdateCounts()
{
    // 21.40 counts bots in the game state's players and teams left (AddToAliveBots / RemoveFromAliveBots); 3.0's game
    // mode only counts its alive players, so the bots are added to its numbers.
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (!GameState)
    {
        return;
    }
    // Each team's count too, until the last bot that was in it is gone (the warmup's surplus bots leave).
    UpdateTeamPlayersLeft(Athena, GameState);
    if ((bAircraftStarted && BotsAtStart == 0) || (!bAircraftStarted && PlayerBots.empty()))
    {
        return;
    }
    // On the warmup island too: its player count (the game state's, which the game mode counts for its players) has
    // the bots in it, as 21.40's AddToAliveBots counts them.
    const int32_t PlayersLeft = Athena->AlivePlayers.Num() + GetAliveBotCount();
    if (!bAircraftStarted && AFortGameStateAthena::HasTotalPlayers() && GameState->TotalPlayers != PlayersLeft)
    {
        GameState->TotalPlayers = PlayersLeft;
    }
    const int32_t TeamsLeft = static_cast<int32_t>(AliveTeams(true).size());
    if (GameState->PlayersLeft != PlayersLeft || GameState->TeamsLeft != TeamsLeft)
    {
        GameState->PlayersLeft = PlayersLeft;
        GameState->TeamsLeft = TeamsLeft;
        GameState->ForceNetUpdate();
    }
}

void FCoreAthenaBotManager::UpdateTeamPlayersLeft(AFortGameModeAthena* Athena, AFortGameStateAthena* GameState)
{
    // The HUD's "1 : 0" in Teams of 20 with bots on every team (#242): AddToAlivePlayers (3.2 0x9AE8D0) counts each
    // player into the game mode's TeamAlivePlayers and, in a large team game, the game state's TeamXPlayersLeft (growing
    // it to the player's team), and RemoveFromAlivePlayers (0x9CA730) takes them out of both. The bots' controllers aren't
    // players' (AddToAlivePlayers writes AFortPlayerControllerAthena's members), so, as 21.40's AddToAliveBots and
    // RemoveFromAliveBots do, each team's count is its alive players' and its alive bots': the game's own count of the
    // team's players, and Core's of its bots. TeamAlivePlayers stays the players' (the game's own win check, which
    // ShouldHoldNativeWinCheck holds while bots are in, reads it). The HUD redraws its count when PlayersLeft changes
    // (3.2 0x2746410), which UpdateCounts sets after this; the map screen reads it each frame (0x2747150).
    // 50v50 too, not only Teams of 20: 3.5.1's HUD (0x2882780) shows "<Team>{YourTeamAlive}</>:<EnemyTeam>{EnemyTeamAlive}</>"
    // whenever the game state's bIsLargeTeamGame is set and TeamXPlayersLeft has two teams or more, in place of
    // PlayersLeft, and its AddToAlivePlayers (0x9DBFB0) counts a player into TeamXPlayersLeft on the same flag. 50v50's
    // two sides were left out here, so its HUD stayed at the players' "1 : 0" as the bots came in and went.
    if (!AFortGameStateAthena::HasTeamXPlayersLeft() || !AFortGameModeAthena::HasTeamAlivePlayers())
    {
        return;
    }
    TArray<int32_t>& TeamXPlayersLeft = GameState->GetMember_TeamXPlayersLeft();
    // Not a large team game's: the game state doesn't say it is one, and no player was counted into it (before 3.5 the
    // gate was the playlist's or the game type's, 3.2 0x9AE8D0).
    const bool bLargeTeamGame = AFortGameStateAthena::HasLargeTeamGame() && GameState->bIsLargeTeamGame;
    if (!bLargeTeamGame && TeamXPlayersLeft.Num() == 0)
    {
        return;
    }
    const uint8_t FirstTeam = EFortTeam::GetHumanPvP_Team1();
    std::map<int32_t, int32_t> Bots;
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive() && Bot->GetTeam() >= FirstTeam)
        {
            ++Bots[Bot->GetTeam() - FirstTeam];
        }
    }
    const int32_t Teams = std::max(PlaylistTeams, Bots.empty() ? 0 : Bots.rbegin()->first + 1);
    if (TeamXPlayersLeft.Num() < Teams)
    {
        TeamXPlayersLeft.AddZeroed(Teams - TeamXPlayersLeft.Num());
    }
    bool bChanged = false;
    for (int32_t Index = 0; Index < TeamXPlayersLeft.Num(); ++Index)
    {
        const auto Found = Bots.find(Index);
        const int32_t Left = Athena->GetTeamAlivePlayers(Index) + (Found != Bots.end() ? Found->second : 0);
        if (TeamXPlayersLeft[Index] != Left)
        {
            TeamXPlayersLeft[Index] = Left;
            bChanged = true;
        }
    }
    if (bChanged)
    {
        GameState->ForceNetUpdate();
    }
}

void FCoreAthenaBotManager::RemoveFromAliveBots(FCoreAthenaBotController* Bot, AFortPlayerStateAthena* KillerState)
{
#if WITH_SERVER_CODE
    // 21.40's AFortGameModeAthena::RemoveFromAliveBots. The team's place is the teams left when its last player is out
    // (counted before this bot is).
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t TeamsBefore = static_cast<int32_t>(AliveTeams(true).size());
    AFortPlayerStateAthena* State = Bot->GetPlayerState();
    FCoreAthenaBotController* KillerBot = FindBot(KillerState);
    if (KillerBot && KillerBot != Bot)
    {
        KillerBot->AddKill();
        // A bot on a player's team: the elimination is its team's, as a player teammate's is.
        if (KillerBot->IsOnPlayersTeam())
        {
            AddTeamKillScore(Athena, KillerBot->GetTeam());
        }
        // The tags a player's kill event for this bot would carry, once a match (what the kill challenges match).
        if (!bLoggedKillTags)
        {
            bLoggedKillTags = true;
            FGameplayTagContainer TargetTags;
            FGameplayTagContainer SourceTags;
            AddKillEventTags(reinterpret_cast<UObject*>(Bot->GetPawn()), reinterpret_cast<AController*>(KillerBot->GetController()), TargetTags, SourceTags);
            const auto Names = [](const FGameplayTagContainer& Tags)
            {
                std::wstring Text;
                for (const FGameplayTag& Tag : Tags.GameplayTags)
                {
                    Text += (Text.empty() ? L"" : L", ") + Tag.TagName.ToStdString();
                }
                return Text.empty() ? std::wstring(L"none") : Text;
            };
            Log::Info(L"Battle Royale bots: a kill event for {} has the target tags [{}] and the killer's weapon tags [{}]", Bot->GetBotName(), Names(TargetTags), Names(SourceTags));
        }
    }
    AFortPlayerControllerAthena* KillerPlayer = KillerState && !KillerBot ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(KillerState->GetOwner())) : nullptr;
    if (KillerPlayer)
    {
        const auto Found = std::find_if(BotKills.begin(), BotKills.end(), [KillerState](const auto& Entry) { return Entry.first.Get() == KillerState; });
        if (Found != BotKills.end())
        {
            ++Found->second;
        }
        else
        {
            BotKills.emplace_back(TWeakObjectPtr<AFortPlayerStateAthena>(KillerState), 1);
        }
        KillerState->ClientReportKill(State);
        // The player's elimination count now (the HUD's, OnRep_Kills; the match results set it again from the same
        // counts once a second), and their team's.
        if (AFortPlayerStateAthena::CountsKills())
        {
            KillerState->Kills = KillerState->Kills + 1;
            KillerState->ForceNetUpdate();
        }
        else if (AFortPlayerStateAthena::HasKillScore())
        {
            KillerState->SetKillScore(KillerState->GetKills() + 1);
        }
        AddTeamKillScore(Athena, static_cast<uint8_t>(KillerState->TeamIndex));

        // Quests: the game's own kill events for this pawn went through UFortQuestManager already, with the bot's tags
        // added (SendStatEventWithTagsHK). AFortGameModeZone::Killed (3.1 0xB5E890, 4.0 0xC1B530) sends each damage
        // contributor's KillContribution, but the killer's Kill only when GetTeamAttitude says the killer and the dead
        // pawn's controller are hostile (4.0 0xC8F200: each one's IFortTeamActorInterface team, or its Instigator's).
        // A bot's controller is a stock AAIController, which has no team and no Instigator, so the attitude is neutral
        // and the game sends no Kill for a bot: the KillContribution it did send isn't the elimination ("Eliminate
        // opponents in Flush Factory", killingblow_athena_player_*). With the setting on, Core sends the events the game
        // didn't, as Killed does: KillContribution, then Kill, with the tags its Kill has: the dead pawn's and the
        // location tags of where it died for the target, the killer's, their weapon's and the location tags of where
        // they are for the source (AFortGameMode::GetPoiTagsFor, 4.0 0xC1BBE7 and 0xC1BBFA).
        UObject* Target = reinterpret_cast<UObject*>(Bot->GetPawn());
        const bool bNativeKill = HasNativeKillEvent(Target, EFortQuestObjectiveStatEvent::GetKill());
        const bool bNativeContribution = HasNativeKillEvent(Target, EFortQuestObjectiveStatEvent::GetKillContribution());
        UFortRegisteredPlayerInfo* PlayerInfo = static_cast<UFortRegisteredPlayerInfo*>(KillerPlayer->MyPlayerInfo);
        UFortQuestManager* QuestManager = PlayerInfo ? PlayerInfo->GetAthenaQuestManager() : nullptr;
        std::wstring Location;
        if (Settings.bEliminationsCountForQuests && !bNativeKill && QuestManager)
        {
            FGameplayTagContainer SourceTags;
            FGameplayTagContainer ContextTags;
            QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
            FGameplayTagContainer TargetTags;
            AddKillEventTags(Target, reinterpret_cast<AController*>(KillerPlayer), TargetTags, SourceTags);
            if (!bNativeContribution)
            {
                QuestManager->SendStatEvent(EFortQuestObjectiveStatEvent::GetKillContribution(), Target, TargetTags, SourceTags, ContextTags, 1, false);
            }
            // Killed (4.2 0xC167B0) builds the Kill's source from the killer's tags and appends the tags it was called
            // with (0xC16DEB), the dead pawn's DeathTags: the damage tags of the hit that killed it. A trap's hit carries
            // its TrapData's tags there (trap.hostile, which killingblow_athena_player_with_trap asks for in the source,
            // CoreV2 #315); the killer's held weapon says nothing about a trap.
            AFortPawn* TargetPawn = Cast<AFortPawn>(Target);
            if (TargetPawn && AFortPawn::HasDeathTags())
            {
                SourceTags.AppendTags(TargetPawn->DeathTags);
            }
            if (Athena)
            {
                FGameplayTagContainer KillerLocation;
                FGameplayTagContainer TargetLocation;
                Athena->GetPoiTagsFor(reinterpret_cast<AActor*>(static_cast<APawn*>(KillerPlayer->Pawn)), KillerLocation);
                Athena->GetPoiTagsFor(reinterpret_cast<AActor*>(Target), TargetLocation);
                SourceTags.AppendTags(KillerLocation);
                TargetTags.AppendTags(TargetLocation);
                for (const FGameplayTag& Tag : TargetLocation.GameplayTags)
                {
                    Location += (Location.empty() ? L"" : L", ") + Tag.TagName.ToStdString();
                }
            }
            QuestManager->SendStatEvent(EFortQuestObjectiveStatEvent::GetKill(), Target, TargetTags, SourceTags, ContextTags, 1, false);
        }
        Log::Info(L"Battle Royale bots: {} eliminated {} ({} bots now; quests: {}{})", KillerState->GetPlayerName().ToStdString(), Bot->GetBotName(), BotKillsOf(KillerState),
            !Settings.bEliminationsCountForQuests ? L"off" : bNativeKill ? L"the game's event" : !QuestManager ? L"no quest manager"
                : bNativeContribution ? L"the game's KillContribution, Core's Kill" : L"Core's events",
            Location.empty() ? std::wstring() : std::format(L", at {}", Location));
    }

    // A player's team is out once its players are out too (Core's death handler left its place to its bots then); in a
    // large team game any of the team's players.
    const bool bPlayersTeam = Bot->IsOnPlayersTeam() || (HasSquadsInTeams() && HumanTeams().contains(Bot->GetTeam()));
    const bool bTeamOut = std::none_of(PlayerBots.begin(), PlayerBots.end(), [Bot](const auto& Other) { return Other.get() != Bot && Other->IsAlive() && Other->GetTeam() == Bot->GetTeam(); })
        && !(bPlayersTeam && HasAlivePlayerOnTeam(Athena, Bot->GetTeam()));
    Bot->OnPossesedPawnDied(KillerState);
    // A playlist that brings its players back (Playground, CoreV2 #344): the bot is still in the match, as a respawning
    // player stays among the alive players; no place, no outlive events, the counts stay.
    if (Bot->IsRespawnPending())
    {
        return;
    }
    if (bTeamOut && bAircraftStarted)
    {
        for (const auto& Member : PlayerBots)
        {
            AFortPlayerStateAthena* MemberState = Member->GetTeam() == Bot->GetTeam() ? Member->GetPlayerState() : nullptr;
            if (MemberState)
            {
                MemberState->Place = TeamsBefore;
                MemberState->ForceNetUpdate();
            }
        }
        if (bPlayersTeam)
        {
            SetPlayersTeamPlace(Bot->GetTeam(), TeamsBefore);
        }
    }
    Log::Info(L"Battle Royale bots: {} is out{} ({} bots left)", Bot->GetBotName(), bTeamOut && bAircraftStarted ? std::format(L", place {}", TeamsBefore) : std::wstring(), GetAliveBotCount());
    // Every player still in the match outlived the bot ("Outlive opponents", AthenaOutlive), as they outlive a player who
    // is out, with the setting on.
    if (Athena && bAircraftStarted && Settings.bEliminationsCountForQuests)
    {
        Athena->SendOutliveStatEvents(reinterpret_cast<AFortPawn*>(Bot->GetPawn()));
    }
    if (Athena)
    {
        UpdateCounts();
        CheckWinConditionLastManStanding();
    }
#endif
}

void FCoreAthenaBotManager::ApplyBotDamage(FCoreAthenaBotController* Attacker, AFortPawn* Victim, float Damage, UFortWeaponItemDefinition* Weapon)
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!Attacker || !Victim || !World || Victim->bIsDying || Victim->IsPendingKill() || Damage <= 0.0f)
    {
        return;
    }
    const float Now = World->GetTimeSeconds();
    NoteBotHit(Attacker, Victim);
    // A pawn that can't be damaged (everyone on the warmup island: AFortGameModeAthena's warmup restart turns
    // bCanBeDamaged off; a player in god mode) takes nothing, as the game's own damage leaves it.
    if (IsInvulnerable(Victim))
    {
        return;
    }

    const float Shield = Victim->GetShield();
    const float ToShield = std::min(Shield, Damage);
    if (ToShield > 0.0f)
    {
        Victim->SetShield(Shield - ToShield);
    }
    const float Health = Victim->GetHealth() - (Damage - ToShield);
    if (Health > 0.5f)
    {
        Victim->SetHealth(Health);
        return;
    }
    // Squads: down, not out, while a teammate stands (the next hit on a downed bot finishes it).
    if (FCoreAthenaBotController* VictimBot = FindBot(reinterpret_cast<const UObject*>(Victim)); VictimBot && VictimBot->CanGoDBNO())
    {
        Log::Info(L"Battle Royale bots: {} knocks {} down with {}", Attacker->GetBotName(), VictimBot->GetBotName(), Weapon ? Weapon->GetName().ToStdString() : std::wstring(L"the harvesting tool"));
        VictimBot->EnterDBNO(Attacker->GetPlayerState(), Now);
        return;
    }
    // The elimination: the death's damage tag is the weapon's own (the kill feed's weapon), the killer the bot.
    FGameplayTag DeathReason{ FName(L"Gameplay.Damage.Physical", FNAME_Find) };
    if (Weapon)
    {
        const FGameplayTagContainer& Tags = Weapon->GameplayTags;
        for (const FGameplayTag& Tag : Tags.GameplayTags)
        {
            DeathReason = Tag;
            break;
        }
    }
    Log::Info(L"Battle Royale bots: {} eliminates {} with {}", Attacker->GetBotName(), Victim->GetName().ToStdString(), Weapon ? Weapon->GetName().ToStdString() : std::wstring(L"the harvesting tool"));
    Victim->SetHealth(1.0f);
    Victim->ForceKill(DeathReason, reinterpret_cast<AController*>(Attacker->GetController()), reinterpret_cast<AActor*>(Attacker->GetPawn()));
#endif
}

bool FCoreAthenaBotManager::IsInvulnerable(AFortPawn* Victim)
{
    // The pawn's damage check (3.3 0xE6AB30) gives a hit no damage while bCanBeDamaged is off or bIsInvulnerable (the
    // god cheat's) is on; the unlimitedhealth cheat's bUnlimitedHealth is read by the same damage path (1.9 0x7AE300).
    // Core's own bot damage (a hit the game's damage didn't take, or a team playlist's bot) asks the same (CoreV2 #252:
    // "cheat god" didn't stop the bots' hits).
    if (!Victim || !Victim->bCanBeDamaged)
    {
        return true;
    }
    if (AFortPawn::HasInvulnerable() && Victim->bIsInvulnerable)
    {
        return true;
    }
    APlayerController* Player = Cast<APlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Victim->Controller)));
    UFortCheatManager* Cheats = Player ? Cast<UFortCheatManager>(static_cast<UObject*>(Player->CheatManager)) : nullptr;
    return Cheats && UFortCheatManager::HasUnlimitedHealthFlag() && Cheats->bUnlimitedHealth;
}

bool FCoreAthenaBotManager::UsesGameDamage(const AFortPawn* Victim) const
{
    return Victim && (!IsBotPawn(reinterpret_cast<const UObject*>(Victim)) || GetTeamSize() <= 1);
}

void FCoreAthenaBotManager::NoteBotHit(FCoreAthenaBotController* Attacker, AFortPawn* Victim)
{
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!Attacker || !Victim || !World)
    {
        return;
    }
    const float Now = World->GetTimeSeconds();
    std::erase_if(BotHits, [Now, Victim](const FBotHit& Hit) { return !Hit.Victim.Get() || Hit.Victim.Get() == Victim || Now - Hit.Time > 15.0f; });
    BotHits.push_back({ TWeakObjectPtr<AFortPawn>(Victim), TWeakObjectPtr<AFortPlayerStateAthena>(Attacker->GetPlayerState()), Now });
}

std::vector<FCoreAthenaBotController*> FCoreAthenaBotManager::TeammatesOf(const FCoreAthenaBotController* Bot) const
{
    // In a large team game a bot's squad: the rest of its team of 20 isn't who it lands, regroups and revives with.
    std::vector<FCoreAthenaBotController*> Teammates;
    for (const auto& Other : PlayerBots)
    {
        if (Other.get() != Bot && Other->IsAlive() && SquadKeyOf(Other.get()) == SquadKeyOf(Bot))
        {
            Teammates.push_back(Other.get());
        }
    }
    return Teammates;
}

std::vector<AFortPlayerPawnAthena*> FCoreAthenaBotManager::PlayerTeammatesOf(const FCoreAthenaBotController* Bot) const
{
    std::vector<AFortPlayerPawnAthena*> Teammates;
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena || !Bot->IsOnPlayersTeam())
    {
        return Teammates;
    }
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Player));
        AFortPlayerPawnAthena* Pawn = State && static_cast<uint8_t>(State->TeamIndex) == Bot->GetTeam() && (!bSquads || static_cast<uint8_t>(State->SquadId) == Bot->GetSquad())
            ? Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Player->GetPawn())) : nullptr;
        if (Pawn && !Pawn->bIsDying && !Pawn->IsPendingKill())
        {
            Teammates.push_back(Pawn);
        }
    }
    return Teammates;
}

bool FCoreAthenaBotManager::GetSquadMapMarker(const FCoreAthenaBotController* Bot, const FVector& From, FVector& OutLocation, std::wstring& OutPlayer) const
{
    // A player's marker is their player state's MapIndicatorPos (3.3: ServerPlaceMapCursor sets it, ServerRemoveMapCursor
    // zeroes it), which their squad's map and compass show: the squad's players in the match, as PlayerTeammatesOf has them.
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena || !Bot->IsOnPlayersTeam() || !AFortPlayerStateAthena::HasMapIndicatorPos())
    {
        return false;
    }
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    float Best = -1.0f;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Alive[Index]));
        if (!State || static_cast<uint8_t>(State->TeamIndex) != Bot->GetTeam() || (bSquads && static_cast<uint8_t>(State->SquadId) != Bot->GetSquad()))
        {
            continue;
        }
        const FVector2D Marker = State->MapIndicatorPos;
        if (Marker.IsZero())
        {
            continue;
        }
        const FVector Location(Marker.X, Marker.Y, From.Z);
        const float Distance = std::sqrt((Location.X - From.X) * (Location.X - From.X) + (Location.Y - From.Y) * (Location.Y - From.Y));
        if (Best < 0.0f || Distance < Best)
        {
            Best = Distance;
            OutLocation = Location;
            OutPlayer = State->GetPlayerName().ToStdString();
        }
    }
    return Best >= 0.0f;
}

AFortPlayerStateAthena* FCoreAthenaBotManager::FindPlayerToSpectate(const FCoreAthenaBotController* Bot, AFortPlayerStateAthena* KillerState) const
{
    // SpectateOnDeath (3.3 0xA31650) watches the one who eliminated them (PlayerToSpectateOnDeath) while alive, else a
    // squadmate or anyone alive. A bot only ever watches its eliminator, and only a player (CoreV2 #255: with a
    // teammate or anyone alive as the fallback, the match's dead bots all piled onto the last player standing): a player
    // still in the match (the game mode's AlivePlayers) with a pawn alive. KillerState is only compared, never read (a
    // player who left leaves it stale).
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena || !KillerState || FindBot(reinterpret_cast<const UObject*>(KillerState)) || KillerState == Bot->GetPlayerState())
    {
        return nullptr;
    }
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Player));
        if (!State || State != KillerState)
        {
            continue;
        }
        AFortPawn* Pawn = Cast<AFortPawn>(reinterpret_cast<UObject*>(Player->GetPawn()));
        return Pawn && !Pawn->IsDead() && !Pawn->IsPendingKill() ? State : nullptr;
    }
    return nullptr;
}

bool FCoreAthenaBotManager::HasStandingTeammate(const FCoreAthenaBotController* Bot) const
{
    // The game's choice between down and out (AFortPlayerPawnAthena's, 3.3 0xA31C20, after AFortPawn's health ran out,
    // 0xE5BAE0): in squad play, another Athena pawn on the pawn's team, the whole team in a large team game and not its
    // squad, neither DBNO nor dead (AFortPawn::IsDead), skydiving or on the ground.
    const auto Standing = [](AFortPawn* Pawn) { return Pawn && !Pawn->bIsDBNO && !Pawn->IsDead(); };
    for (const auto& Other : PlayerBots)
    {
        if (Other.get() != Bot && Other->GetTeam() == Bot->GetTeam() && Standing(reinterpret_cast<AFortPawn*>(Other->GetPawn())))
        {
            return true;
        }
    }
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena)
    {
        return false;
    }
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPlayerStateAthena* State = Player ? AthenaStateOf(reinterpret_cast<AController*>(Player)) : nullptr;
        if (State && static_cast<uint8_t>(State->TeamIndex) == Bot->GetTeam() && Standing(Cast<AFortPawn>(reinterpret_cast<UObject*>(Player->GetPawn()))))
        {
            return true;
        }
    }
    return false;
}

bool FCoreAthenaBotManager::HasPlayersTeamBotsLeft(uint8_t Team) const
{
    const bool bSquads = HasSquadsInTeams();
    return std::any_of(PlayerBots.begin(), PlayerBots.end(), [Team, bSquads](const auto& Bot) { return Bot->IsAlive() && (bSquads || Bot->IsOnPlayersTeam()) && Bot->GetTeam() == Team; });
}

void FCoreAthenaBotManager::SetPlayersTeamPlace(uint8_t Team, int32_t Place)
{
#if WITH_SERVER_CODE
    // What Core's death handler does for the team's last player out (AFortPlayerControllerAthena::OnPawnDied: the game's
    // SetTeamPlace on them): here its bots were the last, so on one of its players, any still connected; every player
    // state of the team takes the place too (one who left has no controller).
    AFortGameStateAthena* GameState = AthenaGameStateOf(GetGameMode());
    if (!GameState)
    {
        return;
    }
    AFortPlayerControllerAthena* Player = nullptr;
    for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (!AthenaState || static_cast<uint8_t>(AthenaState->TeamIndex) != Team || FindBot(reinterpret_cast<const UObject*>(AthenaState)))
        {
            continue;
        }
        AthenaState->Place = Place;
        AthenaState->ForceNetUpdate();
        Player = Player ? Player : Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(AthenaState->GetOwner()));
    }
    if (Player && Natives::AFortPlayerControllerAthena_SetTeamPlace)
    {
        Natives::AFortPlayerControllerAthena_SetTeamPlace(Player, Place);
    }
    Log::Info(L"Battle Royale bots: {} (a player's team) is out with its bots, place {}", EFortTeam{ Team }.ToString(), Place);
#endif
}

void FCoreAthenaBotManager::UpdatePlayerTeamBots(float Now)
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (!GameState || std::none_of(PlayerBots.begin(), PlayerBots.end(), [](const auto& Bot) { return Bot->IsAlive() && Bot->IsOnPlayersTeam(); }))
    {
        return;
    }
    // The team's squad (AFortPlayerStateAthena::SquadId: the squad bar lists a squad's players, OnRep_SquadId), once one of
    // its players has one, checked every second. In a large team game the bot has its squad from the start (its player's).
    if (AFortPlayerStateAthena::HasSquadId() && !HasSquadsInTeams() && Now >= NextPlayerTeamSync)
    {
        NextPlayerTeamSync = Now + 1.0f;
        std::map<uint8_t, uint8_t> Squads;
        for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            if (AthenaState && static_cast<uint8_t>(AthenaState->SquadId) != 0 && !FindBot(reinterpret_cast<const UObject*>(AthenaState)))
            {
                Squads.try_emplace(static_cast<uint8_t>(AthenaState->TeamIndex), static_cast<uint8_t>(AthenaState->SquadId));
            }
        }
        for (const auto& Bot : PlayerBots)
        {
            AFortPlayerStateAthena* State = Bot->IsAlive() && Bot->IsOnPlayersTeam() ? Bot->GetPlayerState() : nullptr;
            const auto Squad = State ? Squads.find(Bot->GetTeam()) : Squads.end();
            if (Squad != Squads.end() && static_cast<uint8_t>(State->SquadId) != Squad->second)
            {
                State->SquadId = Squad->second;
                State->ForceNetUpdate();
                Log::Info(L"Battle Royale bots: {} is in squad {} with its player teammates", Bot->GetBotName(), Squad->second);
            }
        }
    }
    // After the bus: a bot on a player's team goes when its player does, and falls towards them. A player in the aircraft
    // has no pawn (StartAircraftPhase takes the warmup's); the one they have once the drop zone opens is their skydive.
    if (!bAircraftStarted || Now < DropStartTime)
    {
        return;
    }
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    std::map<uint16_t, AFortPlayerPawnAthena*> Jumped;
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Player));
        AFortPlayerPawnAthena* Pawn = State ? Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Player->GetPawn())) : nullptr;
        if (Pawn && !Pawn->bIsDying)
        {
            Jumped.try_emplace(SquadKey(static_cast<uint8_t>(State->TeamIndex), bSquads ? static_cast<uint8_t>(State->SquadId) : 0), Pawn);
        }
    }
    for (const auto& Bot : PlayerBots)
    {
        const bool bFalling = Bot->GetState() == ECoreAthenaBotState::InAircraft || Bot->GetState() == ECoreAthenaBotState::Skydiving;
        const auto Player = Bot->IsOnPlayersTeam() && bFalling ? Jumped.find(SquadKeyOf(Bot.get())) : Jumped.end();
        if (Player != Jumped.end())
        {
            AFortPlayerPawnAthena* Pawn = Player->second;
            Bot->FollowTeammateDrop(LocationOf(reinterpret_cast<const AActor*>(Pawn)), !Pawn->bIsSkydiving && !Pawn->bIsParachuteOpen, Now);
        }
    }
#endif
}

void FCoreAthenaBotManager::ReplicateHealthAndShield()
{
#if WITH_SERVER_CODE
    // As the game's own pass does for a player controller's pawn (3.5.1 0x9F7440): the pawn's GetHealth, GetMaxHealth,
    // GetShield and GetMaxShield onto the player state's replicated CurrentHealth, MaxHealth, CurrentShield and
    // MaxShield, then the team's copy of them (AFortTeamPrivateInfo::SetHealthAndShield). A client's squad bar reads a
    // squadmate's health from that copy, not from the player state (3.5.1 0xF703D0 reads the player state only for the
    // local player): with the player state alone, a bot teammate's bar stayed at the game pass's 0 (CoreV2 #264, #275).
    if (!AFortPlayerStateZone::HasHealthAndShield() || !GetGameMode())
    {
        return;
    }
    for (const auto& Bot : PlayerBots)
    {
        AFortPlayerPawnAthena* Pawn = Bot->IsAlive() && Bot->IsOnPlayersTeam() ? Bot->GetPawn() : nullptr;
        AFortPlayerStateAthena* State = Pawn && !Pawn->bIsDying ? Bot->GetPlayerState() : nullptr;
        if (!State)
        {
            continue;
        }
        State->CurrentHealth = Pawn->GetHealth();
        State->MaxHealth = Pawn->GetMaxHealth();
        State->CurrentShield = Pawn->GetShield();
        State->MaxShield = Pawn->GetMaxShield();
        if (AFortTeamPrivateInfo* TeamPrivate = AFortPlayerState::HasPlayerTeamPrivate() ? static_cast<AFortTeamPrivateInfo*>(State->PlayerTeamPrivate) : nullptr)
        {
            TeamPrivate->SetHealthAndShield(State);
        }
    }
#endif
}

AFortPlayerStateAthena* FCoreAthenaBotManager::LastBotAttackerOf(const AFortPawn* Pawn) const
{
    for (auto It = BotHits.rbegin(); It != BotHits.rend(); ++It)
    {
        if (It->Victim.Get() == Pawn)
        {
            return It->Attacker.Get();
        }
    }
    return nullptr;
}

void FCoreAthenaBotManager::CheckWinConditionLastManStanding()
{
#if WITH_SERVER_CODE
    // 21.40's AFortGameModeAthena::CheckWinConditionLastManStanding, for a match with bots in it: the game's own check
    // only sees its alive players. A team is left alone: its players win (the game's StartEndGamePhase), or, a bots'
    // team, it takes place 1 and the match ends. Bots never keep a match going: once its players are all out, it ends.
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (bMatchEnded || !bAircraftStarted || BotsAtStart == 0 || !GameState || GameState->GamePhase >= EAthenaGamePhase::GetEndGame()
        || !Natives::AFortGameModeAthena_StartEndGamePhase)
    {
        return;
    }
    const std::set<uint8_t> Teams = AliveTeams(true);
    // The game's check (4.5 0xA5FCB0) runs only as someone is taken out (RemoveFromAlivePlayers, which hands it the
    // count from before): its solo win needs that count above 1, its team win TeamsLeft brought down to 1. A match that
    // only ever had one team (Playground's "1 teams of 4", a player and their fill bots) never has a last team standing:
    // declaring one as the bus left ended it with a win the game never gives.
    MostTeamsAlive = std::max(MostTeamsAlive, static_cast<int32_t>(Teams.size()));
    if (MostTeamsAlive <= 1)
    {
        return;
    }
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    const int32_t HumansLeft = Alive.Num();
    if (Teams.size() <= 1)
    {
        bMatchEnded = true;
        if (HumansLeft > 0)
        {
            AFortPlayerControllerAthena* Winner = Alive[0];
            Log::Info(L"Battle Royale bots: {} is the last team standing, it wins", Winner ? Winner->GetName().ToStdString() : std::wstring());
            // Its bots (a player's team filled with bots) win with it.
            for (const auto& Bot : PlayerBots)
            {
                AFortPlayerStateAthena* State = Bot->IsAlive() && Teams.contains(Bot->GetTeam()) ? Bot->GetPlayerState() : nullptr;
                if (State)
                {
                    State->Place = 1;
                    State->ForceNetUpdate();
                }
            }
            Natives::AFortGameModeAthena_StartEndGamePhase(Athena, Winner, nullptr, nullptr, EDeathCause::GetUnspecified());
            return;
        }
        bool bPlayersTeamWon = false;
        for (const auto& Bot : PlayerBots)
        {
            if (Bot->IsAlive())
            {
                if (AFortPlayerStateAthena* State = Bot->GetPlayerState())
                {
                    State->Place = 1;
                    State->ForceNetUpdate();
                }
                bPlayersTeamWon = bPlayersTeamWon || Bot->IsOnPlayersTeam() || (HasSquadsInTeams() && HumanTeams().contains(Bot->GetTeam()));
                Log::Info(L"Battle Royale bots: {} wins the match ({})", Bot->GetBotName(), Bot->IsOnPlayersTeam() ? L"for its players' team" : L"a bots' team");
            }
        }
        // The players whose bots won: their team's place is 1.
        if (bPlayersTeamWon && !Teams.empty())
        {
            SetPlayersTeamPlace(*Teams.begin(), 1);
        }
        Natives::AFortGameModeAthena_StartEndGamePhase(Athena, nullptr, nullptr, nullptr, EDeathCause::GetUnspecified());
        return;
    }
    // Every player out: the match goes on while one of them is still connected, watching it (CoreV2 #255: ending it at
    // the last player's elimination stopped the bots, so those spectating saw them freeze), until a team is left; once
    // none is, it ends.
    if (HumansAtStart > 0 && HumansLeft == 0 && !HasPlayersConnected())
    {
        bMatchEnded = true;
        Log::Info(L"Battle Royale bots: every player is out and gone with {} bots left, so the match ends", GetAliveBotCount());
        // A players' team whose bots are still in shares the place of the teams left.
        for (const uint8_t Team : Teams)
        {
            if (HasPlayersTeamBotsLeft(Team) && (!HasSquadsInTeams() || HumanTeams().contains(Team)))
            {
                SetPlayersTeamPlace(Team, static_cast<int32_t>(Teams.size()));
            }
        }
        Natives::AFortGameModeAthena_StartEndGamePhase(Athena, nullptr, nullptr, nullptr, EDeathCause::GetUnspecified());
    }
#endif
}

void FCoreAthenaBotManager::RefreshEnemies(float Now)
{
    EnemyPawns.clear();
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena)
    {
        return;
    }
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive() && Bot->GetPawn())
        {
            EnemyPawns.emplace_back(reinterpret_cast<AFortPawn*>(Bot->GetPawn()));
        }
    }
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPawn* Pawn = Player ? Cast<AFortPawn>(reinterpret_cast<UObject*>(Player->GetPawn())) : nullptr;
        if (Pawn)
        {
            EnemyPawns.emplace_back(Pawn);
        }
    }
    // DBNO timing (CoreV2 #233): note when each downed pawn (player or bot) was knocked and forget it once it is revived,
    // eliminated or gone, so a bot can give a knocked enemy a few seconds before thirsting it.
    std::erase_if(DBNODowned, [](const FDBNODown& Down) { AFortPawn* Pawn = Down.Pawn.Get(); return !Pawn || !Pawn->bIsDBNO || Pawn->IsPendingKill(); });
    for (const TWeakObjectPtr<AFortPawn>& Weak : EnemyPawns)
    {
        AFortPawn* Pawn = Weak.Get();
        if (Pawn && Pawn->bIsDBNO && std::none_of(DBNODowned.begin(), DBNODowned.end(), [Pawn](const FDBNODown& Down) { return Down.Pawn.Get() == Pawn; }))
        {
            DBNODowned.push_back({ TWeakObjectPtr<AFortPawn>(Pawn), Now });
        }
    }
}

float FCoreAthenaBotManager::DBNODownedElapsed(const AFortPawn* Pawn, float Now) const
{
    for (const FDBNODown& Down : DBNODowned)
    {
        if (Down.Pawn.Get() == Pawn)
        {
            return std::max(Now - Down.DownedAt, 0.0f);
        }
    }
    return FLT_MAX;
}

float FCoreAthenaBotManager::NearestStandingSquadmateDistance(const AFortPawn* Downed) const
{
    AFortPlayerStateAthena* DownedState = Downed ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Downed->PlayerState))) : nullptr;
    if (!DownedState)
    {
        return FLT_MAX;
    }
    // A teammate who could revive: the same team, and the same squad in a large team game (Teams of 20's squads revive
    // their own); neither DBNO nor dead (AFortPawn::IsDead), as HasStandingTeammate counts standing.
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    const uint8_t Team = static_cast<uint8_t>(DownedState->TeamIndex);
    const uint8_t Squad = bSquads ? static_cast<uint8_t>(DownedState->SquadId) : 0;
    const FVector DownedLocation = LocationOf(reinterpret_cast<const AActor*>(Downed));
    float Nearest = FLT_MAX;
    for (const TWeakObjectPtr<AFortPawn>& Weak : EnemyPawns)
    {
        AFortPawn* Pawn = Weak.Get();
        if (!Pawn || Pawn == Downed || Pawn->bIsDBNO || Pawn->IsDead() || Pawn->IsPendingKill())
        {
            continue;
        }
        AFortPlayerStateAthena* State = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Pawn->PlayerState)));
        if (!State || static_cast<uint8_t>(State->TeamIndex) != Team || (bSquads && static_cast<uint8_t>(State->SquadId) != Squad))
        {
            continue;
        }
        Nearest = std::min(Nearest, (LocationOf(reinterpret_cast<const AActor*>(Pawn)) - DownedLocation).Size());
    }
    return Nearest;
}

int32_t FCoreAthenaBotManager::PlayersLeft() const
{
    AFortGameModeAthena* Athena = GetGameMode();
    return (Athena ? Athena->AlivePlayers.Num() : 0) + GetAliveBotCount();
}

void FCoreAthenaBotManager::RefreshLoot(float Now)
{
#if WITH_SERVER_CODE
    Loot.clear();
    AFortGameModeAthena* Athena = GetGameMode();
    UClass* RangedClass = RangedWeaponClass();
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(Athena, AFortPickup::StaticClass()))
    {
        AFortPickup* Pickup = Cast<AFortPickup>(Actor);
        if (!Pickup || Pickup->bPickedUp || Pickup->IsPendingKill())
        {
            continue;
        }
        FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
        UFortItemDefinition* Definition = Entry.ItemDefinition;
        UFortWeaponItemDefinition* Weapon = Cast<UFortWeaponItemDefinition>(Definition);
        // A gun: a ranged weapon whose ammo is an ammo item (consumables, such as the shield potion, are ranged weapons
        // that are their own ammo; grenades have none).
        bool bWeapon = false;
        if (Weapon && RangedClass && reinterpret_cast<UObject*>(Weapon)->IsA(RangedClass))
        {
            auto [Found, bNew] = GunDefinitions.try_emplace(Weapon, false);
            if (bNew)
            {
                UFortWorldItemDefinition* AmmoDefinition = static_cast<TAssetPtr<UFortWorldItemDefinition>&>(Weapon->AmmoData).LoadSynchronous();
                Found->second = AmmoDefinition && Cast<UFortAmmoItemDefinition>(AmmoDefinition) != nullptr;
            }
            bWeapon = Found->second;
        }
        const bool bAmmo = Cast<UFortAmmoItemDefinition>(Definition) != nullptr;
        // A bandage, medkit, shield potion or Chug Jug (21.40's _Loot takes healing items too).
        const bool bConsumable = !bWeapon && !bAmmo && Weapon && FCoreAthenaBotController::GetConsumableInfo(Definition) != nullptr;
        // Wood, stone and metal on the floor, which bots take for building (CoreV2 #252).
        const bool bMaterial = !Weapon && FCoreAthenaBotController::MaterialKindOf(Definition) >= 0;
        if (bWeapon || bAmmo || bConsumable || bMaterial)
        {
            Loot.push_back({ TWeakObjectPtr<AFortPickup>(Pickup), LocationOf(Pickup), bWeapon, bConsumable });
        }
    }
#endif
}

void FCoreAthenaBotManager::LogSummary(float Now)
{
    if (Now < NextSummaryLog || PlayerBots.empty())
    {
        return;
    }
    NextSummaryLog = Now + 15.0f;
    int32_t ByState[5] = {};
    int32_t OnNavMesh = 0;
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    for (const auto& Bot : PlayerBots)
    {
        ++ByState[static_cast<int32_t>(Bot->GetState())];
        if (bHasNavigation && Bot->GetState() == ECoreAthenaBotState::Landed && Bot->GetPawn() && UAthenaNavSystem::IsOnNavMesh(World, LocationOf(reinterpret_cast<const AActor*>(Bot->GetPawn()))))
        {
            ++OnNavMesh;
        }
    }
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    Log::Info(L"Battle Royale bots: {} in the warmup, {} in the aircraft, {} skydiving, {} landed ({} on the navmesh), {} out; {} loot pickups seen; players left {}, teams left {}; "
        L"frame {:.1f} ms average, {:.1f} ms max, {} over 50 ms; bots {:.2f} ms a frame, {:.1f} ms max (one bot {:.1f} ms: {}, loot list {:.1f} ms, a spawn {:.1f} ms)",
        ByState[0], ByState[1], ByState[2], ByState[3], OnNavMesh, ByState[4], Loot.size(), GameState ? static_cast<int32_t>(GameState->PlayersLeft) : 0, GameState ? static_cast<int32_t>(GameState->TeamsLeft) : 0,
        FrameCount ? FrameMsSum / FrameCount : 0.0, FrameMsMax, FrameSpikes, FrameCount ? BotsMsSum / FrameCount : 0.0, BotsMsMax, BotTickMsMax, SlowestBotTick, LootRefreshMsMax, SpawnMsMax);
    FrameMsSum = 0.0;
    FrameMsMax = 0.0;
    BotsMsSum = 0.0;
    FrameCount = 0;
    FrameSpikes = 0;
    BotsMsMax = 0.0;
    BotTickMsMax = 0.0;
    LootRefreshMsMax = 0.0;
    SpawnMsMax = 0.0;
    SlowestBotTick.clear();
}
