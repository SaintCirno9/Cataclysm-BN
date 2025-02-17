#pragma once
#ifndef CATA_SRC_ACTIVITY_ACTOR_H
#define CATA_SRC_ACTIVITY_ACTOR_H

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "activity_type.h"
#include "clone_ptr.h"

class Character;
class JsonIn;
class JsonOut;
class player_activity;

struct act_progress_message {
    /**
     * Whether activity actor implements the method.
     * TODO: remove once migration to actors is complete.
     */
    bool implemented = true;

    std::optional<std::string> msg_extra_info;
    std::optional<std::string> msg_full;

    /**
     * The text will completely overwrite default message.
     */
    static act_progress_message make_full( std::string &&text ) {
        act_progress_message ret;
        ret.msg_full = std::move( text );
        return ret;
    }

    /**
     * The text will be appended to default message.
     */
    static act_progress_message make_extra_info( std::string &&text ) {
        act_progress_message ret;
        ret.msg_extra_info = std::move( text );
        return ret;
    }

    /**
     * There will be no message shown.
     */
    static act_progress_message make_empty() {
        return act_progress_message{};
    }
};

class activity_actor
{
    private:
        /**
         * Returns true if `this` activity is resumable, and `this` and @p other
         * are "equivalent" i.e. similar enough that `this` activity
         * can be resumed instead of starting @p other.
         * Many activities are not resumable, so the default is returning
         * false.
         * @pre @p other is the same type of actor as `this`
         */
        virtual bool can_resume_with_internal( const activity_actor &,
                                               const Character & ) const {
            return false;
        }

    public:
        virtual ~activity_actor() = default;

        /**
         * Should return the activity id of the corresponding activity
         */
        virtual activity_id get_type() const = 0;

        /**
         * Called once at the start of the activity.
         * This may be used to preform setup actions and/or set
         * player_activity::moves_left/moves_total.
         */
        virtual void start( player_activity &act, Character &who ) = 0;

        /**
         * Called on every turn of the activity
         * It may be used to stop the activity prematurely by setting it to null.
         */
        virtual void do_turn( player_activity &act, Character &who ) = 0;

        /**
         * Called when the activity runs out of moves, assuming that it has not
         * already been set to null
         */
        virtual void finish( player_activity &act, Character &who ) = 0;

        /**
         * Called just before Character::cancel_activity() executes.
         * This may be used to perform cleanup
         */
        virtual void canceled( player_activity &/*act*/, Character &/*who*/ ) {};

        /**
         * Called in player_activity::can_resume_with
         * which allows suspended activities to be resumed instead of
         * starting a new activity in certain cases.
         * Checks that @p other has the same type as `this` so that
         * `can_resume_with_internal` can safely `static_cast` @p other.
         */
        bool can_resume_with( const activity_actor &other, const Character &who ) const {
            if( other.get_type() == get_type() ) {
                return can_resume_with_internal( other, who );
            }

            return false;
        }

        /**
         * Returns a deep copy of this object. Example implementation:
         * \code
         * class my_activity_actor {
         *     std::unique_ptr<activity_actor> clone() const override {
         *         return std::make_unique<my_activity_actor>( *this );
         *     }
         * };
         * \endcode
         * The returned value should behave like the original item and must have the same type.
         */
        virtual std::unique_ptr<activity_actor> clone() const = 0;

        /**
         * Must write any custom members of the derived class to json
         * Note that a static member function for deserialization must also be created and
         * added to the `activity_actor_deserializers` hashmap in activity_actor.cpp
         */
        virtual void serialize( JsonOut &jsout ) const = 0;

        virtual act_progress_message get_progress_message(
            const player_activity &, const Character & ) const {
            // TODO: make it create default message once migration to actors is complete.
            act_progress_message msg;
            msg.implemented = false;
            return msg;
        }
};

namespace activity_actors
{

// defined in activity_actor.cpp
extern const std::unordered_map<activity_id, std::unique_ptr<activity_actor>( * )( JsonIn & )>
deserialize_functions;

} // namespace activity_actors

void serialize( const cata::clone_ptr<activity_actor> &actor, JsonOut &jsout );
void deserialize( cata::clone_ptr<activity_actor> &actor, JsonIn &jsin );

#endif // CATA_SRC_ACTIVITY_ACTOR_H
