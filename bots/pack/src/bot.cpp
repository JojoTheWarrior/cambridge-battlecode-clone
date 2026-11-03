// Champion decisions for the pack bot. See bot.h for the design.
#include "bot.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <type_traits>
#include <variant>

namespace {
constexpr bool debug = true;
template <class F>
bool safely(F &&f) {
    try {
        if constexpr (std::is_void_v<decltype(f())>) {
            f();
            return true;
        } else {
            return bool(f());
        }
    } catch (const std::exception &e) {
        if constexpr (debug)
            std::printf("CAUGHT %s\n", e.what());
        return false;
    }
}
inline void probe(const char *what) { std::printf("FUEL %s %llu\n", what, (unsigned long long)bc::fuel_remaining()); }
constexpr int kUnreachable = bc::distance_field::unreachable;
constexpr float kGemGold = 12.0f;     // a gem's worth in gold for target choice
constexpr unsigned kBankBag = 25;     // voluntary bank trips carry this many (Mite: median 22)
constexpr unsigned kShopGold = 300;   // recall to shop with this much and no bag
constexpr int kEnemyMemory = 90;      // rounds an enemy sighting keeps a group costly

using Item = bc::item_type;
struct Step {
    Item item;
    unsigned count;
};
// Build orders by champion: the families the top teams finish first, then a
// repeated stat shard. Buying health on spawn also heals.
std::span<const Step> build_for(bc::champion_type type, Item &repeat) {
    static const Step vesper[]{{Item::treasure_shard_of_strength, 5},  {Item::power_shard_of_strength, 5},
                               {Item::durability_shard_of_health, 5},  {Item::prospector_shard_of_strength, 5},
                               {Item::monster_shard_of_strength, 5},   {Item::jungle_shard_of_health, 5},
                               {Item::skirmish_shard_of_strength, 5}};
    static const Step heart[]{{Item::treasure_shard_of_focus, 5},  {Item::power_shard_of_focus, 5},
                              {Item::home_shard_of_health, 5},     {Item::durability_shard_of_health, 5},
                              {Item::prospector_shard_of_focus, 5}, {Item::monster_shard_of_focus, 5}};
    static const Step vendetta[]{{Item::treasure_shard_of_strength, 5}, {Item::durability_shard_of_health, 5},
                                 {Item::jungle_shard_of_health, 5},     {Item::monster_shard_of_strength, 5},
                                 {Item::capture_shard_of_strength, 5}};
    static const Step marcella[]{{Item::treasure_shard_of_focus, 5}, {Item::durability_shard_of_health, 5}};
    static const Step varg[]{{Item::treasure_shard_of_strength, 5}, {Item::durability_shard_of_health, 5},
                             {Item::jungle_shard_of_health, 5},     {Item::monster_shard_of_strength, 5}};
    static const Step other[]{{Item::treasure_shard_of_strength, 5}, {Item::durability_shard_of_health, 5}};
    switch (type) {
        case bc::champion_type::vesper_vale: repeat = Item::bruiser_shard_of_strength; return vesper;
        case bc::champion_type::heart_of_ruin: repeat = Item::bruiser_shard_of_focus; return heart;
        case bc::champion_type::vendetta: repeat = Item::bruiser_shard_of_strength; return vendetta;
        case bc::champion_type::marcella: repeat = Item::bruiser_shard_of_focus; return marcella;
        case bc::champion_type::varg: repeat = Item::bruiser_shard_of_strength; return varg;
        default: repeat = Item::bruiser_shard_of_strength; return other;
    }
}

std::optional<bc::detail::champion_info> sighting(const bc::champion &c) {
    bc::detail::champion_info info{};
    const auto code = bc::detail::sdk_champion(handle_of(c), &info);
    if (code == static_cast<std::int32_t>(bc::error_code::api_not_in_vision) ||
        code == static_cast<std::int32_t>(bc::error_code::api_not_found))
        return std::nullopt;
    bc::detail::check(code);
    if (info.x < 0 || info.y < 0)
        return std::nullopt;
    return info;
}

bool read_camp(const bc::camp &c, bc::detail::camp_state &out) {
    const auto code = bc::detail::sdk_camp(static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(c)), &out);
    if (code == static_cast<std::int32_t>(bc::error_code::api_not_in_vision) ||
        code == static_cast<std::int32_t>(bc::error_code::api_not_found))
        return false;
    bc::detail::check(code);
    return true;
}

int raw_group_state(const bc::map::camp_group &g, int &timer) {
    const auto handle = static_cast<std::uint32_t>(static_cast<bc::handle_camp_group_t>(g));
    std::uint32_t state = 0;
    bc::detail::check(bc::detail::sdk_camp_group_state(handle, &state));
    if (state == 3) {
        std::int32_t cooldown = 0;
        bc::detail::check(bc::detail::sdk_camp_group_cooldown(handle, &cooldown));
        timer = cooldown;
    }
    return int(state);
}

const std::array<vec2, 8> around8{vec2{-1, -1}, vec2{0, -1}, vec2{1, -1}, vec2{-1, 0},
                                   vec2{1, 0},   vec2{-1, 1}, vec2{0, 1},  vec2{1, 1}};
bool inside(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < bc::map::width() && p.y < bc::map::height(); }
} // namespace

// ---------------------------------------------------------------- world

void Bot::initialise() {
    auto &me = bc::self();
    type = me.champion_name();
    stats = me.base_stats();
    melee = stats.base_attack_range <= 1;
    for (const auto &a : me.abilities())
        if (a.info() && a.is_dash()) {
            has_dash = true;
            dash_slot = int(a.slot());
        }
    ally_roster = bc::champions(bc::team::ally);
    enemy_roster = bc::champions(bc::team::enemy);
    std::sort(ally_roster.begin(), ally_roster.end(),
              [](const bc::champion &a, const bc::champion &b) { return handle_of(a) < handle_of(b); });
    const unsigned mine = handle_of(me);
    for (unsigned i = 0; i < ally_roster.size(); ++i)
        if (handle_of(ally_roster[i]) == mine)
            slot = i;

    holes = bc::map::holes();
    bushes = bc::map::bushes();
    terrain = bc::map::walls();
    terrain |= bc::map::altars();
    terrain |= bc::map::spawn_region(bc::team::enemy);
    terrain_jump = terrain;
    terrain |= holes;
    own_spawn = bc::map::spawn_region(bc::team::ally);
    {
        long sx = 0, sy = 0, n = 0;
        for (auto p : own_spawn.positions()) {
            sx += p.x;
            sy += p.y;
            ++n;
        }
        home = n ? vec2{int(sx / n), int(sy / n)} : me.position();
        sx = sy = n = 0;
        for (auto p : bc::map::spawn_region(bc::team::enemy).positions()) {
            sx += p.x;
            sy += p.y;
            ++n;
        }
        enemy_home = n ? vec2{int(sx / n), int(sy / n)} : home;
    }
    any_spawn.clear();
    for (const auto &g : bc::map::camp_groups()) {
        Group group(g);
        group.gems = g.gem_value();
        group.gold = g.gold_value();
        group.respawn_max = g.max_respawn_timer();
        long sx = 0, sy = 0;
        for (const auto &c : g.camps()) {
            const vec2 p = c.spawn_position();
            group.tiles.push_back(p);
            any_spawn.set(p);
            sx += p.x;
            sy += p.y;
            const auto cs = c.base_stats();
            group.health_total += cs.base_health;
            group.strength_sum += cs.base_strength;
            group.strength_max = std::max(group.strength_max, cs.base_strength);
            group.range_max = std::max(group.range_max, cs.base_attack_range);
            if (c.is_monolith())
                group.monolith = true;
            if (c.is_golem())
                group.golem = true;
            if (!c.is_gem_cluster() && !c.is_gold_cluster())
                group.clusters_only = false;
        }
        group.spawns.clear();
        for (auto p : group.tiles)
            group.spawns.set(p);
        group.centre = vec2{int(sx / long(group.tiles.size())), int(sy / long(group.tiles.size()))};
        group.health_left = group.health_total;
        groups.push_back(std::move(group));
    }
    for (auto &g : groups) {
        for (auto t : g.tiles)
            for (auto d : around8) {
                const vec2 n = t + d;
                if (!inside(n) || terrain.is_set(n) || any_spawn.is_set(n))
                    continue;
                if (std::find(g.edges.begin(), g.edges.end(), n) == g.edges.end())
                    g.edges.push_back(n);
            }
    }
    altars = bc::map::altars().positions();
    altar_edges.resize(altars.size());
    for (std::size_t i = 0; i < altars.size(); ++i)
        for (auto d : around8) {
            const vec2 n = altars[i] + d;
            if (inside(n) && !terrain.is_set(n) && !any_spawn.is_set(n))
                altar_edges[i].push_back(n);
        }
    scratch.clear();
    home_field.compute(terrain, own_spawn, scratch);
    plan_fields();
    plan_sectors();
    ready = true;
    if constexpr (debug)
        std::printf("INIT type=%d slot=%u groups=%zu altars=%zu dash=%d fuel=%llu\n", int(type), slot, groups.size(),
                    altars.size(), int(has_dash), (unsigned long long)bc::fuel_remaining());
}

void Bot::refresh_equipment() {
    auto &me = bc::self();
    jungle = 1.0f;
    camp_bonus = stats.camp_damage_multiplier;
    const auto inventory = me.item_count();
    for (unsigned i = 0; i < static_cast<unsigned>(Item::count); ++i) {
        const Item item(i);
        if (!inventory[item])
            continue;
        const auto passive = item.passive();
        if (passive == bc::item_passive::jungle)
            jungle = 1 - item.passive_magnitude();
        if (passive == bc::item_passive::monster)
            camp_bonus = stats.camp_damage_multiplier * (1 + item.passive_magnitude());
    }
}

void Bot::observe() {
    auto &me = bc::self();
    round = bc::round_number();
    pos = me.position();
    health = me.health();
    max_health = me.max_health();
    strength = me.strength();
    focus = me.focus();
    gems = me.gem_count();
    gold = me.gold_count();
    at_spawn = own_spawn.is_set(pos);
    moved = attacked = cast = false;
    bc::vision::visible_tiles_into(visible);
    bc::shared_memory.read(0, std::span{&shared, 1});

    enemies.clear();
    for (const auto &c : enemy_roster)
        if (const auto info = sighting(c)) {
            Unit u;
            u.handle = handle_of(c);
            u.pos = vec2{info->x, info->y};
            u.health = info->health;
            u.max_health = info->max_health;
            u.strength = info->strength;
            u.range = info->attack_range;
            u.type = info->type;
            u.state = std::uint32_t(info->state);
            enemies.push_back(u);
        }
    allies.clear();
    for (const auto &c : ally_roster) {
        if (handle_of(c) == handle_of(me))
            continue;
        if (const auto info = sighting(c)) {
            Unit u;
            u.handle = handle_of(c);
            u.pos = vec2{info->x, info->y};
            u.health = info->health;
            u.max_health = info->max_health;
            u.strength = info->strength;
            u.range = info->attack_range;
            u.type = info->type;
            u.state = std::uint32_t(info->state);
            allies.push_back(u);
        }
    }
    bc::vision::awake_camp_tiles_into(awake);
    observe_groups();

    if (alive_spawns_dirty) {
        alive_spawns.clear();
        for (const auto &g : groups)
            if (g.status == Group::alive)
                alive_spawns |= g.spawns;
        alive_spawns_dirty = false;
    }
    blocked = terrain;
    blocked |= alive_spawns;
    bc::vision::champion_tiles_into(champs);
    blocked |= champs;
    bc::vision::teleport_tiles_into(scratch);
    blocked |= scratch;
    blocked.set(pos, false);
}

void Bot::read_cooling() {
    const std::uint16_t key = std::uint16_t(shared.cool_group << 8 | shared.cool_until);
    if (key == last_cool_read || shared.cool_group == 0)
        return;
    last_cool_read = key;
    const std::size_t i = shared.cool_group - 1u;
    if (i >= groups.size())
        return;
    // The end round, from its value over 4 modulo 256: the nearest such round.
    const int now4 = int(round) / 4;
    int until4 = now4 - ((now4 - int(shared.cool_until)) & 0xFF);
    if (now4 - until4 > 128)
        until4 += 256;
    const int until = until4 * 4 + 2;
    auto &g = groups[i];
    if (until > int(round) && !g.in_view) {
        if (g.status != Group::cooling)
            alive_spawns_dirty = true;
        g.status = Group::cooling;
        g.respawn_at = until;
        g.health_left = 0;
    }
}

void Bot::observe_groups() {
    read_cooling();
    camps.clear();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        auto &g = groups[i];
        const auto before = g.status;
        g.in_view = false;
        for (auto t : g.tiles)
            if (visible.is_set(t)) {
                g.in_view = true;
                break;
            }
        g.enemies_near = 0;
        for (const auto &e : enemies)
            if (dist(e.pos, g.centre) <= 6)
                ++g.enemies_near;
        if (g.enemies_near)
            g.enemy_seen = int(round);
        if (!g.in_view) {
            if (g.status == Group::reward && int(round) > g.seen + settings().capture_time + 1) {
                g.status = Group::cooling;
                g.respawn_at = g.seen + settings().capture_time + g.respawn_max;
            }
            if (g.status == Group::cooling && int(round) >= g.respawn_at) {
                g.status = Group::alive;
                g.health_left = g.health_total;
            }
        } else {
            g.seen = int(round);
            int timer = 0;
            const int state = raw_group_state(g.unit, timer);
            if (state == 3) {
                if (before != Group::cooling && i < 255) {
                    shared.cool_group = std::uint8_t(i + 1);
                    shared.cool_until = std::uint8_t(((int(round) + std::max(1, timer)) / 4) & 0xFF);
                    last_cool_read = std::uint16_t(shared.cool_group << 8 | shared.cool_until);
                }
                g.status = Group::cooling;
                g.respawn_at = int(round) + std::max(1, timer);
                g.health_left = 0;
            } else if (state == 2) {
                g.status = Group::reward;
                g.health_left = 0;
            } else if (state == 1) {
                g.status = Group::alive;
                if (dist(pos, g.centre) <= 10) {
                    float left = 0;
                    int index = 0;
                    for (const auto &c : g.unit.camps()) {
                        bc::detail::camp_state cs{};
                        if (read_camp(c, cs) && cs.health > 0) {
                            left += cs.health;
                            CampSeen seen{vec2{cs.x, cs.y}, cs.health, c.base_stats().base_strength,
                                          c.base_stats().base_attack_range, int(i), index,
                                          awake.is_set(vec2{cs.x, cs.y})};
                            camps.push_back(seen);
                        } else if (!visible.is_set(c.spawn_position())) {
                            left += c.base_stats().base_health; // unseen member, assume whole
                        }
                        ++index;
                    }
                    g.health_left = left;
                }
            }
        }
        if (g.status != before)
            alive_spawns_dirty = true;
    }
}

void Bot::publish(int claim, bool farming) {
    if (slot < 6)
        shared.claim[slot] = std::uint8_t((claim >= 0 ? std::min(claim + 1, 127) : 0) | (farming ? 0 : 0x80));
    bc::shared_memory.write(0, std::span{&shared, 1});
}

// ---------------------------------------------------------------- navigation

void Bot::plan_fields() {
    const std::size_t n = groups.size();
    walk_field.resize(n);
    jump_field.resize(has_dash ? n : 0);
    walk_ready.assign(n, 0);
    jump_ready.assign(n, 0);
    altar_field.resize(altars.size());
    altar_ready.assign(altars.size(), 0);
    // Holes a dash crosses: open ground on two opposite sides.
    {
        bc::bitboard open = ~terrain;
        auto both = [&](int ax, int ay) {
            bc::bitboard a = open.shift_right(ax).shift_up(ay);
            a &= open.shift_right(-ax).shift_up(-ay);
            return a;
        };
        bc::bitboard any = both(1, 0);
        any |= both(0, 1);
        any |= both(1, 1);
        any |= both(1, -1);
        narrow_holes = holes;
        narrow_holes &= any;
    }
    // Nearest groups first, then the altars, then the rest.
    std::vector<int> order(n);
    for (std::size_t i = 0; i < n; ++i)
        order[i] = int(i);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return dist(groups[a].centre, home) < dist(groups[b].centre, home); });
    std::vector<int> altar_order(altars.size());
    for (std::size_t i = 0; i < altars.size(); ++i)
        altar_order[i] = int(i);
    std::sort(altar_order.begin(), altar_order.end(),
              [&](int a, int b) { return dist(altars[a], home) < dist(altars[b], home); });
    const std::size_t first = std::min<std::size_t>(n, 6);
    for (std::size_t k = 0; k < first; ++k)
        field_tasks.push_back(order[k]);
    for (int a : altar_order)
        field_tasks.push_back(2048 + a);
    for (std::size_t k = first; k < n; ++k)
        field_tasks.push_back(order[k]);
}

int Bot::units() const {
    const int n = int(ally_roster.size());
    return pair_count() + (n - 2 * pair_count());
}

void Bot::plan_sectors() {
    const int k = std::max(1, units());
    sector.assign(groups.size(), -1);
    std::vector<int> ours;
    for (std::size_t i = 0; i < groups.size(); ++i)
        if (!groups[i].monolith && dist(groups[i].centre, home) <= dist(groups[i].centre, enemy_home) + 4)
            ours.push_back(int(i));
    if (ours.empty())
        return;
    // Farthest-point seeds from the group nearest home, then a few k-means passes.
    std::vector<vec2> seeds;
    int first = ours[0];
    for (int i : ours)
        if (dist(groups[i].centre, home) < dist(groups[first].centre, home))
            first = i;
    seeds.push_back(groups[first].centre);
    while (int(seeds.size()) < std::min<int>(k, int(ours.size()))) {
        int far = -1, far_d = -1;
        for (int i : ours) {
            int nd = 1 << 20;
            for (auto c : seeds)
                nd = std::min(nd, dist(groups[i].centre, c));
            if (nd > far_d) {
                far_d = nd;
                far = i;
            }
        }
        seeds.push_back(groups[far].centre);
    }
    std::vector<int> owner(groups.size(), -1);
    for (int pass = 0; pass < 6; ++pass) {
        for (int i : ours) {
            int best = 0;
            for (int c = 1; c < int(seeds.size()); ++c)
                if (dist(groups[i].centre, seeds[c]) < dist(groups[i].centre, seeds[best]))
                    best = c;
            owner[i] = best;
        }
        for (int c = 0; c < int(seeds.size()); ++c) {
            long sx = 0, sy = 0, n = 0;
            for (int i : ours)
                if (owner[i] == c) {
                    sx += groups[i].centre.x;
                    sy += groups[i].centre.y;
                    ++n;
                }
            if (n)
                seeds[c] = vec2{int(sx / n), int(sy / n)};
        }
    }
    // Sectors in order of distance from home, given to slots in order.
    std::vector<int> order(seeds.size());
    for (int c = 0; c < int(seeds.size()); ++c)
        order[c] = c;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return dist(seeds[a], home) < dist(seeds[b], home); });
    std::vector<int> slot_of(seeds.size());
    for (int r = 0; r < int(order.size()); ++r)
        slot_of[order[r]] = r;
    for (int i : ours)
        sector[i] = slot_of[owner[i]];
    if constexpr (debug) {
        std::printf("SECTOR slot=%u:", slot);
        for (std::size_t i = 0; i < groups.size(); ++i)
            if (sector[i] == my_unit())
                std::printf(" %zu", i);
        std::printf("\n");
    }
}

void Bot::build_fields(std::uint64_t reserve) {
    const std::uint64_t cost = 3ull * std::uint64_t(bc::map::width()) * std::uint64_t(bc::map::height()) + 3000;
    while (field_next < field_tasks.size() && bc::fuel_remaining() > reserve + cost) {
        const int task = field_tasks[field_next++];
        const int kind = task >> 10, i = task & 1023;
        if (kind == 0) {
            work = any_spawn;
            work &= ~groups[i].spawns;
            work |= terrain;
            walk_field[i].compute(work, groups[i].spawns, scratch);
            walk_ready[i] = 1;
            if constexpr (debug)
                std::printf("FIELD r=%u slot=%u group=%d home=%d left=%llu\n", round, slot, i, int(walk_field[i].get(home)),
                            (unsigned long long)bc::fuel_remaining());
            // Unreachable on foot from home: a dash champion gets a route over holes.
            if (has_dash && walk_field[i].get(home) == kUnreachable)
                field_tasks.insert(field_tasks.begin() + long(field_next), 1024 + i);
        } else if (kind == 1) {
            work = terrain;
            work &= ~narrow_holes;
            scratch = any_spawn;
            scratch &= ~groups[i].spawns;
            work |= scratch;
            jump_field[i].compute(work, groups[i].spawns, scratch);
            jump_ready[i] = 1;
        } else {
            work = terrain;
            work |= any_spawn;
            dest.clear();
            for (auto e : altar_edges[i])
                dest.set(e);
            altar_field[i].compute(work, dest, scratch);
            altar_ready[i] = 1;
        }
    }
}

int Bot::field_at(const bc::distance_field &field) const {
    const int here = field.get(pos);
    if (here != kUnreachable)
        return here;
    int best = kUnreachable;
    for (auto d : around8) {
        const vec2 n = pos + d;
        if (inside(n) && !holes.is_set(n))
            best = std::min(best, int(field.get(n)));
    }
    return best == kUnreachable ? best : best + 1;
}

int Bot::walk_to_group(const Group &g) const {
    const std::size_t i = std::size_t(&g - groups.data());
    if (!walk_ready[i])
        return kUnreachable;
    const int d = field_at(walk_field[i]);
    if (d == kUnreachable)
        return d;
    return g.status == Group::alive ? std::max(0, d - 1) : d;
}

int Bot::jump_to_group(const Group &g) const {
    const std::size_t i = std::size_t(&g - groups.data());
    if (!has_dash || !jump_ready[i])
        return kUnreachable;
    const int d = field_at(jump_field[i]);
    if (d == kUnreachable)
        return d;
    return g.status == Group::alive ? std::max(0, d - 1) : d;
}

int Bot::walk_to_altar(std::size_t i) const {
    if (!altar_ready[i])
        return std::max(0, dist(pos, altars[i]) * 3 / 2 - 1);
    return field_at(altar_field[i]);
}

bool Bot::dash_toward(const bc::distance_field &field) {
    auto &me = bc::self();
    if (!has_dash || !me.can_cast())
        return false;
    const auto a = me.abilities()[dash_slot];
    if (!is_ready(a))
        return false;
    const int here = field.get(pos);
    int best = here;
    vec2 landing{-1, -1};
    for (auto c : a.valid_tiles().positions()) {
        if (holes.is_set(c) || blocked.is_set(c))
            continue;
        const int d = field.get(c);
        if (d < best) {
            best = d;
            landing = c;
        }
    }
    if (landing.x < 0)
        return false;
    if (safely([&] { return a.use(landing); })) {
        cast = true;
        moved = true;
        pos = me.position();
        bc::vision::visible_tiles_into(visible);
        return true;
    }
    return false;
}

bool Bot::go_tile(vec2 tile, bool) {
    auto &me = bc::self();
    if (!me.can_move() || moved || tile == pos)
        return false;
    if (safely([&] { return me.move_towards(tile); })) {
        moved = true;
        pos = me.position();
        bc::vision::visible_tiles_into(visible);
        return true;
    }
    return false;
}

bool Bot::step_on(const bc::distance_field &field, int stop_at, bool over_holes) {
    auto &me = bc::self();
    if (!me.can_move() || moved)
        return false;
    const int here = field.get(pos); // unreachable on a walled tile: any neighbour is better
    if (here <= stop_at)
        return false;
    vec2 best{-1, -1};
    int best_value = here;
    bool hole_ahead = false;
    for (auto d : around8) {
        const vec2 n = pos + d;
        if (!inside(n))
            continue;
        const int v = field.get(n);
        if (v >= best_value)
            continue;
        if (holes.is_set(n)) {
            hole_ahead = true;
            continue;
        }
        if (blocked.is_set(n))
            continue;
        best_value = v;
        best = n;
    }
    if (best.x < 0 && over_holes && hole_ahead) {
        if (dash_toward(field))
            return true;
        if constexpr (debug) {
            const auto a = me.abilities()[dash_slot];
            int n = 0, lower = 0;
            for (auto c : a.valid_tiles().positions()) {
                ++n;
                if (!holes.is_set(c) && !blocked.is_set(c) && field.get(c) < here)
                    ++lower;
            }
            std::printf("DASHWAIT r=%u slot=%u pos=%d,%d here=%d cast=%d cd=%d valid=%d lower=%d\n", round, slot, pos.x, pos.y,
                        here, int(me.can_cast()), int(a.cooldown()), n, lower);
        }
        return true; // wait at the edge for the dash
    }
    if (best.x < 0) {
        // Blocked by a champion: a sideways step keeps the distance.
        for (auto d : around8) {
            const vec2 n = pos + d;
            if (inside(n) && n != last_pos && !holes.is_set(n) && !blocked.is_set(n) && field.get(n) == here) {
                best = n;
                break;
            }
        }
    }
    if (best.x < 0)
        return false;
    if (safely([&] { return me.move_absolute(best); })) {
        moved = true;
        pos = me.position();
        bc::vision::visible_tiles_into(visible);
        return true;
    }
    return false;
}

bool Bot::go_group(std::size_t g, int stop_at) {
    // Fields count steps to the spawn tiles: an alive group's edge is 1.
    const bool alive = groups[g].status == Group::alive;
    const int walk = walk_ready[g] ? field_at(walk_field[g]) : kUnreachable;
    const int jump = has_dash && jump_ready[g] ? field_at(jump_field[g]) : kUnreachable;
    const int stop = stop_at; (void)alive; // field value to stop at: 1 is beside a spawn tile
    if (jump != kUnreachable && (walk == kUnreachable || jump + 6 < walk))
        return step_on(jump_field[g], stop, true);
    if (walk != kUnreachable)
        return step_on(walk_field[g], stop, false);
    if (walk_ready[g])
        return false; // known unreachable
    return go_tile(groups[g].centre);
}

bool Bot::go_altar(std::size_t i) {
    if (altar_ready[i])
        return step_on(altar_field[i], 0, false);
    return go_tile(altars[i]);
}

bool Bot::step_away(const std::vector<vec2> &threats, bool toward_home) {
    auto &me = bc::self();
    if (!me.can_move() || moved || threats.empty())
        return false;
    auto score = [&](vec2 p) {
        int nearest = 99;
        for (auto t : threats)
            nearest = std::min(nearest, dist(p, t));
        float s = float(std::min(nearest, 6)) * 10.0f;
        if (toward_home) {
            const int h = home_field.get(p);
            s -= (h == kUnreachable ? 200.0f : float(h)) * 0.5f;
        }
        return s;
    };
    vec2 best = pos;
    float best_score = score(pos);
    for (auto d : around8) {
        const vec2 n = pos + d;
        if (!inside(n) || blocked.is_set(n))
            continue;
        const float s = score(n);
        if (s > best_score) {
            best_score = s;
            best = n;
        }
    }
    if (best == pos)
        return false;
    if (safely([&] { return me.move_absolute(best); })) {
        moved = true;
        pos = me.position();
        bc::vision::visible_tiles_into(visible);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- helpers

bool Bot::is_ready(const bc::ability &a) const { return a.info() && a.cooldown() <= 0; }

float Bot::my_dps() const { return strength * camp_bonus + focus * 0.2f; }

float Bot::camp_incoming(vec2 at) const {
    float total = 0;
    for (const auto &c : camps) {
        if (!c.awake || c.health <= 0 || dist(at, c.pos) > c.range)
            continue;
        // Each camp hits only the nearest champion.
        bool nearest = true;
        const int mine = dist(at, c.pos);
        for (const auto &a : allies)
            if (dist(a.pos, c.pos) < mine)
                nearest = false;
        if (nearest)
            total += c.strength * jungle;
    }
    return total;
}

bool Bot::safe_with(const Group &g, int claimers, float hp) const {
    if (g.clusters_only)
        return true;
    if (g.monolith)
        return false;
    // Kill the weakest camp first; the group's strength falls as camps die.
    // Camps hit only the nearest champion, so a pack shares the damage.
    std::vector<std::pair<float, float>> members; // health, strength
    int index = 0;
    for (const auto &c : g.unit.camps()) {
        float h = c.base_stats().base_health;
        if (g.in_view)
            for (const auto &seen : camps)
                if (seen.group == int(&g - groups.data()) && seen.index == index)
                    h = seen.health;
        members.emplace_back(h, c.base_stats().base_strength);
        ++index;
    }
    std::sort(members.begin(), members.end());
    const float dps = my_dps() * float(1 + claimers);
    float remaining = 0, damage = 0;
    for (const auto &m : members)
        remaining += m.second;
    for (const auto &m : members) {
        damage += remaining * jungle * (m.first / std::max(1.0f, dps));
        remaining -= m.second;
    }
    damage /= float(1 + claimers);
    // Half the simulated damage: camps sleep when we step out, and never heal.
    return hp > 0.5f * damage + 100.0f;
}

bool Bot::can_solo(std::size_t g, int mates) {
    if (alone_ok.size() != groups.size()) {
        alone_ok.assign(groups.size(), 0);
        alone_checked.assign(groups.size(), -1000);
    }
    if (int(round) - alone_checked[g] >= 50) {
        // From full health, against the whole group.
        const float left = groups[g].health_left;
        groups[g].health_left = groups[g].health_total;
        alone_ok[g] = safe_with(groups[g], mates, max_health) ? 1 : 0;
        groups[g].health_left = left;
        alone_checked[g] = int(round);
    }
    return alone_ok[g] != 0;
}

bool Bot::altar_locked_by_enemy(vec2 altar) const {
    return visible.is_set(altar) && bc::vision::has_altar_lock(altar) && bc::vision::altar_lock_at(altar).is_enemy();
}

bool Bot::use_attack_reset() {
    auto &me = bc::self();
    if (cast || !me.can_cast() || me.can_attack())
        return false;
    for (const auto &a : me.abilities())
        if (a.is_attack_reset() && is_ready(a)) {
            if (safely([&] { return a.use(); })) {
                cast = true;
                return true;
            }
        }
    return false;
}

bool Bot::use_shield() {
    auto &me = bc::self();
    if (cast || !me.can_cast())
        return false;
    for (const auto &a : me.abilities())
        if (a.is_shield() && is_ready(a)) {
            if (safely([&] { return a.use(); })) {
                cast = true;
                return true;
            }
        }
    return false;
}

// ---------------------------------------------------------------- shop

bool Bot::shop() {
    auto &me = bc::self();
    Item repeat = Item::bruiser_shard_of_strength;
    const auto steps = build_for(type, repeat);
    bool bought = false;
    for (int guard = 0; guard < 12; ++guard) {
        const auto inventory = me.item_count();
        Item next = repeat;
        for (const auto &step : steps) {
            unsigned have = inventory[step.item];
            if (const auto emblem = step.item.result())
                have += 5 * inventory[*emblem];
            if (have < step.count) {
                next = step.item;
                break;
            }
        }
        if (me.gold_count() < next.cost() || !me.can_buy(next, 1))
            break;
        me.buy(next, 1);
        bought = true;
    }
    if (bought) {
        refresh_equipment();
        last_shop_round = int(round);
    }
    return bought;
}

// ---------------------------------------------------------------- fight

void Bot::fight_abilities(const Unit &target) {
    auto &me = bc::self();
    if (cast || !me.can_cast())
        return;
    const bc::champion *handle = nullptr;
    for (const auto &c : enemy_roster)
        if (handle_of(c) == target.handle)
            handle = &c;
    if (!handle)
        return;
    const int d = dist(pos, target.pos);
    for (const auto &a : me.abilities()) {
        if (!is_ready(a) || !a.info())
            continue;
        const auto &info = *a.info();
        const bool exclusive = std::visit([](const auto &p) { return p.exclusive_with_movement; }, info);
        if (exclusive && moved)
            continue;
        if (a.is_cc() && a.can_target_enemies()) {
            const auto &p = std::get<bc::ability_cc>(info);
            if (d <= int(p.range) && safely([&] { return a.use(*handle); })) {
                cast = true;
                return;
            }
        } else if (a.is_damage() && a.can_target_enemies()) {
            const auto &p = std::get<bc::ability_damage>(info);
            if (d <= int(p.range) && safely([&] { return a.use(*handle); })) {
                cast = true;
                return;
            }
        } else if (a.is_aoe_damage() && a.can_target_ground()) {
            const auto &p = std::get<bc::ability_aoe_damage>(info);
            if (d <= int(p.range) && safely([&] { return a.use(target.pos); })) {
                cast = true;
                return;
            }
        } else if (a.is_dash() && melee && d == 2) {
            // Heart of Ruin's landing stuns; a plain dash closes the gap.
            for (auto dd : around8) {
                const vec2 land = target.pos + dd;
                if (inside(land) && dist(land, pos) <= 2 && !blocked.is_set(land) && !holes.is_set(land) &&
                    land != pos) {
                    if (safely([&] { return a.use(land); })) {
                        cast = true;
                        moved = true;
                        pos = me.position();
                        return;
                    }
                }
            }
        }
    }
}

bool Bot::fight() {
    auto &me = bc::self();
    if (enemies.empty())
        return false;
    int nearest = 99;
    for (const auto &e : enemies)
        nearest = std::min(nearest, dist(pos, e.pos));
    if (nearest > 7)
        return false;
    // Local power: health times damage, ours within five tiles, theirs within six.
    const float mine = health * (strength * 1.3f + focus * 0.2f);
    float ours = mine, theirs = 0;
    for (const auto &a : allies)
        if (dist(a.pos, pos) <= 5 && !a.recalling())
            ours += a.health * (a.strength * 1.3f + 20.0f);
    for (const auto &e : enemies)
        if (dist(e.pos, pos) <= 6)
            theirs += e.health * (e.strength * 1.3f + 20.0f);
    const float need = gems >= kBankBag ? 2.5f : 1.6f;
    const bool engage = theirs > 0 && ours >= need * theirs && health > 0.5f * max_health;
    if constexpr (debug)
        std::printf("FIGHT r=%u slot=%u ours=%.0f theirs=%.0f engage=%d nearest=%d gems=%u\n", round, slot, ours,
                    theirs, int(engage), nearest, gems);
    if (engage) {
        // Focus the weakest enemy in reach; otherwise close on the nearest.
        const Unit *best = nullptr;
        const int range = me.attack_range();
        for (const auto &e : enemies)
            if (dist(pos, e.pos) <= range && (!best || e.health < best->health))
                best = &e;
        if (best) {
            const bc::champion *handle = nullptr;
            for (const auto &c : enemy_roster)
                if (handle_of(c) == best->handle)
                    handle = &c;
            if (handle && me.can_attack()) {
                safely([&] { me.attack(*handle); });
                attacked = true;
                if (use_attack_reset() && me.can_attack())
                    safely([&] { me.attack(*handle); });
            }
            fight_abilities(*best);
            if (health < 0.7f * max_health)
                use_shield();
            return true;
        }
        const Unit *near = nullptr;
        for (const auto &e : enemies)
            if (!near || dist(pos, e.pos) < dist(pos, near->pos))
                near = &e;
        if (near) {
            fight_abilities(*near);
            if (!moved && melee)
                go_tile(near->pos, false);
            else if (!moved && !melee && dist(pos, near->pos) > range)
                go_tile(near->pos, false);
            return true;
        }
        return false;
    }
    // Not engaging: leave when they can reach us soon.
    bool threatened = false;
    std::vector<vec2> threats;
    for (const auto &e : enemies) {
        threats.push_back(e.pos);
        if (dist(pos, e.pos) <= e.range + (theirs > 2.0f * ours ? 5 : 2))
            threatened = true;
    }
    if (!threatened)
        return false;
    // Declining a fight beside our target gives the target up for a while:
    // stepping back and forth at the enemy's reach wasted dozens of turns.
    if (target >= 0) {
        for (const auto &e : enemies)
            if (dist(e.pos, groups[target].centre) <= 8) {
                groups[target].unsafe_until = int(round) + 25;
                target = -1;
                break;
            }
    }
    // A melee champion hits what stands beside it before stepping off.
    if (melee && me.can_attack()) {
        for (const auto &e : enemies)
            if (dist(pos, e.pos) <= 1) {
                for (const auto &c : enemy_roster)
                    if (handle_of(c) == e.handle) {
                        safely([&] { me.attack(c); });
                        attacked = true;
                    }
                break;
            }
    }
    if (health < 0.7f * max_health)
        use_shield();
    step_away(threats, true);
    return true;
}

bool Bot::survive() {
    auto &me = bc::self();
    const float incoming = camp_incoming(pos);
    const bool low = health < (enemies.empty() ? 0.3f : 0.5f) * max_health;
    std::vector<vec2> camp_threats;
    for (const auto &c : camps)
        if (c.awake && c.health > 0 && dist(pos, c.pos) <= c.range)
            camp_threats.push_back(c.pos);
    int enemy_near = 99;
    for (const auto &e : enemies)
        enemy_near = std::min(enemy_near, dist(pos, e.pos));
    if (low) {
        if (enemy_near <= 8) {
            std::vector<vec2> threats;
            for (const auto &e : enemies)
                threats.push_back(e.pos);
            if (health < 0.7f * max_health)
                use_shield();
            return step_away(threats, true);
        }
        if (incoming > 0) {
            use_shield();
            if (step_away(camp_threats, true))
                return true;
        }
        if (!at_spawn && me.can_move() && !moved) {
            me.recall();
            return true;
        }
        return false;
    }
    // Two more camp turns would leave less than a fifth: step out of range.
    if (incoming > 0 && health - incoming * 2.5f < 0.2f * max_health) {
        use_shield();
        return step_away(camp_threats, true);
    }
    return false;
}

// ---------------------------------------------------------------- bank

bool Bot::bank() {
    auto &me = bc::self();
    if (gems == 0) {
        banking = false;
        return false;
    }
    const int remaining = std::max(1, int(settings().winning_gems) - int(bc::global_submitted_gems(bc::team::ally)));
    const bool win = int(gems) >= remaining;
    int best = -1, best_d = kUnreachable;
    float best_value = 1e9f;
    for (std::size_t i = 0; i < altars.size(); ++i) {
        const int d = walk_to_altar(i);
        if (d == kUnreachable)
            continue;
        float value = float(d);
        if (altar_locked_by_enemy(altars[i]))
            value += 8;
        for (const auto &e : enemies)
            if (dist(e.pos, altars[i]) <= 5)
                value += 6;
        if (value < best_value) {
            best_value = value;
            best = int(i);
            best_d = d;
        }
    }
    if (best < 0) {
        banking = false;
        if (gems >= kBankBag && !at_spawn && enemies.empty() && camp_incoming(pos) == 0 && me.can_move() && !moved) {
            me.recall(); // walled in: bank from spawn
            return true;
        }
        return false;
    }
    const bool deadline = int(settings().max_rounds - round) <= best_d + channel_turns(gems) + 6;
    const bool nearby = gems >= 10 && best_d <= 2;
    const bool trip = banking || win || deadline || gems >= kBankBag || (gems >= 12 && !enemies.empty()) || nearby || (at_spawn && gems >= 10);
    if (!trip)
        return false;
    banking = true;
    bank_altar = best;
    const vec2 altar = altars[best];
    if (dist(pos, altar) <= settings().submit_gems_range) {
        if (me.buffs().has_stun())
            return true;
        if (altar_locked_by_enemy(altar)) {
            if (me.can_attack() && dist(pos, altar) <= me.attack_range()) {
                safely([&] { me.attack(bc::vision::altar_lock_at(altar)); });
                attacked = true;
                if (use_attack_reset() && me.can_attack() && bc::vision::has_altar_lock(altar))
                    safely([&] { me.attack(bc::vision::altar_lock_at(altar)); });
            } else if (!moved) {
                go_altar(std::size_t(best));
            }
            return true;
        }
        safely([&] { me.submit_gems(altar); });
        return true;
    }
    if (!go_altar(std::size_t(best)))
        ++stuck_turns;
    return true;
}

// ---------------------------------------------------------------- farm

int Bot::choose_target() {
    int best = -1;
    float best_value = 0;
    // Claims by allies (their shared-memory slot, not mine).
    std::array<int, 128> claimers{};
    for (unsigned i = 0; i < 6; ++i) {
        if (i == slot || (paired() && i == partner()))
            continue;
        const std::uint8_t c = shared.claim[i];
        if (c && !(c & 0x80))
            ++claimers[c & 0x7F];
    }
    int enemies_visible = int(enemies.size());
    int pack = 1;
    for (const auto &a : allies)
        if (dist(a.pos, pos) <= 6)
            ++pack;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto &g = groups[i];
        if (g.monolith || int(round) < g.unreachable_until)
            continue;
        const int others = i + 1 < 128 ? claimers[i + 1] : 0;
        // Strong groups (elk, golem) want a pair; wolves and clusters one.
        const bool strong = g.strength_max >= 60;
        const int mates = paired() ? 1 : 0;
        const int assumed = others + mates;
        int d = walk_to_group(g);
        bool via_jump = false;
        if (has_dash) {
            const int dj = jump_to_group(g);
            if (dj != kUnreachable && (d == kUnreachable || dj + 6 < d)) {
                d = dj + 4;
                via_jump = true;
            }
        }
        if (d == kUnreachable)
            continue;
        const float reward = float(g.gems) * kGemGold + float(g.gold);
        float time = 0;
        const float dps = my_dps() * float(1 + assumed);
        if (g.status == Group::alive) {
            if (int(round) < g.unsafe_until && others == 0)
                continue;
            time = float(d) + g.health_left / std::max(1.0f, dps) + float(settings().capture_time);
        } else if (g.status == Group::reward) {
            time = float(d) + float(settings().capture_time);
        } else {
            const int wait = g.respawn_at - int(round);
            if (wait > d + 12)
                continue;
            time = float(std::max(d, wait)) + g.health_total / std::max(1.0f, dps) + float(settings().capture_time);
        }
        float value = reward / float(1 + assumed) / std::max(1.0f, time);
        // Joining adds speed, not reward: worth it on a strong group short of
        // a pair, hardly otherwise.
        if (others > 0)
            value *= (strong && others + mates < 3) ? 1.0f : 0.2f;
        else if (g.status != Group::reward && !g.clusters_only && !can_solo(i, mates)) {
            if constexpr (debug)
                if (round % 50 == 0)
                    std::printf("SKIP r=%u slot=%u g=%zu nosolo d=%d hp=%.0f str=%.0f gemsg=%u\n", round, slot, i, d, max_health,
                                g.strength_sum, g.gems);
            continue; // walking there alone ends in a retreat at its edge
        }
        if (int(round) - g.enemy_seen <= kEnemyMemory)
            value *= pack > std::max(1, g.enemies_near) ? 0.7f : 0.25f;
        if (round < 400 && dist(g.centre, enemy_home) < dist(g.centre, home))
            value *= 0.6f;
        if (g.golem && others == 0 && max_health < 1500)
            value *= 0.4f;
        if (!sector.empty()) {
            if (sector[i] == my_unit())
                value *= 1.8f;
            else if (sector[i] >= 0)
                value *= 0.6f;
        }
        if (int(i) == target)
            value *= 1.25f;
        if (via_jump)
            value *= 0.9f;
        if constexpr (debug)
            if (round % 50 == 0)
                std::printf("CAND r=%u slot=%u g=%zu st=%d d=%d gems=%u gold=%u others=%d solo=%d hpleft=%.0f time=%.0f value=%.2f\n",
                            round, slot, i, int(g.status), d, g.gems, g.gold, others,
                            g.clusters_only ? 2 : int(can_solo(i, mates)), g.health_left, time, value);
        if (value > best_value) {
            best_value = value;
            best = int(i);
        }
    }
    if constexpr (debug)
        if (best != target)
            std::printf("TARGET r=%u slot=%u group=%d value=%.2f\n", round, slot, best, best_value);
    return best;
}

bool Bot::attack_camp(const Group &g) {
    auto &me = bc::self();
    if (!me.can_attack() || attacked)
        return false;
    const int range = me.attack_range();
    const CampSeen *focus = nullptr;
    for (const auto &c : camps) {
        if (c.group != int(&g - groups.data()) || c.health <= 0 || dist(pos, c.pos) > range ||
            !visible.is_set(c.pos))
            continue;
        if (!focus || c.health < focus->health)
            focus = &c;
    }
    if (!focus)
        return false;
    const auto camp_list = g.unit.camps();
    safely([&] { me.attack(camp_list[focus->index]); });
    attacked = true;
    if (use_attack_reset() && me.can_attack())
        safely([&] { me.attack(camp_list[focus->index]); });
    camp_abilities(g, focus);
    return true;
}

void Bot::camp_abilities(const Group &g, const CampSeen *focus) {
    auto &me = bc::self();
    if (cast || !me.can_cast())
        return;
    const auto camp_list = g.unit.camps();
    for (const auto &a : me.abilities()) {
        if (!is_ready(a) || !a.info())
            continue;
        const auto &info = *a.info();
        const bool exclusive = std::visit([](const auto &p) { return p.exclusive_with_movement; }, info);
        if (exclusive && moved)
            continue;
        if ((a.is_damage() || a.is_cc()) && a.can_target_camps() && focus) {
            const float range = a.is_damage() ? std::get<bc::ability_damage>(info).range
                                              : std::get<bc::ability_cc>(info).range;
            if (dist(pos, focus->pos) <= int(range) && focus->health > strength * 0.5f) {
                if (safely([&] { return a.use(camp_list[focus->index]); })) {
                    cast = true;
                    return;
                }
            }
        } else if (a.is_aoe_damage() && a.can_target_ground()) {
            const auto &p = std::get<bc::ability_aoe_damage>(info);
            // The tile hitting most living camps of this group.
            vec2 best{-1, -1};
            int best_hits = 0;
            for (const auto &c : camps) {
                if (c.group != focus->group || c.health <= 0 || dist(pos, c.pos) > int(p.range))
                    continue;
                int hits = 0;
                for (const auto &o : camps)
                    if (o.group == c.group && o.health > 0 && dist(o.pos, c.pos) <= int(p.radius))
                        ++hits;
                if (hits > best_hits) {
                    best_hits = hits;
                    best = c.pos;
                }
            }
            if (best.x >= 0 && safely([&] { return a.use(best); })) {
                cast = true;
                return;
            }
        } else if (a.is_dash() && type == bc::champion_type::heart_of_ruin && !moved) {
            // Land beside two or more camps: damage and a stun on each.
            vec2 best{-1, -1};
            int best_hits = 1;
            for (auto e : g.edges) {
                if (dist(e, pos) > 2 || e == pos || blocked.is_set(e))
                    continue;
                int hits = 0;
                for (const auto &c : camps)
                    if (c.group == focus->group && c.health > 0 && dist(c.pos, e) <= 1)
                        ++hits;
                if (hits > best_hits) {
                    best_hits = hits;
                    best = e;
                }
            }
            if (best.x >= 0 && safely([&] { return a.use(best); })) {
                cast = true;
                moved = true;
                pos = me.position();
                return;
            }
        }
    }
    // Shield against the camps once they are awake on us.
    if (camp_incoming(pos) > 0 && health < 0.8f * max_health)
        use_shield();
}

bool Bot::farm() {
    auto &me = bc::self();
    if (target < 0)
        return false;
    auto &g = groups[target];
    if (g.status == Group::reward) {
        if (g.spawns.is_set(pos))
            return true; // hold the capture
        go_group(std::size_t(target), 0);
        return true;
    }
    if (g.status == Group::cooling) {
        go_group(std::size_t(target), 2);
        return true;
    }
    // Alive: a lone champion checks the camps' damage against its health first.
    const int d = walk_to_group(g);
    if (d == kUnreachable && !(has_dash && jump_to_group(g) != kUnreachable)) {
        g.unreachable_until = int(round) + 60;
        target = -1;
        return false;
    }
    if (d <= 1) {
        int others = 0;
        for (const auto &a : allies)
            if (dist(a.pos, g.centre) <= 5)
                ++others;
        if (!g.clusters_only && !safe_to_clear(g, others) && !can_solo(std::size_t(target), 0) && camp_incoming(pos) == 0) {
            int claimers = 0;
            for (unsigned i = 0; i < 6; ++i)
                if (i != slot && shared.claim[i] && !(shared.claim[i] & 0x80) && int(shared.claim[i] & 0x7F) == target + 1)
                    ++claimers;
            if constexpr (debug)
                std::printf("UNSAFE r=%u slot=%u group=%d hp=%.0f left=%.0f others=%d claimers=%d\n", round, slot,
                            target, health, g.health_left, others, claimers);
            if (claimers > others && safe_to_clear(g, claimers))
                return true; // wait at the edge for the pack
            g.unsafe_until = int(round) + 40;
            target = -1;
            return false;
        }
    }
    // The focus: the weakest camp of the group we can see. Camps inside bushes
    // are hidden from outside (big_world's elk ring: the camp beside the first
    // edge tile sat in a bush, and ours stood there taking hits).
    const int range = me.attack_range();
    const CampSeen *focus = nullptr;
    bool any_in_range = false;
    for (const auto &c : camps) {
        if (c.group != target || c.health <= 0 || !visible.is_set(c.pos))
            continue;
        if (dist(pos, c.pos) <= range)
            any_in_range = true;
        if (!focus || dist(pos, c.pos) < dist(pos, focus->pos) ||
            (dist(pos, c.pos) == dist(pos, focus->pos) && c.health < focus->health))
            focus = &c;
    }
    if constexpr (debug)
        std::printf("FARM r=%u slot=%u group=%d status=%d d=%d camps=%zu focus=%d view=%d\n", round, slot, target,
                    int(g.status), d, camps.size(), focus ? focus->index : -1, int(g.in_view));
    if (!focus) {
        // Nothing visible: bush edges reveal their cluster; otherwise any edge.
        std::vector<vec2> bush_edges;
        for (auto e : g.edges)
            if (bushes.is_set(e))
                bush_edges.push_back(e);
        bool stepped = false;
        if (!bush_edges.empty() && walk_to_group(g) <= 2) {
            vec2 nearest = bush_edges[0];
            for (auto e : bush_edges)
                if (dist(pos, e) < dist(pos, nearest))
                    nearest = e;
            stepped = nearest != pos && go_tile(nearest);
        } else {
            stepped = go_group(std::size_t(target), melee ? 1 : 3);
        }
        if (!stepped)
            ++stuck_turns;
        attack_camp(g);
        return true;
    }
    if (any_in_range) {
        attack_camp(g);
        return true;
    }
    // Close in on the group; at its edge, on the focus itself.
    if (!go_group(std::size_t(target), melee ? 1 : std::max(1, range)) && !go_tile(focus->pos))
        ++stuck_turns;
    if (melee)
        attack_camp(g); // a melee champion attacks after its step
    return true;
}

void Bot::place_ward() {
    auto &me = bc::self();
    if (int(round) - ward_round < 12 || me.ward_cooldown() > 0 || at_spawn)
        return;
    if (target < 0 || walk_to_group(groups[target]) > 3)
        return;
    vec2 best{-1, -1};
    int best_score = -1;
    const auto taken = bc::vision::ward_tiles(bc::team::ally);
    for (auto t : me.ward_placement_tiles().positions()) {
        if (taken.is_set(t))
            continue;
        const int score = dist(t, home) - dist(t, enemy_home);
        if (score > best_score) {
            best_score = score;
            best = t;
        }
    }
    if (best.x >= 0 && safely([&] { return me.place_ward(best); }))
        ward_round = int(round);
}

// ---------------------------------------------------------------- turn

void Bot::play_turn() {
    auto &me = bc::self();
    if (!ready) {
        initialise();
        refresh_equipment();
    }
    if (!me.is_alive()) {
        if constexpr (debug)
            std::printf("ACT r=%u slot=%u dead\n", bc::round_number(), slot);
        shop();
        round = bc::round_number();
        bc::shared_memory.read(0, std::span{&shared, 1});
        publish(-1, false);
        target = -1;
        banking = false;
        build_fields(12000);
        return;
    }
    probe("start");
    observe();
    probe("observed");
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        build_fields(12000);
        if constexpr (debug)
            std::printf("ACT r=%u slot=%u %s hp=%.0f\n", round, slot,
                        me.is_recalling() ? "recall" : me.is_submitting_gems() ? "tribute" : "teleport", health);
        publish(-1, false);
        return;
    }
    if (at_spawn)
        shop();
    if (at_spawn && health < 0.9f * max_health && enemies.empty()) {
        if constexpr (debug)
            std::printf("ACT r=%u slot=%u heal hp=%.0f\n", round, slot, health);
        publish(-1, false);
        build_fields(12000);
        return;
    }
    if (pos == last_pos)
        ++stuck_turns;
    else
        stuck_turns = 0;
    last_pos = pos;
    if (stuck_turns >= 8 && target >= 0) {
        groups[target].unreachable_until = int(round) + 40;
        target = -1;
        stuck_turns = 0;
    }

    auto act = [&](const char *why) {
        if constexpr (debug)
            std::printf("ACT r=%u slot=%u %s hp=%.0f gems=%u att=%d mov=%d tgt=%d\n", round, slot, why, health, gems,
                        int(attacked), int(moved), target);
    };
    if (fight()) {
        act("fight");
        publish(target, false);
        return;
    }
    if (survive()) {
        act("survive");
        publish(target, false);
        return;
    }
    if (bank()) {
        act("bank");
        publish(target, false);
        return;
    }
    // Home for the shop with a full purse and an empty bag.
    if (!at_spawn && gold >= kShopGold && gems < 10 && enemies.empty() && camp_incoming(pos) == 0 && me.can_move() &&
        int(round) - last_shop_round > 60) {
        me.recall();
        act("shoprecall");
        publish(-1, false);
        return;
    }
    bool keep = false;
    if (target >= 0 && int(round) - committed_at < 60) {
        const auto &g = groups[target];
        const int d = std::min(walk_to_group(g), has_dash ? jump_to_group(g) : kUnreachable);
        keep = d != kUnreachable && int(round) >= g.unsafe_until && int(round) >= g.unreachable_until &&
               (g.status != Group::cooling || g.respawn_at - int(round) <= d + 12);
    }
    int led = -1;
    if (paired() && !leader()) {
        const std::uint8_t c = shared.claim[partner()];
        if (c && !(c & 0x80) && int(c & 0x7F) - 1 < int(groups.size()) && int(round) >= groups[(c & 0x7F) - 1].unreachable_until)
            led = int(c & 0x7F) - 1;
    }
    if (led >= 0) {
        target = led;
        committed_at = int(round);
    } else if (!keep) {
        target = choose_target();
        committed_at = int(round);
    }
    if (!farm()) {
        target = choose_target();
        farm();
    }
    place_ward();
    build_fields(12000);
    act(target < 0 ? "idle" : groups[target].status == Group::alive ? "farm" : groups[target].status == Group::reward ? "capture" : "waitrespawn");
    publish(target, true);
    probe("end");
    if constexpr (debug)
        std::printf("TURN r=%u slot=%u pos=%d,%d hp=%.0f gems=%u gold=%u target=%d fuel=%llu\n", round, slot, pos.x,
                    pos.y, health, gems, gold, target, (unsigned long long)bc::fuel_remaining());
}
