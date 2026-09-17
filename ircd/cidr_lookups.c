/*
 * fast_cidr_lookups - cidr_lookups.c
 * Copyright (C) 2025 Hidden <hidden@undernet.org> and
 *                    Entrope <entrope@undernet.org>
 *
 * Source code available at https://github.com/hiddn/fast_cidr_lookups
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

#include <limits.h> /* for CHAR_BIT */
#include <stdio.h> /* for printf, snprintf */
#include <stdlib.h> /* for malloc, free */
#include <assert.h> /* for assert */
#include <stdarg.h> /* for va_list, va_start, va_end */
#include <netinet/in.h> /* for ntohs */

#include "cidr_lookups.h"

#if !defined(IRCU2_BUILD)
/* Do not include irc_stuff if this file is part of Undernet's ircu2 */
#include "../include/irc_stuff.h" /* for irc_in_addr, ircd_ntocidrmask, ipmask_parse, irc_in_addr_is_ipv4, irc_in_addr_cmp */
#define cidr_malloc(size) malloc(size)
#define cidr_free(ptr) free(ptr)
#else
#include "ircd_defs.h" /* CIDR_LEN */
#include "res.h" /* struct irc_in_addr */
#include "ircd_string.h"
#include "ircd_alloc.h" /* MyMalloc, MyFree */
/* MyMalloc never returns NULL: allocation failure invokes the
 * out-of-memory handler. Allocations also take part in DEBUGMODE
 * memory accounting. */
#define cidr_malloc(size) MyMalloc(size)
#define cidr_free(ptr) MyFree(ptr)
#endif /* IRCU2_BUILD */

#define MAX_DEBUG_PAYLOAD 2048

/* local functions */
#if defined(CIDR_DEBUG_ENABLED)
static void DEBUG(char const *format, ...) __attribute__((format(printf, 1, 2)));
#else
# define DEBUG(...)
#endif
static cidr_node* _cidr_create_node(const struct irc_in_addr *ip, const unsigned char bits, void *data);

/** _cidr_bit_diff - find first mismatching bit
 * @param[in] node CIDR tree node to compare against
 * @param[in] ip Address to compare against the node
 * @param[in] lo First bit to compare (some bits 0..lo-1 might also be compared)
 * @param[in] hi One past the last bit to compare
 * @return Index of the first mismatch bit, \a hi if all bits match.
 */
unsigned char _cidr_bit_diff(const cidr_node *node, const struct irc_in_addr *ip, unsigned char lo, unsigned char hi)
{
    unsigned char lo_n = lo / 16;
    unsigned char hi_n = hi / 16;
    int ii;

    for (ii = lo_n; ii < hi_n; ++ii) {
        if (node->ip.in6_16[ii] != ip->in6_16[ii]) {
            int word = ntohs(node->ip.in6_16[ii] ^ ip->in6_16[ii]);
            return ii * 16 + __builtin_clz(word) + 16 - sizeof(int)*CHAR_BIT;
        }
    }

    if ((hi & 15) && (node->ip.in6_16[ii] != ip->in6_16[ii])) {
        int word = ntohs(node->ip.in6_16[ii] ^ ip->in6_16[ii]);
        word &= 0xffff0000 >> (hi & 15);
        if (word)
            return ii * 16 + __builtin_clz(word) + 16 - sizeof(int) * CHAR_BIT;
    }

    return hi;
}

/** cidr_new_tree - create a new CIDR tree
 * @return Pointer to the created CIDR tree root node
 */
cidr_root_node *cidr_new_tree()
{
    struct irc_in_addr ip;
    unsigned char bits;
    int parsed;
    cidr_root_node *root = cidr_malloc(sizeof(cidr_root_node));
    assert(root != 0);
    parsed = ipmask_parse("0.0.0.0/0", &ip, &bits);
    assert(parsed != 0);
    root->ipv4 = _cidr_create_node(&ip, bits, 0);
    parsed = ipmask_parse("0::/0", &ip, &bits);
    assert(parsed != 0);
    root->ipv6 = _cidr_create_node(&ip, bits, 0);
    (void)parsed;
    return root;
}

/** _cidr_free_subtree - free a node and everything under it
 * The data a node points at belongs to whoever put it there and is not
 * touched.  Depth is bounded by the address length (129 levels), so the
 * recursion cannot run away.
 * @param[in] node Node to free, or NULL.
 */
static void _cidr_free_subtree(cidr_node *node)
{
    if (!node)
        return;
    _cidr_free_subtree(node->l);
    _cidr_free_subtree(node->r);
    cidr_free(node);
}

/** cidr_free_tree - free a tree made by cidr_new_tree()
 *
 * The counterpart the API was missing.  The server makes one tree and
 * keeps it for as long as it runs, so nothing there needed this -- but a
 * test that cannot free what it made leaks by construction, and a leak
 * checker cannot tell that kind from the kind that matters.
 *
 * What each node points at is the caller's: a tree does not know what its
 * data is, so it does not get to free it.
 *
 * @param[in] root_tree Tree to free, or NULL.
 */
void cidr_free_tree(cidr_root_node *root_tree)
{
    if (!root_tree)
        return;
    _cidr_free_subtree(root_tree->ipv4);
    _cidr_free_subtree(root_tree->ipv6);
    cidr_free(root_tree);
}

 /* cidr_add_node - add a new node to the CIDR tree */
cidr_node *cidr_add_node(const cidr_root_node *root_tree, const struct irc_in_addr *ip, unsigned char bits, void *data)
{
    unsigned short i = 0;
    cidr_node *n;
    cidr_node *new_node = 0;
    cidr_node *virtual_node = 0;
    cidr_node **child_pptr = 0;
    unsigned short turn_right = 0;
    DEBUG("add_node>    %s\tbits=%d\n", ircd_ntocidrmask(ip, bits), bits);
    n = irc_in_addr_is_ipv4(ip) ? root_tree->ipv4 : root_tree->ipv6;
    for (i = 0; i < 128; ) {
        i = _cidr_bit_diff(n, ip, i, (n->bits < bits) ? n->bits : bits);
        turn_right = (i < 128) ? _cidr_get_bit(ip, i) : 0;
        DEBUG("\tdiff at %u of %u (%c)\n", i, n->bits, turn_right ? 'r' : 'l');
        assert((i <= n->bits) && (i <= bits));
        /* If pos == n->bits == bits, then we are updating n.
         * If pos == n->bits < bits, then we are setting a child of n.
         * If pos < n->bits, n needs a new parent:
         *  - If pos == bits, then the new node is n's new parent.
         *  - Otherwise (pos < bits), and we need a new virtual parent at pos.
         */
        if (i == n->bits) {
            if (i == bits) {
                /* Update n itself. */
                n->data = data;
                return n;
            }
            /* Walk to one of n's children, if it exists. */
            child_pptr = turn_right ? &n->r : &n->l;
            if (!*child_pptr) {
                new_node = _cidr_create_node(ip, bits, data);
                new_node->parent = n;
                *child_pptr = new_node;
                return new_node;
            }
            n = *child_pptr;
        } else { /* i < n->bits */
            new_node = _cidr_create_node(ip, bits, data);
            if (i == bits) {
                /* Insert this node as n's parent. */
                new_node->parent = n->parent;
                *child_pptr = new_node;
                n->parent = new_node;
                child_pptr = _cidr_get_bit(&n->ip, i)
                    ? &new_node->r : &new_node->l;
                *child_pptr = n;
            } else {
                /* Insert virtual node above both new node and n. */
                virtual_node = _cidr_create_node(ip, i, NULL);
                virtual_node->parent = n->parent;
                *child_pptr = virtual_node;
                n->parent = virtual_node;
                new_node->parent = virtual_node;
                virtual_node->l = turn_right ? n : new_node;
                virtual_node->r = turn_right ? new_node : n;
            }
            return new_node;
        }
    }
    assert(0 && "fell off the edge");
    return 0;
}

/** _cidr_find_node - find a non-virtual node in the CIDR tree that covers the given CIDR string
 * @param[in] root_tree Pointer to the root of the CIDR tree
 * @param[in] cidr_string_format CIDR string format
 * @param[in] is_exact_match If 1, look for an exact cidr_string match; if 2,
 * look for an exact match and return it even when it holds no data.
 * Otherwise, get the closest matching node that covers the given CIDR string
 * @return Pointer to the found CIDR node, returns NULL if not found
 */
cidr_node *_cidr_find_node(const cidr_root_node *root_tree, const struct irc_in_addr *ip, unsigned char bits, const unsigned short is_exact_match)
{
    unsigned short i = 0;
    cidr_node *n;
    cidr_node *child_ptr;
    unsigned short turn_right = 0;
    n = irc_in_addr_is_ipv4(ip) ? root_tree->ipv4 : root_tree->ipv6;
    for (i = n->bits; i <= 128; ) {
        i = _cidr_bit_diff(n, ip, i, (n->bits < bits) ? n->bits : bits);
        turn_right = (i < 128) ? _cidr_get_bit(ip, i) : 0;
        DEBUG("\tdiff at %u of %u (%c)\n", i, n->bits, turn_right ? 'r' : 'l');
        assert((i <= n->bits) && (i <= bits));
        if (i == n->bits) {
            if (i == bits) {
                /* Exact match found. Does it have data? */
                if (n->data || is_exact_match == 2)
                    return n;
                if (is_exact_match)
                    return 0;
                return cidr_get_closest_data_parent(n);
            }
            /* Walk to one of n's children, if it exists. */
            child_ptr = turn_right ? n->r : n->l;
            if (!child_ptr) {
                /* No exact match found */
                if (is_exact_match)
                    return 0;
                if (!n->data)
                    return cidr_get_closest_data_parent(n);
                return n;
            }
            n = child_ptr;
        } else { /* i < n->bits */
            if (!is_exact_match)
                return cidr_get_closest_data_parent(n);
            return 0;
        }
    }
    assert(0 && "fell off the edge");
    return 0;
}

/* cidr_get_data - get data associated with a node in the CIDR tree */
void *cidr_get_data(const cidr_root_node *root_tree, const struct irc_in_addr *ip, unsigned char nbits)
{
    cidr_node *node = _cidr_find_exact_node(root_tree, ip, nbits);
    return node ? node->data : 0;
}

/* cidr_rem_node_by_cidr - remove a node from the CIDR tree by CIDR string */
int cidr_rem_node_by_cidr(const cidr_root_node *root_tree, const struct irc_in_addr *ip, unsigned char nbits)
{
    return cidr_rem_node(_cidr_find_exact_node(root_tree, ip, nbits));
}

/** _cidr_collapse_node - unlink and free a data-less node if possible
 * The node is kept when it is a family root or still needed as a virtual
 * node (two children). When removing a childless node leaves its virtual
 * parent with a single child, the parent is spliced out and freed too.
 * @param[in] node Pointer to the node to collapse; node->data must be NULL
 * @return 1 if the node was freed, 0 if it was kept
 */
static int _cidr_collapse_node(cidr_node *node)
{
    cidr_node *parent_node;

    assert(node != 0);
    assert(node->data == 0);
    if (!node->parent) {
        /* It is a family root node. Keep it. */
        return 0;
    }
    if (node->l && node->r) {
        /* Node has two children. Keep it as a virtual node. */
        return 0;
    }
    DEBUG("remove_node> %s\n", ircd_ntocidrmask(&node->ip, node->bits));
    if (node->l || node->r) {
        /* Node has one child. Splice the child into node's place. */
        cidr_node *child_node = node->l ? node->l : node->r;
        child_node->parent = node->parent;
        if (node->parent->l == node)
            node->parent->l = child_node;
        else
            node->parent->r = child_node;
        cidr_free(node);
        return 1;
    }
    /* Node has no children. Unlink it from its parent. */
    if (node->parent->l == node)
        node->parent->l = 0;
    else
        node->parent->r = 0;
    /* If the parent is a non-root virtual node, it just went from two
     * children to one; splice it out as well. */
    parent_node = node->parent;
    if (!parent_node->data && parent_node->parent) {
        cidr_node *grandparent_node = parent_node->parent;
        cidr_node *sibling_node = parent_node->l ? parent_node->l : parent_node->r;
        assert(sibling_node != 0);
        if (grandparent_node->l == parent_node)
            grandparent_node->l = sibling_node;
        else
            grandparent_node->r = sibling_node;
        sibling_node->parent = grandparent_node;
        DEBUG("remove_node> %s\n", ircd_ntocidrmask(&parent_node->ip, parent_node->bits));
        cidr_free(parent_node);
    }
    cidr_free(node);
    return 1;
}

/** cidr_rem_node - remove a node from the CIDR tree
 * @param[in] node Pointer to the node to be removed
 * @return 1 if the node was removed, 0 otherwise
 */
int cidr_rem_node(cidr_node *node)
{
    if (!node) {
        return 0;
    }
    if (!node->data) {
        /* Do not remove virtual nodes. */
        return 0;
    }
    node->data = 0;
    _cidr_collapse_node(node);
    return 1;
}

/** cidr_rem_empty_node - remove a node whose data has been cleared
 * Use this after unlinking the last entry from a node's data pointer;
 * virtual (structural) nodes and family roots are left in place.
 * @param[in] node Pointer to the node to be removed
 * @return 1 if the node was freed, 0 otherwise
 */
int cidr_rem_empty_node(cidr_node *node)
{
    if (!node || node->data)
        return 0;
    return _cidr_collapse_node(node);
}

/** get_cidr_mask - get the CIDR mask of a node
 *  Be careful: it returns a pointer to a static buffer that gets overwritten on each call
 * @param[in] node Pointer to the node
 * @return The CIDR mask of the node
 */
const char *get_cidr_mask(const cidr_node *node)
{
    return ircd_ntocidrmask(&node->ip, node->bits);
}

/** set_cidr_mask - copies the node's cidr mask to buffer buf
 * @param[in] node Pointer to the node
 * @param[out] buf Buffer to store the CIDR mask
 */
void set_cidr_mask(cidr_node *node, char *buf)
{
    assert(node != 0);
    const char *cidr = ircd_ntocidrmask(&node->ip, node->bits);
    strncpy(buf, cidr, CIDR_LEN);
    buf[CIDR_LEN] = 0;
}

/** _cidr_get_bit - get a specific bit from an IP address
 * @param[in] ip Pointer to the IP address
 * @param[in] bit_index Bit index - must be between 0 and 127
 * @return The specific bit from the IP address
 */
unsigned short _cidr_get_bit(const struct irc_in_addr *ip, const unsigned int bit_index)
{
    assert(bit_index < 128);
    unsigned int quot = (127 - bit_index) / 16;
	unsigned int rem = (127 - bit_index) % 16;
    unsigned short t = -1;
    unsigned short ip16 = ntohs(ip->in6_16[7-quot]);
    ip16 &= (1 << (rem)) & t;
    return ip16;
}

#if defined(CIDR_DEBUG_ENABLED)
/** DEBUG - debug message handler
 * @param[in] format Format string
 * @param[in] ... Variable arguments
 */
static void DEBUG (char const *format, ...)
{
	va_list vl;
	int nchars;
	char outbuf[MAX_DEBUG_PAYLOAD+3];

	va_start(vl, format);
	nchars = vsnprintf(outbuf, MAX_DEBUG_PAYLOAD+1, format, vl);
	va_end(vl);
	if (nchars >= MAX_DEBUG_PAYLOAD) {
		DEBUG("Output truncated: ");
		nchars = MAX_DEBUG_PAYLOAD;
	}
    printf("%s", outbuf);
	return;
}
#endif

/** _cidr_create_node - create a new CIDR node
 * @param[in] ip IP address
 * @param[in] bits Number of bits in the CIDR mask
 * @param[in] data Pointer to the data associated with the node
 * @return Pointer to the created CIDR node
 */
static cidr_node *_cidr_create_node(const struct irc_in_addr *ip, const unsigned char bits, void *data)
{
    cidr_node *node;
    assert(ip != 0);
    node = cidr_malloc(sizeof(cidr_node));
    assert(node != 0);
    memset(node, 0, sizeof(cidr_node));
    memcpy(&node->ip, ip, sizeof(node->ip));
    node->bits = bits;
    node->data = data;
    return node;
}

/** cidr_get_closest_data_parent - find the nearest ancestor that holds data
 * @param[in] node Pointer to the node whose ancestors are searched
 * @return Pointer to the closest non-virtual ancestor, or NULL
 */
cidr_node *cidr_get_closest_data_parent(const cidr_node *node)
{
    for (cidr_node *tmp_node = node->parent; tmp_node; tmp_node = tmp_node->parent) {
        if (!tmp_node->data) {
            continue;
        }
        return tmp_node;
    }
    return 0;
}

/* This one is from Undernet's gnuworld, with a couple of modifications */
void irc_in6_CIDRMinIP(struct irc_in_addr *ircip, unsigned int CClonesCIDR)
{
  if (CClonesCIDR == 128) {
    return;
  }
  unsigned int quot = (127 - CClonesCIDR) / 16;
	unsigned int rem = (127 - CClonesCIDR) % 16;
	unsigned int i;

  for (i = 0; i < quot; i++)
		ircip->in6_16[7-i] = 0;
	unsigned short ip16 = ntohs(ircip->in6_16[7-i]);
	ip16 >>= rem+1;
	ip16 <<= rem+1;
	ircip->in6_16[7-i] = htons(ip16);
}
