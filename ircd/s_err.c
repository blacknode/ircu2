/*
 * IRC - Internet Relay Chat, ircd/s_err.c
 * Copyright (C) 1992 Darren Reed
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
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
 * @brief Error handling support.
 * @version $Id$
 */
#include "config.h"

#include "numeric.h"
#include "ircd_log.h"
#include "s_debug.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** One numeric with a format.
 *
 * Written with the code before the format so that xgettext can extract
 * the pair as msgctxt / msgid (--keyword=N:2c,3): send_reply() looks a
 * numeric's translation up with its code as the context, so two numerics
 * that happen to share a text can be translated apart, and a translator
 * sees which numeric each line is.  See doc/readme.translations.
 */
#define N(sym, str, fmt) { sym, fmt, str }

/** Array of Numeric replies, indexed by numeric. */
static Numeric replyTable[] = {
/* 000 */
  { 0 },
/* 001 */
  N(RPL_WELCOME, "001", ":Welcome to the %s IRC Network%s%s, %s"),
/* 002 */
  N(RPL_YOURHOST, "002", ":Your host is %s, running version %s"),
/* 003 */
  N(RPL_CREATED, "003", ":This server was created %s"),
/* 004 */
  N(RPL_MYINFO, "004", "%s %s %s %s %s"),
/* 005 */
  N(RPL_ISUPPORT, "005", "%s :are supported by this server"),
/* 006 */
  { 0 },
/* 007 */
  { 0 },
/* 008 */
  N(RPL_SNOMASK, "008", "%u :: Server notice mask (%#x)"),
/* 009 */
  { 0 },
/* 010 */
  { 0 },
/* 011 */
  { 0 },
/* 012 */
  { 0 },
/* 013 */
  { 0 },
/* 014 */
  { 0 },
/* 015 */
  N(RPL_MAP, "015", ":%s%s%s %s [%u clients]"),
/* 016 */
  N(RPL_MAPMORE, "016", ":%s%s --> *more*"),
/* 017 */
  N(RPL_MAPEND, "017", ":End of /%s"),
/* 018 */
  { 0 },
/* 019 */
  { 0 },
/* 020 */
  { 0 },
/* 021 */
  { 0 },
/* 022 */
  { 0 },
/* 023 */
  { 0 },
/* 024 */
  { 0 },
/* 025 */
  { 0 },
/* 026 */
  { 0 },
/* 027 */
  { 0 },
/* 028 */
  { 0 },
/* 029 */
  { 0 },
/* 030 */
  N(RPL_APASSWARN_SET, "030", ":Channel Admin password (+A) set to '%s'.  Are you SURE you want to use this as Admin password? You will NOT be able to change this password anymore once the channel is more than 48 hours old!"),
/* 031 */
  N(RPL_APASSWARN_SECRET, "031", ":Use \"/MODE %s -A %s\" to remove the password and then immediately set a new one.  IMPORTANT: YOU CANNOT RECOVER THIS PASSWORD, EVER; WRITE THE PASSWORD DOWN (don't store this rescue password on disk)! Now set the channel user password (+U)."),
/* 032 */
  N(RPL_APASSWARN_CLEAR, "032", ":WARNING: You removed the channel Admin password (+A). If you disconnect or leave the channel without setting a new password then you will not be able to set it again!  SET A NEW PASSWORD NOW!"),
/* 033 */
  { 0 },
/* 034 */
  { 0 },
/* 035 */
  { 0 },
/* 036 */
  { 0 },
/* 037 */
  { 0 },
/* 038 */
  { 0 },
/* 039 */
  { 0 },
/* 040 */
  { 0 },
/* 041 */
  { 0 },
/* 042 */
  { 0 },
/* 043 */
  { 0 },
/* 044 */
  { 0 },
/* 045 */
  { 0 },
/* 046 */
  { 0 },
/* 047 */
  { 0 },
/* 048 */
  { 0 },
/* 049 */
  { 0 },
/* 050 */
  { 0 },
/* 051 */
  { 0 },
/* 052 */
  { 0 },
/* 053 */
  { 0 },
/* 054 */
  { 0 },
/* 055 */
  { 0 },
/* 056 */
  { 0 },
/* 057 */
  { 0 },
/* 058 */
  { 0 },
/* 059 */
  { 0 },
/* 060 */
  { 0 },
/* 061 */
  { 0 },
/* 062 */
  { 0 },
/* 063 */
  { 0 },
/* 064 */
  { 0 },
/* 065 */
  { 0 },
/* 066 */
  { 0 },
/* 067 */
  { 0 },
/* 068 */
  { 0 },
/* 069 */
  { 0 },
/* 070 */
  { 0 },
/* 071 */
  { 0 },
/* 072 */
  { 0 },
/* 073 */
  { 0 },
/* 074 */
  { 0 },
/* 075 */
  { 0 },
/* 076 */
  { 0 },
/* 077 */
  { 0 },
/* 078 */
  { 0 },
/* 079 */
  { 0 },
/* 080 */
  { 0 },
/* 081 */
  { 0 },
/* 082 */
  { 0 },
/* 083 */
  { 0 },
/* 084 */
  { 0 },
/* 085 */
  { 0 },
/* 086 */
  { 0 },
/* 087 */
  { 0 },
/* 088 */
  { 0 },
/* 089 */
  { 0 },
/* 090 */
  { 0 },
/* 091 */
  { 0 },
/* 092 */
  { 0 },
/* 093 */
  { 0 },
/* 094 */
  { 0 },
/* 095 */
  { 0 },
/* 096 */
  { 0 },
/* 097 */
  { 0 },
/* 098 */
  { 0 },
/* 099 */
  { 0 },
/* 100 */
  { 0 },
/* 101 */
  { 0 },
/* 102 */
  { 0 },
/* 103 */
  { 0 },
/* 104 */
  { 0 },
/* 105 */
  { 0 },
/* 106 */
  { 0 },
/* 107 */
  { 0 },
/* 108 */
  { 0 },
/* 109 */
  { 0 },
/* 110 */
  { 0 },
/* 111 */
  { 0 },
/* 112 */
  { 0 },
/* 113 */
  { 0 },
/* 114 */
  { 0 },
/* 115 */
  { 0 },
/* 116 */
  { 0 },
/* 117 */
  { 0 },
/* 118 */
  { 0 },
/* 119 */
  { 0 },
/* 120 */
  { 0 },
/* 121 */
  { 0 },
/* 122 */
  { 0 },
/* 123 */
  { 0 },
/* 124 */
  { 0 },
/* 125 */
  { 0 },
/* 126 */
  { 0 },
/* 127 */
  { 0 },
/* 128 */
  { 0 },
/* 129 */
  { 0 },
/* 130 */
  { 0 },
/* 131 */
  { 0 },
/* 132 */
  { 0 },
/* 133 */
  { 0 },
/* 134 */
  { 0 },
/* 135 */
  { 0 },
/* 136 */
  { 0 },
/* 137 */
  { 0 },
/* 138 */
  { 0 },
/* 139 */
  { 0 },
/* 140 */
  { 0 },
/* 141 */
  { 0 },
/* 142 */
  { 0 },
/* 143 */
  { 0 },
/* 144 */
  { 0 },
/* 145 */
  { 0 },
/* 146 */
  { 0 },
/* 147 */
  { 0 },
/* 148 */
  { 0 },
/* 149 */
  { 0 },
/* 150 */
  { 0 },
/* 151 */
  { 0 },
/* 152 */
  { 0 },
/* 153 */
  { 0 },
/* 154 */
  { 0 },
/* 155 */
  { 0 },
/* 156 */
  { 0 },
/* 157 */
  { 0 },
/* 158 */
  { 0 },
/* 159 */
  { 0 },
/* 160 */
  { 0 },
/* 161 */
  { 0 },
/* 162 */
  { 0 },
/* 163 */
  { 0 },
/* 164 */
  { 0 },
/* 165 */
  { 0 },
/* 166 */
  { 0 },
/* 167 */
  { 0 },
/* 168 */
  { 0 },
/* 169 */
  { 0 },
/* 170 */
  { 0 },
/* 171 */
  { 0 },
/* 172 */
  { 0 },
/* 173 */
  { 0 },
/* 174 */
  { 0 },
/* 175 */
  { 0 },
/* 176 */
  { 0 },
/* 177 */
  { 0 },
/* 178 */
  { 0 },
/* 179 */
  { 0 },
/* 180 */
  { 0 },
/* 181 */
  { 0 },
/* 182 */
  { 0 },
/* 183 */
  { 0 },
/* 184 */
  { 0 },
/* 185 */
  { 0 },
/* 186 */
  { 0 },
/* 187 */
  { 0 },
/* 188 */
  { 0 },
/* 189 */
  { 0 },
/* 190 */
  { 0 },
/* 191 */
  { 0 },
/* 192 */
  { 0 },
/* 193 */
  { 0 },
/* 194 */
  { 0 },
/* 195 */
  { 0 },
/* 196 */
  { 0 },
/* 197 */
  { 0 },
/* 198 */
  { 0 },
/* 199 */
  { 0 },
/* 200 */
  N(RPL_TRACELINK, "200", "Link %s.%s %s %s"),
/* 201 */
  N(RPL_TRACECONNECTING, "201", "Try. %s %s"),
/* 202 */
  N(RPL_TRACEHANDSHAKE, "202", "H.S. %s %s"),
/* 203 */
  N(RPL_TRACEUNKNOWN, "203", "???? %s %s"),
/* 204 */
  N(RPL_TRACEOPERATOR, "204", "Oper %s %s %ld"),
/* 205 */
  N(RPL_TRACEUSER, "205", "User %s %s %ld"),
/* 206 */
  N(RPL_TRACESERVER, "206", "Serv %s %dS %dC %s %s!%s@%s %ld %ld"),
/* 207 */
  { 0 },
/* 208 */
  N(RPL_TRACENEWTYPE, "208", "<newtype> 0 %s"),
/* 209 */
  N(RPL_TRACECLASS, "209", "Class %s %u"),
/* 210 */
  { 0 },
/* 211 */
  { RPL_STATSLINKINFO, 0, "211" },
/* 212 */
  N(RPL_STATSCOMMANDS, "212", "%s %u %u"),
/* 213 */
  N(RPL_STATSCLINE, "213", "C %s * %d %d %s %s"),
/* 214 */
  { 0 },
/* 215 */
  N(RPL_STATSILINE, "215", "I %s%s%s %d %s%s %d %s"),
/* 216 */
  N(RPL_STATSKLINE, "216", "%c %s@%s \"%s\" \"%s\" 0 0"),
/* 217 */
  N(RPL_STATSPLINE, "217", "P %d %d %s %s"),
/* 218 */
  N(RPL_STATSYLINE, "218", "%c %s %d %d %u %u %u %u %s"),
/* 219 */
  N(RPL_ENDOFSTATS, "219", "%s :End of /STATS report"),
/* 220 */
  N(RPL_STATSWLINE, "220", "W %s %d :%s"),
/* 221 */
  N(RPL_UMODEIS, "221", "%s"),
/* 222 */
  N(RPL_STATSJLINE, "222", "J %s"),
/* 223 */
  { 0 },
/* 224 */
  { 0 },
/* 225 */
  { 0 },
/* 226 */
  N(RPL_STATSALINE, "226", "%s"),
/* 227 */
  { 0 },
/* 228 */
  N(RPL_STATSQLINE, "228", "Q %s :%s"),
/* 229 */
  { 0 },
/* 230 */
  { 0 },
/* 231 */
  { 0 },
/* 232 */
  { 0 },
/* 233 */
  { 0 },
/* 234 */
  { 0 },
/* 235 */
  { 0 },
/* 236 */
  N(RPL_STATSVERBOSE, "236", "V :Sent as explicit"),
/* 237 */
  N(RPL_STATSENGINE, "237", "%s :Event loop engine"),
/* 238 */
  N(RPL_STATSFLINE, "238", "F %s %s"),
/* 239 */
  { 0 },
/* 240 */
  N(RPL_STATSSLINE, "240", "%Tu %Tu %Lu %s :%s"),
/* 241 */
  N(RPL_STATSLLINE, "241", "Module Description EntryPoint"),
/* 242 */
  N(RPL_STATSUPTIME, "242", ":Server Up %d days, %d:%02d:%02d"),
/* 243 */
  N(RPL_STATSOLINE, "243", "%c %s@%s * %s %s"),
/* 244 */
  { 0 },
/* 245 */
  { 0 },
/* 246 */
  N(RPL_STATSTLINE, "246", "%c %s %s"),
/* 247 */
  N(RPL_STATSGLINE, "247", "%c %s%s%s %Tu %Tu %Tu %s%c :%s"),
/* 248 */
  N(RPL_STATSULINE, "248", "U %s%s"),
/* 249 */
  { RPL_STATSDEBUG, 0, "249" },
/* 250 */
  N(RPL_STATSCONN, "250", ":Highest connection count: %u (%u clients)"),
/* 251 */
  N(RPL_LUSERCLIENT, "251", ":There are %u users and %u invisible on %u servers"),
/* 252 */
  N(RPL_LUSEROP, "252", "%u :operator(s) online"),
/* 253 */
  N(RPL_LUSERUNKNOWN, "253", "%u :unknown connection(s)"),
/* 254 */
  N(RPL_LUSERCHANNELS, "254", "%u :channels formed"),
/* 255 */
  N(RPL_LUSERME, "255", ":I have %u clients and %u servers"),
/* 256 */
  N(RPL_ADMINME, "256", ":Administrative info about %s"),
/* 257 */
  N(RPL_ADMINLOC1, "257", ":%s"),
/* 258 */
  N(RPL_ADMINLOC2, "258", ":%s"),
/* 259 */
  N(RPL_ADMINEMAIL, "259", ":%s"),
/* 260 */
  { 0 },
/* 261 */
  { 0 },
/* 262 */
  N(RPL_TRACEEND, "262", ":End of TRACE"),
/* 263 */
  { 0 },
/* 264 */
  { 0 },
/* 265 */
  { 0 },
/* 266 */
  { 0 },
/* 267 */
  { 0 },
/* 268 */
  { 0 },
/* 269 */
  { 0 },
/* 270 */
  N(RPL_PRIVS, "270", "%s :"),
/* 271 */
  N(RPL_SILELIST, "271", "%s %s%s"),
/* 272 */
  N(RPL_ENDOFSILELIST, "272", "%s :End of Silence List"),
/* 273 */
  { 0 },
/* 274 */
  { 0 },
/* 275 */
  N(RPL_STATSDLINE, "275", "%c %s %s"),
/* 276 */
  N(RPL_STATSRLINE, "276", "%-9s %-9s %-10s %s"),
/* 277 */
  { 0 },
/* 278 */
  { 0 },
/* 279 */
  { 0 },
/* 280 */
  N(RPL_GLIST, "280", "%s%s%s %Tu %Tu %Tu %s %s%c :%s"),
/* 281 */
  N(RPL_ENDOFGLIST, "281", ":End of G-line List"),
/* 282 */
  N(RPL_JUPELIST, "282", "%s %Tu %s %c :%s"),
/* 283 */
  N(RPL_ENDOFJUPELIST, "283", ":End of Jupe List"),
/* 284 */
  { RPL_FEATURE, 0, "284" },
/* 285 */
  { 0 },
/* 286 */
  { 0 },
/* 287 */
  { 0 },
/* 288 */
  { 0 },
/* 289 */
  { 0 },
/* 290 */
  { 0 },
/* 291 */
  { 0 },
/* 292 */
  { 0 },
/* 293 */
  { 0 },
/* 294 */
  { 0 },
/* 295 */
  { 0 },
/* 296 */
  { 0 },
/* 297 */
  { 0 },
/* 298 */
  { 0 },
/* 299 */
  { 0 },
/* 300 */
  { 0 },
/* 301 */
  N(RPL_AWAY, "301", "%s :%s"),
/* 302 */
  N(RPL_USERHOST, "302", ":"),
/* 303 */
  N(RPL_ISON, "303", ":"),
/* 304 */
  { 0 },
/* 305 */
  N(RPL_UNAWAY, "305", ":You are no longer marked as being away"),
/* 306 */
  N(RPL_NOWAWAY, "306", ":You have been marked as being away"),
/* 307 */
  N(RPL_WHOISREGNICK, "307", "%s :is a registered user"),
/* 308 */
  { 0 },
/* 309 */
  { 0 },
/* 310 */
  { 0 },
/* 311 */
  N(RPL_WHOISUSER, "311", "%s %s %s * :%s"),
/* 312 */
  N(RPL_WHOISSERVER, "312", "%s %s :%s"),
/* 313 */
  N(RPL_WHOISOPERATOR, "313", "%s :is an IRC Operator"),
/* 314 */
  N(RPL_WHOWASUSER, "314", "%s %s %s * :%s"),
/* 315 */
  N(RPL_ENDOFWHO, "315", "%s :End of /WHO list."),
/* 316 */
  { 0 },
/* 317 */
  N(RPL_WHOISIDLE, "317", "%s %ld %ld :seconds idle, signon time"),
/* 318 */
  N(RPL_ENDOFWHOIS, "318", "%s :End of /WHOIS list."),
/* 319 */
  N(RPL_WHOISCHANNELS, "319", "%s :%s"),
/* 320 */
  N(RPL_WHOISWEBIRC, "320", "%s :is connected via %s"),
/* 321 */
  N(RPL_LISTSTART, "321", "Channel :Users  Name"),
/* 322 */
  N(RPL_LIST, "322", "%s %u :%s"),
/* 323 */
  N(RPL_LISTEND, "323", ":End of /LIST"),
/* 324 */
  N(RPL_CHANNELMODEIS, "324", "%s %s %s"),
/* 325 */
  { 0 },
/* 326 */
  { 0 },
/* 327 */
  { 0 },
/* 328 */
  { 0 },
/* 329 */
  N(RPL_CREATIONTIME, "329", "%s %Tu"),
/* 330 */
  { 0 },
/* 331 */
  N(RPL_NOTOPIC, "331", "%s :No topic is set."),
/* 332 */
  N(RPL_TOPIC, "332", "%s :%s"),
/* 333 */
  N(RPL_TOPICWHOTIME, "333", "%s %s %Tu"),
/* 334 */
  N(RPL_LISTUSAGE, "334", ":%s"),
/* 335 */
  N(RPL_WHOISBOT, "335", "%s :%s"),
/* 336 */
  { 0 },
/* 337 */
  { 0 },
/* 338 */
  N(RPL_WHOISACTUALLY, "338", "%s %s@%s %s :Actual user@host, Actual IP"),
/* 339 */
  { 0 },
/* 340 */
  N(RPL_USERIP, "340", ":"),
/* 341 */
  N(RPL_INVITING, "341", "%s %s"),
/* 342 */
  { 0 },
/* 343 */
  { 0 },
/* 344 */
  { 0 },
/* 345 */
  N(RPL_ISSUEDINVITE, "345", "%s %s %s :%s has been invited by %s"),
/* 346 */
  N(RPL_INVITELIST, "346", ":%s"),
/* 347 */
  N(RPL_ENDOFINVITELIST, "347", ":End of Invite List"),
/* 348 */
  { 0 },
/* 349 */
  { 0 },
/* 350 */
  { 0 },
/* 351 */
  N(RPL_VERSION, "351", "%s.%s %s :%s"),
/* 352 */
  N(RPL_WHOREPLY, "352", "%s"),
/* 353 */
  N(RPL_NAMREPLY, "353", "%s"),
/* 354 */
  N(RPL_WHOSPCRPL, "354", "%s"),
/* 355 */
  N(RPL_DELNAMREPLY, "355", "%s"),
/* 356 */
  { 0 },
/* 357 */
  { 0 },
/* 358 */
  { 0 },
/* 359 */
  { 0 },
/* 360 */
  { 0 },
/* 361 */
  { 0 },
/* 362 */
  N(RPL_CLOSING, "362", "%s :Operator enforced Close"),
/* 363 */
  N(RPL_CLOSEEND, "363", "%d :Connections Closed"),
/* 364 */
  N(RPL_LINKS, "364", "%s %s :%u P%u %s"),
/* 365 */
  N(RPL_ENDOFLINKS, "365", "%s :End of /LINKS list."),
/* 366 */
  N(RPL_ENDOFNAMES, "366", "%s :End of /NAMES list."),
/* 367 */
  N(RPL_BANLIST, "367", "%s %s %s %Tu"),
/* 368 */
  N(RPL_ENDOFBANLIST, "368", "%s :End of Channel Ban List"),
/* 369 */
  N(RPL_ENDOFWHOWAS, "369", "%s :End of WHOWAS"),
/* 370 */
  { 0 },
/* 371 */
  N(RPL_INFO, "371", ":%s"),
/* 372 */
  N(RPL_MOTD, "372", ":- %s"),
/* 373 */
  { 0 },
/* 374 */
  N(RPL_ENDOFINFO, "374", ":End of /INFO list."),
/* 375 */
  N(RPL_MOTDSTART, "375", ":- %s Message of the Day - "),
/* 376 */
  N(RPL_ENDOFMOTD, "376", ":End of /MOTD command."),
/* 377 */
  { 0 },
/* 378 */
  { 0 },
/* 379 */
  N(RPL_WHOISMODES, "379", "%s :using modes [%s]"),
/* 380 */
  { 0 },
/* 381 */
  N(RPL_YOUREOPER, "381", ":You are now an IRC Operator"),
/* 382 */
  N(RPL_REHASHING, "382", "%s :Rehashing"),
/* 383 */
  { 0 },
/* 384 */
  { 0 },
/* 385 */
  { 0 },
/* 386 */
  { 0 },
/* 387 */
  { 0 },
/* 388 */
  { 0 },
/* 389 */
  { 0 },
/* 390 */
  { 0 },
/* 391 */
  N(RPL_TIME, "391", "%s %Tu %ld :%s"),
/* 392 */
  { 0 },
/* 393 */
  { 0 },
/* 394 */
  { 0 },
/* 395 */
  { 0 },
/* 396 */
  N(RPL_HOSTHIDDEN, "396", "%s :is now your hidden host"),
/* 397 */
  { 0 },
/* 398 */
  { 0 },
/* 399 */
  { 0 },
/* 400 */
  { 0 },
/* 401 */
  N(ERR_NOSUCHNICK, "401", "%s :No such nick"),
/* 402 */
  N(ERR_NOSUCHSERVER, "402", "%s :No such server"),
/* 403 */
  N(ERR_NOSUCHCHANNEL, "403", "%s :No such channel"),
/* 404 */
  N(ERR_CANNOTSENDTOCHAN, "404", "%s :Cannot send to channel"),
/* 405 */
  N(ERR_TOOMANYCHANNELS, "405", "%s :You have joined too many channels"),
/* 406 */
  N(ERR_WASNOSUCHNICK, "406", "%s :There was no such nickname"),
/* 407 */
  N(ERR_TOOMANYTARGETS, "407", "%s :Duplicate recipients. No message delivered"),
/* 408 */
  { 0 },
/* 409 */
  N(ERR_NOORIGIN, "409", ":No origin specified"),
/* 410 */
  N(ERR_UNKNOWNCAPCMD, "410", "%s :Unknown CAP subcommand"),
/* 411 */
  N(ERR_NORECIPIENT, "411", ":No recipient given (%s)"),
/* 412 */
  N(ERR_NOTEXTTOSEND, "412", ":No text to send"),
/* 413 */
  N(ERR_NOTOPLEVEL, "413", "%s :No toplevel domain specified"),
/* 414 */
  N(ERR_WILDTOPLEVEL, "414", "%s :Wildcard in toplevel Domain"),
/* 415 */
  { 0 },
/* 416 */
  N(ERR_QUERYTOOLONG, "416", "%s :Too many lines in the output, restrict your query"),
/* 417 */
  N(ERR_INPUTTOOLONG, "417", ":Input line was too long"),
/* 418 */
  { 0 },
/* 419 */
  { 0 },
/* 420 */
  { 0 },
/* 421 */
  N(ERR_UNKNOWNCOMMAND, "421", "%s :Unknown command"),
/* 422 */
  N(ERR_NOMOTD, "422", ":MOTD File is missing"),
/* 423 */
  N(ERR_NOADMININFO, "423", "%s :No administrative info available"),
/* 424 */
  { 0 },
/* 425 */
  { 0 },
/* 426 */
  { 0 },
/* 427 */
  { 0 },
/* 428 */
  { 0 },
/* 429 */
  { 0 },
/* 430 */
  { 0 },
/* 431 */
  N(ERR_NONICKNAMEGIVEN, "431", ":No nickname given"),
/* 432 */
  N(ERR_ERRONEUSNICKNAME, "432", "%s :Erroneous Nickname"),
/* 433 */
  N(ERR_NICKNAMEINUSE, "433", "%s :Nickname is already in use."),
/* 434 */
  { 0 },
/* 435 */
  { 0 },
/* 436 */
  N(ERR_NICKCOLLISION, "436", "%s :Nickname collision KILL"),
/* 437 */
  N(ERR_BANNICKCHANGE, "437", "%s :Cannot change nickname while banned on channel or channel is moderated"),
/* 438 */
  N(ERR_NICKTOOFAST, "438", "%s :Nick change too fast. Please wait %d seconds."),
/* 439 */
  N(ERR_TARGETTOOFAST, "439", "%s :Target change too fast. Please wait %d seconds."),
/* 440 */
  N(ERR_SERVICESDOWN, "440", "%s :Services are currently unavailable."),
/* 441 */
  N(ERR_USERNOTINCHANNEL, "441", "%s %s :They aren't on that channel"),
/* 442 */
  N(ERR_NOTONCHANNEL, "442", "%s :You're not on that channel"),
/* 443 */
  N(ERR_USERONCHANNEL, "443", "%s %s :is already on channel"),
/* 444 */
  { 0 },
/* 445 */
  { 0 },
/* 446 */
  { 0 },
/* 447 */
  { 0 },
/* 448 */
  { 0 },
/* 449 */
  { 0 },
/* 450 */
  { 0 },
/* 451 */
  N(ERR_NOTREGISTERED, "451", ":You have not registered"),
/* 452 */
  { 0 },
/* 453 */
  { 0 },
/* 454 */
  { 0 },
/* 455 */
  { 0 },
/* 456 */
  { 0 },
/* 457 */
  { 0 },
/* 458 */
  { 0 },
/* 459 */
  { 0 },
/* 460 */
  { 0 },
/* 461 */
  N(ERR_NEEDMOREPARAMS, "461", "%s :Not enough parameters"),
/* 462 */
  N(ERR_ALREADYREGISTRED, "462", ":You may not reregister"),
/* 463 */
  N(ERR_NOPERMFORHOST, "463", ":Your host isn't among the privileged"),
/* 464 */
  N(ERR_PASSWDMISMATCH, "464", ":Password Incorrect"),
/* 465 */
  N(ERR_YOUREBANNEDCREEP, "465", ":You are banned from this server"),
/* 466 */
  N(ERR_YOUWILLBEBANNED, "466", ""),
/* 467 */
  N(ERR_KEYSET, "467", "%s :Channel key already set"),
/* 468 */
  { ERR_INVALIDUSERNAME, 0, "468" },
/* 469 */
  N(ERR_TLSONLYCHAN, "469", "%s :Cannot join channel (+Z)"),
/* 470 */
  { 0 },
/* 471 */
  N(ERR_CHANNELISFULL, "471", "%s :Cannot join channel (+l)"),
/* 472 */
  N(ERR_UNKNOWNMODE, "472", "%c :is unknown mode char to me"),
/* 473 */
  N(ERR_INVITEONLYCHAN, "473", "%s :Cannot join channel (+i)"),
/* 474 */
  N(ERR_BANNEDFROMCHAN, "474", "%s :Cannot join channel (+b)"),
/* 475 */
  N(ERR_BADCHANNELKEY, "475", "%s :Cannot join channel (+k)"),
/* 476 */
  N(ERR_BADCHANMASK, "476", "%s :Bad Channel Mask"),
/* 477 */
  N(ERR_NEEDREGGEDNICK, "477", "%s :Cannot join channel (+r): this channel requires authentication -- you can obtain an account from %s"),
/* 478 */
  N(ERR_BANLISTFULL, "478", "%s %s :Channel ban/ignore list is full"),
/* 479 */
  N(ERR_BADCHANNAME, "479", "%s :Cannot join channel (access denied on this server)"),
/* 480 */
  { 0 },
/* 481 */
  N(ERR_NOPRIVILEGES, "481", ":Permission Denied: Insufficient privileges"),
/* 482 */
  N(ERR_CHANOPRIVSNEEDED, "482", "%s :You're not channel operator"),
/* 483 */
  N(ERR_CANTKILLSERVER, "483", ":You cant kill a server!"),
/* 484 */
  N(ERR_ISCHANSERVICE, "484", "%s %s :Cannot kill, kick or deop a network service"),
/* 485 */
  { 0 },
/* 486 */
  { 0 },
/* 487 */
  { 0 },
/* 488 */
  { 0 },
/* 489 */
  N(ERR_VOICENEEDED, "489", "%s :You're neither voiced nor channel operator"),
/* 490 */
  { 0 },
/* 491 */
  N(ERR_NOOPERHOST, "491", ":No Operator block for your host"),
/* 492 */
  { 0 },
/* 493 */
  N(ERR_NOFEATURE, "493", "%s :No such feature"),
/* 494 */
  N(ERR_BADFEATVALUE, "494", "%s :Bad value for feature %s"),
/* 495 */
  N(ERR_BADLOGTYPE, "495", "%s :No such log type"),
/* 496 */
  N(ERR_BADLOGSYS, "496", "%s :No such log subsystem"),
/* 497 */
  N(ERR_BADLOGVALUE, "497", "%s :Bad value for log type"),
/* 498 */
  N(ERR_ISOPERLCHAN, "498", "%s %s :Cannot kick or deop an IRC Operator on a local channel"),
/* 499 */
  { 0 },
/* 500 */
  { 0 },
/* 501 */
  N(ERR_UMODEUNKNOWNFLAG, "501", "%c :Unknown user MODE flag"),
/* 502 */
  N(ERR_USERSDONTMATCH, "502", ":Cant change mode for other users"),
/* 503 */
  { 0 },
/* 504 */
  { 0 },
/* 505 */
  { 0 },
/* 506 */
  { 0 },
/* 507 */
  { 0 },
/* 508 */
  { 0 },
/* 509 */
  { 0 },
/* 510 */
  { 0 },
/* 511 */
  N(ERR_SILELISTFULL, "511", "%s :Your silence list is full"),
/* 512 */
  N(ERR_NOSUCHGLINE, "512", "%s :No such gline"),
/* 513 */
  { ERR_BADPING, 0, "513" },
/* 514 */
  N(ERR_NOSUCHJUPE, "514", "%s :No such jupe"),
/* 515 */
  N(ERR_BADEXPIRE, "515", "%Tu :Bad expire time"),
/* 516 */
  N(ERR_DONTCHEAT, "516", "%s :Don't Cheat."),
/* 517 */
  N(ERR_DISABLED, "517", "%s :Command disabled."),
/* 518 */
  N(ERR_LONGMASK, "518", ":Mask is too long"),
/* 519 */
  N(ERR_TOOMANYUSERS, "519", "%d :Too many users affected by mask"),
/* 520 */
  N(ERR_MASKTOOWIDE, "520", "%s :Mask is too wide"),
/* 521 */
  { 0 },
/* 522 */
  { 0 },
/* 523 */
  { 0 },
/* 524 */
  N(ERR_QUARANTINED, "524", "%s :Channel is quarantined : %s"),
/* 525 */
  N(ERR_INVALIDKEY, "525", "%s :Key is not well-formed"),
/* 526 */
  { 0 },
/* 527 */
  { 0 },
/* 528 */
  { 0 },
/* 529 */
  { 0 },
/* 530 */
  { 0 },
/* 531 */
  { 0 },
/* 532 */
  N(ERR_TLSCLIFINGERPRINT, "532", ":TLS certificate fingerprint did not match"),
/* 533 */
  { 0 },
/* 534 */
  { 0 },
/* 535 */
  { 0 },
/* 536 */
  { 0 },
/* 537 */
  { 0 },
/* 538 */
  { 0 },
/* 539 */
  { 0 },
/* 540 */
  { 0 },
/* 541 */
  { 0 },
/* 542 */
  { 0 },
/* 543 */
  { 0 },
/* 544 */
  { 0 },
/* 545 */
  { 0 },
/* 546 */
  { 0 },
/* 547 */
  { 0 },
/* 548 */
  { 0 },
/* 549 */
  { 0 },
/* 550 */
  { 0 },
/* 551 */
  { 0 },
/* 552 */
  { 0 },
/* 553 */
  { 0 },
/* 554 */
  { 0 },
/* 555 */
  { 0 },
/* 556 */
  { 0 },
/* 557 */
  { 0 },
/* 558 */
  { 0 },
/* 559 */
  { 0 },
/* 560 */
  N(ERR_NOTLOWEROPLEVEL, "560", "%s %s %hu %hu :Cannot %s someone with %s op-level"),
/* 561 */
  N(ERR_NOTMANAGER, "561", "%s :You must be channel Admin to add or remove a password. Use /JOIN %s <AdminPass>."),
/* 562 */
  N(ERR_CHANSECURED, "562", "%s :Channel is older than 48 hours and secured. Cannot change Admin pass anymore"),
/* 563 */
  N(ERR_UPASSSET, "563", "%s :Cannot remove Admin pass (+A) while User pass (+U) is still set. First use /MODE %s -U <userpass>"),
/* 564 */
  N(ERR_UPASSNOTSET, "564", "%s :Cannot set user pass (+U) until Admin pass (+A) is set. First use /MODE %s +A <adminpass>"),
/* 565 */
  { 0 },
/* 566 */
  N(ERR_NOMANAGER, "566", "%s :Re-create the channel. The channel must be completely empty for a period of %s before it can be recreated."),
/* 567 */
  N(ERR_UPASS_SAME_APASS, "567", "%s :Cannot use the same pass for both admin (+A) and user (+U) pass."),
/* 568 */
  { 0 },
/* 569 */
  { 0 },
/* 570 */
  { 0 },
/* 571 */
  { 0 },
/* 572 */
  { 0 },
/* 573 */
  { 0 },
/* 574 */
  { 0 },
/* 575 */
  { 0 },
/* 576 */
  { 0 },
/* 577 */
  { 0 },
/* 578 */
  { 0 },
/* 579 */
  { 0 },
/* 580 */
  { 0 },
/* 581 */
  { 0 },
/* 582 */
  { 0 },
/* 583 */
  { 0 },
/* 584 */
  { 0 },
/* 585 */
  { 0 },
/* 586 */
  { 0 },
/* 587 */
  { 0 },
/* 588 */
  { 0 },
/* 589 */
  { 0 },
/* 590 */
  { 0 },
/* 591 */
  { 0 },
/* 592 */
  { 0 },
/* 593 */
  { 0 },
/* 594 */
  { 0 },
/* 595 */
  { 0 },
/* 596 */
  { 0 },
/* 597 */
  { 0 },
/* 598 */
  { 0 },
/* 599 */
  { 0 },
/* 600 */
  { 0 },
/* 601 */
  { 0 },
/* 602 */
  { 0 },
/* 603 */
  { 0 },
/* 604 */
  { 0 },
/* 605 */
  { 0 },
/* 606 */
  { 0 },
/* 607 */
  { 0 },
/* 608 */
  { 0 },
/* 609 */
  { 0 },
/* 610 */
  { 0 },
/* 611 */
  { 0 },
/* 612 */
  { 0 },
/* 613 */
  { 0 },
/* 614 */
  { 0 },
/* 615 */
  { 0 },
/* 616 */
  { 0 },
/* 617 */
  { 0 },
/* 618 */
  { 0 },
/* 619 */
  { 0 },
/* 620 */
  { 0 },
/* 621 */
  { 0 },
/* 622 */
  { 0 },
/* 623 */
  { 0 },
/* 624 */
  { 0 },
/* 625 */
  { 0 },
/* 626 */
  { 0 },
/* 627 */
  { 0 },
/* 628 */
  { 0 },
/* 629 */
  { 0 },
/* 630 */
  { 0 },
/* 631 */
  { 0 },
/* 632 */
  { 0 },
/* 633 */
  { 0 },
/* 634 */
  { 0 },
/* 635 */
  { 0 },
/* 636 */
  { 0 },
/* 637 */
  { 0 },
/* 638 */
  { 0 },
/* 639 */
  { 0 },
/* 640 */
  { 0 },
/* 641 */
  { 0 },
/* 642 */
  { 0 },
/* 643 */
  { 0 },
/* 644 */
  { 0 },
/* 645 */
  { 0 },
/* 646 */
  { 0 },
/* 647 */
  { 0 },
/* 648 */
  { 0 },
/* 649 */
  { 0 },
/* 650 */
  { 0 },
/* 651 */
  { 0 },
/* 652 */
  { 0 },
/* 653 */
  { 0 },
/* 654 */
  { 0 },
/* 655 */
  { 0 },
/* 656 */
  { 0 },
/* 657 */
  { 0 },
/* 658 */
  { 0 },
/* 659 */
  { 0 },
/* 660 */
  { 0 },
/* 661 */
  { 0 },
/* 662 */
  { 0 },
/* 663 */
  { 0 },
/* 664 */
  { 0 },
/* 665 */
  { 0 },
/* 666 */
  { 0 },
/* 667 */
  { 0 },
/* 668 */
  { 0 },
/* 669 */
  { 0 },
/* 670 */
  { 0 },
/* 671 */
  N(RPL_WHOISSECURE, "671", "%s :is using a secure connection%s"),
/* 672 */
  { 0 },
/* 673 */
  { 0 },
/* 674 */
  { 0 },
/* 675 */
  { 0 },
/* 676 */
  { 0 },
/* 677 */
  { 0 },
/* 678 */
  { 0 },
/* 679 */
  { 0 },
/* 680 */
  { 0 },
/* 681 */
  { 0 },
/* 682 */
  { 0 },
/* 683 */
  { 0 },
/* 684 */
  { 0 },
/* 685 */
  { 0 },
/* 686 */
  { 0 },
/* 687 */
  N(RPL_YOURLANGUAGESARE, "687", "%s :Language preferences have been set."),
/* 688 */
  { 0 },
/* 689 */
  { 0 },
/* 690 */
  N(RPL_WHOISLANGUAGE, "690", "%s %s :can speak these languages."),
/* 691 */
  N(RPL_WHOISEMAIL, "691", "%s %s :is the address this account belongs to"),
/* 692 */
  N(RPL_WHOISFROZEN, "692", "%s :must identify to this nickname before doing anything else"),
/* 693 */
  { 0 },
/* 694 */
  { 0 },
/* 695 */
  { 0 },
/* 696 */
  { 0 },
/* 697 */
  { 0 },
/* 698 */
  { 0 },
/* 699 */
  { 0 },
/* 700 */
  { 0 },
/* 701 */
  { 0 },
/* 702 */
  { 0 },
/* 703 */
  { 0 },
/* 704 */
  { 0 },
/* 705 */
  { 0 },
/* 706 */
  { 0 },
/* 707 */
  { 0 },
/* 708 */
  { 0 },
/* 709 */
  { 0 },
/* 710 */
  { 0 },
/* 711 */
  { 0 },
/* 712 */
  { 0 },
/* 713 */
  { 0 },
/* 714 */
  { 0 },
/* 715 */
  { 0 },
/* 716 */
  { 0 },
/* 717 */
  { 0 },
/* 718 */
  { 0 },
/* 719 */
  { 0 },
/* 720 */
  { 0 },
/* 721 */
  { 0 },
/* 722 */
  { 0 },
/* 723 */
  { 0 },
/* 724 */
  { 0 },
/* 725 */
  { 0 },
/* 726 */
  { 0 },
/* 727 */
  { 0 },
/* 728 */
  { 0 },
/* 729 */
  { 0 },
/* 730 */
  { 0 },
/* 731 */
  { 0 },
/* 732 */
  { 0 },
/* 733 */
  { 0 },
/* 734 */
  { 0 },
/* 735 */
  { 0 },
/* 736 */
  { 0 },
/* 737 */
  { 0 },
/* 738 */
  { 0 },
/* 739 */
  { 0 },
/* 740 */
  { 0 },
/* 741 */
  { 0 },
/* 742 */
  { 0 },
/* 743 */
  { 0 },
/* 744 */
  { 0 },
/* 745 */
  { 0 },
/* 746 */
  { 0 },
/* 747 */
  { 0 },
/* 748 */
  { 0 },
/* 749 */
  { 0 },
/* 750 */
  { 0 },
/* 751 */
  { 0 },
/* 752 */
  { 0 },
/* 753 */
  { 0 },
/* 754 */
  { 0 },
/* 755 */
  { 0 },
/* 756 */
  { 0 },
/* 757 */
  { 0 },
/* 758 */
  { 0 },
/* 759 */
  { 0 },
/* 760 */
  { 0 },
/* 761 */
  { 0 },
/* 762 */
  { 0 },
/* 763 */
  { 0 },
/* 764 */
  { 0 },
/* 765 */
  { 0 },
/* 766 */
  { 0 },
/* 767 */
  { 0 },
/* 768 */
  { 0 },
/* 769 */
  { 0 },
/* 770 */
  { 0 },
/* 771 */
  { 0 },
/* 772 */
  { 0 },
/* 773 */
  { 0 },
/* 774 */
  { 0 },
/* 775 */
  { 0 },
/* 776 */
  { 0 },
/* 777 */
  { 0 },
/* 778 */
  { 0 },
/* 779 */
  { 0 },
/* 780 */
  { 0 },
/* 781 */
  { 0 },
/* 782 */
  { 0 },
/* 783 */
  { 0 },
/* 784 */
  { 0 },
/* 785 */
  { 0 },
/* 786 */
  { 0 },
/* 787 */
  { 0 },
/* 788 */
  { 0 },
/* 789 */
  { 0 },
/* 790 */
  { 0 },
/* 791 */
  { 0 },
/* 792 */
  { 0 },
/* 793 */
  { 0 },
/* 794 */
  { 0 },
/* 795 */
  { 0 },
/* 796 */
  { 0 },
/* 797 */
  { 0 },
/* 798 */
  { 0 },
/* 799 */
  { 0 },
/* 800 */
  { 0 },
/* 801 */
  { 0 },
/* 802 */
  { 0 },
/* 803 */
  { 0 },
/* 804 */
  { 0 },
/* 805 */
  { 0 },
/* 806 */
  { 0 },
/* 807 */
  { 0 },
/* 808 */
  { 0 },
/* 809 */
  { 0 },
/* 810 */
  { 0 },
/* 811 */
  { 0 },
/* 812 */
  { 0 },
/* 813 */
  { 0 },
/* 814 */
  { 0 },
/* 815 */
  { 0 },
/* 816 */
  { 0 },
/* 817 */
  { 0 },
/* 818 */
  { 0 },
/* 819 */
  { 0 },
/* 820 */
  { 0 },
/* 821 */
  { 0 },
/* 822 */
  { 0 },
/* 823 */
  { 0 },
/* 824 */
  { 0 },
/* 825 */
  { 0 },
/* 826 */
  { 0 },
/* 827 */
  { 0 },
/* 828 */
  { 0 },
/* 829 */
  { 0 },
/* 830 */
  { 0 },
/* 831 */
  { 0 },
/* 832 */
  { 0 },
/* 833 */
  { 0 },
/* 834 */
  { 0 },
/* 835 */
  { 0 },
/* 836 */
  { 0 },
/* 837 */
  { 0 },
/* 838 */
  { 0 },
/* 839 */
  { 0 },
/* 840 */
  { 0 },
/* 841 */
  { 0 },
/* 842 */
  { 0 },
/* 843 */
  { 0 },
/* 844 */
  { 0 },
/* 845 */
  { 0 },
/* 846 */
  { 0 },
/* 847 */
  { 0 },
/* 848 */
  { 0 },
/* 849 */
  { 0 },
/* 850 */
  { 0 },
/* 851 */
  { 0 },
/* 852 */
  { 0 },
/* 853 */
  { 0 },
/* 854 */
  { 0 },
/* 855 */
  { 0 },
/* 856 */
  { 0 },
/* 857 */
  { 0 },
/* 858 */
  { 0 },
/* 859 */
  { 0 },
/* 860 */
  { 0 },
/* 861 */
  { 0 },
/* 862 */
  { 0 },
/* 863 */
  { 0 },
/* 864 */
  { 0 },
/* 865 */
  { 0 },
/* 866 */
  { 0 },
/* 867 */
  { 0 },
/* 868 */
  { 0 },
/* 869 */
  { 0 },
/* 870 */
  { 0 },
/* 871 */
  { 0 },
/* 872 */
  { 0 },
/* 873 */
  { 0 },
/* 874 */
  { 0 },
/* 875 */
  { 0 },
/* 876 */
  { 0 },
/* 877 */
  { 0 },
/* 878 */
  { 0 },
/* 879 */
  { 0 },
/* 880 */
  { 0 },
/* 881 */
  { 0 },
/* 882 */
  { 0 },
/* 883 */
  { 0 },
/* 884 */
  { 0 },
/* 885 */
  { 0 },
/* 886 */
  { 0 },
/* 887 */
  { 0 },
/* 888 */
  { 0 },
/* 889 */
  { 0 },
/* 890 */
  { 0 },
/* 891 */
  { 0 },
/* 892 */
  { 0 },
/* 893 */
  { 0 },
/* 894 */
  { 0 },
/* 895 */
  { 0 },
/* 896 */
  { 0 },
/* 897 */
  { 0 },
/* 898 */
  { 0 },
/* 899 */
  { 0 },
/* 900 */
  N(RPL_LOGGEDIN, "900", "%s %s :You are now logged in as %s"),
/* 901 */
  N(RPL_LOGGEDOUT, "901", "%s :You are now logged out"),
/* 902 */
  N(ERR_NICKLOCKED, "902", ":You must use a different nickname"),
/* 903 */
  N(RPL_SASLSUCCESS, "903", ":SASL authentication successful"),
/* 904 */
  N(ERR_SASLFAIL, "904", ":SASL authentication failed"),
/* 905 */
  N(ERR_SASLTOOLONG, "905", ":SASL message too long"),
/* 906 */
  N(ERR_SASLABORTED, "906", ":SASL authentication aborted"),
/* 907 */
  N(ERR_SASLALREADY, "907", ":You have already authenticated using SASL"),
/* 908 */
  N(RPL_SASLMECHS, "908", "%s :are the available SASL mechanisms"),
/* 909 */
  { 0 },
/* 910 */
  { 0 },
/* 911 */
  { 0 },
/* 912 */
  { 0 },
/* 913 */
  { 0 },
/* 914 */
  { 0 },
/* 915 */
  { 0 },
/* 916 */
  { 0 },
/* 917 */
  { 0 },
/* 918 */
  { 0 },
/* 919 */
  { 0 },
/* 920 */
  { 0 },
/* 921 */
  { 0 },
/* 922 */
  { 0 },
/* 923 */
  { 0 },
/* 924 */
  { 0 },
/* 925 */
  { 0 },
/* 926 */
  { 0 },
/* 927 */
  { 0 },
/* 928 */
  { 0 },
/* 929 */
  { 0 },
/* 930 */
  { 0 },
/* 931 */
  { 0 },
/* 932 */
  { 0 },
/* 933 */
  { 0 },
/* 934 */
  { 0 },
/* 935 */
  { 0 },
/* 936 */
  { 0 },
/* 937 */
  { 0 },
/* 938 */
  { 0 },
/* 939 */
  { 0 },
/* 940 */
  { 0 },
/* 941 */
  { 0 },
/* 942 */
  { 0 },
/* 943 */
  { 0 },
/* 944 */
  { 0 },
/* 945 */
  { 0 },
/* 946 */
  { 0 },
/* 947 */
  { 0 },
/* 948 */
  { 0 },
/* 949 */
  { 0 },
/* 950 */
  { 0 },
/* 951 */
  { 0 },
/* 952 */
  { 0 },
/* 953 */
  { 0 },
/* 954 */
  { 0 },
/* 955 */
  { 0 },
/* 956 */
  { 0 },
/* 957 */
  { 0 },
/* 958 */
  { 0 },
/* 959 */
  { 0 },
/* 960 */
  { 0 },
/* 961 */
  { 0 },
/* 962 */
  { 0 },
/* 963 */
  { 0 },
/* 964 */
  { 0 },
/* 965 */
  { 0 },
/* 966 */
  { 0 },
/* 967 */
  { 0 },
/* 968 */
  { 0 },
/* 969 */
  { 0 },
/* 970 */
  { 0 },
/* 971 */
  { 0 },
/* 972 */
  { 0 },
/* 973 */
  { 0 },
/* 974 */
  { 0 },
/* 975 */
  { 0 },
/* 976 */
  { 0 },
/* 977 */
  { 0 },
/* 978 */
  { 0 },
/* 979 */
  { 0 },
/* 980 */
  { 0 },
/* 981 */
  N(ERR_TOOMANYLANGUAGES, "981", "%u :You specified too many languages."),
/* 982 */
  N(ERR_NOLANGUAGE, "982", "%s :Languages are not supported by this server."),
/* 983 */
  N(ERR_ACCOUNTFAIL, "983", ":Authentication failed: %s"),
/* 984 */
  N(RPL_ACCOUNTLIST, "984", "%s %s :%s"),
/* 985 */
  N(RPL_ENDOFACCOUNTLIST, "985", ":End of ACCOUNT LIST"),
/* 986 */
  N(ERR_NOTAUTHENTICATED, "986", ":You have not authenticated to an account"),
/* 987 */
  N(ERR_FROZEN, "987", "%s :You must identify to this nickname before you can use that"),
/* 988 */
  { 0 },
/* 989 */
  { 0 },
/* 990 */
  { 0 },
/* 991 */
  { 0 },
/* 992 */
  { 0 },
/* 993 */
  { 0 },
/* 994 */
  { 0 },
/* 995 */
  { 0 },
/* 996 */
  { 0 },
/* 997 */
  { 0 },
/* 998 */
  { 0 },
/* 999 */
  { 0 }
};

/** Return a pointer to the Numeric for a particular code.
 * @param n %Numeric to look up.
 * @return Numeric structure.
 */
const struct Numeric* get_error_numeric(int n)
{
  assert(0 < n);
  assert(n < ERR_LASTERROR);
  assert(0 != replyTable[n].value);

  return &replyTable[n];
}

/** Return a format string for a numeric response.
 * @param n %Numeric to look up.
 * @return Pointer to a static buffer containing the format string.
 */
char* rpl_str(int n)
{
  static char numbuff[512];
  Numeric* p;

  assert(0 < n);
  assert(n < ERR_LASTERROR);
  assert(0 != replyTable[n].value);

  p = &replyTable[n];
  strcpy(numbuff, ":%s 000 %s ");
  if (p->str) {
    numbuff[4] = p->str[0];
    numbuff[5] = p->str[1];
    numbuff[6] = p->str[2];
    strcpy(numbuff + 11, p->format);
  }

  return numbuff;
}
