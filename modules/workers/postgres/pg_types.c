/*
 * IRC - Internet Relay Chat, modules/workers/postgres/pg_types.c
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
 * @brief The abstract #DbType on one side, a PostgreSQL OID on the other.
 *
 * The OIDs are written out rather than taken from @c server/catalog/pg_type_d.h,
 * which is part of the server headers and not of libpq: a client that needs
 * @c INT4OID normally spells it 23, and these numbers have been fixed since
 * before ircu had a configure script.
 */
#include "postgres.h"

/** OID of each #DbType, indexed by the enum. */
static const Oid pg_type_oids[DB_TYPE_LAST] = {
  0,      /* DB_TYPE_UNKNOWN -- the server infers it */
  0,      /* DB_TYPE_NULL    -- no value, so no type to declare */
  16,     /* DB_TYPE_BOOL        bool */
  21,     /* DB_TYPE_SMALLINT    int2 */
  23,     /* DB_TYPE_INT         int4 */
  20,     /* DB_TYPE_BIGINT      int8 */
  701,    /* DB_TYPE_FLOAT       float8 */
  1700,   /* DB_TYPE_NUMERIC     numeric */
  25,     /* DB_TYPE_TEXT        text */
  17,     /* DB_TYPE_BYTEA       bytea */
  114,    /* DB_TYPE_JSON        json */
  2950,   /* DB_TYPE_UUID        uuid */
  1082,   /* DB_TYPE_DATE        date */
  1083,   /* DB_TYPE_TIME        time */
  1114,   /* DB_TYPE_TIMESTAMP   timestamp */
  1184,   /* DB_TYPE_TIMESTAMPTZ timestamptz */
  869     /* DB_TYPE_INET        inet */
};

Oid pg_type_oid(enum DbType type)
{
  unsigned int index = (unsigned int) type;

  if (index >= DB_TYPE_LAST)
    return 0;

  return pg_type_oids[index];
}

int pg_type_binary_length(enum DbType type)
{
  /* Only the fixed-width types can be sent in binary, because #DbParam
   * carries a pointer and no length; see the note on #DbFormat.  Everything
   * else is refused rather than guessed at with strlen(), which would
   * truncate any value with a zero byte in it.
   */
  switch (type) {
  case DB_TYPE_BOOL:
    return 1;
  case DB_TYPE_SMALLINT:
    return 2;
  case DB_TYPE_INT:
  case DB_TYPE_DATE:
    return 4;
  case DB_TYPE_BIGINT:
  case DB_TYPE_FLOAT:
  case DB_TYPE_TIME:
  case DB_TYPE_TIMESTAMP:
  case DB_TYPE_TIMESTAMPTZ:
    return 8;
  case DB_TYPE_UUID:
    return 16;
  default:
    return -1;
  }
}
