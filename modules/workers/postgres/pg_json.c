/*
 * IRC - Internet Relay Chat, modules/workers/postgres/pg_json.c
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
 * @brief Turning a PGresult into the JSON array db.h promises.
 *
 * Results are fetched in text format on purpose.  Binary format would mean
 * decoding every type by hand, getting the endianness and the epoch right
 * for each one, and being wrong in a new way for every type PostgreSQL
 * gains; text format hands over exactly what the server would have printed,
 * and this file decides what that is worth as JSON.
 *
 * The mapping is deliberately conservative.  Numbers, booleans and JSON
 * documents become their JSON equivalents, NULL becomes @c null, and
 * everything else -- timestamps, UUIDs, arrays, the @c \\x form of a bytea,
 * a type nobody here has heard of -- stays a string.  A string is never
 * wrong; guessing at a structure would be.
 *
 * Everything here runs in a connection thread, so the allocator is
 * jansson's (@c malloc, not the core's pools) and nothing touches core
 * state.
 */
#include "postgres.h"

#include <errno.h>
#include <stdlib.h>

/* OIDs; see pg_types.c for why they are written out. */
#define PG_OID_BOOL    16
#define PG_OID_INT8    20
#define PG_OID_INT2    21
#define PG_OID_INT4    23
#define PG_OID_JSON    114
#define PG_OID_FLOAT4  700
#define PG_OID_FLOAT8  701
#define PG_OID_NUMERIC 1700
#define PG_OID_JSONB   3802

/** Convert one text-format field to JSON.
 *
 * @param[in] oid Type the column was declared as.
 * @param[in] text The server's rendering of the value.
 * @return A new reference, or NULL if there was no memory.
 */
static json_t* pg_json_value(Oid oid, const char* text)
{
  json_t* value;
  char* end;

  switch (oid) {
  case PG_OID_BOOL:
    /* The server prints exactly "t" or "f". */
    return json_boolean(text[0] == 't');

  case PG_OID_INT2:
  case PG_OID_INT4:
  case PG_OID_INT8: {
    long long number;

    errno = 0;
    number = strtoll(text, &end, 10);
    if (errno || end == text || *end)
      break;                  /* not what we were promised; keep the text */

    return json_integer((json_int_t) number);
  }

  case PG_OID_FLOAT4:
  case PG_OID_FLOAT8:
  case PG_OID_NUMERIC: {
    double number;

    /* "NaN" and "Infinity" are valid here and have no JSON spelling, so
     * they fall through to the string below rather than being rounded into
     * something that is not the value.
     */
    errno = 0;
    number = strtod(text, &end);
    if (errno || end == text || *end)
      break;

    return json_real(number);
  }

  case PG_OID_JSON:
  case PG_OID_JSONB:
    /* Embed the document rather than a string containing it: a column of
     * JSON is the one case where the server has already done the work.
     */
    if ((value = json_loads(text, JSON_DECODE_ANY, 0)))
      return value;
    break;                    /* malformed: hand back the text as it came */

  default:
    break;
  }

  return json_string(text);
}

json_t* pg_json_rows(const PGresult* res)
{
  json_t* rows;
  int nrows;
  int ncols;
  int row;
  int col;

  if (!(rows = json_array()))
    return 0;

  nrows = PQntuples(res);
  ncols = PQnfields(res);

  for (row = 0; row < nrows; row++) {
    json_t* object = json_object();

    if (!object) {
      json_decref(rows);
      return 0;
    }

    for (col = 0; col < ncols; col++) {
      const char* name = PQfname(res, col);
      json_t* value;

      if (PQgetisnull(res, row, col))
        value = json_null();
      else
        value = pg_json_value(PQftype(res, col), PQgetvalue(res, row, col));

      if (!value || json_object_set_new(object, name ? name : "", value)) {
        /* json_object_set_new() steals the reference even when it fails,
         * so there is nothing left to release but the row itself.
         */
        json_decref(object);
        json_decref(rows);
        return 0;
      }
    }

    if (json_array_append_new(rows, object)) {
      json_decref(rows);
      return 0;
    }
  }

  return rows;
}
