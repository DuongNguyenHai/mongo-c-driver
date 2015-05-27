/*
 * Copyright 2015 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FUTURE_VALUE_H
#define FUTURE_VALUE_H

#include <bson.h>
#include <mongoc.h>


typedef enum {
   future_value_no_type = 0,
   future_value_bool_type,
   future_value_bson_error_ptr_type,
   future_value_bson_ptr_type,
   future_value_const_bson_ptr_ptr_type,
   future_value_char_ptr_ptr_type,
   future_value_mongoc_bulk_operation_ptr_type,
   future_value_mongoc_cursor_ptr_type,
   future_value_mongoc_database_ptr_type,
   future_value_uint32_t_type,
} future_value_type_t;


typedef char **char_ptr_ptr;
typedef mongoc_bulk_operation_t *mongoc_bulk_operation_ptr;
typedef mongoc_cursor_t *mongoc_cursor_ptr;
typedef mongoc_database_t *mongoc_database_ptr;
typedef bson_t *bson_ptr;
typedef const bson_t **const_bson_ptr_ptr;
typedef bson_error_t *bson_error_ptr;


typedef struct _future_value_t future_value_t;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

struct _future_value_t
{
   future_value_type_t type;
   union {
      bool bool_value;
      bson_ptr bson_ptr_value;
      const_bson_ptr_ptr const_bson_ptr_ptr_value;
      bson_error_ptr bson_error_ptr_value;
      char_ptr_ptr char_ptr_ptr_value;
      mongoc_bulk_operation_ptr mongoc_bulk_operation_ptr_value;
      mongoc_cursor_ptr mongoc_cursor_ptr_value;
      mongoc_database_ptr mongoc_database_ptr_value;
      uint32_t uint32_t_value;
   };
};

#define MAKE_GETTER_AND_SETTER(TYPE) \
   static void \
   future_value_set_ ## TYPE(future_value_t *future_value, TYPE value) \
   { \
      future_value->type = future_value_ ## TYPE ## _type; \
      future_value->TYPE ## _value = value; \
   } \
   static TYPE \
   future_value_get_ ## TYPE (future_value_t *future_value) \
   { \
      assert (future_value->type == future_value_ ## TYPE ## _type); \
      return future_value->TYPE ## _value; \
   }

MAKE_GETTER_AND_SETTER(bool)
MAKE_GETTER_AND_SETTER(bson_ptr)
MAKE_GETTER_AND_SETTER(const_bson_ptr_ptr)
MAKE_GETTER_AND_SETTER(bson_error_ptr)
MAKE_GETTER_AND_SETTER(char_ptr_ptr)
MAKE_GETTER_AND_SETTER(uint32_t)
MAKE_GETTER_AND_SETTER(mongoc_bulk_operation_ptr)
MAKE_GETTER_AND_SETTER(mongoc_cursor_ptr)
MAKE_GETTER_AND_SETTER(mongoc_database_ptr)

#undef MAKE_FUTURE_GETTER

#pragma clang diagnostic pop

#endif //FUTURE_VALUE_H