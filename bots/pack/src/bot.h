#pragma once
// A pack farmer written from scratch on 2026-11-03: champions farm the richest
// reachable groups, join an ally's claim on rich groups, keep clear of camps
// and enemies they cannot beat, bank bags of forty, and shop on each visit home.
#include <array>
#include <cambc.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace bc = cambc;
using bc::vec2;

inline int dist(vec2 a, vec2 b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }

// Engine settings, read once on first use (the SDK's constants copies may be
// filled before the engine loads them).
inline const bc::detail::game_settings &settings() {
    static const bc::detail::game_settings value = [] {
        bc::detail::game_settings result{};
        bc::detail::check(bc::detail::sdk_get_settings(&result));
        return result;
    }();
    return value;
}

inline int channel_turns(unsigned gems) {
    const int raw = static_cast<int>(std::ceil(gems * settings().submit_gem_duration_multiplier - 1e-9)) +
                    settings().submit_gem_base_duration;
    return std::max(settings().submit_gem_minimum_duration, raw);
}

inline unsigned handle_of(const bc::champion &c) { return static_cast<unsigned>(static_cast<bc::handle_champion_t>(c)); }

struct Group {
    bc::map::camp_group unit;
    bc::bitboard spawns;
    std::vector<vec2> tiles;  // camp spawn tiles
    std::vector<vec2> edges;  // walkable tiles beside the spawn tiles
    vec2 centre{0, 0};
    unsigned gems = 0, gold = 0;
    int respawn_max = 0;
    float health_total = 0, strength_sum = 0, strength_max = 0;
    int range_max = 0;
    bool monolith = false, golem = false, clusters_only = true;
    enum Status { alive, reward, cooling } status = alive;
    int seen = -1000, respawn_at = 0, enemy_seen = -1000, unsafe_until = -1, unreachable_until = -1;
    int enemies_near = 0;
    float health_left = 0;
    bool in_view = false;
    Group(const bc::map::camp_group &g) : unit(g) {}
};

struct Unit {
    unsigned handle = 0;
    vec2 pos{0, 0};
    float health = 0, max_health = 0, strength = 0;
    int range = 1, type = 0;
    std::uint32_t state = 0;
    bool recalling() const { return state & (1u << 1); }
    bool submitting() const { return state & (1u << 2); }
};

struct CampSeen {
    vec2 pos;
    float health, strength;
    int range, group, index;
    bool awake;
};

// Eight bytes of team memory: one claim per body, and the last enemy sighting.
struct Shared {
    std::uint8_t claim[6]{};  // group index + 1, 0 = none; 0x80 = not farming (banking, fleeing, home)
    // The latest respawn timer one of us saw start: group index + 1 and the
    // round it ends, over 4, modulo 256.
    std::uint8_t cool_group = 0, cool_until = 0;
};
static_assert(sizeof(Shared) == 8);

class Bot {
public:
    void play_turn();

private:
    // Fixed world.
    bool ready = false;
    std::vector<Group> groups;
    std::vector<vec2> altars;
    std::vector<std::vector<vec2>> altar_edges;
    bc::bitboard terrain, terrain_jump, any_spawn, own_spawn, holes, bushes;
    vec2 home{0, 0}, enemy_home{0, 0};
    bc::distance_field home_field;  // walking distance to our spawn, terrain only
    // Static route fields, built in spare fuel (one whole-map search costs
    // about 25k fuel on a 100x100 map, so none are searched per turn): to each
    // group's spawn tiles on foot and, for dash champions, over holes a dash
    // crosses; to each altar's tribute tiles.
    std::vector<bc::distance_field> walk_field, jump_field, altar_field;
    std::vector<std::uint8_t> walk_ready, jump_ready, altar_ready;
    std::vector<int> field_tasks;
    // Each champion's home sector: the groups on our half split into one
    // cluster per champion (every champion computes the same split).
    std::vector<int> sector;  // per group: owning unit, -1 outside our half
    // Pairs: slots (0,1) and (2,3) farm together, the leader choosing; a
    // slot left over farms alone. unit = the pair's or single's index.
    int pair_count() const { return 0; } // solo farming: top teams capture alone 93% of the time
    bool paired() const { return int(slot) < 2 * pair_count(); }
    bool leader() const { return !paired() || slot % 2 == 0; }
    unsigned partner() const { return slot ^ 1u; }
    int my_unit() const { return paired() ? int(slot) / 2 : pair_count() + int(slot) - 2 * pair_count(); }
    int units() const;
    void plan_sectors();
    std::size_t field_next = 0;
    std::uint64_t field_cost = 0; // the dearest field so far, in fuel
    bc::bitboard narrow_holes, work;
    std::vector<bc::champion> ally_roster, enemy_roster;
    unsigned slot = 0;  // my index among ally handles
    bc::champion_type type{};
    bc::champion_stats stats{};
    bool melee = true, has_dash = false;
    int dash_slot = -1;

    // Per turn.
    unsigned round = 0;
    vec2 pos{0, 0};
    float health = 0, max_health = 0, strength = 0, focus = 0;
    unsigned gems = 0, gold = 0;
    bool at_spawn = false;
    std::vector<Unit> enemies, allies;
    std::vector<CampSeen> camps;  // camps of alive groups in view
    bc::bitboard visible, blocked, blocked_jump, alive_spawns, awake, scratch, dest, champs;
    bc::distance_field reach, reach_jump, goal, goal_jump;
    bool reach_jump_ok = false;
    bool alive_spawns_dirty = true;
    Shared shared{};
    float jungle = 1.0f, camp_bonus = 1.0f;
    bool moved = false, attacked = false, cast = false;

    // Persistent decisions.
    int target = -1;
    bool banking = false;
    int bank_altar = -1;
    int last_shop_round = -1000;
    int stuck_turns = 0;
    vec2 last_pos{-1, -1};
    int ward_round = -1000;
    int committed_at = -1000;
    // The opening: every champion on one group near home until it is taken.
    int opening_group = -2; // -2 not chosen yet, -1 none
    bool opening_done = false;
    int choose_opening() const;
    std::uint16_t last_cool_read = 0xFFFF;
    void read_cooling();

    void initialise();
    void observe();
    void observe_groups();
    void refresh_equipment();
    void publish(int claim, bool farming);

    // Navigation.
    int walk_to_group(const Group &g) const;
    int jump_to_group(const Group &g) const;
    int walk_to_altar(std::size_t i) const;
    bool go_tile(vec2 tile, bool allow_jump = false);
    bool step_on(const bc::distance_field &field, int stop_at, bool over_holes);
    int field_at(const bc::distance_field &field) const;
    bool go_group(std::size_t g, int stop_at);
    bool go_altar(std::size_t i);
    void plan_fields();
    void build_fields(std::uint64_t reserve);
    bool step_away(const std::vector<vec2> &threats, bool toward_home);
    bool dash_toward(const bc::distance_field &field);

    // Decisions.
    bool shop();
    bool fight();
    bool survive();
    bool bank();
    int choose_target();
    bool farm();
    bool attack_camp(const Group &g);
    void camp_abilities(const Group &g, const CampSeen *focus);
    void fight_abilities(const Unit &target);
    void place_ward();
    float my_dps() const;
    float camp_incoming(vec2 at) const;
    bool safe_to_clear(const Group &g, int claimers) const { return safe_with(g, claimers, health); }
    bool safe_with(const Group &g, int claimers, float hp) const;
    // Whether this champion clears a group alone from full health, cached.
    std::vector<std::uint8_t> alone_ok;
    std::vector<int> alone_checked;
    bool can_solo(std::size_t g, int mates);
    bool altar_locked_by_enemy(vec2 altar) const;
    bool use_attack_reset();
    bool use_shield();
    bool is_ready(const bc::ability &a) const;
};
