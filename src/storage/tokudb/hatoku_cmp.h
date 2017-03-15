/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#ifndef _HATOKU_CMP
#define _HATOKU_CMP

#include "stdint.h"

#include <db.h>

//
// A MySQL row is encoded in TokuDB, as follows:
//   Keys:
//     Keys pack the defined columns in the order that they are declared.
//     The primary key contains only the columns listed
//     If no primary key is defined, then an eight byte hidden primary key is autogenerated (like an auto increment) and used
//     Secondary keys contains the defined key and the primary key.
//     Two examples:
//       1) table foo (a int, b int, c int, d int, key(b))
//           The key of the main dictionary contains an eight byte autogenerated hidden primary key
//           The key of key-b is the column 'b' followed by the hidden primary key
//       2) table foo (a int, b int, c int, d int, primary key(a), key(b))
//           The key of the main dictionary contains 'a'
//           The key of key-b is the column 'b followed by 'a'
//    Vals:
//      For secondary keys they are empty.
//      For the main dictionary and clustering keys, they contain all columns that do not show up in the dictionary's key
//      Two examples:
//        1) table foo (a int, b int, c int, d varchar(100), primary key(a), clustering key d(d), clustering key d2(d(20))
//           the val of the main dictionary contains (b,c,d)
//           the val of d contains (b,c)
//           the val of d2 contains (b,c,d). d is there because the entire row does not show up in the key
//      Vals are encoded as follows. They have four components:
//      1) Null bytes: contains a bit field that states what columns are NULL. 
//      2) Fixed fields: all fixed fields are then packed together. If a fixed field is NULL, its data is considered junk
//      3) varchars and varbinaries: stored in two pieces, first all the offsets and then all the data. If a var field is NULL, its data is considered junk
//      4) blobs: stored in (length, data) pairs. If a blob is NULL, its data is considered junk
//      An example:
//        Table: (a int, b varchar(20), c blob, d bigint, e varbinary(10), f largeblob, g varchar(10)) <-- no primary key defined
//        Row inserted: (1, "bbb", "cc", 100, "eeeee", "ffff", "g")
//        The packed format of the val looks like:
//          NULL byte <-- 1 byte to encode nothing is NULL
//          1 <-- four bytes for 'a'
//          100 <-- four bytes for 'd'
//          3,8,9 <--offsets for location of data fields, note offsets point to where data ENDS
//          "bbbeeeeeg" <-- data for variable length stuff
//          2,"cc",4,"ffff"<-- data that stores the blobs
// The structures below describe are used for the TokuDB encoding of a row
//

// used for queries
typedef struct st_col_pack_info {
    uint32_t col_pack_val; //offset if fixed, pack_index if var
} COL_PACK_INFO;

//
// used to define a couple of characteristics of a packed val for the main dictionary or a clustering dictionary
// fixed_field_size is the size of the fixed fields in the val.
// len_of_offsets is the size of the bytes that make up the offsets of variable size columns
// Some notes:
//   If the val has no fixed fields, fixed_field_size is 0
//   If the val has no variable fields, len_of_offsets is 0
//   The number of null bytes at the beginning of a row is not saved, it is derived from table_share->null_bytes
//   The pointer to where the variable data in a val starts is table_share->null_bytes + fixed_field_size + len_of_offsets
//   To figure out where the blobs start, find the last offset listed (if offsets exist)
//
typedef struct st_multi_col_pack_info {
    uint32_t fixed_field_size; //where the fixed length stuff ends and the offsets for var stuff begins
    uint32_t len_of_offsets; //length of the offset bytes in a packed row
} MULTI_COL_PACK_INFO;

typedef struct st_key_and_col_info {
    //
    // bitmaps for each key. key_filters[i] is associated with the i'th dictionary
    // States what columns are not stored in the vals of each key, because 
    // the column is stored in the key. So, for example, the table (a int, b int, c int, d int, primary key (b,d)) will
    // have bit 1 (for 'b') and bit 3 (for 'd') of the primary key's bitmap set for the main dictionary's bitmap,
    // because 'b' and 'd' do not show up in the val
    //
    MY_BITMAP key_filters[MAX_KEY+1];
    //
    // following three arrays are used to identify the types of rows in the field
    // If table->field[i] is a fixed field:
    //   field_lengths[i] stores the field length, which is fixed
    //   length_bytes[i] is 0
    //   'i' does not show up in the array blob_fields
    // If table->field[i] is a varchar or varbinary:
    //   field_lengths[i] is 0
    //   length_bytes[i] stores the number of bytes MySQL uses to encode the length of the field in table->record[0]
    //   'i' does not show up in the array blob_fields
    // If table->field[i] is a blob:
    //   field_lengths[i] is 0
    //   length_bytes[i] is 0
    //   'i' shows up in blob_fields
    //
    void *multi_ptr;
    enum { TOKUDB_FIXED_FIELD, TOKUDB_VARIABLE_FIELD, TOKUDB_BLOB_FIELD};
    uint8_t *field_types;
    uint16_t* field_lengths; //stores the field lengths of fixed size fields (1<<16 - 1 max), 
    uint8_t* length_bytes; // stores the length of lengths of varchars and varbinaries
    uint32_t* blob_fields; // list of indexes of blob fields, 
    uint32_t num_blobs; // number of blobs in the table
    //
    // val packing info for all dictionaries. i'th one represents info for i'th dictionary
    //
    MULTI_COL_PACK_INFO mcp_info[MAX_KEY+1];
    COL_PACK_INFO* cp_info[MAX_KEY+1];
    //
    // number bytes used to represent an offset in a val. Can be 1 or 2.
    // The number of var fields in a val for dictionary i can be evaluated by
    // mcp_info[i].len_of_offsets/num_offset_bytes.
    //
    uint32_t num_offset_bytes; //number of bytes needed to encode the offset
} KEY_AND_COL_INFO;

static bool is_fixed_field(KEY_AND_COL_INFO *kcinfo, uint field_num) {
    return kcinfo->field_types[field_num] == KEY_AND_COL_INFO::TOKUDB_FIXED_FIELD;
}

static bool is_variable_field(KEY_AND_COL_INFO *kcinfo, uint field_num) {
    return kcinfo->field_types[field_num] == KEY_AND_COL_INFO::TOKUDB_VARIABLE_FIELD;
}

static bool is_blob_field(KEY_AND_COL_INFO *kcinfo, uint field_num) {
    return kcinfo->field_types[field_num] == KEY_AND_COL_INFO::TOKUDB_BLOB_FIELD;
}

static bool field_valid_for_tokudb_table(Field* field);

static void get_var_field_info(
    uint32_t* field_len, 
    uint32_t* start_offset, 
    uint32_t var_field_index, 
    const uchar* var_field_offset_ptr, 
    uint32_t num_offset_bytes
    );

static void get_blob_field_info(
    uint32_t* start_offset, 
    uint32_t len_of_offsets,
    const uchar* var_field_data_ptr, 
    uint32_t num_offset_bytes
    );

static inline uint32_t get_blob_field_len(const uchar* from_tokudb, uint32_t len_bytes) {
    uint32_t length = 0;
    switch (len_bytes) {
    case (1):
        length = (uint32_t)(*from_tokudb);
        break;
    case (2):
        length = uint2korr(from_tokudb);
        break;
    case (3):
        length = tokudb_uint3korr(from_tokudb);
        break;
    case (4):
        length = uint4korr(from_tokudb);
        break;
    default:
        assert(false);
    }
    return length;
}


static inline const uchar* unpack_toku_field_blob(uchar *to_mysql, const uchar* from_tokudb, uint32_t len_bytes, bool skip) {
    uint32_t length = 0;
    const uchar* data_ptr = NULL;
    if (!skip) {
        memcpy(to_mysql, from_tokudb, len_bytes);
    }
    length = get_blob_field_len(from_tokudb,len_bytes);

    data_ptr = from_tokudb + len_bytes;
    if (!skip) {
        memcpy(to_mysql + len_bytes, (uchar *)(&data_ptr), sizeof data_ptr);
    }
    return from_tokudb + len_bytes + length;
}

static inline uint get_null_offset(TABLE* table, Field* field) {
#if (50606 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50699) || \
    (50700 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50799)
    return field->null_offset(table->record[0]);
#else
    return (uint) ((uchar*) field->null_ptr - (uchar*) table->record[0]);
#endif
}

typedef enum {
    toku_type_int = 0,
    toku_type_double,
    toku_type_float,
    toku_type_fixbinary,
    toku_type_fixstring,
    toku_type_varbinary,
    toku_type_varstring,
    toku_type_blob,
    toku_type_hpk, //for hidden primary key
    toku_type_unknown
} TOKU_TYPE;


static TOKU_TYPE mysql_to_toku_type (Field* field);

static uchar* pack_toku_varbinary_from_desc(
    uchar* to_tokudb, 
    const uchar* from_desc, 
    uint32_t key_part_length, //number of bytes to use to encode the length in to_tokudb
    uint32_t field_length //length of field
    );

static uchar* pack_toku_varstring_from_desc(
    uchar* to_tokudb, 
    const uchar* from_desc, 
    uint32_t key_part_length, //number of bytes to use to encode the length in to_tokudb
    uint32_t field_length,
    uint32_t charset_num//length of field
    );


static uchar* pack_toku_key_field(
    uchar* to_tokudb,
    uchar* from_mysql,
    Field* field,
    uint32_t key_part_length //I really hope this is temporary as I phase out the pack_cmp stuff
    );

static uchar* pack_key_toku_key_field(
    uchar* to_tokudb,
    uchar* from_mysql,
    Field* field,
    uint32_t key_part_length //I really hope this is temporary as I phase out the pack_cmp stuff
    );

static uchar* unpack_toku_key_field(
    uchar* to_mysql,
    uchar* from_tokudb,
    Field* field,
    uint32_t key_part_length
    );


//
// for storing NULL byte in keys
//
#define NULL_COL_VAL 0
#define NONNULL_COL_VAL 1

//
// for storing if rest of key is +/- infinity
//
#define COL_NEG_INF -1 
#define COL_ZERO 0 
#define COL_POS_INF 1

#define COL_FIX_FIELD 0x11
#define COL_VAR_FIELD 0x22
#define COL_BLOB_FIELD 0x33

//
// information for hidden primary keys
//
#define TOKUDB_HIDDEN_PRIMARY_KEY_LENGTH 8

//
// function to convert a hidden primary key into a byte stream that can be stored in DBT
//
static inline void hpk_num_to_char(uchar* to, ulonglong num) {
    int8store(to, num);
}

//
// function that takes a byte stream of a hidden primary key and returns a ulonglong
//
static inline ulonglong hpk_char_to_num(uchar* val) {
    return uint8korr(val);
}

static int tokudb_compare_two_keys(
    const void* new_key_data, 
    const uint32_t new_key_size, 
    const void*  saved_key_data,
    const uint32_t saved_key_size,
    const void*  row_desc,
    const uint32_t row_desc_size,
    bool cmp_prefix,
    bool* read_string
    );

static int tokudb_cmp_dbt_key(DB* db, const DBT *keya, const DBT *keyb);

//TODO: QQQ Only do one direction for prefix.
static int tokudb_prefix_cmp_dbt_key(DB *file, const DBT *keya, const DBT *keyb);

static int tokudb_compare_two_key_parts(
    const void* new_key_data, 
    const uint32_t new_key_size, 
    const void*  saved_key_data,
    const uint32_t saved_key_size,
    const void*  row_desc,
    const uint32_t row_desc_size,
    uint max_parts
    );

static int tokudb_cmp_dbt_key_parts(DB *file, const DBT *keya, const DBT *keyb, uint max_parts);

static int create_toku_key_descriptor(
    uchar* buf, 
    bool is_first_hpk, 
    KEY* first_key, 
    bool is_second_hpk, 
    KEY* second_key
    );


static uint32_t create_toku_main_key_pack_descriptor (
    uchar* buf
    );

static uint32_t get_max_clustering_val_pack_desc_size(
    TABLE_SHARE* table_share
    );

static uint32_t create_toku_clustering_val_pack_descriptor (
    uchar* buf,
    uint pk_index,
    TABLE_SHARE* table_share,
    KEY_AND_COL_INFO* kc_info,
    uint32_t keynr,
    bool is_clustering
    );

static inline bool is_key_clustering(
    void* row_desc,
    uint32_t row_desc_size
    ) 
{
    return (row_desc_size > 0);
}

static uint32_t pack_clustering_val_from_desc(
    uchar* buf,
    void* row_desc,
    uint32_t row_desc_size,
    const DBT* pk_val
    );

static uint32_t get_max_secondary_key_pack_desc_size(
    KEY_AND_COL_INFO* kc_info
    );

static uint32_t create_toku_secondary_key_pack_descriptor (
    uchar* buf,
    bool has_hpk,
    uint pk_index,
    TABLE_SHARE* table_share,
    TABLE* table,
    KEY_AND_COL_INFO* kc_info,
    KEY* key_info,
    KEY* prim_key
    );

static inline bool is_key_pk(
    void* row_desc,
    uint32_t row_desc_size
    ) 
{
    uchar* buf = (uchar *)row_desc;
    return buf[0];
}

static uint32_t max_key_size_from_desc(
    void* row_desc,
    uint32_t row_desc_size
    );


static uint32_t pack_key_from_desc(
    uchar* buf,
    void* row_desc,
    uint32_t row_desc_size,
    const DBT* pk_key,
    const DBT* pk_val
    );

static bool fields_have_same_name(
    Field* a,
    Field* b
    );

static bool fields_are_same_type(
    Field* a, 
    Field* b
    );

static bool are_two_fields_same(
    Field* a,
    Field* b
    );

#endif

