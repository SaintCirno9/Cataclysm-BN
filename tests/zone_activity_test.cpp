#include "avatar.h"
#include "avatar_action.h"
#include "activity_actor_definitions.h"
#include "catch/catch.hpp"
#include "clzones.h"
#include "faction.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "player.h"
#include "player_helpers.h"
#include "point.h"
#include "vehicle.h"
#include "vpart_position.h"


static const zone_type_id zone_LOOT_CORPSE( "LOOT_CORPSE" );
static const zone_type_id zone_LOOT_CUSTOM( "LOOT_CUSTOM" );
static const zone_type_id zone_LOOT_DRINK( "LOOT_DRINK" );
static const zone_type_id zone_LOOT_FOOD( "LOOT_FOOD" );
static const zone_type_id zone_LOOT_IGNORE( "LOOT_IGNORE" );
static const zone_type_id zone_LOOT_PDRINK( "LOOT_PDRINK" );
static const zone_type_id zone_LOOT_PFOOD( "LOOT_PFOOD" );
static const zone_type_id zone_LOOT_SEEDS( "LOOT_SEEDS" );
static const zone_type_id zone_LOOT_UNSORTED( "LOOT_UNSORTED" );
static const zone_type_id zone_LOOT_WOOD( "LOOT_WOOD" );
static const zone_type_id zone_LOOT_CONTAINERS( "LOOT_CONTAINERS" );
static const zone_type_id zone_LOOT_CLOTHING( "LOOT_CLOTHING" );
static const zone_type_id zone_LOOT_FCLOTHING( "LOOT_FCLOTHING" );

static const std::string flag_FILTHY( "FILTHY" );

void sort_zone_loot( player &dummy, map &m, int src_tile_count )
{
    dummy.assign_activity( player_activity( move_loot_activity_actor() ) );
    process_activity( dummy );
    // Each tile has to be processed at most once.
    for( int i = 0; i < src_tile_count; i++ ) {
        while( dummy.has_destination() ) {
            auto act = dummy.get_next_auto_move_direction();
            if( act == ACTION_NULL ) {
                dummy.clear_destination();
                break;
            }
            point dest_delta = get_delta_from_movement_action( act, iso_rotate::yes );
            avatar_action::move( *dummy.as_avatar(), m, dest_delta );
        }
        if( dummy.has_destination_activity() ) {
            // starts destination activity after the player successfully reached destination
            dummy.start_destination_activity();
        }
        process_activity( dummy );
    }
}

void clear_zone( zone_manager *mgr )
{
    mgr->reset_manager();
    mgr = &zone_manager::get_manager();
}

void place_items_in_zone( map &m, tripoint &start, tripoint &end, item &it, int count,
                          bool is_vehicle_zone = false )
{
    if( is_vehicle_zone ) {
        for( int x = start.x; x <= end.x; x++ ) {
            for( int y = start.y; y <= end.y; y++ ) {
                tripoint loc( x, y, start.z );
                if( auto vp = g->m.veh_at( loc ).part_with_feature( "CARGO", false ) ) {
                    vehicle &veh = vp->vehicle();
                    int vpart = vp->part_index();
                    auto v_stack = veh.get_items( vp->part_index() );
                    for( int i = 0; i < count; i++ ) {
                        veh.add_item( vpart, it );
                    }
                    int cnt = 0;
                    for( auto &v_it : veh.get_items( vpart ) ) {
                        if( v_it.tname() == it.tname() ) {
                            cnt += v_it.count();
                        }
                    }
                    INFO( "Checking for " << count << " " << it.tname() << " in " << loc );
                    REQUIRE( cnt == count );
                }
            }
        }
    } else {
        for( int x = start.x; x <= end.x; x++ ) {
            for( int y = start.y; y <= end.y; y++ ) {
                tripoint loc( x, y, start.z );
                for( int i = 0; i < count; i++ ) {
                    m.add_item_or_charges( loc, it );
                }
                int cnt = 0;
                for( auto &m_it : m.i_at( loc ) ) {
                    if( m_it.tname() == it.tname() ) {
                        cnt += m_it.count();
                    }
                }
                INFO( "Checking for " << count << " " << it.tname() << " in " << loc );
                REQUIRE( cnt == count );
            }
        }
    }
}

void check_items_in_zone( map &m, tripoint &start, tripoint &end, item &it, int count,
                          bool require = false, bool is_vehicle_zone = false )
{
    int cnt = 0;
    if( is_vehicle_zone ) {
        for( int x = start.x; x <= end.x; x++ ) {
            for( int y = start.y; y <= end.y; y++ ) {
                tripoint loc( x, y, start.z );
                if( auto vp = g->m.veh_at( loc ).part_with_feature( "CARGO", false ) ) {
                    auto v_stack = vp->vehicle().get_items( vp->part_index() );
                    for( auto &v_it : v_stack ) {
                        if( v_it.tname() == it.tname() ) {
                            cnt += v_it.count();
                        }
                    }
                }
            }
        }
    }
    for( int x = start.x; x <= end.x; x++ ) {
        for( int y = start.y; y <= end.y; y++ ) {
            for( auto &m_it : m.i_at( { x, y, start.z } ) ) {
                if( m_it.tname() == it.tname() ) {
                    cnt += m_it.count();
                }
            }
        }
    }
    if( require ) {
        INFO( "Checking for " << count << " " << it.tname() << " in zone" << start << " to " << end );
        REQUIRE( cnt == count );
    } else {
        INFO( "Checking for " << count << " " << it.tname() << " in zone" << start << " to " << end );
        CHECK( cnt == count );
    }
}

TEST_CASE( "Sort out the zone", "[activity][sort_zone]" )
{
    item an_item( "bottle_glass" );
    item a_cloth( "tshirt" );
    item a_filthy_cloth( "tshirt" );
    a_filthy_cloth.set_flag( flag_FILTHY );
    item a_firewood( "log" );
    item a_corpse = item::make_corpse();
    item a_food( "protein_bar_evac" );
    item a_drink( "jar_glass" );
    REQUIRE( a_drink.is_watertight_container() );
    a_drink.put_in( item( "water", calendar::start_of_cataclysm, 2 ) );
    item a_comestible_food( "meat" );
    item a_comestible_drink( "jar_glass" );
    // If it's not watertight the milk will spill.
    REQUIRE( a_comestible_drink.is_watertight_container() );
    a_comestible_drink.put_in( item( "milk", calendar::start_of_cataclysm, 2 ) );


    zone_manager *mgr = &zone_manager::get_manager();
    faction_id fac = g->u.get_faction()->id;
    map &m = get_map();
    player &dummy = get_avatar();
    clear_map();
    clear_vehicles();
    clear_zone( mgr );
    dummy.clear_destination();

    auto add_zone = [&mgr, &m]( const std::string & name, const zone_type_id & type,
                                const faction_id & faction,
                                bool invert, bool enabled,
                                const tripoint & start, const tripoint & end,
    shared_ptr_fast<zone_options> options = nullptr ) {
        const tripoint start_abs = m.getabs( start );
        const tripoint end_abs = m.getabs( end );
        mgr->add( name, type, faction, invert, enabled, start_abs, end_abs, options );
        REQUIRE( mgr->get_zones( type, start_abs ).size() == 1 );
    };
    
    GIVEN( "A 1x1 size unsorted zone with some items inside " ) {
        tripoint src_tile( 60, 60, 0 );
        add_zone( "Loot:Unsorted", zone_LOOT_UNSORTED, fac, false, true, src_tile, src_tile );
        place_items_in_zone( m, src_tile, src_tile, an_item, 10 );
        AND_GIVEN( "A 1x1 size custom zone with no fliter option, in 60 tiles range" ) {
            tripoint dest_tile( 61, 61, 0 );
            add_zone( "Loot:Custom", zone_LOOT_CUSTOM, fac, false, true, dest_tile, dest_tile );
            check_items_in_zone( m, dest_tile, dest_tile, an_item, 0, true );

            WHEN( "Character stand next to the Unsorted zone" ) {
                // It is said to be the local pos, but is equal to the absolute pos
                dummy.setpos( {61, 61, 0} );
                THEN( "Character start sorting the zone" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "The items should be sorted to the custom zone" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 0 );
                        check_items_in_zone( m, dest_tile, dest_tile, an_item, 10 );
                    }
                }

            }
            WHEN( "Character stand away from the Unsorted zone, but in 60 tiles range" ) {
                dummy.setpos( {70, 70, 0} );
                THEN( "Character start sorting the zone" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "The items should be sorted to the custom zone" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 0 );
                        check_items_in_zone( m, dest_tile, dest_tile, an_item, 10 );
                    }
                }
            }
            WHEN( "Character stand away from the Unsorted zone, not in 60 tiles range" ) {
                dummy.setpos( {170, 170, 0} );
                THEN( "Character start sorting the zone" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "No items should be sorted to the custom zone" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 10 );
                        check_items_in_zone( m, dest_tile, dest_tile, an_item, 0 );
                    }
                }
            }
        }
        AND_GIVEN( "A 1x1 size custom zone with no fliter option, not in 60 tiles range" ) {
            tripoint dest_tile( 161, 161, 0 );
            add_zone( "Loot:Custom", zone_LOOT_CUSTOM, fac, false, true, dest_tile, dest_tile );
            check_items_in_zone( m, dest_tile, dest_tile, an_item, 0, true );
            WHEN( "Character stand away from the Unsorted zone, but in 60 tiles range" ) {
                dummy.setpos( {70, 70, 0} );
                THEN( "Character start sorting the zone" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "No items should be sorted to the custom zone" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 10 );
                        check_items_in_zone( m, dest_tile, dest_tile, an_item, 0 );
                    }
                }
            }
        }
    }
    GIVEN( "A 1x1 size unsorted zone with some items inside, and a Character" ) {
        tripoint src_tile( 60, 60, 0 );
        add_zone( "Loot:Unsorted", zone_LOOT_UNSORTED, fac, false, true, src_tile, src_tile );
        place_items_in_zone( m, src_tile, src_tile, an_item, 10 );
        dummy.setpos( {70, 70, 0} );
        AND_GIVEN( "Two 1x1 size custom zone with no fliter option" ) {
            tripoint dest_tile1( 61, 61, 0 );
            tripoint dest_tile2( 62, 62, 0 );
            add_zone( "Loot:Custom1", zone_LOOT_CUSTOM, fac, false, true, dest_tile1, dest_tile1 );
            add_zone( "Loot:Custom2", zone_LOOT_CUSTOM, fac, false, true, dest_tile2, dest_tile2 );
            check_items_in_zone( m, dest_tile1, dest_tile1, an_item, 0, true );
            check_items_in_zone( m, dest_tile2, dest_tile2, an_item, 0, true );
            THEN( "Character start sorting the zone" ) {
                sort_zone_loot( dummy, m, 1 );
                AND_THEN( "Items should be sorted to Loot:Custom1" ) {
                    check_items_in_zone( m, src_tile, src_tile, an_item, 0 );
                    check_items_in_zone( m, dest_tile1, dest_tile1, an_item, 10 );
                    check_items_in_zone( m, dest_tile2, dest_tile2, an_item, 0 );
                }
            }
        }
        AND_GIVEN( "Two 1x1 size custom zone with no fliter option" ) {
            tripoint dest_tile1( 61, 61, 0 );
            tripoint dest_tile2( 62, 62, 0 );
            add_zone( "Loot:Custom1", zone_LOOT_CUSTOM, fac, false, true, dest_tile1, dest_tile1 );
            add_zone( "Loot:Custom2", zone_LOOT_CUSTOM, fac, false, true, dest_tile2, dest_tile2 );
            check_items_in_zone( m, dest_tile1, dest_tile1, an_item, 0, true );
            check_items_in_zone( m, dest_tile2, dest_tile2, an_item, 0, true );
            THEN( "Swap the two custom zones" ) {
                auto zones = mgr->get_zones( );
                mgr->swap( zones[1], zones[2] );
                // Update map zone cache to apply the swap
                mgr->cache_data();
                THEN( "Character start sorting the zone" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "Items should be sorted to Loot:Custom2" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 0 );
                        check_items_in_zone( m, dest_tile1, dest_tile1, an_item, 0 );
                        check_items_in_zone( m, dest_tile2, dest_tile2, an_item, 10 );
                    }
                }
            }
        }
        AND_GIVEN( "Two 1x1 size custom zone, the second one has filter option match the item name" ) {
            tripoint dest_tile1( 61, 61, 0 );
            tripoint dest_tile2( 62, 62, 0 );
            auto loot_option = std::dynamic_pointer_cast<loot_options>( zone_options::create(
                                   zone_LOOT_CUSTOM ) );
            loot_option->set_mark( an_item.tname() );
            add_zone( "Loot:Custom1", zone_LOOT_CUSTOM, fac, false, true, dest_tile1, dest_tile1 );
            add_zone( "Loot:Custom2", zone_LOOT_CUSTOM, fac, false, true, dest_tile2, dest_tile2, loot_option );
            check_items_in_zone( m, dest_tile1, dest_tile1, an_item, 0, true );
            check_items_in_zone( m, dest_tile2, dest_tile2, an_item, 0, true );
            THEN( "Character start sorting the zone" ) {
                sort_zone_loot( dummy, m, 1 );
                AND_THEN( "Items should be sorted to Loot:Custom2" ) {
                    check_items_in_zone( m, src_tile, src_tile, an_item, 0 );
                    check_items_in_zone( m, dest_tile1, dest_tile1, an_item, 0 );
                    check_items_in_zone( m, dest_tile2, dest_tile2, an_item, 10 );
                }
            }
        }
    }
    GIVEN( "A 4x4 size unsorted zone, each tile has some items, and a dummy characher away from the unsorted zone" ) {
        tripoint src_zone_start( 60, 60, 0 );
        tripoint src_zone_end( 63, 63, 0 );
        add_zone( "Loot:Unsorted", zone_LOOT_UNSORTED, fac, false, true, src_zone_start, src_zone_end );
        place_items_in_zone( m, src_zone_start, src_zone_end, an_item, 10 );
        dummy.setpos( {65, 65, 0} );
        tripoint dest_zone_start( 70, 70, 0 );
        tripoint dest_zone_end( 73, 73, 0 );
        AND_GIVEN( "A 4x4 size custom zone with no option" ) {
            add_zone( "Loot:Custom", zone_LOOT_CUSTOM, fac, false, true, dest_zone_start, dest_zone_end );
            check_items_in_zone( m, dest_zone_start, dest_zone_end, an_item, 0, true );
            THEN( "Character start sorting the zone" ) {
                sort_zone_loot( dummy, m, 16 );
                AND_THEN( "The items should be sorted to the custom zone" ) {
                    check_items_in_zone( m, dest_zone_start, dest_zone_end, an_item, 160 );
                }
            }
        }
    }
    GIVEN( "A 1x1 un-sorted zone with different items, and a dummy character away from the zone" ) {
        tripoint src_tile( 60, 60, 0 );
        add_zone( "Loot:Unsorted", zone_LOOT_UNSORTED, fac, false, true, src_tile, src_tile );
        place_items_in_zone( m, src_tile, src_tile, an_item, 5 );
        place_items_in_zone( m, src_tile, src_tile, a_cloth, 5 );
        place_items_in_zone( m, src_tile, src_tile, a_filthy_cloth, 5 );
        place_items_in_zone( m, src_tile, src_tile, a_corpse, 5 );
        place_items_in_zone( m, src_tile, src_tile, a_firewood, 5 );
        place_items_in_zone( m, src_tile, src_tile, a_food, 5 );
        place_items_in_zone( m, src_tile, src_tile, a_drink, 5 );
        place_items_in_zone( m, src_tile, src_tile, a_comestible_food, 5 );
        place_items_in_zone( m, src_tile, src_tile, a_comestible_drink, 5 );
        dummy.setpos( {65, 65, 0} );
        AND_GIVEN( "A custom loot zone with no options" ) {
            tripoint dest_tile( 70, 70, 0 );
            add_zone( "Loot:Custom", zone_LOOT_CUSTOM, fac, true, true, dest_tile, dest_tile );
            check_items_in_zone( m, dest_tile, dest_tile, an_item, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_cloth, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_filthy_cloth, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_corpse, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_firewood, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_food, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_drink, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_comestible_food, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_comestible_drink, 0, true );
            THEN( "Character start sorting the zone" ) {
                sort_zone_loot( dummy, m, 1 );
                AND_THEN( "The items should be sorted to the zone" ) {
                    check_items_in_zone( m, dest_tile, dest_tile, an_item, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_cloth, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_filthy_cloth, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_corpse, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_firewood, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_food, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_drink, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_comestible_food, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_comestible_drink, 5 );
                }
            }
        }

        AND_GIVEN( "A custom loot zone with options partially matching item names" ) {
            tripoint dest_tile( 70, 70, 0 );
            auto loot_option = std::dynamic_pointer_cast<loot_options>( zone_options::create(
                                   zone_LOOT_CUSTOM ) );
            loot_option->set_mark( a_cloth.tname() + "," + a_filthy_cloth.tname() );
            add_zone( "Loot:Custom", zone_LOOT_CUSTOM, fac, true, true, dest_tile, dest_tile,
                      loot_option );

            check_items_in_zone( m, dest_tile, dest_tile, an_item, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_cloth, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_filthy_cloth, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_corpse, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_firewood, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_food, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_drink, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_comestible_food, 0, true );
            check_items_in_zone( m, dest_tile, dest_tile, a_comestible_drink, 0, true );
            THEN( "Character start sorting the zone" ) {
                sort_zone_loot( dummy, m, 1 );
                AND_THEN( "Only two kinds of items should be sorted to the zone" ) {
                    check_items_in_zone( m, dest_tile, dest_tile, an_item, 0 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_cloth, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_filthy_cloth, 5 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_corpse, 0 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_firewood, 0 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_food, 0 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_drink, 0 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_comestible_food, 0 );
                    check_items_in_zone( m, dest_tile, dest_tile, a_comestible_drink, 0 );
                }
            }
        }

        AND_GIVEN( "Some loot zones that match the items" ) {
            tripoint dest_container_tile( 70, 70, 0 );
            tripoint dest_cloth_tile( 71, 71, 0 );
            tripoint dest_filthy_cloth_tile( 72, 72, 0 );
            tripoint dest_corpse_tile( 73, 73, 0 );
            tripoint dest_firewood_tile( 74, 74, 0 );
            tripoint dest_food_tile( 75, 75, 0 );
            tripoint dest_drink_tile( 76, 76, 0 );
            tripoint dest_comestible_food_tile( 77, 77, 0 );
            tripoint dest_comestible_drink_tile( 78, 78, 0 );
            WHEN( "Each item has a target zone" ) {
                add_zone( "Loot:Container", zone_LOOT_CONTAINERS, fac, true, true, dest_container_tile,
                          dest_container_tile );
                add_zone( "Loot:Clothing", zone_LOOT_CLOTHING, fac, true, true, dest_cloth_tile, dest_cloth_tile );
                add_zone( "Loot:Filthy Clothing", zone_LOOT_FCLOTHING, fac, true, true,
                          dest_filthy_cloth_tile, dest_filthy_cloth_tile );
                add_zone( "Loot:Corpses", zone_LOOT_CORPSE, fac, true, true, dest_corpse_tile, dest_corpse_tile );
                add_zone( "Loot:Wood", zone_LOOT_WOOD, fac, true, true, dest_firewood_tile,
                          dest_firewood_tile );
                add_zone( "Loot:Food", zone_LOOT_FOOD, fac, true, true, dest_food_tile, dest_food_tile );
                add_zone( "Loot:Drink", zone_LOOT_DRINK, fac, true, true, dest_drink_tile, dest_drink_tile );
                add_zone( "Loot:Comestible Food", zone_LOOT_PFOOD, fac, true, true,
                          dest_comestible_food_tile, dest_comestible_food_tile );
                add_zone( "Loot:Comestible Drink", zone_LOOT_PDRINK, fac, true, true,
                          dest_comestible_drink_tile, dest_comestible_drink_tile );









                check_items_in_zone( m, dest_container_tile, dest_container_tile, an_item, 0, true );
                check_items_in_zone( m, dest_cloth_tile, dest_cloth_tile, a_cloth, 0, true );
                check_items_in_zone( m, dest_filthy_cloth_tile, dest_filthy_cloth_tile, a_filthy_cloth, 0, true );
                check_items_in_zone( m, dest_corpse_tile, dest_corpse_tile, a_corpse, 0, true );
                check_items_in_zone( m, dest_firewood_tile, dest_firewood_tile, a_firewood, 0, true );
                check_items_in_zone( m, dest_food_tile, dest_food_tile, a_food, 0, true );
                check_items_in_zone( m, dest_drink_tile, dest_drink_tile, a_drink, 0, true );
                check_items_in_zone( m, dest_comestible_food_tile, dest_comestible_food_tile,
                                     a_comestible_food, 0, true );
                check_items_in_zone( m, dest_comestible_drink_tile, dest_comestible_drink_tile,
                                     a_comestible_drink, 0, true );
                THEN( "Character start sorting the zones" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "The items should be sorted to the zone" ) {
                        check_items_in_zone( m, dest_container_tile, dest_container_tile, an_item, 5 );
                        check_items_in_zone( m, dest_cloth_tile, dest_cloth_tile, a_cloth, 5 );
                        check_items_in_zone( m, dest_filthy_cloth_tile, dest_filthy_cloth_tile, a_filthy_cloth, 5 );
                        check_items_in_zone( m, dest_corpse_tile, dest_corpse_tile, a_corpse, 5 );
                        check_items_in_zone( m, dest_firewood_tile, dest_firewood_tile, a_firewood, 5 );
                        check_items_in_zone( m, dest_food_tile, dest_food_tile, a_food, 5 );
                        check_items_in_zone( m, dest_drink_tile, dest_drink_tile, a_drink, 5 );
                        check_items_in_zone( m, dest_comestible_food_tile, dest_comestible_food_tile,
                                             a_comestible_food, 5 );
                        check_items_in_zone( m, dest_comestible_drink_tile, dest_comestible_drink_tile,
                                             a_comestible_drink, 5 );

                    }
                }
            }
            WHEN( "Some item share a target zone (No priority zone)" ) {
                add_zone( "Loot:Container", zone_LOOT_CONTAINERS, fac, true, true, dest_container_tile,
                          dest_container_tile );
                add_zone( "Loot:Clothing", zone_LOOT_CLOTHING, fac, true, true, dest_cloth_tile, dest_cloth_tile );
                add_zone( "Loot:Corpses", zone_LOOT_CORPSE, fac, true, true, dest_corpse_tile, dest_corpse_tile );
                add_zone( "Loot:Wood", zone_LOOT_WOOD, fac, true, true, dest_firewood_tile,
                          dest_firewood_tile );
                add_zone( "Loot:Food", zone_LOOT_FOOD, fac, true, true, dest_food_tile, dest_food_tile );
                check_items_in_zone( m, dest_container_tile, dest_container_tile, an_item, 0, true );
                check_items_in_zone( m, dest_cloth_tile, dest_cloth_tile, a_cloth, 0, true );
                check_items_in_zone( m, dest_corpse_tile, dest_corpse_tile, a_corpse, 0, true );
                check_items_in_zone( m, dest_firewood_tile, dest_firewood_tile, a_firewood, 0, true );
                check_items_in_zone( m, dest_food_tile, dest_food_tile, a_food, 0, true );
                THEN( "Character start sorting the zones" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "The items should be sorted to the zone" ) {
                        check_items_in_zone( m, dest_container_tile, dest_container_tile, an_item, 5 );
                        check_items_in_zone( m, dest_cloth_tile, dest_cloth_tile, a_cloth, 5 );
                        check_items_in_zone( m, dest_cloth_tile, dest_cloth_tile, a_filthy_cloth, 5 );
                        check_items_in_zone( m, dest_corpse_tile, dest_corpse_tile, a_corpse, 5 );
                        check_items_in_zone( m, dest_firewood_tile, dest_firewood_tile, a_firewood, 5 );
                        check_items_in_zone( m, dest_food_tile, dest_food_tile, a_food, 5 );
                        check_items_in_zone( m, dest_food_tile, dest_food_tile, a_drink, 5 );
                        check_items_in_zone( m, dest_food_tile, dest_food_tile,
                                             a_comestible_food, 5 );
                        check_items_in_zone( m, dest_food_tile, dest_food_tile,
                                             a_comestible_drink, 5 );

                    }
                }
            }
        }
    }
    GIVEN( "A dummy characher" ) {
        dummy.setpos( {70, 70, 0} );
        AND_GIVEN( "A 1x1 size unsorted zone with some items inside" ) {
            tripoint src_tile( 60, 60, 0 );
            add_zone( "Loot:Unsorted", zone_LOOT_UNSORTED, fac, false, true, src_tile, src_tile );
            place_items_in_zone( m, src_tile, src_tile, an_item, 500 );
            AND_GIVEN( "A 1x1 size custom zone with no fliter option but attached to a vehicle" ) {
                tripoint dest_tile( 61, 61, 0 );
                vehicle *veh_ptr = g->m.add_vehicle( vproto_id( "shopping_cart" ), dest_tile, 0_degrees, 0, 0 );
                REQUIRE( veh_ptr != nullptr );
                REQUIRE( dest_tile == veh_ptr->global_pos3() );
                auto cargo_parts = veh_ptr->get_parts_at( dest_tile, "CARGO", part_status_flag::any );
                REQUIRE( !cargo_parts.empty( ) );
                vehicle_part *cargo_part = cargo_parts.front();
                REQUIRE( cargo_part != nullptr );
                //Must not be broken yet
                REQUIRE( !cargo_part->is_broken() );
                mgr->create_vehicle_loot_zone( *veh_ptr, cargo_part->mount, zone_data( "Loot:Custom",
                                               zone_LOOT_CUSTOM, fac,
                                               false, true, m.getabs( dest_tile ), m.getabs( dest_tile ) ) );

                int cargo_part_index = veh_ptr->index_of_part( cargo_part );
                // Make sure the cargo has no items, since the vehicle may spawn with items in it
                for( auto &it : veh_ptr->get_items( cargo_part_index ) ) {
                    veh_ptr->remove_item( cargo_part_index, &it );
                }
                check_items_in_zone( m, dest_tile, dest_tile, an_item, 0, true );
                WHEN( "Vehicle cargo is not broken" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "The items should be sorted to the vehicle cargo, reach the volume limit" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 300 );
                        check_items_in_zone( m, dest_tile, dest_tile, an_item, 200, false, true );
                    }
                }
                WHEN( "Vehicle cargo is broken" ) {
                    //For some reason (0 - cargo_part->hp()) is just not enough to destroy a part
                    REQUIRE( veh_ptr->mod_hp( *cargo_part, -( 1 + cargo_part->hp() ), DT_BASH ) );
                    //Now it must be broken
                    REQUIRE( cargo_part->is_broken() );
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "No items should be sorted to the vehicle cargo, since the part is broken" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 500 );
                        check_items_in_zone( m, dest_tile, dest_tile, an_item, 0, false, true );
                    }
                }
                WHEN( "Vehicle is moved" ) {
                    tripoint dp( 10, 0, 0 );
                    m.displace_vehicle( *veh_ptr, dp );
                    sort_zone_loot( dummy, m, 1 );
                    // cargo pos turns out not to be the position of the vehicle anymore, why?
                    /* auto cargo_parts = veh_ptr->get_parts_at( veh_ptr->global_pos3(), "CARGO", part_status_flag::any );
                    REQUIRE( !cargo_parts.empty( ) ); */
                    tripoint cargo_pos = veh_ptr->global_pos3() + tripoint( -1, -1, 0 );
                    AND_THEN( "The items should be sorted to the vehicle cargo, reach the volume limit" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 300 );
                        check_items_in_zone( m, cargo_pos, cargo_pos, an_item, 200, false, true );
                    }
                }
            }
        }
        AND_GIVEN( "A 1x1 size unsorted zone attached to a vehicle, with some items inside the vehicle and some items on the ground" ) {
            tripoint src_tile( 60, 60, 0 );
            vehicle *veh_ptr = g->m.add_vehicle( vproto_id( "shopping_cart" ), src_tile, 0_degrees, 0, 0 );
            REQUIRE( veh_ptr != nullptr );
            REQUIRE( src_tile == veh_ptr->global_pos3() );
            auto cargo_parts = veh_ptr->get_parts_at( src_tile, "CARGO", part_status_flag::any );
            REQUIRE( !cargo_parts.empty( ) );
            vehicle_part *cargo_part = cargo_parts.front();
            REQUIRE( cargo_part != nullptr );
            //Must not be broken yet
            REQUIRE( !cargo_part->is_broken() );
            mgr->create_vehicle_loot_zone( *veh_ptr, cargo_part->mount, zone_data( "Loot:Unsorted",
                                           zone_LOOT_UNSORTED, fac, false, true, m.getabs( src_tile ), m.getabs( src_tile ) ) );

            int cargo_part_index = veh_ptr->index_of_part( cargo_part );
            // Make sure the cargo has no items, since the vehicle may spawn with items in it
            for( auto &it : veh_ptr->get_items( cargo_part_index ) ) {
                veh_ptr->remove_item( cargo_part_index, &it );
            }
            place_items_in_zone( m, src_tile, src_tile, an_item, 200, true );
            place_items_in_zone( m, src_tile, src_tile, an_item, 300 );
            AND_GIVEN( "A 1x1 size custom zone with no options" ) {
                tripoint dest_tile( 61, 61, 0 );
                add_zone( "Loot:Custom", zone_LOOT_CUSTOM, fac, false, true, dest_tile, dest_tile );
                check_items_in_zone( m, dest_tile, dest_tile, an_item, 0, true );
                WHEN( "Vehicle cargo is not broken" ) {
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "The items should be sorted to the dest tile" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 0, false, true );
                        check_items_in_zone( m, dest_tile, dest_tile, an_item, 500 );
                    }
                }
                WHEN( "Vehicle cargo is broken" ) {
                    //For some reason (0 - cargo_part->hp()) is just not enough to destroy a part
                    REQUIRE( veh_ptr->mod_hp( *cargo_part, -( 1 + cargo_part->hp() ), DT_BASH ) );
                    //Now it must be broken
                    REQUIRE( cargo_part->is_broken() );
                    sort_zone_loot( dummy, m, 1 );
                    AND_THEN( "The items should be sorted to the dest tile" ) {
                        check_items_in_zone( m, src_tile, src_tile, an_item, 0, false, true );
                        check_items_in_zone( m, dest_tile, dest_tile, an_item, 500 );
                    }
                }
            }
        }
    }
}
