/* capab_stub.c - stand-ins for the parts of the server the capability
 * register talks to.
 *
 * The register is deliberately ignorant of clients and of sending: the
 * only thing it reaches out to is the announcement of a capability
 * appearing (cap_new) or going away (cap_del), which m_cap.c does by
 * walking the local clients.  There are none in a unit test, so the two
 * are recorded here and the test asserts on the record.
 *
 * feature_bool() is the other edge: a core capability can be gated by a
 * feature, and cap_is_available() has to honour it.
 */

#include "capab.h"
#include "ircd_features.h"

#include <stddef.h>

/** Position of the most recent cap_new(), or CAP_NONE. */
int stub_cap_new;
/** Position of the most recent cap_del(), or CAP_NONE. */
int stub_cap_del;
/** Number of cap_new() calls since the test last reset it. */
int stub_cap_new_calls;
/** Number of cap_del() calls since the test last reset it. */
int stub_cap_del_calls;

/** Feature that feature_bool() reports as off, or FEAT_LAST_F for none. */
enum Feature stub_feature_off = FEAT_LAST_F;

void cap_new(int cap)
{
  stub_cap_new = cap;
  stub_cap_new_calls++;
}

void cap_del(int cap)
{
  stub_cap_del = cap;
  stub_cap_del_calls++;
}

int feature_bool(enum Feature feat)
{
  return feat != stub_feature_off;
}
