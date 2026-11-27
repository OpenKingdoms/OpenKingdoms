/*
 * shipped_maps.c -- the maps the original's releases ship, by fingerprint.
 *
 * The first eight bytes of each map's fingerprint (tak_map_fingerprint.h),
 * read high byte first, and the release that ships it: the game, The Iron
 * Plague, or Cavedog's own map packs. A fingerprint is a hash, so nothing
 * of any map is here. The rows come from `map_inspect --shipped-table` run
 * on a clean install, and stay sorted for the search below.
 */

#include "tak_map_share.h"
#include "tak_net_protocol.h"

typedef struct ShippedMap {
    uint64_t print;
    uint8_t  share;
} ShippedMap;

static const ShippedMap shipped[] = {
    { 0x0014cbddaf6ebb0bull, TAK_MAPSHARE_GAME },  /* abnar's terrace */
    { 0x003f7b5d0a4c154eull, TAK_MAPSHARE_MAP_PACK },  /* inzacata */
    { 0x00d08354705a14ffull, TAK_MAPSHARE_MAP_PACK },  /* fel doom */
    { 0x00deb89886bdd79eull, TAK_MAPSHARE_GAME },  /* riparian conflict */
    { 0x01ab0ad22cb27dcfull, TAK_MAPSHARE_MAP_PACK },  /* east kaluen */
    { 0x029da50e10fce2b4ull, TAK_MAPSHARE_MAP_PACK },  /* volcanic stone */
    { 0x03328c93f92e1845ull, TAK_MAPSHARE_MAP_PACK },  /* rump's roost */
    { 0x0463a0988a2ac720ull, TAK_MAPSHARE_GAME },  /* lern forrest lakes_jm */
    { 0x05c0334f2bfe381eull, TAK_MAPSHARE_GAME },  /* king of the hill */
    { 0x0642e557dfe88e3bull, TAK_MAPSHARE_MAP_PACK },  /* whispering wood */
    { 0x085c26ad3b2030f9ull, TAK_MAPSHARE_MAP_PACK },  /* mukiltrom valley */
    { 0x08771fad81f08f1bull, TAK_MAPSHARE_MAP_PACK },  /* hanging gardens */
    { 0x08a31c6c6f4e4dc4ull, TAK_MAPSHARE_MAP_PACK },  /* volcanic isles */
    { 0x0942bab5a91e6130ull, TAK_MAPSHARE_MAP_PACK },  /* arches of zhon */
    { 0x094755f9a10269d4ull, TAK_MAPSHARE_MAP_PACK },  /* rynn's valley */
    { 0x0ba621bbd312006aull, TAK_MAPSHARE_MAP_PACK },  /* burial mound */
    { 0x1252db3258ff0cfaull, TAK_MAPSHARE_MAP_PACK },  /* edmont's field */
    { 0x140181d91e10e0bfull, TAK_MAPSHARE_MAP_PACK },  /* twitching rock */
    { 0x143166f9b6c7093full, TAK_MAPSHARE_MAP_PACK },  /* extinct volcano */
    { 0x1576aeb913b29bd3ull, TAK_MAPSHARE_MAP_PACK },  /* fey citadel */
    { 0x161810dfb8c35a40ull, TAK_MAPSHARE_MAP_PACK },  /* robber road */
    { 0x17c52abdeb191b5dull, TAK_MAPSHARE_MAP_PACK },  /* highland village */
    { 0x1a61bf808f64dde6ull, TAK_MAPSHARE_MAP_PACK },  /* phantom crest */
    { 0x1ab6091005511df0ull, TAK_MAPSHARE_MAP_PACK },  /* seas of flame */
    { 0x1b003067092f5e29ull, TAK_MAPSHARE_EXPANSION },  /* crusader's keep */
    { 0x1d69ff7c3f9d55fbull, TAK_MAPSHARE_GAME },  /* tarosian plain */
    { 0x21091fcf772ae38eull, TAK_MAPSHARE_MAP_PACK },  /* twisted grove */
    { 0x211e6f0577ac1df0ull, TAK_MAPSHARE_EXPANSION },  /* gemini run */
    { 0x21565af07b45b248ull, TAK_MAPSHARE_MAP_PACK },  /* zirra channel */
    { 0x21a91c5e5ff49dabull, TAK_MAPSHARE_MAP_PACK },  /* isle of sand */
    { 0x21ee1c3e768592d4ull, TAK_MAPSHARE_MAP_PACK },  /* bell toll */
    { 0x2202b2ee5c0348dbull, TAK_MAPSHARE_MAP_PACK },  /* drialia's cliffs */
    { 0x224cf2cde9201d70ull, TAK_MAPSHARE_GAME },  /* two castles */
    { 0x22de4ead890653e3ull, TAK_MAPSHARE_MAP_PACK },  /* grimm farms */
    { 0x24955eeee86474c2ull, TAK_MAPSHARE_MAP_PACK },  /* tribelands */
    { 0x24c3f31a0a8872beull, TAK_MAPSHARE_MAP_PACK },  /* harsh dunes */
    { 0x260ea1f1d841f96eull, TAK_MAPSHARE_MAP_PACK },  /* irontaste lake */
    { 0x283fde52caf9be90ull, TAK_MAPSHARE_MAP_PACK },  /* the glavnos */
    { 0x284a31e5b9d17c39ull, TAK_MAPSHARE_MAP_PACK },  /* stone baths */
    { 0x289132425364a422ull, TAK_MAPSHARE_MAP_PACK },  /* kraken temple */
    { 0x295ecd20c5243109ull, TAK_MAPSHARE_MAP_PACK },  /* gaerion wood */
    { 0x2995fbf2b31465cfull, TAK_MAPSHARE_EXPANSION },  /* whisper hills pass */
    { 0x2a2ae339b240babdull, TAK_MAPSHARE_MAP_PACK },  /* ihom coast */
    { 0x2a5901deed3c0059ull, TAK_MAPSHARE_MAP_PACK },  /* oggin landing */
    { 0x2ba4f2c5ba8e5d07ull, TAK_MAPSHARE_MAP_PACK },  /* theatre of cruelty */
    { 0x2c445186a3cc534dull, TAK_MAPSHARE_MAP_PACK },  /* sea castle */
    { 0x2f40e0bfd05adcb2ull, TAK_MAPSHARE_MAP_PACK },  /* canal walls */
    { 0x3255b3539225ef34ull, TAK_MAPSHARE_MAP_PACK },  /* ill fens */
    { 0x328c229c454e6bbfull, TAK_MAPSHARE_MAP_PACK },  /* jungle island */
    { 0x32e384cab72c2a6dull, TAK_MAPSHARE_MAP_PACK },  /* narmer harbor */
    { 0x35ce947afd4f85adull, TAK_MAPSHARE_MAP_PACK },  /* orc rocks */
    { 0x378a67146eeaa6b4ull, TAK_MAPSHARE_MAP_PACK },  /* slaver den */
    { 0x37969f317f977e5bull, TAK_MAPSHARE_EXPANSION },  /* moka's fingers */
    { 0x3927dad61f30a0cbull, TAK_MAPSHARE_MAP_PACK },  /* peninsular vina */
    { 0x39d51370a5ee2ea1ull, TAK_MAPSHARE_GAME },  /* sea dragon spine */
    { 0x3a3615df1ebc7b59ull, TAK_MAPSHARE_MAP_PACK },  /* sardonic isles */
    { 0x3b199188b283cae4ull, TAK_MAPSHARE_MAP_PACK },  /* terror town */
    { 0x3bb9b9d2b61b226dull, TAK_MAPSHARE_MAP_PACK },  /* navigator island */
    { 0x3c3b4c2d1f5fa1e7ull, TAK_MAPSHARE_GAME },  /* varro passage */
    { 0x3c662d7b2e4c8721ull, TAK_MAPSHARE_MAP_PACK },  /* crossings */
    { 0x3d2e6acfd51bf6ceull, TAK_MAPSHARE_MAP_PACK },  /* endocott isle */
    { 0x3e7d07fc9e5dc61cull, TAK_MAPSHARE_MAP_PACK },  /* into the woods */
    { 0x41cbc8b5229539adull, TAK_MAPSHARE_MAP_PACK },  /* troubled waters */
    { 0x42a537fce24c10f1ull, TAK_MAPSHARE_MAP_PACK },  /* human menagerie */
    { 0x432c88345b31450cull, TAK_MAPSHARE_MAP_PACK },  /* living wastes */
    { 0x4430afcec39cff9dull, TAK_MAPSHARE_MAP_PACK },  /* shiaugh lakes */
    { 0x448d03fcc66120a8ull, TAK_MAPSHARE_GAME },  /* threesacharm */
    { 0x452d96fbd2c69c1dull, TAK_MAPSHARE_MAP_PACK },  /* mithril run */
    { 0x45c2e953a8f50d4full, TAK_MAPSHARE_MAP_PACK },  /* mud fern lakes */
    { 0x4623b8f5a9511503ull, TAK_MAPSHARE_MAP_PACK },  /* mateo grasslands */
    { 0x468230f60e21f039ull, TAK_MAPSHARE_MAP_PACK },  /* mana quarry */
    { 0x4900bf10f65b60fcull, TAK_MAPSHARE_MAP_PACK },  /* hillspire vista */
    { 0x49f723b53665dfb8ull, TAK_MAPSHARE_EXPANSION },  /* nobia's temple */
    { 0x4a28486c041e3aa9ull, TAK_MAPSHARE_EXPANSION },  /* sand river plain */
    { 0x4ad13558571efa63ull, TAK_MAPSHARE_MAP_PACK },  /* yew wood */
    { 0x4c4fedce455af612ull, TAK_MAPSHARE_MAP_PACK },  /* overseer plateau */
    { 0x4ce9b7dc60a4d67eull, TAK_MAPSHARE_MAP_PACK },  /* ensign's lament */
    { 0x4d73effa99ed3207ull, TAK_MAPSHARE_EXPANSION },  /* berms of glyn */
    { 0x4d9f78b58a889268ull, TAK_MAPSHARE_MAP_PACK },  /* fossil cleft */
    { 0x4dffaad11fe2f095ull, TAK_MAPSHARE_GAME },  /* savannah hunt */
    { 0x4f5b6b1dc53e9b00ull, TAK_MAPSHARE_EXPANSION },  /* rift of grief */
    { 0x4fff6def48d14c7full, TAK_MAPSHARE_MAP_PACK },  /* verdun cliffs */
    { 0x52774dc763142465ull, TAK_MAPSHARE_MAP_PACK },  /* still lake */
    { 0x52b00ee9414cd56eull, TAK_MAPSHARE_GAME },  /* ulin's folly */
    { 0x55472bdd3de82230ull, TAK_MAPSHARE_GAME },  /* per mare per terras */
    { 0x55e6051caf5f550cull, TAK_MAPSHARE_MAP_PACK },  /* dan'gel ravine */
    { 0x55fe470081d9c698ull, TAK_MAPSHARE_GAME },  /* lake lokken */
    { 0x58945d939f98e567ull, TAK_MAPSHARE_MAP_PACK },  /* greenstone point */
    { 0x5987a10a96cc3bc5ull, TAK_MAPSHARE_MAP_PACK },  /* sea snake */
    { 0x5a4e3e7f039270d3ull, TAK_MAPSHARE_MAP_PACK },  /* mountain temple */
    { 0x5a7079c501db9beeull, TAK_MAPSHARE_MAP_PACK },  /* lobster manor */
    { 0x5c284f86be1789feull, TAK_MAPSHARE_MAP_PACK },  /* cursed temple */
    { 0x5cc9aeecd5ae81e4ull, TAK_MAPSHARE_MAP_PACK },  /* peasant grove */
    { 0x5e3a65495079f63bull, TAK_MAPSHARE_GAME },  /* castle */
    { 0x5f9dd18be68ebe51ull, TAK_MAPSHARE_MAP_PACK },  /* alchemist glen */
    { 0x61048115597543adull, TAK_MAPSHARE_EXPANSION },  /* haunted waterworks */
    { 0x6209840920ab163full, TAK_MAPSHARE_MAP_PACK },  /* peream land */
    { 0x631b7c7297ac69bfull, TAK_MAPSHARE_MAP_PACK },  /* riverlook */
    { 0x65217b3f5d21e4b0ull, TAK_MAPSHARE_MAP_PACK },  /* kingmaker isle */
    { 0x65893d62069b44fbull, TAK_MAPSHARE_MAP_PACK },  /* sandy shore */
    { 0x66692a3f9c06b84aull, TAK_MAPSHARE_MAP_PACK },  /* blood feud */
    { 0x669af16311e0f192ull, TAK_MAPSHARE_EXPANSION },  /* yaviza */
    { 0x6726a4f85cc7f8b9ull, TAK_MAPSHARE_MAP_PACK },  /* the muldoons */
    { 0x67533369c47f2c06ull, TAK_MAPSHARE_MAP_PACK },  /* siren rocks */
    { 0x698ecd30fed2957eull, TAK_MAPSHARE_GAME },  /* increasing strength */
    { 0x6b0155a6cfa81affull, TAK_MAPSHARE_MAP_PACK },  /* iobar's theatre */
    { 0x6c50a1faeeca1433ull, TAK_MAPSHARE_MAP_PACK },  /* manorhome */
    { 0x6c8a4186e2ed5db8ull, TAK_MAPSHARE_GAME },  /* meredoc keys_jm */
    { 0x6ce2b7e52b2e7cd6ull, TAK_MAPSHARE_MAP_PACK },  /* oracle at pentus */
    { 0x6d5802325f79faf3ull, TAK_MAPSHARE_MAP_PACK },  /* field of dasanko */
    { 0x6dca6750262259e4ull, TAK_MAPSHARE_MAP_PACK },  /* fallow fields */
    { 0x6e4da37e647019eaull, TAK_MAPSHARE_MAP_PACK },  /* oldhenge */
    { 0x710b785414908bc5ull, TAK_MAPSHARE_MAP_PACK },  /* ulasem */
    { 0x73c674c24a77f1a4ull, TAK_MAPSHARE_EXPANSION },  /* isle of palms */
    { 0x74756ccde2b1f4e8ull, TAK_MAPSHARE_MAP_PACK },  /* spectre keep */
    { 0x7781a543e8a509b5ull, TAK_MAPSHARE_MAP_PACK },  /* new hindigal */
    { 0x783375ab17acbf6full, TAK_MAPSHARE_MAP_PACK },  /* hallowed river */
    { 0x78bfc34e511f40f1ull, TAK_MAPSHARE_MAP_PACK },  /* signal pointe */
    { 0x7f1d1c3d3c80c6c0ull, TAK_MAPSHARE_MAP_PACK },  /* salt pillars */
    { 0x7f89f32bda49cca0ull, TAK_MAPSHARE_EXPANSION },  /* lake cuhmoniwanakilya */
    { 0x8007a897d026ed5cull, TAK_MAPSHARE_MAP_PACK },  /* marlok bay */
    { 0x8575c1ace69ec60dull, TAK_MAPSHARE_MAP_PACK },  /* scribe's slough */
    { 0x8658d4eedefcfb11ull, TAK_MAPSHARE_MAP_PACK },  /* unholy purpose */
    { 0x88ce77d665719c84ull, TAK_MAPSHARE_MAP_PACK },  /* sinibad's cove */
    { 0x8b89d197ec79deb3ull, TAK_MAPSHARE_MAP_PACK },  /* ghost shipping */
    { 0x8b923ab99c33709dull, TAK_MAPSHARE_MAP_PACK },  /* seygon canal */
    { 0x8babc7fd7490b56eull, TAK_MAPSHARE_MAP_PACK },  /* plateau aerinarr */
    { 0x8e3de22f92c8b715ull, TAK_MAPSHARE_MAP_PACK },  /* fire ant field */
    { 0x8e829dd9ba4b3498ull, TAK_MAPSHARE_MAP_PACK },  /* the vault */
    { 0x8e843d462df02f8eull, TAK_MAPSHARE_MAP_PACK },  /* path of pardu */
    { 0x8eea451eb15ea2f4ull, TAK_MAPSHARE_MAP_PACK },  /* tribal shores */
    { 0x8fe869e59fdffcc6ull, TAK_MAPSHARE_MAP_PACK },  /* lost river valley */
    { 0x901f84310b6ccbe3ull, TAK_MAPSHARE_MAP_PACK },  /* urban riverside */
    { 0x9199f1034c0cbe99ull, TAK_MAPSHARE_MAP_PACK },  /* lieber's project */
    { 0x922a83f429635514ull, TAK_MAPSHARE_MAP_PACK },  /* terror hamlet */
    { 0x93c448a835bf58f4ull, TAK_MAPSHARE_GAME },  /* muntil's ford guard */
    { 0x94a0d0a811a8fee9ull, TAK_MAPSHARE_MAP_PACK },  /* shifting sands */
    { 0x9872606e78eae241ull, TAK_MAPSHARE_MAP_PACK },  /* cauldron waters */
    { 0x98aa4eff841902cfull, TAK_MAPSHARE_MAP_PACK },  /* voyager isles */
    { 0x98cb2b4cc3bcdee5ull, TAK_MAPSHARE_MAP_PACK },  /* three jewels */
    { 0x98fe63252e2eb7e9ull, TAK_MAPSHARE_MAP_PACK },  /* the sea wood */
    { 0x99e58db098efa048ull, TAK_MAPSHARE_MAP_PACK },  /* dancing river */
    { 0x9acaa49d3c0601aaull, TAK_MAPSHARE_EXPANSION },  /* theion's island */
    { 0x9ae6c3efa795c89cull, TAK_MAPSHARE_MAP_PACK },  /* tarza's river */
    { 0x9b60a82e1e0d04e0ull, TAK_MAPSHARE_MAP_PACK },  /* treehenge */
    { 0x9be0ca5551633822ull, TAK_MAPSHARE_MAP_PACK },  /* drake point */
    { 0x9c0aa376d20f9c71ull, TAK_MAPSHARE_MAP_PACK },  /* jungle hunt */
    { 0x9c168ac322dbacffull, TAK_MAPSHARE_MAP_PACK },  /* dry ring */
    { 0x9c675e4256aaec11ull, TAK_MAPSHARE_GAME },  /* angvir's maze */
    { 0x9d7b1f42f7f456dfull, TAK_MAPSHARE_MAP_PACK },  /* the lochs */
    { 0x9eb1041fce919eb1ull, TAK_MAPSHARE_GAME },  /* inner circle */
    { 0x9ebd13a36849d39dull, TAK_MAPSHARE_EXPANSION },  /* lost lake */
    { 0x9fadf199b5e31ea5ull, TAK_MAPSHARE_EXPANSION },  /* no zhonian is an island */
    { 0xa069ec0933fe2aa9ull, TAK_MAPSHARE_MAP_PACK },  /* crash barrens */
    { 0xa0ecc326d45848cbull, TAK_MAPSHARE_MAP_PACK },  /* kandran moat */
    { 0xa29eddf72f58e80aull, TAK_MAPSHARE_EXPANSION },  /* the drafis bridge */
    { 0xa2f5b294f74fd3b1ull, TAK_MAPSHARE_MAP_PACK },  /* qinzi plazas */
    { 0xa54e34ff7f4dcae4ull, TAK_MAPSHARE_EXPANSION },  /* blood and roses */
    { 0xa87eb2d89764094aull, TAK_MAPSHARE_MAP_PACK },  /* evergreen ridge */
    { 0xa8a822c4f6e43cf6ull, TAK_MAPSHARE_EXPANSION },  /* islands of the mer warrior */
    { 0xa90c0c01bdc3eb1bull, TAK_MAPSHARE_MAP_PACK },  /* fountainhead */
    { 0xab9c480110c31929ull, TAK_MAPSHARE_MAP_PACK },  /* mountain madness */
    { 0xac6e6a04b14c9b7aull, TAK_MAPSHARE_EXPANSION },  /* eye of belial */
    { 0xb12ee13f76e38647ull, TAK_MAPSHARE_MAP_PACK },  /* ivy mountains */
    { 0xb907191a0f34e55bull, TAK_MAPSHARE_GAME },  /* cairbray coast landing */
    { 0xb9193f0187bc6cffull, TAK_MAPSHARE_EXPANSION },  /* black heart jungle */
    { 0xb927dd2ca2e2b2b1ull, TAK_MAPSHARE_MAP_PACK },  /* walls of elam */
    { 0xba22c60f2e10405dull, TAK_MAPSHARE_MAP_PACK },  /* the quandary */
    { 0xba329fdc7d33779cull, TAK_MAPSHARE_MAP_PACK },  /* syphon channels */
    { 0xbb85429d372a9a42ull, TAK_MAPSHARE_EXPANSION },  /* the gardens of atys */
    { 0xbe46fd02531ed32bull, TAK_MAPSHARE_MAP_PACK },  /* long canal */
    { 0xbe93284b061f52fcull, TAK_MAPSHARE_MAP_PACK },  /* bay of mist */
    { 0xbee605e7a695d041ull, TAK_MAPSHARE_GAME },  /* iapur narrows */
    { 0xbf3dcf29602a1b60ull, TAK_MAPSHARE_MAP_PACK },  /* hall of heroes */
    { 0xc09ded076cb73606ull, TAK_MAPSHARE_GAME },  /* fortresses of laingen */
    { 0xc174fb53061fadf3ull, TAK_MAPSHARE_MAP_PACK },  /* caer conlin */
    { 0xc23031a854ee054bull, TAK_MAPSHARE_MAP_PACK },  /* streets of pain */
    { 0xc2b59dd69dee13a7ull, TAK_MAPSHARE_MAP_PACK },  /* two sins */
    { 0xc2fc1c517bba9af5ull, TAK_MAPSHARE_MAP_PACK },  /* inclined forest */
    { 0xc35caf65cc335d76ull, TAK_MAPSHARE_MAP_PACK },  /* sparkly bay */
    { 0xc3629b884850abe1ull, TAK_MAPSHARE_MAP_PACK },  /* dreadrock canal */
    { 0xc42fdb6f8fa39930ull, TAK_MAPSHARE_MAP_PACK },  /* the magic knot */
    { 0xc4be26cab3dcac08ull, TAK_MAPSHARE_MAP_PACK },  /* overseer island */
    { 0xc7d20f3852a73243ull, TAK_MAPSHARE_MAP_PACK },  /* quartz springs */
    { 0xc913e98f97e1373dull, TAK_MAPSHARE_GAME },  /* lake ferrix_jm */
    { 0xc93f4492d511a1c1ull, TAK_MAPSHARE_GAME },  /* ladron's tarn */
    { 0xc982a125c78e0825ull, TAK_MAPSHARE_MAP_PACK },  /* worm wood */
    { 0xc9a341d02b57d743ull, TAK_MAPSHARE_MAP_PACK },  /* canal crossing */
    { 0xca74cc0e7501a367ull, TAK_MAPSHARE_MAP_PACK },  /* temple of blood */
    { 0xcb70954f3ad3f10bull, TAK_MAPSHARE_MAP_PACK },  /* icy peaks */
    { 0xcd9ffcf977c8b563ull, TAK_MAPSHARE_MAP_PACK },  /* green island */
    { 0xcdadf64feb1c4f44ull, TAK_MAPSHARE_MAP_PACK },  /* aibel's seaport */
    { 0xd0319ecb7d74cd4eull, TAK_MAPSHARE_GAME },  /* the old riverbed */
    { 0xd16ed68c50247f97ull, TAK_MAPSHARE_MAP_PACK },  /* precipices */
    { 0xd42a721d688caa1eull, TAK_MAPSHARE_MAP_PACK },  /* mogrium mound */
    { 0xd4630a2415fea74dull, TAK_MAPSHARE_MAP_PACK },  /* zhon highlands */
    { 0xd4d78957a78d63acull, TAK_MAPSHARE_MAP_PACK },  /* jungle adventure */
    { 0xd547e96dc7f81dbaull, TAK_MAPSHARE_MAP_PACK },  /* soulspit */
    { 0xd561adf5cff72265ull, TAK_MAPSHARE_MAP_PACK },  /* lantern district */
    { 0xd65174649116c47aull, TAK_MAPSHARE_MAP_PACK },  /* m'hari outskirts */
    { 0xda9166332a375620ull, TAK_MAPSHARE_MAP_PACK },  /* crab cove */
    { 0xdb7fb8b59624ed43ull, TAK_MAPSHARE_GAME },  /* loch brynn */
    { 0xdb87ef9af41dd8a4ull, TAK_MAPSHARE_GAME },  /* frey river plain */
    { 0xdb9238a09b1b948bull, TAK_MAPSHARE_MAP_PACK },  /* clan village */
    { 0xdd282f667b5c4d76ull, TAK_MAPSHARE_MAP_PACK },  /* invasion bluffs */
    { 0xddaff3d4566577e4ull, TAK_MAPSHARE_EXPANSION },  /* rival hill */
    { 0xde7764a4a5a26200ull, TAK_MAPSHARE_MAP_PACK },  /* zhorl valley */
    { 0xdf9c164ae8bd0f8full, TAK_MAPSHARE_EXPANSION },  /* kaf's pit */
    { 0xe1a0b2d0389335a9ull, TAK_MAPSHARE_MAP_PACK },  /* barb coast */
    { 0xe2351637f089ea38ull, TAK_MAPSHARE_MAP_PACK },  /* tenfinger road */
    { 0xe2902c0ab32f8b04ull, TAK_MAPSHARE_MAP_PACK },  /* talgath's toll */
    { 0xe38a0cb6cd4c4f0full, TAK_MAPSHARE_MAP_PACK },  /* hedgemaze */
    { 0xe591a2fd5d2e1b16ull, TAK_MAPSHARE_GAME },  /* athri cay */
    { 0xe677c228d5278df2ull, TAK_MAPSHARE_MAP_PACK },  /* brandon's woods */
    { 0xe8bd6366e422e2aaull, TAK_MAPSHARE_MAP_PACK },  /* thorn boscage */
    { 0xe8fb90e5823ecf1bull, TAK_MAPSHARE_EXPANSION },  /* alkhest quadrille */
    { 0xec12f71084058672ull, TAK_MAPSHARE_MAP_PACK },  /* toad river */
    { 0xed70716b45f3a6a7ull, TAK_MAPSHARE_MAP_PACK },  /* neegam's pass */
    { 0xee9ea5cdbfaecd32ull, TAK_MAPSHARE_MAP_PACK },  /* waterworks */
    { 0xf04f10a63e46fa21ull, TAK_MAPSHARE_MAP_PACK },  /* greenwater */
    { 0xf0aa1ca52036d866ull, TAK_MAPSHARE_GAME },  /* ground war */
    { 0xf15a848b8e4390caull, TAK_MAPSHARE_MAP_PACK },  /* windswept isle */
    { 0xf212e569d20e8d45ull, TAK_MAPSHARE_MAP_PACK },  /* four barracks */
    { 0xf3c86d5d0a076749ull, TAK_MAPSHARE_MAP_PACK },  /* walls of carin vel */
    { 0xf46eca43a70ed929ull, TAK_MAPSHARE_MAP_PACK },  /* black forest */
    { 0xf561b217b57fd6dcull, TAK_MAPSHARE_MAP_PACK },  /* riverfork wood */
    { 0xf57039c2f37ff0f4ull, TAK_MAPSHARE_MAP_PACK },  /* tartaglia narrows */
    { 0xf7b5dc0f3fddf9afull, TAK_MAPSHARE_GAME },  /* sewers of elam */
    { 0xf87fae095ddd3993ull, TAK_MAPSHARE_MAP_PACK },  /* inwaari temple */
    { 0xfa85a238f3630fe2ull, TAK_MAPSHARE_MAP_PACK },  /* marina largo */
    { 0xfa9f95ff52dcb09cull, TAK_MAPSHARE_EXPANSION },  /* ulasem arena */
    { 0xfb5e399312bd76b2ull, TAK_MAPSHARE_MAP_PACK },  /* valysia city */
    { 0xfb8242c2a688f24cull, TAK_MAPSHARE_MAP_PACK },  /* parched sea */
    { 0xfc12b19ab4e8791cull, TAK_MAPSHARE_MAP_PACK },  /* savage isle */
    { 0xfce18a0df3de36a5ull, TAK_MAPSHARE_MAP_PACK },  /* adamantine gate */
    { 0xfd713c6d6a4f4dbbull, TAK_MAPSHARE_MAP_PACK },  /* red rivers */
};

int TAK_MapShare_Shipped(const uint8_t fp[TAK_MAP_FINGERPRINT_BYTES]) {
    if (!fp) return 0;
    uint64_t print = 0;
    for (int i = 0; i < 8; i++) print = (print << 8) | fp[i];
    size_t lo = 0, hi = sizeof shipped / sizeof shipped[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (shipped[mid].print == print) return shipped[mid].share;
        if (shipped[mid].print < print) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}
