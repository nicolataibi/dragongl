* Thu Nov 12 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.12.01
- DOCS: LORE.md: added scene illustrations for every narrative paragraph
  of Chapter 1 (Floor 0 sanctuary, characters, descent to Floor 1).

* Wed Nov 11 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.11.03
- PACKAGING: remove CHANGELOG.md from %doc in dragongl.spec: avoids
  rpmlint file-contains-current-date false positives on release day;
  the package changelog is provided natively by %autochangelog.

* Wed Nov 11 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.11.02
- CODE: format OpenSSL 4.0 TLS changes according to .clang-format
  (server_tls.c and test_tls.c).

* Wed Nov 11 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.11.01
- TLS: fix OpenSSL 4.0 compiler warnings across client, server and unit tests:
  use X509_VERIFY_PARAM_set1_host on SSL_get0_param instead of deprecated
  SSL_set1_host in client_tls; allocate and set X509_NAME with
  X509_NAME_new, X509_set_subject_name and X509_NAME_free instead of
  modifying the const pointer returned by X509_get_subject_name in
  server_tls and test_tls.
- TESTS: test_other_protocol_versions_fail_the_handshake in the server
  network integration test suite accepts any ssl.SSLError on an ALPN
  mismatch rather than expecting the exact 'NO_APPLICATION_PROTOCOL'
  string, which OpenSSL 4.0 reports as an unknown SSL error in Python.
- DOCS: LORE.md: Dragon's Maw Fantasy Novel added new pictures.

* Mon Nov 09 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.09.03
- DATA: every spell of data/spells.json has an explicit "effect": kind
  (damage, heal, condition, buff, utility, special), target (single, self,
  circle, cone, line, cloud), range and radius in cells of 5 feet, roll
  (auto, attack, save), save, on_save, fixed dc (martial maneuvers), dice
  (count, sides, bonus, ability added), damage_type, cantrip_scaling,
  condition and rounds, buff stat and value, handler, terrain.
  data/schema/spells.schema.json documents and validates every field. The
  effects were generated once from the descriptions and then checked one
  by one; where the old keyword deduction contradicted the description,
  the description won.
- SERVER: the deduction of the spell from its description is gone
  (spell_range, spell_from_description and the defaults it filled in); the
  server refuses to start on an invalid effect, naming the spell and the
  field. The magic router dispatches on the effect's handler, not on the
  spell's name.
- GAMEPLAY: 551 spells: 175 damage, 16 heal, 44 condition, 31 buff, 22
  special, 263 utility. Of the 171 spells that were and still are damage,
  54 changed dice and 43 their damage type (the old parser took the first
  dice of the text and the first word of a fixed list found in it:
  Thunderwave dealt lightning, Shatter cold). 199 spells changed target
  (Fireball and Ice Storm were single-target spells). The old "debuff" (a
  -2 that no rule read) was 196 spells: 26 are conditions, 147 utility (no
  combat effect), the rest damage, buff, heal or special; 75 old "damage"
  spells (no damage in the text) are utility.
- GAMEPLAY: a spell resolves as its record says. Spell attack: proficiency
  + casting ability modifier against AC, a natural 20 doubles the dice;
  saving throws against 8 + proficiency (+2 at levels 1-4 ... +6 at 17) +
  casting ability modifier, where the casting ability is the class's (INT
  wizard; WIS cleric, druid, ranger, monk; CHA bard, sorcerer, warlock,
  paladin; DEX rogue; STR barbarian, fighter) instead of INT for
  everybody; martial maneuvers use their printed DC. "half" damage only
  for the spells that say so (it was every spell). NPC saving throws: +2,
  +1 every 10 floors, +2 for a boss (they were +0). Resistances,
  immunities and vulnerabilities of the bestiary, and its condition
  immunities, apply to spells.
- GAMEPLAY: cantrips scale at character levels 5, 11 and 17 (Fire Bolt
  2d10 at 5). Condition riders: Thunderous Smite knocks prone, Blinding
  Smite blinds, Searing Smite burns, Ray of Sickness poisons, Sunburst
  blinds, Weird frightens, each with the save of the text. Buffs work:
  Mage Armor and Barkskin +3 AC, Haste and Holy Aura +2 AC, defensive
  maneuvers (Ki Shield, Total Defense, ...) their bonus, Bless +2 to
  attack, Divine Favor, Hex, Hunter's Mark, Crusader's Mantle and Absorb
  Elements add damage to weapon attacks. Casting a buff again refreshes
  it.
- GAMEPLAY: areas. A circle or cloud with a range is centred on the
  nearest enemy within range, not on the caster (Cloudkill used to go off
  at the caster's feet); cones and lines start at the caster towards the
  nearest enemy within their length. Ranges are cells (they were feet read
  as cells: Fire Bolt reached 120 cells, now 24). Clouds last 3 rounds (2
  for a cantrip): the engine has no concentration, and a 1-minute cloud
  dealt several times the damage of any other spell of its level; a
  condition from a cloud lasts one round and is renewed each round.
- GAMEPLAY: Ice Storm deals its damage and then ices the ground (its
  terrain hook used to swallow the damage); Wall of Stone raises walls,
  Transmute Rock turns the ground to mud. Conjure Elemental and Conjure
  Minor Elementals summon an elemental, Dimension Door teleports, Mislead
  makes invisible. Charm Person and Dominate Monster use the caster's DC
  against the NPC's save (Charm Person gave the NPC +10).
- GAMEPLAY: a spell with no enemy to aim at is not cast and keeps its slot
  (the slot used to be spent first). Utility spells say they have no
  combat effect. Paralyzed, stunned, unconscious, petrified, frozen and
  incapacitated players cannot cast or attack; incapacitated NPCs lose
  their turn. A condition of N rounds now costs an NPC N turns (effects
  aged before the turn: a 1-round stun did nothing). Spells never hit
  summoned or dominated allies, treasure chests or gold piles. A kill by a
  spell or a cloud gives the same rewards as a melee kill (party XP, event
  progress, loot); the cloud credits its caster while he is online.
- DATA: spelling. Cordon of Arrows (was "Cordon Of Arrows"), the name of
  the Planar Mischievous Sprite (misspelled) and 23 misspelled words in
  descriptions (energy, within, receive, ...). The NPC file is tied to
  the monster names: an existing world is generated anew at the first
  start of this version; characters are kept.
- TESTS: unit test test_spell_rules: the effect parser (valid records and
  one error per field) and the resolution with a forced d20 (attack, crit,
  miss, save, half, none, rider save, immunity, resistance, cantrip tiers,
  DC). Integration harness: one test per kind of effect through the real
  cast command (attack, save area, cone and line, conditions, heal and
  buffs, cloud, handler and terrain, refusals) and spell_every_effect,
  which casts each of the 551 spells and checks the outcome against its
  record. rules_force_d20() fixes the d20 for tests.
- TESTS: balance check spell_balance (CTest): every damage spell is
  simulated 4000 times with the game's own loader and resolution, by the
  reference caster of its level against the reference target of its level;
  the expected damage per target, heals, buff values and unresisted
  control must stay within tests/spell_balance.json, so a data change that
  breaks the balance fails CI. --report prints the table.
- TESTS: line coverage 54.5% (was 49.6%): the coverage floor is 54.0%;
  34 functions over 120 lines (was 37): the limit follows.
- TOOLS: tools/check_data.py checks that the schema and the engine list
  the same conditions and handlers, that only cantrips scale, that only
  maneuvers have a fixed DC and that a rider save has a condition.
  tools/quality.sh spelling runs codespell (.codespellrc) on the data, the
  documentation, the man pages, the sources and the tools, in the CI lint
  job. The Data Studio shows the effect of a spell (column, filter, detail
  line) and edits it as validated JSON.
- CODE: the Italian left in the code is gone: comments, the Italian
  aliases of damage types, conditions and item bonuses in the data loader
  (the data is English), "Arco Corto", "Scheletro", "Nube Acida".
  Damage-type and ability names have canonical tables in rules.c, like
  conditions. src/server/spell_cast.c casts, src/shared/spell_rules.c
  resolves, src/shared/spell_effect.c parses; aoe.c keeps the geometry,
  the terrain chemistry and the clouds. The kill rewards of melee and
  magic are one function, npc_killed().
- DOCS: DEVELOPMENT.md "Spells: data, resolution, balance" and the
  spelling check; LORE.md describes the effect records, the handlers and
  the terrain spells.

* Mon Nov 09 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.09.02
- SECURITY: every connection is TLS 1.3 (OpenSSL 3): passwords, chat and
  game state no longer travel in clear. TLS 1.2 and older, compression and
  renegotiation are refused; there are no session tickets.
- SECURITY: the server's identity. Without --tls-cert/--tls-key the server
  creates tls/server.key (ECDSA P-256, mode 0600, in a 0700 directory) and
  a self-signed tls/server.crt for the host name (10 years) in its state
  directory at the first start, and logs the SHA-256 fingerprint at every
  start. A certificate without its key, or one option without the other,
  stops the server with exit status 2; a key readable by group or others
  is used with a warning, as the password files are.
- SECURITY: the client authenticates the server before it sends the
  login. It accepts a server whose certificate has the fingerprint given
  with the new --fingerprint option, or is signed by an authority the
  system trusts for the host name typed, or is recorded in
  ~/.config/dragongl/known_servers ($XDG_CONFIG_HOME/dragongl); the first
  connection to a self-signed server prints the fingerprint and records
  it (trust on first use, as SSH). A changed certificate is refused with
  a warning that names the file and line to remove.
- PROTOCOL: version 5. The version is also the ALPN protocol
  "dragongl/5": a client or server of another version fails the TLS
  handshake with a clear message (no_application_protocol), and a client
  that offers no ALPN protocol is disconnected. The frames are those of v4.
  Clients and servers of older versions cannot connect.
- PROTOCOL: one table, protocol/messages.json, defines every message:
  fields, wire types, sizes, ranges, cross-field checks and an example.
  tools/gen_protocol.py generates from it the structs
  (include/protocol_gen.h), the codec (include/wire.h, src/shared/wire.c),
  a conformance test of the C codec (tests/test_protocol_conformance.c,
  13 valid and 65 invalid vectors), the vectors for the Python codec
  (tests/protocol_vectors.json) and sections 2 and 3 of PROTOCOL.md;
  protocol/codec.py reads the same table for the tests. CI fails on a
  stale generated file (tools/quality.sh protocol).
- PROTOCOL: every message has the same codec interface,
  wire_put_<msg>(w, &msg) and wire_get_<msg>(payload, n, &msg); MsgMapChunk
  carries its tiles, MsgEntityUpdate its records. Encoders now refuse the
  values decoders refuse (a tile above the voxel table, a time of day out
  of range, a step longer than one tile), so neither side can send what
  the other would drop it for. Each message has its exact maximum payload
  (WIRE_MAX_PAYLOAD_<MSG>), checked on reception.
- SERVER: the TLS handshake is driven by the non-blocking loop like any
  other read or write, and counts against the 10 s login timeout. A
  connection refused at once (server full, address locked out, too many
  pending logins) completes the handshake in one of 16 refusal slots and
  receives its MSG_AUTH_FAIL there within 3 s; beyond 16 it is closed
  without a reason. Disconnections send close_notify. Bytes TLS decrypted
  beyond the receive buffer are read as soon as messages make room.
- SERVER: the command line is in server_options.c, out of main_server.c.
- TESTS: network tests for TLS: the identity is created once and kept
  across restarts, the client sees the logged fingerprint, plaintext and
  TLS 1.2 clients are refused, other ALPN versions fail the handshake,
  --tls-cert/--tls-key, half an identity and a lone TLS option are
  refused. The client smoke test checks trust on first use, a known
  server, a changed certificate and --fingerprint. Unit test test_tls:
  fingerprints, ALPN and the known_servers store. All network tests now
  run over TLS, under ASan/UBSan and ThreadSanitizer.
- TESTS: line coverage 49.6% (was 48.4%): the coverage floor is 49.0%.
- DOCS: SECURITY.md has a threat model (assets, actors and trust
  boundaries, threats and mitigations, residual risks). PROTOCOL.md,
  INSTALL.md ("TLS identity", with systemd credentials for a certificate
  of your own), HOWTO.md, README.md, BUILD.md, DEVELOPMENT.md and the man
  pages describe TLS, the fingerprint and the protocol table.
- PACKAGING: BuildRequires pkgconfig(openssl); CI installs libssl-dev. No
  RPM macro in a spec comment any more (rpmlint macro-in-comment).
- GIT: .git-blame-ignore-revs lists the reformatting commit of phase 8.
  The 2026.11.09.01 entry of this changelog is restored (its patch hunk
  had been rejected next to the 2026.11.08.16 shader fix), the leftovers
  of that patch (CHANGELOG.md.orig/.rej, dragongl.spec.orig/.rej) are
  removed, and .gitignore keeps *.orig and *.rej out of the repository.

* Mon Nov 09 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.09.01
- QUALITY: every quality criterion of the project has a number measured
  in CI and a check that protects it. tools/quality.sh runs each gate
  locally (format, format-fix, tidy, data, metrics, coverage, tsan,
  fuzz, all) with the clang 18 tools CI pins; tools/quality_limits.json
  holds the limits, which a change may tighten and never loosen.
- STYLE: .clang-format (LLVM base, 4 spaces, 100 columns, braces always)
  and the whole C tree formatted with it once; CI fails on an unformatted
  file. Vendored cJSON keeps its upstream style. The reformat changes no
  behavior: the 60 generated floors and the loaded game data were
  compared byte for byte with the previous release.
- STATIC ANALYSIS: .clang-tidy (bugprone, cert, clang-analyzer, misc,
  performance, portability, readability; every finding is an error, each
  disabled check carries its reason). Zero findings on the server, the
  client, the shared code, the tests and the fuzz targets; CI runs it.
- FIX: numbers typed by players and read from the data go through
  include/strnum.h (parse_int, parse_ll, parse_double, str_to_int,
  scan_ints) instead of atoi, atoll, atof and sscanf, whose behavior is
  undefined on overflow: 'dm_gold 99999999999999999999', a huge book or
  spell index for cast, an overflowing DRAGONGL_WINDOW. dm_gold accepts
  at most 10^12 in either direction and says so.
- FIX: the server started without a data file that did not parse (the
  loader reported the error and went on), and the message itself read
  the freed text of the file to show where the error was. A data file
  that cannot be read or parsed now stops the server (exit status 1)
  with the file name and the line and column of the error.
- FIX: items are compared field by field (trade offers, empty save
  slots), not with memcmp, which also compared the padding bytes.
- CODE: header hygiene (duplicate prototypes and extern redeclarations
  removed, headers included by name), explicit narrowing conversions,
  duplicated branches merged, nested ternaries and strcat/strcpy chains
  rewritten, Italian comments translated, the unused VOXEL_EMPTY and
  VOXEL_SOLID aliases removed. The shop-ring angle has one definition
  (city_shop_angle), the floor generator one entry point (floor_init).
  The source lists of the server and the shared code are defined once in
  cmake/DragonGL.cmake and used by every CMake project.
- TESTS: line coverage of src/server and src/shared is measured with
  llvm-cov over the unit and integration suites, the servers started by
  the network tests included: 48.4% (functions 75.8%), the floor of the
  coverage gate. The HTML report is a CI artifact.
- TESTS: the integration suite also runs under ThreadSanitizer, the
  network tests with a TSan server: no data race. A sanitizer report
  (ASan, UBSan, TSan, LSan) in the log of a server started by
  test_server.py now fails the test; before, only the exit status was
  looked at and a report could go unnoticed.
- TESTS: the in-process fixture of the harness (game data, world, NPC
  table, socketpair players) is shared in tests/integration/fixture.h.
- FUZZING: tests/fuzz holds three libFuzzer targets built with ASan and
  UBSan: fuzz_wire (one frame into the decoder of its type), fuzz_persist
  (a payload, wrapped in a valid container, into the reader of player,
  NPC, tombstone, flag and world files; or a raw file for the container
  checks) and fuzz_text_cmd (a MSG_TEXT_CMD into handle_text_cmd, for a
  player and the DM, in a generated city with its merchants, restored
  before every input). The targets write their seeds from the encoders
  of the tree, so no binary file is kept in the repository. CI fuzzes
  60 s per target, a nightly workflow 20 minutes per target on a corpus
  kept between runs. About an hour of CPU time found no defect.
- CI: new jobs for format, game data and metrics, clang-tidy, coverage,
  ThreadSanitizer and fuzzing; CodeQL code scanning (security-and-quality)
  on main and weekly; the nightly fuzzing workflow.
- DATA: data/schema/ holds a JSON Schema (2020-12) for bestiary.json,
  items.json and spells.json, and tools/check_data.py checks the schema,
  the references the engine resolves by name (book spells, cast_spell),
  unique names, the classes of every book against its spells, that every
  spell is in a book, and the canonical form, the one the Data Studio
  writes (2-space indentation, UTF-8). Run in CI and in the RPM %check
  (BuildRequires: python3-jsonschema).
- DATA: what the checks found is fixed. Two monsters were exact copies of
  the one before them (Feline Stalker, Silent Night-Hunter) and are gone;
  the rarity 'VERY RARE' of 9 items is VERY_RARE like the others (Ring of
  Shooting Stars: its attunement note moved to the properties); costs are
  numbers, not strings; the 70 items without a description (36 weapons,
  4 ammunition packs, 30 pieces of gear) have one; the base weapons have
  their damage type; the Trident deals 1d6 (it was 1d7) and the Longbow
  is not a 'loading' weapon; weapon properties use one separator.
- SAVES: npcs.dat stores a fingerprint of the bestiary (format version
  2). Saved NPCs refer to their monster by index, so a file written for
  another bestiary (a monster added, removed or renamed) is refused, and
  the server generates a new world instead of binding NPCs to the wrong
  monsters. Worlds of the previous release are regenerated once at the
  first start; characters are kept.
- DOCS: DEVELOPMENT.md describes the quality checks, the CI jobs and two
  new contribution rules (numbers from text through strnum.h, data
  changes with their schema); BUILD.md and CONTRIBUTING.md point to
  tools/quality.sh.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.16
- SHADER: mark gpd_scene_verts SSBO as readonly in the GPD scene vertex
  shader (gpd_scene.vert). Fixes Vulkan validation error
  VUID-RuntimeSpirv-NonWritable-06341 when vertexPipelineStoresAndAtomics
  is not enabled.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.15
- PACKAGING: rpmlint passes on Fedora. The descriptions no longer use
  words the Fedora en_US dictionary rejects ('procedurally', and 'conf'
  from the configuration path), and the rpmlintrc filter for
  requires-on-release is gone: Fedora's rpmlint does not emit that
  warning, and an unused filter is itself an error.
- CI: the RPM job installs glibc-langpack-en, so the en_US.UTF-8 locale
  of the runner exists (no setlocale warnings from rpm's shells).

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.14
- SPELLS: the 12 class transit cantrips are learned from a book and cast
  only with it, like every other spell. Each class has its own travel
  tome (Totem of the Ancestors, Songbook of the Last Refrain, Book of
  Divine Recall, Leaves of the Green Path, Warrior's Marching Orders,
  Treatise of the Flowing Step, Codex of the Oathbound Passage, Journal
  of the Hunter's Trail, Notes on the Back Door, Grimoire of the
  Bloodline Rift, Pact of Passage, Primer of Arcane Transit; 2,000 gp),
  sold by the book shops of floor 0 and studied in the class temple. The
  'innate' spell field is gone: from spells.json, the loader, the login
  (which granted the spells) and cast (which skipped the book check),
  and from the Data Studio spell editor. The books are appended at the
  end of items.json, so the item indexes in the saves do not move.
- COMMANDS: a prefix matches a whole word. Every prefix of the command
  table ends with a space, so 'trainer', 'prayer', 'useless', 'getaway'
  and the like are no longer taken for train, pray, use, get. 'pray'
  alone is the prayer (100 XP for a quarter of the HP), 'pray <book>'
  the cleric ritual: the ritual used to catch the bare word too.
- COMMANDS: a command typed without its arguments answers with its usage
  ('Usage: buy <n|item>', 'Usage: dm_goto <floor> <x> <y>', ...); any
  other unknown input gets "Unrecognized command". 'talk' far from a
  merchant says so instead of "Unrecognized command 'talk'". The dm_*
  usages are shown only to the DM.
- FIX: 'train con' raised nothing: the code compared the stat with
  "with" (a translation slip for "con"), so Constitution could only be
  raised by 'train random'. train takes the abbreviation or the full name
  of the ability, or random, and nothing else ('train strawberry' used to
  raise Strength).
- FIX: dm_shop with an item id outside the item table stored it in the
  merchant (out-of-bounds read when the shop was listed); it now says
  which ids are valid, and says when no merchant is within 5 steps.
  dm_spawn, dm_place and dm_goto without valid arguments show their usage.
- DOCS: README's package table had the old table appended to it (a
  broken edit of 2026.11.08.12); the command table documents train,
  pray, p and the books. DEVELOPMENT.md describes word prefixes and
  usage lines. Help lines of dm_shop, dm_place and dm_find_* corrected.
- CODE: the last Italian comments translated.
- TESTS: the harness checks that every prefix ends with a space, that
  near-miss words are rejected, the usage answers, train for every
  ability, pray and p, and that each transit cantrip has exactly one
  book of its class, sold in the city, learned only in the temple and
  cast only with the book.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.13
- SERVER: text commands are a table. handle_text_cmd() (4,400 lines in
  one function) is now a COMMANDS[] table in server_commands.c, each row
  with its exact names and/or prefixes, and one handler per command in
  one file per group: cmd_character.c, cmd_social.c, cmd_shop.c,
  cmd_items.c, cmd_magic.c, cmd_world.c, cmd_dm.c (the dm_* commands
  have their own table and run only for the dm account). The rows keep
  the old order and first-match rule, so every command parses as before:
  the bodies were first moved unchanged (checked token by token) and the
  match rules compared with the old conditions on about a thousand
  inputs; the changes below came after.
- SERVER: MSG_MOVE moved out of main() into server_move.c: player_move()
  with the rate limit and the conditions, bump_into() (merchant, loot,
  monster, player), pick_up_loot() (gold, ghost, dropped item, chest) and
  step_onto() (terrain, shop doors, bleeding, tile events, traps). main()
  is now option parsing, start-up, run_server() and shutdown_server(),
  with the message dispatch in handle_message()/serve_client().
- SERVER: O(1) entity lookup. npc_by_entity_id() reads an id -> slot map
  rebuilt with the floor index (a fresh spawn is found by one scan); a
  step into a monster, a merchant or loot no longer scans the 50,000 NPC
  slots.
- SERVER: the per-floor NPC index is exact and used everywhere. It is
  kept sorted and npc_set_active() inserts every NPC that becomes
  active, so floor_iter()/floor_iter_next() return the same NPCs, in
  the same order, as a scan of all the slots (spawns during the walk
  included). The merchant, look, examine, get, retrieve, throw, cast
  auto-target, dm_boss, dm_merchant and dm_shop lookups, the body-swap
  trap, the boss check of the stairs, the swarm split, the aggro of an
  attack and the entity broadcast walk only the NPCs of their floor.
- SERVER: no function longer than 300 lines (the longest was 4,395).
  update_world() is split into autosave, global event, player round,
  trap and crystal respawn, NPC round and density monitor; check_traps()
  into detection, wall traps, saving throw, damage, world effects and
  re-arming; the item and spell loaders into one parser per item
  category and per spell field; the city of floor 0 into
  generate_city(). Same game data (compared field by field) and same
  maps for the same seed.
- FIX: 'open <dir>' and 'close <dir>' (and 'o <dir>') work. Only the bare
  words matched, and they always answered "Use: open <n|s|e|w>". 'c <n>'
  stays cast.
- FIX: the 12 class transit cantrips are innate again (known without a
  book): the loader set the flag and then reset it with the defaults.
- CODE: comments describe the current code; the history they told
  (defect ids, old behaviour) is in this changelog. DEVELOPMENT.md
  documents the server layout, the command table, npc_set_active, the
  floor walk and the 300-line rule.
- TESTS: the harness checks the command table (known and unknown
  commands, DM commands), the doors, the innate spells, the floor walk
  against a full scan (after spawns and deaths, during a walk, on a
  floor over the index cap), the entity lookup and player movement
  (step, wall, rate limit, gold pick-up, merchant).

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.12
- BUILD: single version source. CMake reads the 'Version:' line of
  dragongl.spec (YYYY.MM.DD.NN, checked) and generates dragongl_config.h
  (DRAGONGL_VERSION, DRAGONGL_PKGDATADIR) and the man pages from
  man/*.1.in, so binaries, man pages and package always carry the same
  version and date. Both binaries accept -V/--version.
- BUILD: distribution-friendly flags in cmake/DragonGL.cmake, shared by the
  main, unit-test and integration projects. CMake's own optimisation level
  instead of a forced -O3; DRAGONGL_HARDENING (default ON: PIE,
  -fstack-protector-strong, -D_FORTIFY_SOURCE=2 outside Debug and only if
  the flags do not set it already, full RELRO), turned off by the RPM whose
  %optflags do the same. New options DRAGONGL_BUILD_SERVER,
  DRAGONGL_BUILD_CLIENT (server-only builds need neither Vulkan, GLFW nor
  glslc), DRAGONGL_INSTALL_SYSTEMD_UNIT and DRAGONGL_SYSTEMD_UNITDIR. The
  install follows GNUInstallDirs, with the configuration under
  CMAKE_INSTALL_FULL_SYSCONFDIR (/etc, never /usr/etc). build.sh -S now
  configures a server-only build; build.sh -x stages with the configured
  prefix (paths are fixed at configure time, so --prefix /usr at install
  time gave a mixed layout).
- SERVER: data directory found without looking at the current directory:
  --data-dir, $DRAGONGL_DATA_DIR, data/ of the source tree when the binary
  runs from a build directory inside it, then <datadir>/dragongl/data of
  the install prefix. Without data the server exits with status 2 and says
  how to fix it. The DM PDF generator and the client shaders are looked up
  under the install prefix instead of a hard-coded /usr/share. The
  development launcher no longer changes directory.
- PACKAGING: systemd unit dragongl-server.service (DynamicUser, state in
  /var/lib/dragongl with mode 0700, no restart after a configuration error,
  full sandbox: 'systemd-analyze security' exposure 1.2) and its
  environment file /etc/dragongl/dragongl-server.conf (root-only, 0600,
  %config(noreplace)) for the passwords and extra options.
- PACKAGING: dragongl.spec split by role. dragongl is the client and its
  shaders (Recommends mesa-vulkan-drivers); the new dragongl-server holds
  the server, launchers, unit and configuration and requires dragongl-data;
  dragongl-data, dragongl-tools and dragongl-doc are noarch. A player
  machine no longer installs the server and libsodium, a server no longer
  installs Vulkan and GLFW. Existing server hosts must install
  dragongl-server. License is GPL-3.0-or-later only: the CC BY 4.0 artwork
  of readme_assets/ (notice in readme_assets/LICENSE.md) is kept out of
  the tarball by .gitattributes export-ignore (with .github/), which
  shrinks the source archive from more than 500 MB to about 1.3 MB.
  Dropped the unused compiler-rt build requirement. %check runs the
  release check and the unit suite. dragongl.rpmlintrc documents the
  remaining expected rpmlint findings.
- TOOLS: tools/check_release.py checks that the spec version, the newest
  CHANGELOG entry and its date agree, that every entry header is well
  formed with the right weekday, and that dates never go forward and no
  version repeats. Fixed the two headers it found wrong (Thu Sep 24 and
  Wed Sep 16 2026).
- CI: three more checks, seven jobs in all: a server-only build without
  any graphics package (configure, build, install, --version), the RPM
  build with rpmlint in a Fedora container, and the release check in the
  static-analysis job.
- DOCS: INSTALL, README, BUILD, DEVELOPMENT, LORE and the man pages
  describe the package split, the systemd service, the CMake options and
  the new data directory order. BUILD.md no longer refers to the missing
  local_check.sh script and lists the protocol as v4.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.11
- CLIENT: single renderer, Vulkan 1.3 GPU-driven. Removed the OpenGL
  backend (render_gl.c/.h) and the Vulkan CPU-vertex path (DRAGONGL_GPD=0,
  update_vertex_buffer and its mesh cache): the scene is always culled and
  generated by the compute shaders. The client no longer links OpenGL/GLU.
  Usage is now 'dragongl_client [server_host [port]]' (no backend
  argument); the launcher rejects 'gl'/'vk' with an explanation.
- VULKAN: target 1.3 instead of the 1.4 polyfill (a hand-written
  VkPhysicalDeviceVulkan14Features with a guessed sType that the validation
  layer rejected). Only the features used are enabled: dynamicRendering and
  synchronization2 (no longer multiDrawIndirect, fillModeNonSolid,
  drawIndirectCount, bufferDeviceAddress, maintenance4, pushDescriptor).
  The loader version is checked before creating the instance.
- VULKAN: device selection. Every GPU is listed at start-up with the reason
  it is unusable (no Vulkan 1.3, no swapchain, no graphics+compute queue
  that can present to the window, no format/present mode); the best one
  wins (discrete > integrated > virtual > software, then device memory).
  DRAGONGL_GPU=<index|name> overrides the choice. It used to take device 0
  and never checked that its queue could present to the window.
- VULKAN: frame synchronization. The fence of the frame slot is waited
  BEFORE vkAcquireNextImageKHR (the image semaphore could still be pending:
  VUID-vkAcquireNextImageKHR-semaphore-01779 on every frame); one
  render-finished semaphore per swapchain image instead of per frame slot;
  the fence is reset only once an image was acquired. The shared depth
  image gets a layout transition and a write-after-write barrier every
  frame (it was used in an undefined layout). The counters buffer cleared
  with vkCmdFillBuffer now has TRANSFER_DST usage
  (VUID-vkCmdFillBuffer-dstBuffer-00029 on every frame).
- VULKAN: swapchain recreation on VK_ERROR_OUT_OF_DATE_KHR and
  VK_SUBOPTIMAL_KHR (acquire and present) and on GLFW resize events, with
  oldSwapchain, an extent clamped to the surface limits and a minimized
  window waited out. Viewport and scissor are dynamic state, so pipelines
  survive a resize (rebuilt only if the colour format changes). sRGB 8-bit
  surface format preferred; depth format chosen among those the device
  supports; DRAGONGL_VSYNC=0 selects MAILBOX/IMMEDIATE. Device loss and
  other fatal errors close the window with a message instead of looping.
- VULKAN: init failures are checked (buffers, memory types, shader modules,
  semaphores, fences) and vk_cleanup() is safe after a partial init. The
  indirect-command and counter buffers are device local (only the GPU
  touches them). Shaders are read from $DRAGONGL_SHADER_DIR, next to the
  executable or /usr/share/dragongl/shaders, no longer from the current
  directory.
- WINDOW: no more fixed 1920x1080 window. It opens at 80% of the primary
  monitor's work area (DRAGONGL_WINDOW=WxH to choose), is resizable (min
  640x480) and titled "Dragon GL". The minimap and the right-hand panels
  were off-screen on displays smaller than 1920x1080.
- CLIENT: stdout is line-buffered, so the [VK]/[NET] messages reach a log
  file as they happen; the console says "Server host" and "Dragon GL
  Client".
- TESTS: new client smoke test (tests/client/test_client.py, './build.sh
  -k', new CI job under ASan/UBSan): a real server and the real client on
  Xvfb with Mesa lavapipe and the Khronos validation layer; it logs in,
  checks that frames are drawn, resizes the window three times, quits with
  Escape, and checks that the client exits when the server stops. Any
  validation message fails it (the previous client produced two errors per
  frame). CI no longer installs OpenGL/GLU.
- PACKAGING: dragongl.spec drops the gl/glu BuildRequires; summary and
  description name Vulkan only. The dragongl-client launcher is rewritten
  (finds the binary next to itself or in build/, no 'cd'), with new
  dragongl_client(1) and dragongl-client(1) man pages (rendering, GPU
  choice, environment variables).
- DOCS: README, HOWTO, BUILD, INSTALL, DEVELOPMENT and LORE describe the
  single Vulkan renderer, its requirements and the new client usage.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.10
- PROTOCOL: version 4, incompatible with 3. Every message is serialized
  field by field, little-endian, by one module shared by server and client
  (new include/wire.h, src/shared/wire.c): no struct is sent or read as raw
  memory any more, so the format no longer depends on the compiler, the
  struct padding or the host byte order. 8-byte header (u16 version, u16
  type, u32 length); strings are u8 length + bytes; payloads must decode
  exactly (trailing bytes, oversized strings, out-of-range counts, enums,
  coordinates and tile values are protocol errors and close the
  connection). Frames are smaller: a MSG_TEXT carries its text instead of
  256 fixed bytes, a MSG_STATE shrinks from ~1.1 KB to a few hundred
  bytes (the equipment names travel at their real length).
- PROTOCOL: 64-byte credential fields. Personal and server passwords may be
  up to 63 characters (were 31); server secrets 8-63 characters.
- PROTOCOL: MSG_LOGIN keeps only what the server uses (credentials, race,
  subrace, class, alignment); the client-rolled ability scores and the
  cosmetic age/height/weight/social class are no longer sent. MSG_MOVE is
  two bytes (dx, dy in -1..1); MSG_WELCOME drops the always-1 'success'.
- PROTOCOL: new MSG_COMBAT_EVENT (kind, entity, amount) for damage taken
  and dealt, critical hits, misses, healing, gold and level-ups. The client
  draws the floating combat text from it; fct_parse_log(), which guessed
  the events from the English wording of MSG_TEXT lines (and broke on the
  Italian "Colpito! ... danni" line), is removed. Those lines now read
  "Hit! ... damage", "RISEN TO LEVEL" and "You pick up a stack of".
- SERVER: frames are encoded into one reused buffer and queued with a
  single send; broadcasts (spell effects, "left the floor") are encoded
  once for every receiver. The receive path validates the header and the
  per-type length limit before reading the payload.
- NETWORK: the server listens on IPv6 and IPv4 (dual-stack socket, IPv4
  only where IPv6 is unavailable); IPv4 peers are logged and rate-limited
  under their plain a.b.c.d address. The client resolves the server with
  getaddrinfo() (host names, IPv4 and IPv6 addresses), tries every address
  with a 5 s connect timeout and reports why a connection failed.
- CLIENT: the network thread reads with poll() into a receive buffer and
  decodes complete frames, instead of sleep-and-retry reads that could
  desynchronize the stream on a slow link; any malformed frame ends the
  session with the reason. TCP keepalive detects a server that vanished, a
  failed send ends the session, and a closed standard input no longer makes
  the console thread spin. The login frame and the typed passwords are
  wiped after sending. (Passwords were already read without echo.)
- CLEANUP: removed net_receive(), net_receive_all() and
  net_receive_exact() (sleep-and-retry readers) and the raw MsgHeader
  struct with its msg_hdr() helper.
- TESTS: tests/test_protocol.c rewritten as encode/decode round trips of
  every message plus malformed payloads; the integration client
  (dglproto.py) speaks v4; new network tests for an old protocol version,
  an oversized login string, trailing bytes, 63-character passwords, the
  level-up combat event, IPv4 addresses in the log and IPv6 loopback.
- DOCS: PROTOCOL.md describes the v4 byte layout of every message;
  SECURITY.md, DEVELOPMENT.md, the launchers and the man pages follow the
  new password lengths and the IPv6 support.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.09
- PERFORMANCE (server): saves no longer stall the game loop. New writer
  thread (src/server/writer.c): the loop encodes the snapshot in memory and
  the thread writes, fsyncs and renames it. A newer snapshot replaces one
  still queued for the same file, so the queue holds at most one job per
  file whatever the disk speed. Player saves, the 60 s autosave of
  world.dat and npcs.dat, tombstones (writes and removals, in order) and
  artifacts.dat all go through it. At shutdown the queue is drained and the
  final saves are written synchronously.
- PERFORMANCE (server): logins no longer stall the game loop. New login
  worker thread (src/server/login_worker.c) for the DM password check, the
  save read and Argon2id (verify, or hash for a new character), about
  50-100 ms per login that used to pause every player. The result comes
  back through an eventfd in the epoll set and is applied only if the slot
  still holds the same connection (generation counter). While a login is
  checked the slot reads nothing more, its name counts as online (two
  connections can no longer create the same character at once), and the
  messages sent behind MSG_LOGIN are handled once it succeeds. A login
  waits for any pending write of its save, so a relogin right after a
  logout always sees the latest save.
- PERFORMANCE (server): dm_pdf / dm_mapfloor run the PDF generator in the
  background (posix_spawn, reaped with waitpid(WNOHANG) once per loop pass)
  instead of blocking the server until it exits; at most two at a time and
  one per floor, the DM is notified on completion.
- PERFORMANCE (server): 'top' scans saves/ at most once a minute instead of
  reading every save at each call; characters online are taken live from
  memory (a new character is listed before its first save).
- SECURITY (server): the PDF generator script is no longer looked up in the
  current directory (only next to the server binary and in
  /usr/share/dragongl/tools). The listening socket, the epoll descriptor
  and the combat log are now close-on-exec like the client sockets and the
  state lock, so a child process inherits none of them. listen() backlog
  raised to SOMAXCONN.
- FIX (server): server_log() is thread-safe (localtime_r and the stdout
  stream lock); the client table is zero-initialized (calloc).
- REFACTOR (server): login split into login_begin() / login_finish();
  load_player_data() kept as the synchronous path (tests) on top of the
  same job code; save/load error messages logged once, by the writer.
- TESTS: harness tests writer_queue, login_job_outcomes and
  login_worker_async; network tests for a relogin right after logout,
  concurrent creation of one character, the game loop answering during a
  burst of logins, messages queued behind MSG_LOGIN, a doubled MSG_LOGIN,
  the leaderboard with characters in play and the background DM PDF.
- DOCS: SECURITY.md, PROTOCOL.md, DEVELOPMENT.md, LORE.md and
  dragongl_server(1) describe the server threads and the asynchronous
  logins, saves and PDF maps.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.08
- PERSISTENCE (server): new state directory for everything the server
  writes (world, NPCs, artifacts, characters, tombstones, combat log, DM
  maps). Chosen by -S/--state-dir, $DRAGONGL_STATE_DIR,
  $XDG_STATE_HOME/dragongl or ~/.local/state/dragongl; created with mode
  0700 and locked with flock (a second server on it is refused). The data
  directory is now read-only: installed from RPM, the server used to write
  world.dat into /usr/share/dragongl/data, fail silently and generate a
  new world at every start.
- PERSISTENCE (server): portable on-disk formats. New include/binio.h /
  src/shared/binio.c (little-endian field writer/reader and a container
  with magic, version, length and CRC-32) and src/server/persist.c, which
  serializes saves, world.dat, npcs.dat, tombstones and artifacts.dat field
  by field. No raw struct and no pointer reaches the disk any more; counts,
  enums and coordinates are validated on load and the payload must be
  consumed exactly. NPC ghost loot is stored sparsely: npcs.dat goes from
  ~130 MB to ~11 MB.
- PERSISTENCE (server): every write reports its result. world/NPC save
  failures are logged with the reason, a new world that cannot be saved
  stops the server, and the log no longer claims "saved" when it was not.
  Writes fsync the directory after the rename. The world, the NPCs and the
  artifacts are also saved at shutdown.
- FORMATS: save v3, world v4, npcs/tombstone/artifacts v1. Older files are
  refused (an unreadable world means a new one): run dragongl-reset-world.
- CLEANUP: removed the legacy bones_*.dat reader (no code wrote those files
  any more), the single-file tombstone_load(), the old-name tombstone
  cleanup, the raw-struct world_save()/world_load() of map.c and the
  atomic_write_* / cleanup_stale_tmp_files helpers.
- LAUNCHERS: dragongl-server accepts -S/--state-dir and keeps a development
  tree's state in the git-ignored state/. dragongl-reset-world rewritten: it
  resolves the state directory like the server, refuses a directory
  without saves/ and refuses to run while the server holds the lock;
  -y/--yes skips the confirmation.
- TOOLS: analyze_balance.py finds combat_log.json in the state directory.
- TESTS: round trips of world.dat, npcs.dat, tombstones and artifact flags
  (with corruption detection); network tests for the state layout, the
  read-only data directory, an unusable state directory, a second server
  on the same state, a corrupt world (new world generated) and a corrupt
  save (login refused, file kept).
- DOCS: dragongl_server(1), dragongl-server(1), dragongl-reset-world(1),
  INSTALL.md, SECURITY.md, README.md, DEVELOPMENT.md and LORE.md describe
  the state directory and the formats.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.07
- SECURITY (server): no default credentials. The server does not start
  without a server access password, and the DM account exists only when a
  DM password is configured (dragongl_secret / dragongl_dm_secret are gone).
- SECURITY (server): passwords are never accepted on the command line
  (-p/--password and -d/--dm-password are refused: ps shows them to every
  user). They come from --password-file / --dm-password-file or from
  DRAGONGL_SERVER_PASSWORD / DRAGONGL_DM_PASSWORD, which the server wipes
  in place at start-up (gone from /proc/<pid>/environ). Server passwords:
  8-31 printable ASCII characters without spaces (31 is what the client can
  send). Unknown options and an invalid --port are now errors.
- SECURITY (server): new module src/server/auth.c on libsodium. Character
  and DM passwords are Argon2id strings (crypto_pwhash_str, interactive
  limits, random salt); the gate password is a keyed BLAKE2b digest
  compared in constant time. The hand-written SHA-256/PBKDF2 code (4
  iterations) is removed. The plaintext is verified once at login and wiped
  (MsgLogin copy, receive buffer); the session keeps only the hash, so
  saves no longer re-hash a stored plaintext. Empty or control-character
  personal passwords are refused.
- SAVE FORMAT: SAVE_VERSION 2 (Argon2id string). Version 1 saves are
  refused as unreadable: reset the world (dragongl-reset-world) or remove
  saves/*.save.
- SECURITY (server): control characters are replaced in text commands, so
  chat and private messages can no longer carry terminal escape sequences
  to other players. The RNG seed is no longer written to the log.
- LAUNCHER: dragongl-server takes --password-file / --dm-password-file,
  otherwise uses the environment or asks on the terminal (no echo); it runs
  the dragongl_server installed next to it, or build/dragongl_server in a
  development tree.
- CLIENT: server and personal passwords are typed without echo.
- REFACTOR (server): the MSG_LOGIN handler moved out of main() into
  handle_login() / login_with().
- BUILD: libsodium is a new dependency (CMake, spec BuildRequires
  pkgconfig(libsodium), CI, BUILD.md, INSTALL.md).
- BUILD FIX (build.sh): switching compiler (-g) inside an existing build
  directory made CMake drop the -D values of that run, so -Werror and the
  sanitizer flags were silently lost. The script now resets the CMake cache
  itself when the compiler changes.
- BUILD: the whole tree (main project, unit suite, integration suite) now
  builds with gcc -Werror as well as clang -Werror. Fixed: strncpy without
  a guaranteed terminator replaced by copy_str (client login fields, party
  invites, trade partner, cloud owner, ghost and tombstone names), snprintf
  truncation of save and script paths, oversized status buffers in the HUD,
  a maybe-uninitialized polygon array, test_gpu_mesh not linking libm.
- SERVER: the NPC behavior is an AiBehavior enum instead of a function
  pointer stored in a void * (not valid ISO C). The NPC layout changed, so
  the npcs.dat magic is now 0xDEAD7ECD: an old file triggers a fresh world.
- CI: the Release -Werror build runs for clang and gcc (matrix).
- TESTS: start-up without a password, command-line passwords refused, weak
  password refused, secrets absent from /proc cmdline/environ and from the
  log, password file, DM disabled without a DM password, DM login, save
  holding only an Argon2id hash, control characters stripped from chat;
  the persistence harness also checks a wrong password is refused.
- DOCS: SECURITY.md, README.md, HOWTO.md, LORE.md and the man pages
  dragongl_server(1) / dragongl-server(1) describe the new rules.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.06
- BUGFIX (server, P1-1): a full backpack (32/32 items) survives a reload.
  load_player_data() treated backpack_count == MAX_BACKPACK as corrupt and
  reset it to 0, deleting the whole inventory on the next login.
- SECURITY FIX (server, P1-2): one session per character. A second login of
  a character already online is refused with "Character already online."
  (two sessions saved independently, the last save won: item duplication).
- SECURITY FIX (server, P1-3): player-to-player trades are re-validated
  when both sides have accepted. 'offer' keeps a byte copy of the item;
  the exchange is cancelled if an offered item changed or is gone, if gold
  is insufficient, if the players moved apart or if a backpack is full.
  Before, an item sold or dropped after 'offer' was still handed over
  (duplication, negative backpack_count) and a full backpack had its last
  slot overwritten. Both characters are saved right after the exchange.
- SERVER: TCP keepalive (60 s idle, 3 probes 10 s apart) on client sockets,
  so a client that vanished without closing releases its character.
- TESTS: P1-1, P1-2 and P1-3 are regular tests; new tests for a normal trade
  and for a trade into a full backpack. No expected failures remain.
- DOCS: PROTOCOL.md and SECURITY.md describe the one-session rule and the
  keepalive.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.05
- PACKAGING FIX: references updated after the renames of
  dragongl_reset_world -> dragongl-reset-world, HOWTO.txt -> HOWTO.md and
  changelog -> CHANGELOG.md. CMake installed the old script name, so
  "cmake --install" (and therefore rpmbuild) failed; the spec listed
  HOWTO.txt in %doc. Man page renamed to dragongl-reset-world.1 and
  cross-referenced from dragongl_server.1.
- PACKAGING: the -doc subpackage ships every Markdown document (INSTALL,
  BUILD, DEVELOPMENT, PROTOCOL, SECURITY, CONTRIBUTING, CODE_OF_CONDUCT,
  ACCESSIBILITY, LORE, CHANGELOG).
- DOCS: HOWTO.md rewritten as Markdown (the license lines of the old text
  file rendered as headings); README.md, INSTALL.md and LORE.md point to the
  new names.
- CI / BUILD: the unit suite is now in the repository, so the CI job and
  build.sh no longer skip it when tests/CMakeLists.txt is missing.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.04
- BUILD: new build.sh, the quick dev loop. Default: Release build of the
  main project + unit suite + server integration suite. Build types -d
  (Debug + ASan/UBSan), -r (RelWithDebInfo), -s (MinSizeRel); -t tests only,
  -n build only, -c clean rebuild, -u / -i one test suite only, -S server
  target only, -a cppcheck as in CI, -x install into build/stage, -w
  -Werror, -g gcc, -j N jobs, -v verbose, -h help. Short options combine
  (-cn, -dwa, -j8), long forms are accepted (--debug, --no-tests, ...),
  conflicting options are rejected. Compiler and sanitizer flags are passed
  to CMake on every run, so switching build type in build/ never keeps stale
  flags; a per-step summary with durations is printed, and the failing step
  is named on error.
- DOCS: BUILD.md documents build.sh (new section 2); DEVELOPMENT.md points
  to it.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.03
- SECURITY FIX (server, P0-1): SIGPIPE is ignored and every send uses
  MSG_NOSIGNAL. Players disconnecting at the same moment no longer kill the
  server (exit status 141). Same fix in the client.
- SECURITY FIX (server, P0-2): non-blocking connections. New module
  src/server/server_conn.c gives every client a receive buffer and a send
  queue: only complete, validated messages are handled and output is
  written when the socket accepts it (EPOLLOUT). A partial header no longer
  freezes the game loop for 5 s, and a slow reader no longer stalls it
  for 1 s per send.
- SECURITY FIX (server, P0-3): accepted connections that cannot be served
  (server full, too many pending logins from the address, address locked
  out) receive MSG_AUTH_FAIL and are closed instead of leaking their file
  descriptor. accept() runs until EAGAIN.
- SECURITY (server): 10 s login timeout; at most 8 unauthenticated
  connections per address; 5 failed logins within 60 s lock the address
  out for 60 s; 2 MiB cap on the output queued for one client.
- SECURITY (server): MSG_MOVE / MSG_TEXT_CMD before login and a second
  MSG_LOGIN on a live session now close the connection (they used to
  desync the stream or re-create the player entity).
- SECURITY FIX (server): MSG_TEXT is zeroed before formatting; the bytes
  after the string used to carry uninitialized stack memory to the client.
- BUGFIX (server): one disconnect path. Errors only mark a connection; the
  main loop reaps it once per iteration (save, entity grid cleanup,
  "left floor" notice, close). A rejected login clears its authentication
  state first, so a wrong password can never overwrite the save. A slot is
  fully cleared when it is reused.
- BUGFIX (server): characters online are saved on SIGINT/SIGTERM (the
  autosave runs only once a minute).
- BUGFIX (client): a server shutdown is detected (the "Server disconnected"
  branch was unreachable) and a truncated message ends the session instead
  of desyncing the stream. net_receive_exact() now distinguishes
  NET_RECV_CLOSED, NET_RECV_IDLE and NET_RECV_ERROR.
- TESTS: P0-1, P0-2 (now 8 hostile sockets, 'who' under 250 ms) and P0-3 are
  regular tests; new tests for server full, login timeout, per-address
  limits, lockout, rejected login not touching the save, messages before
  login, malformed header, save on shutdown.
- CI: the unit-test job is skipped with a warning while tests/CMakeLists.txt
  is not in the repository.
- DOCS: PROTOCOL.md lists the server-side limits; SECURITY.md describes the
  denial-of-service protections.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.02
- TESTS: new server integration suite in tests/integration (standalone
  CMake project, only compiler + CMake + Python 3 required). test_server.py
  starts a real dragongl_server on a free port in a private copy of a
  generated world and drives it through dglproto.py, a protocol-v3 client;
  server_harness links the server sources without main() and tests
  persistence and trade in-process.
- TESTS: the open defects found by the technical review are reproduced and
  marked as expected failures with their id: P0-1 server killed by SIGPIPE
  when players disconnect together, P0-2 one partial header stalls the game
  loop for 5 s, P0-3 file descriptors leaked when the server is full,
  P1-1 full backpack (32/32) emptied on reload, P1-2 same character logged
  in twice, P1-3 trade offer not revalidated on accept (negative
  backpack_count, item duplication).
- BUILD: main() of the server is excluded with -DDRAGONGL_NO_MAIN, used only
  by the in-process harness.
- CI: tests/ is no longer git-ignored, so the unit suite runs in CI again.
  Workflow split into four jobs pinned to ubuntu-24.04: clang Release build
  with -Werror, unit tests (Release and ASan/UBSan, libclang-rt-18-dev
  added so sanitizer builds link), server integration tests under
  ASan/UBSan, cppcheck with --error-exitcode=1.
- CLEANUP: cppcheck false positives suppressed inline with a justification
  (boss bitmask shift, weapon slot pointer); leaderboard buffer zeroed;
  protocol version printed with matching %u arguments in the client.
- DOCS: BUILD.md and DEVELOPMENT.md describe the integration suite and the
  CI jobs.

* Sun Nov 08 2026 Nicola Taibi <nicola.taibi.1967@gmail.com> - 2026.11.08.01
- Refactoring.

* Thu Sep 24 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.24.01
- dragongl.spec updates.

* Thu Sep 17 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.17.02
- VULKAN FIX (client): fixed UI projection for Player Names and
  Floating Combat Text (FCT) in the GPU-driven path. Removed old 2D hardcoded 
  offset calculations and switched to accurate world-space projection via 
  MVP matrix.
- CI/BUILD FIX: Handled missing Vulkan 1.4 API definitions on older systems 
  (e.g., GitHub Actions ubuntu-latest) by safely polyfilling VK_API_VERSION_1_4 
  and VkPhysicalDeviceVulkan14Features if not provided by the system 
  libvulkan-dev.
- CI FIX: Added mesa-vulkan-drivers (Lavapipe) to CI dependencies so headless 
  Vulkan compute tests run correctly instead of skipping.
- CLEANUP: Fixed a compilation warning related to unused variables in render_vk.c.

* Thu Sep 17 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.17.01
- VULKAN GPU-DRIVEN ARCHITECTURE: Ported the Vulkan backend to a modern GPU-driven
  architecture using Vulkan 1.4 dynamic rendering. Compute Shaders now perform culling
  and geometry expansion directly on the GPU for map tiles, dynamic entities, and
  particles via GpdInstance and GpdOriented structures.
- VULKAN UI PARITY: Ported the Floating Combat Text (FCT) and Player Names to the 
  Vulkan backend (previously exclusive to OpenGL), mapping 3D world coordinates to
  the 2D HUD layer and rendering them safely using the existing mutexes. Fixed the 
  Vulkan HUD rendering by ensuring the 2D vertex buffer is correctly bound when 
  using the GPU-driven path.
- FALLBACK PATH MAINTAINED: The legacy CPU-driven vertex generation path 
  (update_vertex_buffer) has been preserved as a fully functional fallback, which
  can be dynamically selected by setting the DRAGONGL_GPD=0 environment variable.

* Wed Sep 16 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.16.01
- BUGFIX (client, threading): fixed the data races on the shared VFX and
  FCT pools (B1). spawn_vfx() and fct_parse_log() run on the NET thread
  (MSG_SPELL_VFX / MSG_TEXT) while particles_update()/fct_update() and
  the GL/VK draw passes run on the RENDER thread, with NO synchronization
  on g_particles[]/g_fct[] at all — a ThreadSanitizer-flaggable race the
  code even documented ("render-thread-only" comment in render_gl.c, the
  "tolerated loose race" in render_vk.c). Each pool now has its own mutex
  (g_particles_mutex, g_fct_mutex): spawn and update take it internally,
  the GL/VK draw passes take it for the whole pass. GL draw_particles()
  also read g_my_x/g_my_y/g_vision_radius live; it now takes px/py/vr
  from the per-frame FrameSnapshot. The two new mutexes are never nested
  with g_state_mutex. Regression test: tests/test_vfx_threads.c
  (concurrent spawn/update/draw hammer — clean under ThreadSanitizer).
- BUGFIX (client, threading): the GL minimap passed the LIVE g_local_map
  (90 KB) to minimap_update() without the lock while the NET thread
  rewrites it under g_state_mutex on every map chunk (B2): it now passes
  the FrameSnapshot copy (snap.map), like the rest of the frame. The VK
  path was already safe (called while holding g_state_mutex) and is now
  documented as such.
- BUGFIX (client, threading): g_map_dirty was a plain bool set by the
  NET thread under the lock and read/cleared WITHOUT the lock by the VK
  render thread (B3) — a data race AND a lost update: the unsynchronized
  clear could swallow a set, leaving the floor mesh stale until the next
  tile change. It is now an atomic_bool and the renderer clears it with
  atomic_exchange so a concurrent set can never be lost (same treatment
  as g_running, L1). The declaration also moved inside the
  client_state.h header guard, where it belongs.
- BUGFIX (client, threading): the GL HUD read ~20 shared scalars (HP,
  floor, status icons, gold, spell slots, equipment names, log lines)
  without the lock while the NET thread writes them all under
  g_state_mutex on EVERY MSG_STATE (B4, found during the B1-B3 audit):
  the HUD pass now runs under g_state_mutex — the VK precedent
  (render_vk_hud is always called under the lock) — and the key-hold
  movement polling uses the movement_cooldown copied into the
  FrameSnapshot instead of the live g_movement_cooldown.

* Tue Sep 15 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.15.03
- BUGFIX (Vulkan): Fix frame flickering caused by shared command and vertex
  buffers between frames in flight. Allocate independent per-frame rendering
  resources and synchronize their reuse through per-frame fences.
* Tue Sep 15 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.15.02
- BUGFIX: AoE kills now call check_level_up() immediately (aoe.c), like the
  melee path — magical kills no longer delay the level-up until the next
  melee kill. Persistent clouds also remember their caster: kills caused by
  a lingering cloud now credit XP (and the level-up check) to the owner
  instead of granting none.
- BUGFIX: The per-floor O(1) NPC stats cache (density monitor) was only
  updated in a few of the ~30 spawn/death sites, so AoE kills, DM spawns,
  summons, drops and ghosts all left the cache stale. New helper
  npc_set_active() (server_world.c) is now the single point where an NPC's
  alive/dead state changes, and it keeps the cache in sync using exactly
  the same accounting as floor_stats_rebuild(). Emergency respawns now
  trigger on time.
- BUGFIX: The three trap death sites (dart wall, spring spear, floor traps
  in main_server.c) set floor_id/x/y/hp by hand instead of calling
  player_respawn_town(): the dead player stayed a "ghost" on other
  clients' screens until the next sync and his own client kept showing the
  dungeon. All death sites now use the helper.
- BUGFIX (Vulkan): particles never decayed on the VK backend —
  particles_update() was only called by the GL render loop. Particles froze
  in place (alpha stuck at 1.0) and the 2000-slot pool saturated after a
  few seconds of magic, after which no new VFX were spawned at all. The VK
  frame loop now updates the simulation once per frame, like GL.
- BUGFIX: the game clock advanced twice — once per tick in update_world()
  (correct, 5/s) and once per player move in the MSG_MOVE handler (leftover
  of the pre-epoll action-based design). The per-move increment is removed:
  day/night, hunger and trap respawns no longer run faster the more players
  walk.
- BUGFIX: tombstone_pickup() deleted the tombstone even when the player's
  backpack was full, so every item that did not fit (backpack overflow,
  belt, equipment, gold) was destroyed forever. Transferred items are now
  consumed from the tombstone; if anything is left behind the tombstone
  stays active (re-saved to disk, original expiry clock) and tells the
  player to come back with free space.
* Tue Sep 15 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.15.01
- BUGFIX: Fix crash server, Client: UB mutex + anti-stuttering, etc.
* Fri Sep 11 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.11.01
- FEATURE: Added the "Data Studio", a set of self-contained HTML workbenches in
  `tools/web/` for full CRUD management of the three JSON databases
  (`bestiary.json`, `items.json`, `spells.json`):
  * `menu.html` — main menu with live record counts and workflow guide.
  * `bestiary.html` — monsters: search, archetype/XP/HP filters, distribution
    chart, detail inspector (defenses, treasure), create/edit/duplicate/delete.
  * `items.html` — items: category/rarity filters with canonical rarity colors,
    cost/AC/damage columns, properties chips, full CRUD.
  * `spells.html` — spells: level/school/class filters, ritual toggle, VFX color
    editor with live preview, full CRUD.
  Data loading is automatic over HTTP and falls back to manual file pick /
  drag & drop (plus direct write-back via the File System Access API in
  Chrome/Edge). Exports are 2-space pretty JSON that preserve every field,
  including engine-only extras (book_seq, cast_spell, vfx, ...).
  Rebuild the generated HTML after editing the _-prefixed source fragments:
  `python3 tools/web/build.py`.
* Thu Sep 10 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.10.01
- OPTIMIZATION: Removed NPC Morale saving throws from the global `combat_log.json`. 
  Because of the server's scale, the log was being spammed with thousands 
  of Morale failure records per minute. This wasn't a bug, but rather the 
  result of natural ecosystem mechanics:
  * `ARCH_SWARM` monsters (like Slimes) halve their HP when cloning 
  themselves. After a few generations, their HP naturally falls below 
  the 20% threshold, triggering the Morale check without player intervention.
  * Wandering monsters taking continuous damage from persistent player AoE 
  spells (clouds) in other floors.
  By silencing the `clog_save` for Morale, the combat log remains clean and 
  focused on actual player combat metrics.
* Thu Sep 10 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.09.03
- FEATURE: Implemented one-way collision for closed shops at night. Players 
  caught inside a shop when the curfew hits can now freely push through the 
  closed door (`VOXEL_WALL`) to exit, while players outside remain blocked 
  from entering.
- BUGFIX (Vulkan): Restored the AR Compass convergence. The projection 
  function now uses absolute global coordinates instead of relative coordinates, 
  fixing the compass floating off-screen.
- BUGFIX (Vulkan): Fixed player names rendering at the NW corner of the map. 
  Names now correctly project using absolute coordinates, anchoring above the 
  characters' heads.
- OPTIMIZATION (Vulkan): Restored liquid animations (water/lava ripples) by 
  moving the calculation entirely to the GPU's Vertex Shader via Push Constants, 
  removing the need for CPU-side geometry updates.
* Wed Sep 09 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.09.02
- OPTIMIZATION: Implemented GPU-side Map Culling (Vulkan 3.3) and 
  Persistent Memory Mapping (Vulkan 3.4).
  * The map geometry is now uploaded once into a static section of a 
  persistently mapped Vertex Buffer (`HOST_COHERENT_BIT`), eliminating the 
  crippling CPU overhead of frame-by-frame map iteration and `vkMapMemory` 
  calls.
  * `shader.vert` and `shader.frag` now use absolute world coordinates and 
  receive the player's position via Push Constants. The fragment shader 
  dynamically discards out-of-bounds pixels using `discard`, achieving 
  zero-cost soft Fog of War on the GPU.
- OPTIMIZATION: Replaced the O(N) `poll()` network loop with Linux `epoll` 
  (O(1) event-driven). The server now scales effortlessly to thousands of 
  concurrent connections without needlessly iterating over idle or empty 
  sockets on every tick.
- REFACTOR: Began decoupling the monolithic main_server.c by extracting 
  all networking and packet broadcasting functions (e.g., net_send_client, 
  send_map_chunk, broadcast_spell_vfx) into a new, dedicated server_net.c 
  module. This shrinks main_server.c by over 500 lines, improving code 
  organization and reducing compilation times.
* Wed Sep 09 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.09.01
- BUGFIX: Fixed an infinite loop in NPC Morale saving throws. NPCs at
  < 20% HP without the Frightened condition were endlessly rolling a save
  5 volte al secondo if they succeeded. Now uses the `morale` field in 
  EntityInstance as a check flag to ensure the Morale saving throw is 
  only rolled once per life. This stops massive combat_log.json spam.
- BUGFIX: g_running was a plain bool shared by three client threads
  (render, net, CLI) — a data race that TSan flags. It is now an
  atomic_bool (stdatomic.h); use sites are unchanged.
- BUGFIX: a failed net_send() (~1 s of retries on a stuck/dead client)
  was ignored by every server caller: the message vanished and the
  client sat in a stale state until a full state that may never come.
  New net_send_client() drops the client on the FIRST failed send
  (save + left-floor notice + close), and net_send()/net_receive_*()
  now fail immediately on permanent errors (EPIPE/ENOTCONN/EBADF)
  instead of spinning the whole retry budget.
- BUGFIX: RESPAWN_TICKS/DENSITY_CHECK/DENSITY_MIN_PCT/RESPAWN_TRAPS_TICKS
  were #defined in both main_server.c and server_internal.h (two sources
  of truth, equal only by luck). server_internal.h is now the single
  source.
- BUILD: CMake no longer force-sets the C compiler to clang (unfair to
  foreign toolchains); the default is now the DRAGONGL_USE_CLANG option
  (ON), overridable with -DCMAKE_C_COMPILER or $CC. cmake_minimum_required
  raised to 3.16 (makes the explicit CMP0072 policy redundant), and
  -D_FORTIFY_SOURCE=2 moved to Release builds only (it is inert — and
  sometimes warned about — with -O0). Applied to the tests project too.
- NETWORKING: wire protocol v3 — MsgMapChunk start/width/height
  narrowed from int32_t to int16_t (a 300x300 grid fits in 16 bits;
  8-byte chunk header instead of 16).
- NETWORKING: players now use the same compact path as NPCs after the
  first full state (per-client seen-bitmap, 12-byte EntityUpdateRec
  instead of a ~1 KB MsgState per player per step: ~60 KB/step with 60
  players on one plane drops to a few hundred bytes). The bit is reset
  on floor change and cleared on every left-floor removal, so a pair
  of players always re-syncs with a full state when they meet again.
- PERFORMANCE: the server poll() timeout went from 5 ms (200 wake-ups/s
  on an EMPTY server) to TICK_MS/2 (100 ms): the fixed-step simulation
  is driven by a real-time accumulator, so game punctuality is
  unchanged.
- CLIENT: documented the 512-slot entity cache limit — on a crowded
  plane, new entities arriving with the cache full are silently
  dropped (feature limit, not a bug); CLIENT_MAX_ENTITIES is the knob.
- REPO: .gitignore now also covers the combat log rotation file
  (combat_log.json.1, up to 10 MB); the 92 MB combat_log.json, world
  state .dat files, saves/ and build/ were already excluded — none of
  them can reach a tarball or the RPM (the data package ships only the
  JSON datasets).
- I18N: player-facing messages are now consistently English (server
  logs, client console and game data were already): fixed the leftover
  Italian and broken ita/eng mix-ins (auth failure reasons, merchant
  welcome, pickup/chest/bleed messages, trap direction names).
- VULKAN: optional validation layers — the client now enables the
  layers listed in the standard VK_INSTANCE_LAYERS environment
  variable when the driver provides them (missing layers are skipped
  with a warning). CI installs vulkan-validationlayers and exports the
  variable, so dev/CI runs can catch synchronization errors early.
* Tue Sep 08 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.08.02
- SECURITY: Replaced legacy FNV-1a password hashing with PBKDF2-SHA256 to 
  prevent offline brute-force attacks; updated SaveData and DM authentication 
  to support the new 64-char digest.
- SECURITY: Enforced NUL-termination for MsgLogin fields (username, password, 
  server_pass) to eliminate potential out-of-bounds reads during authentication.
- PERSISTENCE: Implemented a versioned save format (v1) with a magic header 
  and CRC-32 checksum to detect file corruption and prevent the loading of 
  incompatible legacy saves.
- PERSISTENCE: Fixed the serialization of active effects by storing canonical 
  ConditionType IDs instead of memory pointers, ensuring effects persist 
  correctly across sessions.
- NETWORKING: Updated wire protocol to version 2; expanded EntityUpdateRec.hp 
  to int32_t to prevent overflows for high-HP entities on deep floors.
- NETWORKING: Optimized map transmission by restricting MSG_MAP_CHUNK 
  broadcasts to actual coordinate or floor changes, reducing redundant 
  bandwidth.
- GAMEPLAY: Moved ability score rolls for new characters to the server to 
  ensure authoritative statistics and prevent client-side manipulation.
- GAMEPLAY: Fixed the train command to use rules_get_modifier for 
  Constitution, correcting HP bonus calculation for odd scores.
- LOGGING: Refactored combat_log.json to implement proper JSON escaping, 
  periodic flushing, and size-based rotation to maintain performance and 
  validity.
* Tue Sep 08 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.08.01
- Full technical review of the codebase, II pass, HIGH SEVERITY:
- SECURITY load_player_data: complete sanitization of the state restored
  from .save files, which are untrusted input (write access to saves/
  plus a crash-corrupted file). Previously every field was trusted
  blindly and several are used as array indices:
  - floor_id clamped to [0, MAX_FLOORS), forced to 0 when invalid.
    Before, floor_id=9999 read ~90 KB past the 44 MB world allocation
    in the login map chunk (guaranteed segfault) and floor_id=150
    silently corrupted the heap; the entity_grid write at login was
    equally unguarded;
  - x/y must be strictly inside [0, MAP_WIDTH) x [0, MAP_HEIGHT):
    the old check only rejected >= WIDTH/HEIGHT, so a NEGATIVE x or y
    survived and produced an OOB entity_grid write on the first action;
  - hp clamped to [0, max_hp] and max_hp must be > 0 (fallback:
    new-character default 20 + CON modifier). A negative hp was a
    "dead" character that came back full on the next damage — a free
    heal via a modified save;
  - backpack_count clamped to [0, MAX_BACKPACK), else 0: an invalid
    count made the next pickup write past backpack[32] in an ~8.5 KB
    struct;
  - every ItemInstance restored from disk (32 backpack, 4 belt, 10
    equipment slots, 10 rings) sanitized by a new
    sanitize_item_instance(): template_idx must be -1 or a valid
    item_database index, otherwise the slot is dropped (hostile values
    would OOB-read item_database[]);
  - same class of bug closed for race/subrace/class/alignment (table
    indices, fallbacks identical to the new-character path), level
    [1, 20], xp >= 0, the six attributes through clamp_stat, spell
    slots into [0, max], max_floor_explored [0, MAX_FLOORS), and the
    hunger/light/exhaustion/kills/steps/unspent_stat_points counters;
  - a tampered file is logged (SAVE "state sanitized on load").
- SECURITY top/leaderboard (server_commands.c): the readdir loop now
  stops at the capacity of the stack array. Anyone placing 1001 .save
  files in saves/ crashed the server (or overwrote the
  canary/return address) with the plain `top` command: entries[1000]
  was a fixed stack array and entry_count was never checked.
- SECURITY MSG_TEXT_CMD rate limit (main_server.c): text commands now
  reuse the exact 200 ms last_action_ms guard of MSG_MOVE (max 5/s
  per client). The flag was written but never tested, so `top`, `cast`
  and `look` could be spammed at 10 kHz and saturate the server CPU —
  each `top` alone does fopen+fread+qsort over every save file.
- save_player_data is now an atomic write: the save goes to
  saves/<user>.save.<pid>.tmp, is fsync'ed, then rename()'d over the
  final name (atomic on the same filesystem). On any write/rename
  error the temp file is deleted and the last good save is kept, so a
  crash or kill mid-write can no longer leave a truncated .save that
  the next login would half-read (the load-side sanitization above
  remains the last line of defense).
- All the other persistent state is written atomically too, through a
  shared pair of helpers (atomic_write_begin/atomic_write_end,
  server_internal.h): npcs.dat (both the fresh-world write and the
  periodic autosave — a truncated npcs.dat used to be read as
  "world/NPCs inconsistent" and trigger a total world regeneration),
  artifacts.dat (format byte-identical, per-field fwrites) and the
  tombstone files. world_save already used temp+rename; it now also
  fsyncs before the rename so the data is on the device when the name
  lands. At boot, cleanup_stale_tmp_files() deletes any *.tmp left by
  a crashed run from saves/ and the data dir (a stale
  <user>.save.<pid>.tmp would otherwise appear as a duplicate entry in
  the 'top' leaderboard scan). Verified with a live smoke test:
  stale .tmp files in saves/ are gone right after boot, world +
  artifacts + tombstones load clean.
* Mon Sep 07 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.07.01
- The Archive of a Thousand Battles (BOOKS_MARTIAL, the NE bookshop) now
  has a living floating tome in BOTH 3D backends. The book flies a random,
  meandering trajectory around its merchant while staying INSIDE the shop
  room, and tumbles on itself with a random, smoothly-changing rotation.
  Shared animation module src/client/client_tome.c(.h): a per-entity-slot
  state that (a) measures the room by scanning the local map from the
  merchant tile out to the nearest rock/wall/door/water/lava in the four
  cardinal directions (shrunk by a wall margin, so the book never leaves
  the shop; a conservative fallback box is used until the map chunk
  arrives), (b) re-rolls a random orbit waypoint (angle+radius around the
  merchant, clamped to that box) every 1.2-3.5 s and steers toward it with
  a first-order velocity lag, and (c) re-rolls a random spin (random unit
  axis x random speed) on each waypoint and integrates the orientation as a
  4x4 rotation matrix. render_gl.c draws it with a new draw_cube_oriented()
  (translate -> rotation matrix -> scale) and render_vk.c with a new
  push_box_oriented() (36-vertex box, corners AND face normals rotated in
  CPU space, so the diffuse lighting follows the tumble). Both renderers
  call the same tome_anim_update(), so GL and VK show the identical path.
  New ctest unit: tests/test_tome.c (bounds, wander, orthonormal rotation,
  fallback + reset paths).
- Full technical review of the codebase (src/shared, src/server,
  src/client, include, tools, shaders, CMakeLists, RPM spec, launch
  scripts, JSON data) and the resulting fix rounds, tagged FIRST
  ROUND / SECOND ROUND below. The audit found a guaranteed buffer
  overflow with the shipped data (known_spells[] sized for 512 spells
  while data/spells.json contains 551), an unauthenticated DM role
  with shell access, raw write() on non-blocking sockets, O(50k)
  scans in the AI hot path and a 73 MB voxel world; every finding is
  addressed in the two rounds below.
- FIRST ROUND SECURITY known_spells: MAX_SPELL_DB_SIZE raised 512 ->
  1024 and a defensive `idx < MAX_SPELL_DB_SIZE` guard added at every
  bit set/get site (load_player_data innate-class loop, study/spells/
  cast gatekeepers, entity helpers). With the current dataset the
  innate class cantrips sit at indices 539-550, so the old code wrote
  past the bitfield on every login of the affected classes, corrupting
  the fields right after it (bosses_defeated, needs_study,
  unspent_stat_points).
- FIRST ROUND SECURITY DM role: knowing the server access password no
  longer grants DM powers. The "dm" account now also has to present a
  dedicated DM password (-d/--dm-password), hashed with its own salt;
  on mismatch the connection is closed with MSG_AUTH_FAIL. Knowing the
  server password alone no longer unlocks dm_gold, dm_level, dm_spawn,
  dm_goto or the dm_pdf/dm_mapfloor shell path.
- FIRST ROUND SECURITY login: usernames are validated against
  [A-Za-z0-9_]{1,16} and rejected otherwise (a hostile name could
  otherwise escape saves/ via "../" in the .save and tombstone file
  names), and the six attributes are clamped to 3-18 server-side, so a
  modified client can no longer send str=9999.
- FIRST ROUND SECURITY passwords: all hashes are now salted (character
  salt derived from the username, so identical passwords hash
  differently per character; the DM account has its own salt) instead
  of the old unsalted 8-hex FNV-1a. A boot-time WARNING is logged when
  the default access password or the default DM password are in use,
  and the lack of encrypted transport is documented as LAN-only.
- FIRST ROUND SECURITY traps: TRAP_FALLING_FLOOR now scatters within
  bounds and guarantees a walkable landing via find_walkable_near()
  (the old `x += rand()%5-2` had no bounds check and could push the
  player to x<0 near the map edge, OOB map access on the very next
  tick); TRAP_TELEPORT/TRAP_FAKE_DOOR only land on walkable tiles —
  never in rock, on a staircase (involuntary descent) or on an
  occupied cell.
- FIRST ROUND SECURITY doors: open/close now bounds-check the adjacent
  tile; 'open' only works on a real VOXEL_DOOR and registers the tile
  in a per-floor "recently opened doors" list; 'close' can only
  restore a registered door. This kills the exploit that turned any
  adjacent floor tile into a door and persisted it in world.dat,
  sealing corridors permanently.
- FIRST ROUND NETWORKING broadcast_spell_vfx() now uses net_send()
  instead of raw write() on the non-blocking sockets: a single partial
  write() desynchronized the whole protocol stream of the affected
  client (every later message became garbage). The client sanity-checks
  MsgMapChunk before malloc'ing the chunk buffer (width/height in
  1..256 and hdr.length must match the declared payload size).
- FIRST ROUND COMBAT new_round bug (server_world.c): the round flag
  was a single local shared by the NPC loop, and the FIRST NPC acting
  in a tick cleared it — so only one NPC per whole tick (~1 attack/s
  for the whole server, and poison/burning/paralysis decaying many
  times slower than intended) could act "fresh". The tick now captures
  was_new_round up front and passes it to every NPC; round_active is
  used only for the entity-grid/AoE sync it was meant for. This
  changes the balance of every fight.
- FIRST ROUND COMBAT AI: the Mage's Magic Missile now deals damage (3
  homing missiles, 1d4+1 force each) instead of consuming the slot
  and doing nothing; the Void Crawler's non-attack branch applies
  Mucus only (it also called perform_attack_npc, a double attack per
  tick); swarm clones now respawn where they split, not at the
  parent's original spawn point, and are registered in the
  entity_grid on creation.
- FIRST ROUND GAMEPLAY the cumulative +20 spell-slot bonus on every
  login (a one-shot legacy migration left in the load path) is
  removed: slots no longer grow by 20 per level on each reconnect.
- FIRST ROUND GAMEPLAY city doors re-aligned with the shop ring:
  update_city_doors() now derives the door positions from the same
  formula as map.c (11 shops on the radius-26 circle, door on the
  dominant axis 4 tiles out, facing the center); the 10 hard-coded
  (±20/±12) coordinates were a leftover of the obsolete 1000x1000
  layout and flipped tiles that were not doors, while the real shop
  doors never changed.
- FIRST ROUND GAMEPLAY commands: "t n/s/e/w/d" was intercepted by the
  takeoff alias (first-match-wins) before the tunnel handler could see
  it — the short alias now belongs to tunnel; the second, dead
  'disarm' handler is removed, the disarm logic lives in one place
  only.
- FIRST ROUND GAMEPLAY get_full_item_name() no longer appends "End"
  for QUALITY_FINE (a visible "Fine " typo at every item); walking on
  water no longer causes a guaranteed Prone on every step — only ice
  is slippery.
- FIRST ROUND CLIENT the client no longer calls world_init() at
  startup: it used to generate the full 101-floor dungeon plus traps
  (~73 MB, seconds of CPU) only to copy floor 0 into the local map.
  The local map now starts as rock and is filled by the first
  MSG_MAP_CHUNK from the server.
- FIRST ROUND MEMORY VoxelType is now a uint8_t (33 types need no
  4-byte enum): the world is 4x smaller in RAM (73 MB -> ~19 MB), map
  chunks on the wire are 4x smaller and every world.dat write is 4x
  smaller.
- FIRST ROUND PERFORMANCE broadcast_nearby_entities() filters by
  distance (vision radius + margin, clamped 48-64): it used to ship
  the state of EVERY active NPC on the floor (~300+ MsgState, ~450 KB
  per single step on a full floor) even though the client discards
  anything beyond its own view; entities re-appear automatically as
  the player gets closer, since the broadcast runs on every step.
- FIRST ROUND PERFORMANCE cell_occupied() is an O(1) entity_grid
  lookup instead of an O(50k) scan of every NPC on every AI step — the
  dominant CPU cost of the tick with ~20k active NPCs.
- FIRST ROUND PERFORMANCE saves/ is created with stat()+mkdir()
  instead of system("mkdir -p") (one fork+exec per save_player_data,
  i.e. per client per minute); the RNG is seeded once at process start
  with getrandom() — previously srand(time(NULL)) lived in
  world_init(), which is skipped when world.dat exists, so after every
  reboot the server replayed the same dice sequence until the first
  reseed; npcs.dat now uses a compact [magic 0xDEAD7ECC][count]
  format.
- FIRST ROUND BUILD/PACKAGING the JSON game data is installed to
  share/dragongl/data and the server resolves the data directory
  (explicit --data-dir/-D, then local ./data, then
  /usr/share/dragongl/data), so the RPM no longer fails to find
  bestiary.json when the server runs from /usr/bin; CMake links GLU
  through the GLVND OpenGL package (OpenGL::GLU); the circular
  Requires between the main package and the -data subpackage is
  removed (the main package already requires the subpackage).
- SECOND ROUND SECURITY saved effects: ActiveEffect holds a NAME POINTER, and the
  raw-struct saves (npcs.dat, *.save) serialized it — after a restart
  every saved effect dangled into the previous process's memory and the
  first rules_has_condition() of the tick segfaulted (the current world
  has 3281 NPCs saved with active effects, so the crash was
  deterministic). Effects are now dropped at load time (they are
  transient, a few rounds of poison/fire): no format change, the saved
  fields stay in the file and are ignored.
- SECOND ROUND PROTOCOL versioned wire format: MsgHeader carries PROTOCOL_VERSION
  (+reserved) and every send site builds the header through msg_hdr();
  server and client reject a version mismatch before parsing anything,
  so an old client and a new server fail fast with a clear protocol
  error instead of silently desynchronizing. The server validates
  version+type+length of every incoming header.
- SECOND ROUND PROTOCOL/bandwidth MSG_ENTITY_UPDATE: the per-step entity broadcast
  used to resend a full ~1.5 KB MsgState for EVERY NPC in view on every
  step (~300 messages, ~450 KB on a full floor). The server now keeps a
  per-client "seen" bitmap (reset on floor change): the full state goes
  out only the first time an entity enters the client's view, and every
  following update is a 12-byte record (id/x/y/hp) batched into a single
  message ([int32 count][records], count and length validated on the
  client before the malloc, like the map chunks).
- SECOND ROUND PERFORMANCE fixed-step game clock: update_world() now runs at exactly
  5 rounds/s through a fixed-step accumulator in the main loop (5 ticks
  of catch-up, the excess is dropped, never fast-forwarded). Before the
  fix the simulation ran once per poll-loop iteration (~200/s when
  idle, slower under load) and NPC energy accumulated with it — game
  speed followed the CPU load instead of the clock. 1 round is now
  always 200 ms (1 game day = 1440 rounds = 4.8 real minutes).
- SECOND ROUND PERFORMANCE per-floor active-NPC index: one array per floor, rebuilt
  once per tick; the broadcast on player step, the aggro scans on every
  attack (player and NPC), the swarm split adjacency count and the
  boss-seals-stairs check now walk O(entities on the floor) instead of
  scanning all 50,000 slots (full-scan fallback if a floor overflows
  the cap).
- SECOND ROUND PERFORMANCE A* (pathfinding.c): a real binary min-heap with
  decrease-key (the previous "implicit heap" was a sorted array —
  O(n) per push/pop, O(n^2) overall) and per-node visited stamps
  instead of a 2.9 MB memset on every query (A* runs on every AI step
  of every monster).
- SECOND ROUND PERFORMANCE net_send worst case capped at 100 retries x 10 ms (1 s)
  instead of 200: a single slow/stuck client can no longer hold the
  whole server loop for several seconds.
- SECOND ROUND PERFORMANCE/CLIENT the GL and Vulkan renderers no longer hold
  g_state_mutex for the entire 3D scene: both take a FrameSnapshot
  under a short lock (~120 KB copy) and render from it, so the net
  thread (map chunks, entity moves, input) is not blocked for the whole
  frame.
- SECOND ROUND PERFORMANCE/CLIENT Vulkan: 3 frames in flight (semaphore pair + fence
  per slot, the only wait is on slot reuse) instead of the old
  vkWaitForFences(UINT64_MAX) at the end of every frame that kept CPU
  and GPU from ever overlapping; the vertex-buffer overflow guard now
  stops the whole tile scan (the old break exited only the inner row
  loop); the shared command buffer is explicitly reset before
  re-recording.
- SECOND ROUND COMBAT rules_get_modifier() now implements the DnD floor division:
  plain C truncation returned -3/-2/-1 for scores 3/5/7 instead of the
  DnD-correct -4/-3/-2 (affects every odd sub-10 score). Caught by the
  new unit tests.
- SECOND ROUND CONDITIONS compared by value end-to-end: rules.c owns the canonical
  ConditionType <-> name table (condition_to_name/
  condition_from_name) and the new enum-based rules_has_condition_t();
  all ~44 string comparisons across main_server, server_combat,
  server_world and server_commands now go through the table, so
  renaming an effect string can no longer break the logic silently in
  one place only.
- SECOND ROUND DM TOOLS dm_pdf/dm_mapfloor: the shell command built by snprintf was
  replaced with posix_spawn and a clean argv (python3
  tools/generate_pdf_map.py ...), no string concatenation and no shell
  in between; the wait status is decoded exactly as before. Follow-up
  (same command, caught in play): glibc's posix_spawn() performs no
  PATH search for a slash-less program name (POSIX leaves it
  implementation-defined), so the bare "python3" failed with ENOENT on
  every host — the interpreter is now resolved to an absolute path
  (manual PATH walk, then /usr/bin, /usr/local/bin, /bin) and the
  script is located via the CWD, the server-binary directory (and its
  parent, for the build/ layout) or /usr/share/dragongl/tools, with an
  error naming the searched locations when nothing is found.
- SECOND ROUND GAMEPLAY handle_boss_death design documented in code: the gold reward
  and the bosses_defeated flag are SHARED by every active client on the
  boss' floor on purpose (the stairs are a floor-level gate, so the
  whole floor benefits); the killer argument is intentionally unused.
- SECOND ROUND GAMEPLAY the hunger/time scale is now documented at the constants:
  1 round = 0.2 s real (TICK_MS), HUNGER_MAX 2000 = ~6.7 real minutes
  to starvation, game day = 4.8 real minutes.
- SECOND ROUND BUILD C standard lowered 23 -> 11: no C23 feature is actually used,
  and C11 is the portable floor for the RPM toolchain; the whole tree
  still compiles clean with -Wall -Wextra -Wpedantic.
- SECOND ROUND TESTS new ctest suite: test_rules (dice ranges, advantage vs
  disadvantage, modifier table, damage modifiers, condition-table
  round-trip, effect decay and persistence), test_pathfinding
  (straight path, forced detour through a wall gap, unreachable
  target, input validation, determinism, 1000 repeated queries to
  exercise the stamp reset), test_protocol (pins the wire layout,
  msg_hdr, the MSG_ENTITY_UPDATE payload formula). CI workflow
  (.github/workflows/ci.yml): Release build + tests, then Debug build
  with ASan/UBSan + tests.
- SECOND ROUND DOCS README aligned with the current code: the DM login section now
  describes the dedicated DM password (-d/--dm-password, default
  dragongl_dm_secret) instead of the old "any personal password on the
  dm profile"; the launch section documents -p/-d/-P/--port/-D/--data-dir
  and the client usage [gl|vk] [server_ip] [port]; the PDF maps are
  described as color (v2 dump), not "ASCII"; the door-command, tunnel
  alias and ice-only Prone sections now match the FIRST ROUND fixes
  above. .gitignore: map_dump*.txt/map_floor*.pdf (DM artifacts) and
  artifacts.dat added to the release repository.
* Fri Sep 04 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.04.02
- Updated the README to replace the Italian term “razza” with the English term “races”.
* Fri Sep 04 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.04.01
- dm_pdf/dm_mapfloor: the PDF map legend now lists the coordinates of
  every staircase. The Stairs Up and Stairs Down entries append '@ x,y'
  for each staircase tile found in the dump (Floor 0: the central
  STAIRS_DOWN on the marble island; sub-floors 1-100: the STAIRS_UP in
  the first room and the STAIRS_DOWN in the last room; all coordinates
  are listed when a floor has more than one staircase). The player
  position entry, which already shows the player coordinates, is
  unchanged. Positions are derived from the tile grid itself (v2 voxel
  codes 04/05, legacy v1 '<'/'>'), so no server change is required and
  existing dumps can be re-rendered as-is. Man page
  generate_pdf_map.1 updated.
* Thu Sep 03 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.03.05
- VOXEL doc
* Thu Sep 03 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.03.04
- Floor 0: the second martial statue ring (r=104) is removed. It was a
  pure duplicate of the r=90 statue ring (same 3x3 crystals, same class
  colors, same cardinal axes — only the radius differed), so the r=90
  statues remain the outermost crystal circle. The martial fountains
  (r=97) are untouched, and the 'statue'/'crystal' meditation still works
  at the r=90 statues (it detects any nearby crystal generically). README
  city rings list updated. Requires a world reset (./dragongl_reset_world)
  to take effect.
* Thu Sep 03 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.03.03
- Floor 0: the four broken martial fountains (their 3x3 water sat exactly
  on the magic statues ring r=65 and was overwritten by the crystal
  statues, leaving only the 5x5 borders) are rebuilt on two new outer
  rings, symmetric on the four cardinal axes: fountains at r=97 (Fighter
  N, Barbarian E, Rogue S, Monk W, with marble/cobblestone/ash/marble
  borders) and a second statue ring at r=104 with the class crystal
  colors. The existing outermost crystal ring (martial statues, r=90)
  and the rest of floor 0 are left untouched.
  Requires a world reset (./dragongl_reset_world) to take effect.
- Martial learning rituals completed: the TEMPLES[] rectangles of the
  four dojos now match the actual 11x11 buildings (they were 1 tile too
  wide/tall), so 'drill <n>' (Fighter), 'rage <n>' (Barbarian),
  'sneak <n>' (Rogue) and 'meditate <n>' (Monk) activate exactly inside
  the dojo, like the magic 'study' rituals.
- Ki progression unified for the four martial classes: Barbarian and
  Rogue now unlock Ki tiers every 3 character levels up to L6 (like
  Fighter/Monk; previously capped at L4), so tiers 5-6 of every martial
  codex sold at The Archive of a Thousand Battles are learnable and
  castable.
- README updated: city rings (dojos r=78, statues r=90, fountains r=97,
  statues r=104), martial ritual aliases in the book-commands summary,
  Ki progression table.
* Thu Sep 03 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.03.02
- Man file generate_pdf_map.1 updated
* Thu Sep 03 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.03.01
- dm_mapfloor/dm_pdf: floor dump upgraded to format v2 (2-digit hex
  VoxelType per tile + header with floor size and player position), so
  the PDF renders every tile with its exact in-game color (the legacy
  1-char dump collapsed wood/cobble/ice/sand/ash/mud/marble/traps and
  six crystal colors into '.', and wall with obsidian into '#').
- tools/generate_pdf_map.py rewritten: tile palette kept in sync with
  the in-game radar minimap (client_minimap.c tile_to_color), so the
  PDF matches the 2D top-down view; title + subtitle (size, carved %,
  tile types, dump format, timestamp); coordinate ticks every 25 tiles;
  legend with swatches, labels and per-type counts (auto column layout);
  player position marker; legacy v1 dumps still supported, with a
  footnote explaining the lumps; vector run-length fallback when
  Pillow is unavailable (old reportlab).
- Verified pixel-by-pixel on floors 0 and 1 (22 and 20 tile types).
* Tue Sep 01 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.01.03
- generate_pdf_map.py build error
* Tue Sep 01 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.01.02
- Floor 0: the four outer-ring dojo doors (Gladiator's Arena N, Fighting
  Pit E, Den of Shadows S, Lotus Dojo W) were floating 2 tiles away from
  their temple walls, blocking entry. Each door now sits on the perimeter
  tile facing the city center, like the inner-ring temple doors.
  Requires a world reset (./dragongl_reset_world) to take effect.
- The Archive of a Thousand Battles moved into the city shop ring: the
  floor-0 stores are now 11 buildings evenly spaced (360/11 degrees) on
  the radius-26 circle, with the Archive occupying slot 10 (it stood
  outside the ring at 140,99, breaking the symmetric layout).
  Requires a world reset (./dragongl_reset_world) to take effect.
- New floor-0 shop: The Archive of a Thousand Battles (BOOKS_MARTIAL),
  selling the discipline codices of Fighter, Barbarian, Rogue and Monk.
- book_class_mask is now read exclusively from items.json (name heuristics
  removed); masks corrected for all 72 books (12 classes x 6 tomes).
- Protocol: MsgState carries shop_spec so the client renders the Archive
  with a unique crimson sprite (OpenGL and Vulkan backends).
- README aligned with the code: 11 city shops, 8 errant traders,
  temple names/positions and shop book references.
* Mon Aug 31 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.09.01.01
- First release.
* Tue May 26 2026 nicolataibi <nicola.taibi.1967@gmail.com> - 2026.05.26.01
- DRAGON GL, the beginning!
