#include "activity_actor.h"
#include "activity_actor_definitions.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <string>
#include <utility>

#include "avatar_action.h"
#include "activity_handlers.h" // put_into_vehicle_or_drop, drop_on_map, move_items
#include "advanced_inv.h"
#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "debug.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "field_type.h"
#include "game.h"
#include "gates.h"
#include "iexamine.h"
#include "int_id.h"
#include "item.h"
#include "item_group.h"
#include "item_location.h"
#include "json.h"
#include "line.h"
#include "map.h"
#include "map_iterator.h"
#include "mapdata.h"
#include "messages.h"
#include "npc.h"
#include "output.h"
#include "options.h"
#include "pickup.h"
#include "player.h"
#include "player_activity.h"
#include "point.h"
#include "ranged.h"
#include "rng.h"
#include "sounds.h"
#include "timed_event.h"
#include "translations.h"
#include "uistate.h"
#include "units.h"
#include "vehicle.h"
#include "vpart_position.h"

static const itype_id itype_bone_human( "bone_human" );
static const itype_id itype_electrohack( "electrohack" );

static const skill_id skill_computer( "computer" );

static const mtype_id mon_zombie( "mon_zombie" );
static const mtype_id mon_zombie_fat( "mon_zombie_fat" );
static const mtype_id mon_zombie_rot( "mon_zombie_rot" );
static const mtype_id mon_skeleton( "mon_skeleton" );
static const mtype_id mon_zombie_crawler( "mon_zombie_crawler" );

static const zone_type_id zone_LOOT_IGNORE( "LOOT_IGNORE" );
static const zone_type_id zone_LOOT_IGNORE_FAVORITES( "LOOT_IGNORE_FAVORITES" );
static const zone_type_id zone_LOOT_UNSORTED( "LOOT_UNSORTED" );

static const std::string flag_RELOAD_AND_SHOOT( "RELOAD_AND_SHOOT" );

const int ACTIVITY_SEARCH_DISTANCE = 60;

aim_activity_actor::aim_activity_actor()
{
    initial_view_offset = get_avatar().view_offset;
}

aim_activity_actor aim_activity_actor::use_wielded()
{
    return aim_activity_actor();
}

aim_activity_actor aim_activity_actor::use_bionic( const item &fake_gun,
        const units::energy &cost_per_shot )
{
    aim_activity_actor act = aim_activity_actor();
    act.bp_cost_per_shot = cost_per_shot;
    act.fake_weapon = fake_gun;
    return act;
}

aim_activity_actor aim_activity_actor::use_mutation( const item &fake_gun )
{
    aim_activity_actor act = aim_activity_actor();
    act.fake_weapon = fake_gun;
    return act;
}

void aim_activity_actor::start( player_activity &act, Character &/*who*/ )
{
    // Time spent on aiming is determined on the go by the player
    act.moves_total = 1;
    act.moves_left = 1;
}

void aim_activity_actor::do_turn( player_activity &act, Character &who )
{
    if( !who.is_avatar() ) {
        debugmsg( "ACT_AIM not implemented for NPCs" );
        aborted = true;
        act.moves_left = 0;
        return;
    }
    avatar &you = get_avatar();

    item *weapon = get_weapon();
    if( !weapon || !avatar_action::can_fire_weapon( you, get_map(), *weapon ) ) {
        aborted = true;
        act.moves_left = 0;
        return;
    }

    gun_mode gun = weapon->gun_current_mode();
    if( first_turn && gun->has_flag( flag_RELOAD_AND_SHOOT ) && !gun->ammo_remaining() ) {
        if( !load_RAS_weapon() ) {
            aborted = true;
            act.moves_left = 0;
            return;
        }
    }
    cata::optional<shape_factory> shape_gen;
    if( weapon->ammo_current() && weapon->ammo_current()->ammo &&
        weapon->ammo_current()->ammo->shape ) {
        shape_gen = weapon->ammo_current()->ammo->shape;
    }

    g->temp_exit_fullscreen();
    target_handler::trajectory trajectory;
    if( !shape_gen ) {
        trajectory = target_handler::mode_fire( you, *this );
    } else {
        trajectory = target_handler::mode_shaped( you, *shape_gen, *this );
    }
    g->reenter_fullscreen();

    if( aborted ) {
        act.moves_left = 0;
    } else {
        if( !trajectory.empty() ) {
            fin_trajectory = trajectory;
            act.moves_left = 0;
        }
        // If aborting on the first turn, keep 'first_turn' as 'true'.
        // This allows refunding moves spent on unloading RELOAD_AND_SHOOT weapons
        // to simulate avatar not loading them in the first place
        first_turn = false;

        // Allow interrupting activity only during 'aim and fire'.
        // Prevents '.' key for 'aim for 10 turns' from conflicting with '.' key for 'interrupt activity'
        // in case of high input lag (curses, sdl sometimes...), but allows to interrupt aiming
        // if a bug happens / stars align to cause an endless aiming loop.
        act.interruptable_with_kb = action != "AIM";
    }
}

void aim_activity_actor::finish( player_activity &act, Character &who )
{
    act.set_to_null();
    restore_view();
    item *weapon = get_weapon();
    if( !weapon ) {
        return;
    }
    if( aborted ) {
        unload_RAS_weapon();
        if( reload_requested ) {
            // Reload the gun / select different arrows
            // May assign ACT_RELOAD
            g->reload_wielded( true );
        }
        return;
    }

    // Fire!
    gun_mode gun = weapon->gun_current_mode();
    int shots_fired = static_cast<player *>( &who )->fire_gun( fin_trajectory.back(), gun.qty, *gun );

    // TODO: bionic power cost of firing should be derived from a value of the relevant weapon.
    if( shots_fired && ( bp_cost_per_shot > 0_J ) ) {
        who.mod_power_level( -bp_cost_per_shot * shots_fired );
    }
}

void aim_activity_actor::canceled( player_activity &/*act*/, Character &/*who*/ )
{
    restore_view();
    unload_RAS_weapon();
}

void aim_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "fake_weapon", fake_weapon );
    jsout.member( "bp_cost_per_shot", bp_cost_per_shot );
    jsout.member( "first_turn", first_turn );
    jsout.member( "action", action );
    jsout.member( "aif_duration", aif_duration );
    jsout.member( "aiming_at_critter", aiming_at_critter );
    jsout.member( "snap_to_target", snap_to_target );
    jsout.member( "shifting_view", shifting_view );
    jsout.member( "initial_view_offset", initial_view_offset );

    jsout.end_object();
}

std::unique_ptr<activity_actor> aim_activity_actor::deserialize( JsonIn &jsin )
{
    aim_activity_actor actor = aim_activity_actor();

    JsonObject data = jsin.get_object();

    data.read( "fake_weapon", actor.fake_weapon );
    data.read( "bp_cost_per_shot", actor.bp_cost_per_shot );
    data.read( "first_turn", actor.first_turn );
    data.read( "action", actor.action );
    data.read( "aif_duration", actor.aif_duration );
    data.read( "aiming_at_critter", actor.aiming_at_critter );
    data.read( "snap_to_target", actor.snap_to_target );
    data.read( "shifting_view", actor.shifting_view );
    data.read( "initial_view_offset", actor.initial_view_offset );

    return actor.clone();
}

item *aim_activity_actor::get_weapon()
{
    if( fake_weapon.has_value() ) {
        // TODO: check if the player lost relevant bionic/mutation
        return &fake_weapon.value();
    } else {
        // Check for lost gun (e.g. yanked by zombie technician)
        // TODO: check that this is the same gun that was used to start aiming
        item *weapon = &get_player_character().weapon;
        return weapon->is_null() ? nullptr : weapon;
    }
}

void aim_activity_actor::restore_view()
{
    avatar &player_character = get_avatar();
    bool changed_z = player_character.view_offset.z != initial_view_offset.z;
    player_character.view_offset = initial_view_offset;
    if( changed_z ) {
        get_map().invalidate_map_cache( player_character.view_offset.z );
        g->invalidate_main_ui_adaptor();
    }
}

bool aim_activity_actor::load_RAS_weapon()
{
    // TODO: use activity for fetching ammo and loading weapon
    player &you = get_avatar();
    item *weapon = get_weapon();
    gun_mode gun = weapon->gun_current_mode();
    const auto ammo_location_is_valid = [&]() -> bool {
        if( !you.ammo_location )
        {
            return false;
        }
        if( !gun->can_reload_with( you.ammo_location->typeId() ) )
        {
            return false;
        }
        if( square_dist( you.pos(), you.ammo_location.position() ) > 1 )
        {
            return false;
        }
        return true;
    };
    item::reload_option opt = ammo_location_is_valid() ? item::reload_option( &you, weapon,
                              weapon, you.ammo_location ) : you.select_ammo( *gun );
    if( !opt ) {
        // Menu canceled
        return false;
    }
    int reload_time = 0;
    reload_time += opt.moves();
    if( !gun->reload( you, std::move( opt.ammo ), 1 ) ) {
        // Reload not allowed
        return false;
    }

    // Burn 0.2% max base stamina x the strength required to fire.
    you.mod_stamina( gun->get_min_str() * static_cast<int>( 0.002f *
                     get_option<int>( "PLAYER_MAX_STAMINA" ) ) );
    // At low stamina levels, firing starts getting slow.
    int sta_percent = ( 100 * you.get_stamina() ) / you.get_stamina_max();
    reload_time += ( sta_percent < 25 ) ? ( ( 25 - sta_percent ) * 2 ) : 0;

    you.moves -= reload_time;
    return true;
}

void aim_activity_actor::unload_RAS_weapon()
{
    // Unload reload-and-shoot weapons to avoid leaving bows pre-loaded with arrows
    avatar &you = get_avatar();
    item *weapon = get_weapon();
    if( !weapon ) {
        return;
    }

    gun_mode gun = weapon->gun_current_mode();
    if( gun->has_flag( flag_RELOAD_AND_SHOOT ) ) {
        int moves_before_unload = you.moves;

        // Note: this code works only for avatar
        item_location loc = item_location( you, gun.target );
        you.unload( loc );

        // Give back time for unloading as essentially nothing has been done.
        if( first_turn ) {
            you.moves = moves_before_unload;
        }
    }
}

void autodrive_activity_actor::start( player_activity &act, Character &who )
{
    const bool in_vehicle = who.in_vehicle && who.controlling_vehicle;
    const optional_vpart_position vp = get_map().veh_at( who.pos() );
    if( !( vp && in_vehicle ) ) {
        who.cancel_activity();
        return;
    }

    player_vehicle = &vp->vehicle();
    player_vehicle->is_autodriving = true;
    act.moves_left = calendar::INDEFINITELY_LONG;
}

void autodrive_activity_actor::do_turn( player_activity &act, Character &who )
{
    if( who.in_vehicle && who.controlling_vehicle && player_vehicle ) {
        if( who.moves <= 0 ) {
            // out of moves? the driver's not doing anything this turn
            // (but the vehicle will continue moving)
            return;
        }
        switch( player_vehicle->do_autodrive( who ) ) {
            case autodrive_result::ok:
                if( who.moves > 0 ) {
                    // if do_autodrive() didn't eat up all our moves, end the turn
                    // equivalent to player pressing the "pause" button
                    who.moves = 0;
                }
                sounds::reset_markers();
                break;
            case autodrive_result::abort:
                who.cancel_activity();
                break;
            case autodrive_result::finished:
                act.moves_left = 0;
                break;
        }
    } else {
        who.cancel_activity();
    }
}

void autodrive_activity_actor::canceled( player_activity &act, Character &who )
{
    who.add_msg_if_player( m_info, _( "Auto-drive canceled." ) );
    who.omt_path.clear();
    if( player_vehicle ) {
        player_vehicle->stop_autodriving( false );
    }
    act.set_to_null();
}

void autodrive_activity_actor::finish( player_activity &act, Character &who )
{
    who.add_msg_if_player( m_info, _( "You have reached your destination." ) );
    player_vehicle->stop_autodriving( false );
    act.set_to_null();
}

void autodrive_activity_actor::serialize( JsonOut &jsout ) const
{
    // Activity is not being saved but still provide some valid json if called.
    jsout.write_null();
}

std::unique_ptr<activity_actor> autodrive_activity_actor::deserialize( JsonIn & )
{
    return autodrive_activity_actor().clone();
}

void dig_activity_actor::start( player_activity &act, Character & )
{
    act.moves_total = moves_total;
    act.moves_left = moves_total;
}

void dig_activity_actor::do_turn( player_activity &, Character & )
{
    sfx::play_activity_sound( "tool", "shovel", sfx::get_heard_volume( location ) );
    if( calendar::once_every( 1_minutes ) ) {
        //~ Sound of a shovel digging a pit at work!
        sounds::sound( location, 10, sounds::sound_t::activity, _( "hsh!" ) );
    }
}

void dig_activity_actor::finish( player_activity &act, Character &who )
{
    const bool grave = g->m.ter( location ) == t_grave;

    if( grave ) {
        if( one_in( 10 ) ) {
            static const std::array<mtype_id, 5> monids = { {
                    mon_zombie, mon_zombie_fat,
                    mon_zombie_rot, mon_skeleton,
                    mon_zombie_crawler
                }
            };

            g->place_critter_at( random_entry( monids ), byproducts_location );
            g->m.furn_set( location, f_coffin_o );
            who.add_msg_if_player( m_warning, _( "Something crawls out of the coffin!" ) );
        } else {
            g->m.spawn_item( location, itype_bone_human, rng( 5, 15 ) );
            g->m.furn_set( location, f_coffin_c );
        }
        std::vector<item *> dropped = g->m.place_items( item_group_id( "allclothes" ), 50, location,
                                      location, false,
                                      calendar::turn );
        g->m.place_items( item_group_id( "grave" ), 25, location, location, false, calendar::turn );
        g->m.place_items( item_group_id( "jewelry_front" ), 20, location, location, false, calendar::turn );
        for( item * const &it : dropped ) {
            if( it->is_armor() ) {
                it->item_tags.insert( "FILTHY" );
                it->set_damage( rng( 1, it->max_damage() - 1 ) );
            }
        }
        g->events().send<event_type::exhumes_grave>( who.getID() );
    }

    g->m.ter_set( location, ter_id( result_terrain ) );

    for( int i = 0; i < byproducts_count; i++ ) {
        g->m.spawn_items( byproducts_location,
                          item_group::items_from( item_group_id( byproducts_item_group ),
                                  calendar::turn ) );
    }

    const int helpersize = g->u.get_crafting_helpers( 3 ).size();
    who.mod_stored_nutr( 5 - helpersize );
    who.mod_thirst( 5 - helpersize );
    who.mod_fatigue( 10 - ( helpersize * 2 ) );
    if( grave ) {
        who.add_msg_if_player( m_good, _( "You finish exhuming a grave." ) );
    } else {
        who.add_msg_if_player( m_good, _( "You finish digging the %s." ),
                               g->m.ter( location ).obj().name() );
    }

    act.set_to_null();
}

void dig_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "moves", moves_total );
    jsout.member( "location", location );
    jsout.member( "result_terrain", result_terrain );
    jsout.member( "byproducts_location", byproducts_location );
    jsout.member( "byproducts_count", byproducts_count );
    jsout.member( "byproducts_item_group", byproducts_item_group );

    jsout.end_object();
}

std::unique_ptr<activity_actor> dig_activity_actor::deserialize( JsonIn &jsin )
{
    dig_activity_actor actor( 0, tripoint_zero,
                              {}, tripoint_zero, 0, {} );

    JsonObject data = jsin.get_object();

    data.read( "moves", actor.moves_total );
    data.read( "location", actor.location );
    data.read( "result_terrain", actor.result_terrain );
    data.read( "byproducts_location", actor.byproducts_location );
    data.read( "byproducts_count", actor.byproducts_count );
    data.read( "byproducts_item_group", actor.byproducts_item_group );

    return actor.clone();
}

void dig_channel_activity_actor::start( player_activity &act, Character & )
{
    act.moves_total = moves_total;
    act.moves_left = moves_total;
}

void dig_channel_activity_actor::do_turn( player_activity &, Character & )
{
    sfx::play_activity_sound( "tool", "shovel", sfx::get_heard_volume( location ) );
    if( calendar::once_every( 1_minutes ) ) {
        //~ Sound of a shovel digging a pit at work!
        sounds::sound( location, 10, sounds::sound_t::activity, _( "hsh!" ) );
    }
}

void dig_channel_activity_actor::finish( player_activity &act, Character &who )
{

    g->m.ter_set( location, ter_id( result_terrain ) );

    for( int i = 0; i < byproducts_count; i++ ) {
        g->m.spawn_items( byproducts_location,
                          item_group::items_from( item_group_id( byproducts_item_group ),
                                  calendar::turn ) );
    }

    who.mod_stored_kcal( -40 );
    who.mod_thirst( 5 );
    who.mod_fatigue( 10 );
    who.add_msg_if_player( m_good, _( "You finish digging up %s." ),
                           g->m.ter( location ).obj().name() );

    act.set_to_null();
}

void dig_channel_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "moves", moves_total );
    jsout.member( "location", location );
    jsout.member( "result_terrain", result_terrain );
    jsout.member( "byproducts_location", byproducts_location );
    jsout.member( "byproducts_count", byproducts_count );
    jsout.member( "byproducts_item_group", byproducts_item_group );

    jsout.end_object();
}

std::unique_ptr<activity_actor> dig_channel_activity_actor::deserialize( JsonIn &jsin )
{
    dig_channel_activity_actor actor( 0, tripoint_zero,
                                      {}, tripoint_zero, 0, {} );

    JsonObject data = jsin.get_object();

    data.read( "moves", actor.moves_total );
    data.read( "location", actor.location );
    data.read( "result_terrain", actor.result_terrain );
    data.read( "byproducts_location", actor.byproducts_location );
    data.read( "byproducts_count", actor.byproducts_count );
    data.read( "byproducts_item_group", actor.byproducts_item_group );

    return actor.clone();
}

drop_activity_actor::drop_activity_actor( Character &ch, const drop_locations &items,
        bool force_ground, const tripoint &relpos )
    : force_ground( force_ground ), relpos( relpos )
{
    this->items = pickup::reorder_for_dropping( ch, items );
}

void drop_activity_actor::start( player_activity &act, Character & )
{
    // Set moves_left to value other than zero to indicate ongoing activity
    act.moves_total = 1;
    act.moves_left = 1;
}

void drop_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "items", items );
    jsout.member( "force_ground", force_ground );
    jsout.member( "relpos", relpos );

    jsout.end_object();
}

std::unique_ptr<activity_actor> drop_activity_actor::deserialize( JsonIn &jsin )
{
    drop_activity_actor actor;

    JsonObject data = jsin.get_object();

    data.read( "items", actor.items );
    data.read( "force_ground", actor.force_ground );
    data.read( "relpos", actor.relpos );

    return actor.clone();
}

void hacking_activity_actor::start( player_activity &act, Character & )
{
    act.moves_total = to_moves<int>( 5_minutes );
    act.moves_left = to_moves<int>( 5_minutes );
}

enum hack_result {
    HACK_UNABLE,
    HACK_FAIL,
    HACK_NOTHING,
    HACK_SUCCESS
};

enum hack_type {
    HACK_SAFE,
    HACK_DOOR,
    HACK_GAS,
    HACK_NULL
};

static int hack_level( const Character &who )
{
    ///\EFFECT_COMPUTER increases success chance of hacking card readers
    // odds go up with int>8, down with int<8
    // 4 int stat is worth 1 computer skill here
    ///\EFFECT_INT increases success chance of hacking card readers
    return who.get_skill_level( skill_computer ) + who.int_cur / 2 - 8;
}

static hack_result hack_attempt( Character &who, const bool using_bionic )
{
    // TODO: Remove this once player -> Character migration is complete
    {
        player *p = dynamic_cast<player *>( &who );
        p->practice( skill_computer, 20 );
    }

    // only skilled supergenius never cause short circuits, but the odds are low for people
    // with moderate skills
    const int hack_stddev = 5;
    int success = std::ceil( normal_roll( hack_level( who ), hack_stddev ) );
    if( success < 0 ) {
        who.add_msg_if_player( _( "You cause a short circuit!" ) );
        if( using_bionic ) {
            who.mod_power_level( -25_kJ );
        } else {
            who.use_charges( itype_electrohack, 25 );
        }

        if( success <= -5 ) {
            if( !using_bionic ) {
                who.add_msg_if_player( m_bad, _( "Your electrohack is ruined!" ) );
                who.use_amount( itype_electrohack, 1 );
            } else {
                who.add_msg_if_player( m_bad, _( "Your power is drained!" ) );
                who.mod_power_level( units::from_kilojoule( -rng( 25,
                                     units::to_kilojoule( who.get_power_level() ) ) ) );
            }
        }
        return HACK_FAIL;
    } else if( success < 6 ) {
        return HACK_NOTHING;
    } else {
        return HACK_SUCCESS;
    }
}

static hack_type get_hack_type( tripoint examp )
{
    hack_type type = HACK_NULL;
    const furn_t &xfurn_t = g->m.furn( examp ).obj();
    const ter_t &xter_t = g->m.ter( examp ).obj();
    if( xter_t.examine == &iexamine::pay_gas || xfurn_t.examine == &iexamine::pay_gas ) {
        type = HACK_GAS;
    } else if( xter_t.examine == &iexamine::cardreader || xfurn_t.examine == &iexamine::cardreader ) {
        type = HACK_DOOR;
    } else if( xter_t.examine == &iexamine::gunsafe_el || xfurn_t.examine == &iexamine::gunsafe_el ) {
        type = HACK_SAFE;
    }
    return type;
}

hacking_activity_actor::hacking_activity_actor( use_bionic )
    : using_bionic( true )
{
}

void hacking_activity_actor::finish( player_activity &act, Character &who )
{
    tripoint examp = act.placement;
    hack_type type = get_hack_type( examp );
    switch( hack_attempt( who, using_bionic ) ) {
        case HACK_UNABLE:
            who.add_msg_if_player( _( "You cannot hack this." ) );
            break;
        case HACK_FAIL:
            // currently all things that can be hacked have equivalent alarm failure states.
            // this may not always be the case with new hackable things.
            g->events().send<event_type::triggers_alarm>( who.getID() );
            sounds::sound( who.pos(), 60, sounds::sound_t::music, _( "an alarm sound!" ), true, "environment",
                           "alarm" );
            if( examp.z > 0 && !g->timed_events.queued( TIMED_EVENT_WANTED ) ) {
                g->timed_events.add( TIMED_EVENT_WANTED, calendar::turn + 30_minutes, 0,
                                     who.global_sm_location() );
            }
            break;
        case HACK_NOTHING:
            who.add_msg_if_player( _( "You fail the hack, but no alarms are triggered." ) );
            break;
        case HACK_SUCCESS:
            if( type == HACK_GAS ) {
                int tankGasUnits;
                const cata::optional<tripoint> pTank_ = iexamine::getNearFilledGasTank( examp, tankGasUnits );
                if( !pTank_ ) {
                    break;
                }
                const tripoint pTank = *pTank_;
                const cata::optional<tripoint> pGasPump = iexamine::getGasPumpByNumber( examp,
                        uistate.ags_pay_gas_selected_pump );
                if( pGasPump && iexamine::toPumpFuel( pTank, *pGasPump, tankGasUnits ) ) {
                    who.add_msg_if_player( _( "You hack the terminal and route all available fuel to your pump!" ) );
                    sounds::sound( examp, 6, sounds::sound_t::activity,
                                   _( "Glug Glug Glug Glug Glug Glug Glug Glug Glug" ), true, "tool", "gaspump" );
                } else {
                    who.add_msg_if_player( _( "Nothing happens." ) );
                }
            } else if( type == HACK_SAFE ) {
                who.add_msg_if_player( m_good, _( "The door on the safe swings open." ) );
                g->m.furn_set( examp, furn_str_id( "f_safe_o" ) );
            } else if( type == HACK_DOOR ) {
                who.add_msg_if_player( _( "You activate the panel!" ) );
                who.add_msg_if_player( m_good, _( "The nearby doors unlock." ) );
                g->m.ter_set( examp, t_card_reader_broken );
                for( const tripoint &tmp : g->m.points_in_radius( ( examp ), 3 ) ) {
                    if( g->m.ter( tmp ) == t_door_metal_locked ) {
                        g->m.ter_set( tmp, t_door_metal_c );
                    }
                }
            }
            break;
    }
    act.set_to_null();
}

void hacking_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "using_bionic", using_bionic );
    jsout.end_object();
}

std::unique_ptr<activity_actor> hacking_activity_actor::deserialize( JsonIn &jsin )
{
    hacking_activity_actor actor;
    if( jsin.test_null() ) {
        // Old saves might contain a null instead of an object.
        // Since we do not know whether a bionic or an item was chosen we assume
        // it was an item.
        actor.using_bionic = false;
    } else {
        JsonObject jsobj = jsin.get_object();
        jsobj.read( "using_bionic", actor.using_bionic );
    }
    return actor.clone();
}

void move_items_activity_actor::do_turn( player_activity &act, Character &who )
{
    const tripoint dest = relative_destination + who.pos();

    while( who.moves > 0 && !target_items.empty() ) {
        item_location target = std::move( target_items.back() );
        const int quantity = quantities.back();
        target_items.pop_back();
        quantities.pop_back();

        if( !target ) {
            debugmsg( "Lost target item of ACT_MOVE_ITEMS" );
            continue;
        }

        // Don't need to make a copy here since movement can't be canceled
        item &leftovers = *target;
        // Make a copy to be put in the destination location
        item newit = leftovers;

        // Handle charges, quantity == 0 means move all
        if( quantity != 0 && newit.count_by_charges() ) {
            newit.charges = std::min( newit.charges, quantity );
            leftovers.charges -= quantity;
        } else {
            leftovers.charges = 0;
        }

        // Check that we can pick it up.
        if( !newit.made_of( LIQUID ) ) {
            // This is for hauling across zlevels, remove when going up and down stairs
            // is no longer teleportation
            if( newit.is_owned_by( who, true ) ) {
                newit.set_owner( who );
            } else {
                continue;
            }
            const tripoint src = target.position();
            const int distance = src.z == dest.z ? std::max( rl_dist( src, dest ), 1 ) : 1;
            who.mod_moves( -pickup::cost_to_move_item( who, newit ) * distance );
            if( to_vehicle ) {
                put_into_vehicle_or_drop( who, item_drop_reason::deliberate, { newit }, dest );
            } else {
                drop_on_map( who, item_drop_reason::deliberate, { newit }, dest );
            }
            // If we picked up a whole stack, remove the leftover item
            if( leftovers.charges <= 0 ) {
                target.remove_item();
            }
        }
    }

    if( target_items.empty() ) {
        // Nuke the current activity, leaving the backlog alone.
        act.set_to_null();
    }
}

void move_items_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "target_items", target_items );
    jsout.member( "quantities", quantities );
    jsout.member( "to_vehicle", to_vehicle );
    jsout.member( "relative_destination", relative_destination );

    jsout.end_object();
}

std::unique_ptr<activity_actor> move_items_activity_actor::deserialize( JsonIn &jsin )
{
    move_items_activity_actor actor( {}, {}, false, tripoint_zero );

    JsonObject data = jsin.get_object();

    data.read( "target_items", actor.target_items );
    data.read( "quantities", actor.quantities );
    data.read( "to_vehicle", actor.to_vehicle );
    data.read( "relative_destination", actor.relative_destination );

    return actor.clone();
}

void pickup_activity_actor::do_turn( player_activity &, Character &who )
{
    // If we don't have target items bail out
    if( target_items.empty() ) {
        who.cancel_activity();
        return;
    }

    // If the player moves while picking up (i.e.: in a moving vehicle) cancel
    // the activity, only populate starting_pos when grabbing from the ground
    if( starting_pos && *starting_pos != who.pos() ) {
        who.cancel_activity();
        who.add_msg_if_player( _( "Moving canceled auto-pickup." ) );
        return;
    }

    // Auto_resume implies autopickup.
    const bool autopickup = who.activity.auto_resume;

    // False indicates that the player canceled pickup when met with some prompt
    const bool keep_going = pickup::do_pickup( target_items, autopickup );

    // If there are items left we ran out of moves, so continue the activity
    // Otherwise, we are done.
    if( !keep_going || target_items.empty() ) {
        who.cancel_activity();

        if( who.get_value( "THIEF_MODE_KEEP" ) != "YES" ) {
            who.set_value( "THIEF_MODE", "THIEF_ASK" );
        }

        if( !keep_going ) {
            // The user canceled the activity, so we're done
            // AIM might have more pickup activities pending, also cancel them.
            // TODO: Move this to advanced inventory instead of hacking it in here
            cancel_aim_processing();
        }
    }
}

void pickup_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "target_items", target_items );
    jsout.member( "starting_pos", starting_pos );

    jsout.end_object();
}

std::unique_ptr<activity_actor> pickup_activity_actor::deserialize( JsonIn &jsin )
{
    pickup_activity_actor actor( {}, cata::nullopt );

    JsonObject data = jsin.get_object();

    data.read( "target_items", actor.target_items );
    data.read( "starting_pos", actor.starting_pos );

    return actor.clone();
}

void migration_cancel_activity_actor::do_turn( player_activity &act, Character &who )
{
    // Stop the activity
    act.set_to_null();

    // Ensure that neither avatars nor npcs end up in an invalid state
    if( who.is_npc() ) {
        npc &npc_who = dynamic_cast<npc &>( who );
        npc_who.revert_after_activity();
    } else {
        avatar &avatar_who = dynamic_cast<avatar &>( who );
        avatar_who.clear_destination();
        avatar_who.backlog.clear();
    }
}

void migration_cancel_activity_actor::serialize( JsonOut &jsout ) const
{
    // This will probably never be called, but write null to avoid invalid json in
    // the case that it is
    jsout.write_null();
}

std::unique_ptr<activity_actor> migration_cancel_activity_actor::deserialize( JsonIn & )
{
    return migration_cancel_activity_actor().clone();
}

void open_gate_activity_actor::start( player_activity &act, Character & )
{
    act.moves_total = moves_total;
    act.moves_left = moves_total;
}

void open_gate_activity_actor::finish( player_activity &act, Character & )
{
    gates::open_gate( placement );
    act.set_to_null();
}

void open_gate_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "moves", moves_total );
    jsout.member( "placement", placement );

    jsout.end_object();
}

std::unique_ptr<activity_actor> open_gate_activity_actor::deserialize( JsonIn &jsin )
{
    open_gate_activity_actor actor( 0, tripoint_zero );

    JsonObject data = jsin.get_object();

    data.read( "moves", actor.moves_total );
    data.read( "placement", actor.placement );

    return actor.clone();
}

void wash_activity_actor::start( player_activity &act, Character & )
{
    act.moves_total = moves_total;
    act.moves_left = moves_total;
}

stash_activity_actor::stash_activity_actor( Character &ch, const drop_locations &items,
        const tripoint &relpos ) : relpos( relpos )
{
    this->items = pickup::reorder_for_dropping( ch, items );
}

void stash_activity_actor::start( player_activity &act, Character & )
{
    // Set moves_left to value other than zero to indicate ongoing activity
    act.moves_total = 1;
    act.moves_left = 1;
}

void stash_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "items", items );
    jsout.member( "relpos", relpos );

    jsout.end_object();
}

std::unique_ptr<activity_actor> stash_activity_actor::deserialize( JsonIn &jsin )
{
    stash_activity_actor actor;

    JsonObject data = jsin.get_object();

    data.read( "items", actor.items );
    data.read( "relpos", actor.relpos );

    return actor.clone();
}

void wash_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "targets", targets );
    jsout.member( "moves_total", moves_total );

    jsout.end_object();
}

std::unique_ptr<activity_actor> wash_activity_actor::deserialize( JsonIn &jsin )
{
    wash_activity_actor actor;

    JsonObject data = jsin.get_object();

    data.read( "targets", actor.targets );
    data.read( "moves_total", actor.moves_total );

    return actor.clone();
}

void move_loot_activity_actor::start( player_activity &act, Character &who )
{
    // set moves_left to a large number because we don't know how long it will take
    act.moves_left = calendar::INDEFINITELY_LONG;
    const auto abspos = g->m.getabs( who.pos() );

    // if unsorted_zone_tripoints is not empty, it means this is an restart, go to do_turn
    if( !unsorted_zone_tripoints.empty() ) {
        // we have moved too long, recalculate src tile order
        if( last_moved_distance > 16 ) {
            const auto cmp = [abspos]( tripoint a, tripoint b ) {
                const int da = rl_dist( abspos, a );
                const int db = rl_dist( abspos, b );

                return da < db;
            };

            std::sort( unsorted_zone_tripoints.begin(), unsorted_zone_tripoints.end(), cmp );
        }
        return;
    }

    start_pos = abspos;
    auto &mgr = zone_manager::get_manager();
    if( g->m.check_vehicle_zones( g->get_levz() ) ) {
        mgr.cache_vzones();
    }

    // get all src zone tripoints, do not search the z level
    auto unsorted_zone_tripoints_set = mgr.get_near( zone_LOOT_UNSORTED, abspos,
                                       ACTIVITY_SEARCH_DISTANCE );

    // find position waiting for move loot
    for( auto unsorted_zone_tripoints_set_iter = unsorted_zone_tripoints_set.begin();
         unsorted_zone_tripoints_set_iter != unsorted_zone_tripoints_set.end(); ) {
        const tripoint src_abs = *unsorted_zone_tripoints_set_iter;
        const tripoint src_loc = g->m.getlocal( src_abs );

        // skip tiles in IGNORE zone and tiles on fire
        // (to prevent taking out wood off the lit brazier)
        // and inaccessible furniture, like filled charcoal kiln
        if( mgr.has( zone_LOOT_IGNORE, src_abs ) || g->m.get_field( src_loc, fd_fire ) != nullptr ||
            !g->m.can_put_items_ter_furn( src_loc ) ) {
            unsorted_zone_tripoints_set_iter = unsorted_zone_tripoints_set.erase(
                                                   unsorted_zone_tripoints_set_iter );
            continue;
        }

        //nothing to sort?
        const cata::optional<vpart_reference> vp = g->m.veh_at( src_loc ).part_with_feature( "CARGO",
                false );
        if( ( !vp || vp->vehicle().get_items( vp->part_index() ).empty() )
            && g->m.i_at( src_loc ).empty() ) {
            unsorted_zone_tripoints_set_iter = unsorted_zone_tripoints_set.erase(
                                                   unsorted_zone_tripoints_set_iter );
            continue;
        }

        ++unsorted_zone_tripoints_set_iter;
    }

    unsorted_zone_tripoints = get_sorted_tiles_by_distance( abspos, unsorted_zone_tripoints_set );
}

void move_loot_activity_actor::do_turn( player_activity &act, Character &who )
{
    enum class set_destination_result {
        success,
        failed,
        unnecessary,
        outofmap
    };

    auto set_destination = [this]( player_activity & act, Character & who, const tripoint & src_loc ) {
        // checks for npcs
        if( !g->m.inbounds( src_loc ) ) {
            if( !g->m.inbounds( who.pos() ) ) {
                // who is implicitly an NPC that has been moved off the map, so reset the activity
                // and unload them
                who.cancel_activity();
                who.assign_activity( act );
                who.set_moves( 0 );
                g->reload_npcs();
                return set_destination_result::outofmap;
            }
            // get route to nearest adjacent tile
            auto route = route_adjacent( who, src_loc );
            if( route.empty() ) {
                unreachable_src_abs_points.emplace_back( g->m.getabs( src_loc ) );
                return set_destination_result::failed;
            }
            last_moved_distance = route.size();
            who.set_destination( route, act );
            who.cancel_activity();
            return set_destination_result::success;
        }

        bool is_adjacent_or_closer = square_dist( who.pos(), src_loc ) <= 1;
        if( !is_adjacent_or_closer ) {
            // get route to nearest adjacent tile
            auto route = route_adjacent( who, src_loc );
            // check if we found path to source adjacent tile
            if( route.empty() ) {
                unreachable_src_abs_points.emplace_back( g->m.getabs( src_loc ) );
                return set_destination_result::failed;
            }

            // set the destination and restart activity after player arrives there
            // we don't need to check for safe mode,
            // activity will be restarted only if
            // player arrives on destination tile
            last_moved_distance = route.size();
            who.set_destination( route, act );
            who.cancel_activity();
            return set_destination_result::success;
        }
        return set_destination_result::unnecessary;
    };

    for( auto unsorted_zone_tripoints_iter = unsorted_zone_tripoints.begin();
         unsorted_zone_tripoints_iter != unsorted_zone_tripoints.end(); ) {
        const tripoint &src_abs = *unsorted_zone_tripoints_iter;
        const tripoint src_loc = g->m.getlocal( src_abs );

        auto &mgr = zone_manager::get_manager();
        auto abspos = g->m.getabs( who.pos() );

        tripoint dest_loc = tripoint_max;

        vehicle *src_veh, *dest_veh;
        int src_part, dest_part;

        // lambda to check if the item should be skipped
        auto should_skip = [&]( item &this_item ) {
            // skip unpickable liquid
            if( this_item.made_of( LIQUID ) ) {
                return true;
            }

            // skip favorite items in ignore favorite zones
            if( this_item.is_favorite &&
                mgr.has( zone_LOOT_IGNORE_FAVORITES, src_abs ) ) {
                return true;
            }

            return false;
        };
        //Check source for cargo part
        const auto vp = g->m.veh_at( src_loc ).part_with_feature( "CARGO", false );
        if( vp ) {
            src_veh = &vp->vehicle();
            src_part = vp->part_index();
        } else {
            src_veh = nullptr;
            src_part = -1;
        }
        if( items_cache.empty() ) {
            //map_stack and vehicle_stack are different types but inherit from item_stack
            // TODO: use one for loop
            if( vp ) {
                for( auto &it : src_veh->get_items( src_part ) ) {
                    items_cache.push_back( std::make_pair( &it, true ) );
                }
            }
            for( auto &it : g->m.i_at( src_loc ) ) {
                items_cache.push_back( std::make_pair( &it, false ) );
            }
        }

        bool any_item_move_failed = false;
        // loop through all items in the source tile, controlled manually
        for( auto iter = items_cache.begin(); iter != items_cache.end(); ) {

            item &this_item = *iter->first;
            bool in_vehicle = iter->second;

            if( should_skip( this_item ) ) {
                iter = items_cache.erase( iter );
                continue;
            }

            // determine destination zones, search z-level if fov_3d is true
            const auto dest_zones = mgr.get_near_zones_for_item( this_item, src_abs,
                                    ACTIVITY_SEARCH_DISTANCE, g->u.get_faction()->id, fov_3d );

            // check whether the item has destination zones
            if( dest_zones.empty() ) {
                iter = items_cache.erase( iter );
                continue;
            };

            // try to get the best destination point
            for( auto &dest_zone : dest_zones ) {
                // no need to move item if it is already in the destination zone
                if( dest_zone->has_inside( src_abs ) ) {
                    dest_loc = tripoint_min;
                    break;
                }

                tripoint_range<tripoint> dest_zone_tiles = tripoint_range<tripoint>( dest_zone->get_start_point(),
                        dest_zone->get_end_point() );
                for( auto &absp : dest_zone_tiles ) {
                    const tripoint &locp = g->m.getlocal( absp );
                    if( square_dist( absp, src_abs ) > ACTIVITY_SEARCH_DISTANCE ) {
                        continue;
                    }
                    //Check destination for cargo part
                    if( const cata::optional<vpart_reference> vp = g->m.veh_at( locp ).part_with_feature( "CARGO",
                            false ) ) {
                        dest_veh = &vp->vehicle();
                        dest_part = vp->part_index();
                    } else {
                        dest_veh = nullptr;
                        dest_part = -1;
                    }

                    // skip tiles with inaccessible furniture, like filled charcoal kiln
                    if( !g->m.can_put_items_ter_furn( locp ) ||
                        static_cast<int>( g->m.i_at( locp ).size() ) >= MAX_ITEM_IN_SQUARE ) {
                        continue;
                    }

                    // if there's a vehicle with space do not check the tile beneath
                    auto free_space = dest_veh ? dest_veh->free_volume( dest_part ) : g->m.free_volume( locp );
                    // check free space at destination
                    if( free_space >= this_item.volume() ) {
                        dest_loc = locp;
                        break;
                    }
                }
                if( dest_loc != tripoint_max ) {
                    break;
                }
            }

            if( dest_loc == tripoint_max || dest_loc == tripoint_min ) {
                // No destination found or already in the destination zone
                any_item_move_failed = dest_loc == tripoint_max;
                iter = items_cache.erase( iter );
                continue;
            }

            // if we reach here, it means the there are actually items to move
            auto result = set_destination( act, who, src_loc );
            if( result != set_destination_result::unnecessary ) {
                if( result == set_destination_result::success || result == set_destination_result::outofmap ) {
                    return;
                } else if( result == set_destination_result::failed ) {
                    break;
                }
            }

            // if we reach here, it means we are adjacent to the source tile, start moving!
            move_item( who, this_item, this_item.count(), src_loc, dest_loc, in_vehicle ? src_veh : nullptr,
                       in_vehicle ? src_part : -1 );
            // item was moved, reset the destination
            dest_loc = tripoint_max;
            iter = items_cache.erase( iter );

            // no moves left, so return and go to next turn
            if( who.moves <= 0 ) {
                if( items_cache.empty() ) {
                    unsorted_zone_tripoints_iter = unsorted_zone_tripoints.erase( unsorted_zone_tripoints_iter );
                }
                return;
            }
        }
        if( any_item_move_failed ) {
            add_msg( m_info,
                     _( "%s can't move some items to the destination zone. No avialable destination point." ),
                     who.disp_name() );
        }

        // finished this src tile, move to next
        // to avoid infinite loop
        unsorted_zone_tripoints_iter = unsorted_zone_tripoints.erase( unsorted_zone_tripoints_iter );
        items_cache.clear();
    }
    // we're done, set moves_left to 0 to prevent infinite loop
    act.moves_left = 0;
}

void move_loot_activity_actor::finish( player_activity &act, Character &who )
{
    add_msg( m_info, _( "%s sorted out every item possible." ), who.disp_name( false, true ) );
    if( !unreachable_src_abs_points.empty() ) {
        std::string tripoints_message;
        for( auto &tp : unreachable_src_abs_points ) {
            tripoints_message += direction_suffix( start_pos, tp );
            tripoints_message += " ";
        }
        tripoints_message.erase( tripoints_message.size() - 1 );
        add_msg( m_info,
                 _( "%s can't reach the source tile at %s form the start point. Try to sort out loot without a cart." ),
                 who.disp_name(), tripoints_message );
    }
    if( who.is_npc() ) {
        npc *guy = dynamic_cast<npc *>( &who );
        guy->revert_after_activity();
    } else {
        // get route to start poistion
        auto route = route_adjacent( who, g->m.getlocal( start_pos ) );

        if( !route.empty() ) {
            // return to start position
            who.set_destination( route, player_activity() );
        }
    }
    act.set_to_null();
}

void move_loot_activity_actor::canceled( player_activity &act, Character &who )
{
    /* if( !unreachable_src_abs_points.empty() ) {
        const tripoint who_abs = g->m.getabs( who.pos() );
        std::string tripoints_message;
        for( auto &tp : unreachable_src_abs_points ) {
            tripoints_message += direction_suffix( who_abs, tp );
            tripoints_message += " ";
        }
        tripoints_message.erase( tripoints_message.size() - 1 );
        add_msg( m_info, _( "%s can't reach the source tile at %s. Try to sort out loot without a cart." ),
                 who.disp_name(), tripoints_message );
    } */
}

void move_loot_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    //do nothing

    jsout.end_object();
}

std::unique_ptr<activity_actor> move_loot_activity_actor::deserialize( JsonIn &jsin )
{
    move_loot_activity_actor actor;

    JsonObject data = jsin.get_object();

    //do nothing

    return actor.clone();
}


namespace activity_actors
{

// Please keep this alphabetically sorted
const std::unordered_map<activity_id, std::unique_ptr<activity_actor>( * )( JsonIn & )>
deserialize_functions = {
    { activity_id( "ACT_AIM" ), &aim_activity_actor::deserialize },
    { activity_id( "ACT_AUTODRIVE" ), &autodrive_activity_actor::deserialize },
    { activity_id( "ACT_DIG" ), &dig_activity_actor::deserialize },
    { activity_id( "ACT_DIG_CHANNEL" ), &dig_channel_activity_actor::deserialize },
    { activity_id( "ACT_DROP" ), &drop_activity_actor::deserialize },
    { activity_id( "ACT_HACKING" ), &hacking_activity_actor::deserialize },
    { activity_id( "ACT_MIGRATION_CANCEL" ), &migration_cancel_activity_actor::deserialize },
    { activity_id( "ACT_MOVE_ITEMS" ), &move_items_activity_actor::deserialize },
    { activity_id( "ACT_MOVE_LOOT" ), &move_loot_activity_actor::deserialize },
    { activity_id( "ACT_OPEN_GATE" ), &open_gate_activity_actor::deserialize },
    { activity_id( "ACT_PICKUP" ), &pickup_activity_actor::deserialize },
    { activity_id( "ACT_STASH" ), &stash_activity_actor::deserialize },
    { activity_id( "ACT_WASH" ), &wash_activity_actor::deserialize },
};
} // namespace activity_actors

void serialize( const cata::clone_ptr<activity_actor> &actor, JsonOut &jsout )
{
    if( !actor ) {
        jsout.write_null();
    } else {
        jsout.start_object();

        jsout.member( "actor_type", actor->get_type() );
        jsout.member( "actor_data", *actor );

        jsout.end_object();
    }
}

void deserialize( cata::clone_ptr<activity_actor> &actor, JsonIn &jsin )
{
    if( jsin.test_null() ) {
        actor = nullptr;
    } else {
        JsonObject data = jsin.get_object();
        if( data.has_member( "actor_data" ) ) {
            activity_id actor_type;
            data.read( "actor_type", actor_type );
            auto deserializer = activity_actors::deserialize_functions.find( actor_type );
            if( deserializer != activity_actors::deserialize_functions.end() ) {
                actor = deserializer->second( *data.get_raw( "actor_data" ) );
            } else {
                debugmsg( "Failed to find activity actor deserializer for type \"%s\"", actor_type.c_str() );
                actor = nullptr;
            }
        } else {
            debugmsg( "Failed to load activity actor" );
            actor = nullptr;
        }
    }
}
