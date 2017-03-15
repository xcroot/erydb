/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "ft/msg_buffer.h"
#include "util/dbt.h"

void message_buffer::create() {
    _num_entries = 0;
    _memory = nullptr;
    _memory_usable = 0;
    _memory_size = 0;
    _memory_used = 0;
}

void message_buffer::clone(message_buffer *src) {
    _num_entries = src->_num_entries;
    _memory_used = src->_memory_used;
    _memory_size = src->_memory_size;
    XMALLOC_N(_memory_size, _memory);
    memcpy(_memory, src->_memory, _memory_size);
    _memory_usable = toku_malloc_usable_size(_memory);
}

void message_buffer::destroy() {
    if (_memory != nullptr) {
        toku_free(_memory);
        _memory_usable = 0;
    }
}

void message_buffer::deserialize_from_rbuf(struct rbuf *rb,
                                           int32_t **fresh_offsets, int32_t *nfresh,
                                           int32_t **stale_offsets, int32_t *nstale,
                                           int32_t **broadcast_offsets, int32_t *nbroadcast) {
    // read the number of messages in this buffer
    int n_in_this_buffer = rbuf_int(rb);
    if (fresh_offsets != nullptr) {
        XMALLOC_N(n_in_this_buffer, *fresh_offsets);
    }
    if (stale_offsets != nullptr) {
        XMALLOC_N(n_in_this_buffer, *stale_offsets);
    }
    if (broadcast_offsets != nullptr) {
        XMALLOC_N(n_in_this_buffer, *broadcast_offsets);
    }

    _resize(rb->size + 64); // rb->size is a good hint for how big the buffer will be

    // deserialize each message individually, noting whether it was fresh
    // and putting its buffer offset in the appropriate offsets array
    for (int i = 0; i < n_in_this_buffer; i++) {
        XIDS xids;
        bool is_fresh;
        const ft_msg msg = ft_msg::deserialize_from_rbuf(rb, &xids, &is_fresh);

        int32_t *dest;
        if (ft_msg_type_applies_once(msg.type())) {
            if (is_fresh) {
                dest = fresh_offsets ? *fresh_offsets + (*nfresh)++ : nullptr;
            } else {
                dest = stale_offsets ? *stale_offsets + (*nstale)++ : nullptr;
            }
        } else {
            invariant(ft_msg_type_applies_all(msg.type()) || ft_msg_type_does_nothing(msg.type()));
            dest = broadcast_offsets ? *broadcast_offsets + (*nbroadcast)++ : nullptr;
        }

        enqueue(msg, is_fresh, dest);
        toku_xids_destroy(&xids);
    }

    invariant(_num_entries == n_in_this_buffer);
}

MSN message_buffer::deserialize_from_rbuf_v13(struct rbuf *rb,
                                              MSN *highest_unused_msn_for_upgrade,
                                              int32_t **fresh_offsets, int32_t *nfresh,
                                              int32_t **broadcast_offsets, int32_t *nbroadcast) {
    // read the number of messages in this buffer
    int n_in_this_buffer = rbuf_int(rb);
    if (fresh_offsets != nullptr) {
        XMALLOC_N(n_in_this_buffer, *fresh_offsets);
    }
    if (broadcast_offsets != nullptr) {
        XMALLOC_N(n_in_this_buffer, *broadcast_offsets);
    }

    // Atomically decrement the header's MSN count by the number
    // of messages in the buffer.
    MSN highest_msn_in_this_buffer = {
        .msn = toku_sync_sub_and_fetch(&highest_unused_msn_for_upgrade->msn, n_in_this_buffer)
    };

    // Create the message buffers from the deserialized buffer.
    for (int i = 0; i < n_in_this_buffer; i++) {
        XIDS xids;
        // There were no stale messages at this version, so call it fresh.
        const bool is_fresh = true;

        // Increment our MSN, the last message should have the
        // newest/highest MSN.  See above for a full explanation.
        highest_msn_in_this_buffer.msn++;
        const ft_msg msg = ft_msg::deserialize_from_rbuf_v13(rb, highest_msn_in_this_buffer, &xids);

        int32_t *dest;
        if (ft_msg_type_applies_once(msg.type())) {
            dest = fresh_offsets ? *fresh_offsets + (*nfresh)++ : nullptr;
        } else {
            invariant(ft_msg_type_applies_all(msg.type()) || ft_msg_type_does_nothing(msg.type()));
            dest = broadcast_offsets ? *broadcast_offsets + (*nbroadcast)++ : nullptr;
        }

        enqueue(msg, is_fresh, dest);
        toku_xids_destroy(&xids);
    }

    return highest_msn_in_this_buffer;
}

void message_buffer::_resize(size_t new_size) {
    XREALLOC_N(new_size, _memory);
    _memory_size = new_size;
    _memory_usable = toku_malloc_usable_size(_memory);
}

static int next_power_of_two (int n) {
    int r = 4096;
    while (r < n) {
        r*=2;
        assert(r>0);
    }
    return r;
}

struct message_buffer::buffer_entry *message_buffer::get_buffer_entry(int32_t offset) const {
    return (struct buffer_entry *) (_memory + offset);
}

void message_buffer::enqueue(const ft_msg &msg, bool is_fresh, int32_t *offset) {
    int need_space_here = msg_memsize_in_buffer(msg);
    int need_space_total = _memory_used + need_space_here;
    if (_memory == nullptr || need_space_total > _memory_size) {
        // resize the buffer to the next power of 2 greater than the needed space
        int next_2 = next_power_of_two(need_space_total);
        _resize(next_2);
    }
    uint32_t keylen = msg.kdbt()->size;
    uint32_t datalen = msg.vdbt()->size;
    struct buffer_entry *entry = get_buffer_entry(_memory_used);
    entry->type = (unsigned char) msg.type();
    entry->msn = msg.msn();
    toku_xids_cpy(&entry->xids_s, msg.xids());
    entry->is_fresh = is_fresh;
    unsigned char *e_key = toku_xids_get_end_of_array(&entry->xids_s);
    entry->keylen = keylen;
    memcpy(e_key, msg.kdbt()->data, keylen);
    entry->vallen = datalen;
    memcpy(e_key + keylen, msg.vdbt()->data, datalen);
    if (offset) {
        *offset = _memory_used;
    }
    _num_entries++;
    _memory_used += need_space_here;
}

void message_buffer::set_freshness(int32_t offset, bool is_fresh) {
    struct buffer_entry *entry = get_buffer_entry(offset);
    entry->is_fresh = is_fresh;
}

bool message_buffer::get_freshness(int32_t offset) const {
    struct buffer_entry *entry = get_buffer_entry(offset);
    return entry->is_fresh;
}

ft_msg message_buffer::get_message(int32_t offset, DBT *keydbt, DBT *valdbt) const {
    struct buffer_entry *entry = get_buffer_entry(offset);
    uint32_t keylen = entry->keylen;
    uint32_t vallen = entry->vallen;
    enum ft_msg_type type = (enum ft_msg_type) entry->type;
    MSN msn = entry->msn;
    const XIDS xids = (XIDS) &entry->xids_s;
    const void *key = toku_xids_get_end_of_array(xids);
    const void *val = (uint8_t *) key + entry->keylen;
    return ft_msg(toku_fill_dbt(keydbt, key, keylen), toku_fill_dbt(valdbt, val, vallen), type, msn, xids);
}

void message_buffer::get_message_key_msn(int32_t offset, DBT *key, MSN *msn) const {
    struct buffer_entry *entry = get_buffer_entry(offset);
    if (key != nullptr) {
        toku_fill_dbt(key, toku_xids_get_end_of_array((XIDS) &entry->xids_s), entry->keylen);
    }
    if (msn != nullptr) {
        *msn = entry->msn;
    }
}

int message_buffer::num_entries() const {
    return _num_entries;
}

size_t message_buffer::buffer_size_in_use() const {
    return _memory_used;
}

size_t message_buffer::memory_size_in_use() const {
    return sizeof(*this) + _memory_used;
}

size_t message_buffer::memory_footprint() const {
#ifdef TOKU_DEBUG_PARANOID
    // Enable this code if you want to verify that the new way of computing
    // the memory footprint is the same as the old.
    // It slows the code down by perhaps 10%.
    assert(_memory_usable == toku_malloc_usable_size(_memory));
    size_t fp = toku_memory_footprint(_memory, _memory_used);
    size_t fpg = toku_memory_footprint_given_usable_size(_memory_used, _memory_usable);
    if (fp != fpg) printf("ptr=%p mu=%ld fp=%ld fpg=%ld\n", _memory, _memory_usable, fp, fpg);
    assert(fp  == fpg);
#endif // TOKU_DEBUG_PARANOID
    return sizeof(*this) + toku_memory_footprint_given_usable_size(_memory_used, _memory_usable);
}

bool message_buffer::equals(message_buffer *other) const {
    return (_memory_used == other->_memory_used &&
            memcmp(_memory, other->_memory, _memory_used) == 0);
}

void message_buffer::serialize_to_wbuf(struct wbuf *wb) const {
    wbuf_nocrc_int(wb, _num_entries);
    struct msg_serialize_fn {
        struct wbuf *wb;
        msg_serialize_fn(struct wbuf *w) : wb(w) { }
        int operator()(const ft_msg &msg, bool is_fresh) {
            msg.serialize_to_wbuf(wb, is_fresh);
            return 0;
        }
    } serialize_fn(wb);
    iterate(serialize_fn);
}
//void static stats(struct wbuf *wb) const {
//    wbuf_nocrc_int(wb, _num_entries);
//    struct msg_serialize_fn {
//        struct wbuf *wb;
//        msg_serialize_fn(struct wbuf *w) : wb(w) { }
//        int operator()(const ft_msg &msg, bool is_fresh) {
//            msg.serialize_to_wbuf(wb, is_fresh);
//            return 0;
//        }
//    } serialize_fn(wb);
//    iterate(serialize_fn);
//}
size_t message_buffer::msg_memsize_in_buffer(const ft_msg &msg) {
    const uint32_t keylen = msg.kdbt()->size;
    const uint32_t datalen = msg.vdbt()->size;
    const size_t xidslen = toku_xids_get_size(msg.xids());
    return sizeof(struct buffer_entry) + keylen + datalen + xidslen - sizeof(XIDS_S);
}
