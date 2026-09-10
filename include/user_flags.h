#ifndef INCLUDED__user_flags_h
#define INCLUDED__user_flags_h

#define BITSET 1ull

/**< User flags */

#define FLAG_HIDEIDLE (BITSET << 8) /**< Hide idle time from non-opers */
#define FLAG_LOCOP (BITSET << 14)   /**< Local operator */
#define FLAG_BLOCK_UNAUTH_USERS                                                \
  (BITSET << 17) /**< Block msgs from unauthenticated users */
#define FLAG_COMMONCHANS                                                       \
  (BITSET << 28) /**< only accepts messages from users in common channels */
#define FLAG_DEAF (BITSET << 29)      /**< Makes user deaf */
#define FLAG_DEBUG (BITSET << 32)     /**< send global debug/anti-hack info */
#define FLAG_INVISIBLE (BITSET << 34) /**< makes user invisible */
#define FLAG_CHSERV                                                            \
  (BITSET << 36) /**< Disallow KICK or MODE -o on the user; don't display      \
                    channels in /whois */
#define FLAG_OPER (BITSET << 40)       /**< Operator */
#define FLAG_ACCOUNT (BITSET << 43)    /**< account name has been set */
#define FLAG_SERVNOTICE (BITSET << 44) /**< server notices such as kill */
#define FLAG_WALLOP (BITSET << 48)     /**< send wallops to them */
#define FLAG_HIDDENHOST (BITSET << 49) /**< user's host is hidden */
#define FLAG_TLS (BITSET << 51)        /**< user is using TLS */

/** User modes that are never sent to other servers. */
#define FLAG_LOCAL_UMODES (FLAG_LOCOP | FLAG_SERVNOTICE)

/** User modes that are propagated network-wide: everything but the
 * local ones.  Defined by exclusion so that modes registered by modules
 * are global too and the network stays in sync; keeping every node
 * loading the same set of modules is the network operator's job.
 * It cannot be an ordering test any more ("at or above FLAG_OPER"), the
 * flags are bit masks now and FLAG_SERVNOTICE sits above FLAG_OPER.
 */
#define FLAG_GLOBAL_UMODES (~FLAG_LOCAL_UMODES)

#define FLAG_LAST_UFLAG FLAG_TLS /**< last user flag  */

/** Direction of a user mode change, as set_user_mode() walks the string.
 * These used to be MODE_ADD and MODE_DEL from channel.h; those are bits of
 * a channel mode mask now and no longer fit in an int.
 */
#define UMODE_NULL 0
#define UMODE_ADD  1
#define UMODE_DEL  2

#define UMODE_ALREADY_EXISTS 0x01
#define UMODE_INVALID_MODE 0x02
#define UMODE_APPEND_OK 0x04
#define UMODE_UNKNOWN_MODE 0x08
#define UMODE_CORE_MODE 0x10
#define UMODE_REMOVE_OK 0x20


#endif /** INCLUDED__user_flags_h */
