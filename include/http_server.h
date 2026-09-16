/*
 * IRC - Internet Relay Chat, include/http_server.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/** @file
 * @brief Starting and stopping the HTTP listener.
 *
 * Three calls, for ircd.c.  Everything else about HTTP is include/http.h,
 * and nothing outside ircd/http_server.c knows that Mongoose exists.
 */
#ifndef INCLUDED_http_server_h
#define INCLUDED_http_server_h

/** Bring the listener up, or take it down, to match the features.
 *
 * Idempotent, and cheap when nothing changed: a rehash that did not touch
 * @c HTTP_PORT, @c HTTP_BIND, @c HTTP_MAX_CLIENTS or the TLS files does
 * nothing at all, because restarting a listener drops every connection on
 * it.  With @c HTTP_PORT at 0 it stops whatever is running.
 *
 * Called once the configuration is complete -- after start-up and after
 * every rehash -- never in the middle of the parse.
 */
extern void http_server_reconfigure(void);

/** Stop the listener and wait for its thread. */
extern void http_server_stop(void);

/** Non-zero if the listener thread is running. */
extern int http_server_running(void);

#endif /* INCLUDED_http_server_h */
