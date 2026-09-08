/** @file parse.h
 * @brief Declarations for parsing input from users and other servers.
 * @version $Id$
 */
#ifndef INCLUDED_parse_h
#define INCLUDED_parse_h

#ifndef INCLUDED_ircd_handler_h
#include "ircd_handler.h"
#endif

struct Client;
struct Message;
struct MsgTag;
struct s_map;

/*
 * Prototypes
 */

extern int parse_client(struct Client *cptr, char *buffer, char *bufend);
extern int parse_server(struct Client *cptr, char *buffer, char *bufend);
/** Tags parsed from the current input line (valid only during handler). */
extern struct MsgTag *parse_tags(void);
extern void initmsgtree(void);

extern int register_mapping(struct s_map *map);
extern int unregister_mapping(struct s_map *map);

/* Run-time command registration, used by the module API. */
/* parameters is the maximum number of parameters to split the line into,
 * not a minimum; see struct Message in msg.h. */
extern struct Message *parse_add_command(const char *cmd, const char *tok,
                                         unsigned int parameters,
                                         unsigned int flags,
                                         MessageHandler handlers[]);
extern void parse_del_command(struct Message *msg);

#endif /* INCLUDED_parse_h */
