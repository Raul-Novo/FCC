// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t FccTypeId;

typedef struct FccTypeRecordField {
  const char* name;
  FccTypeId type_id;
  size_t offset;
} FccTypeRecordField;

typedef struct FccTypeRecordFieldInput {
  const char* name;
  FccTypeId type_id;
} FccTypeRecordFieldInput;

enum {
  FCC_TYPE_ID_INVALID = 0,
};

typedef enum FccTypeKind {
  FCC_TYPE_INVALID = 0,
  FCC_TYPE_VOID = 1,
  FCC_TYPE_BOOL = 2,
  FCC_TYPE_CHAR = 3,
  FCC_TYPE_SIGNED_CHAR = 4,
  FCC_TYPE_UNSIGNED_CHAR = 5,
  FCC_TYPE_SHORT = 6,
  FCC_TYPE_UNSIGNED_SHORT = 7,
  FCC_TYPE_INT = 8,
  FCC_TYPE_UNSIGNED_INT = 9,
  FCC_TYPE_LONG = 10,
  FCC_TYPE_UNSIGNED_LONG = 11,
  FCC_TYPE_LONG_LONG = 12,
  FCC_TYPE_UNSIGNED_LONG_LONG = 13,
  FCC_TYPE_FLOAT = 14,
  FCC_TYPE_DOUBLE = 15,
  FCC_TYPE_LONG_DOUBLE = 16,
  FCC_TYPE_POINTER = 17,
  FCC_TYPE_ARRAY = 18,
  FCC_TYPE_STRUCT = 19,
  FCC_TYPE_UNION = 20,
  FCC_TYPE_ENUM = 21,
  FCC_TYPE_TYPEDEF = 22,
  FCC_TYPE_FUNCTION = 23
} FccTypeKind;

enum {
  FCC_TYPE_QUALIFIER_NONE = 0,
  FCC_TYPE_QUALIFIER_CONST = 1 << 0,
};

typedef struct FccType {
  FccTypeKind kind;
  uint32_t qualifiers;

  union {
    struct {
      FccTypeId pointee_type_id;
    } pointer;

    struct {
      FccTypeId element_type_id;
      size_t element_count;
      bool is_vla;
    } array;

    struct {
      const char* tag_name;
      size_t first_field_index;
      size_t field_count;
      size_t size;
      size_t alignment;
      bool is_complete;
    } record;

    struct {
      const char* tag_name;
    } tagged;

    struct {
      const char* alias_name;
      FccTypeId aliased_type_id;
    } typedef_type;

    struct {
      FccTypeId return_type_id;
      size_t first_parameter_index;
      size_t parameter_count;
      bool is_variadic;
    } function;
  } data;
} FccType;

/*
 * Type IDs are stable 1-based indexes into FccTypeContext.types. Compound type
 * payloads store indexes into side arrays so records/functions can be interned
 * without embedding variable-length data directly in FccType.
 */
typedef struct FccTypeContext {
  FccType* types;
  size_t type_count;
  size_t type_capacity;
  FccTypeId* function_parameter_type_ids;
  size_t function_parameter_count;
  size_t function_parameter_capacity;
  FccTypeRecordField* record_fields;
  size_t record_field_count;
  size_t record_field_capacity;
} FccTypeContext;

void fcc_type_context_init(FccTypeContext* context);

void fcc_type_context_dispose(FccTypeContext* context);

const FccType* fcc_type_context_get(const FccTypeContext* context, FccTypeId type_id);

FccTypeId fcc_type_context_get_builtin(FccTypeContext* context, FccTypeKind kind,
                                       bool is_const_qualified);

FccTypeId fcc_type_context_qualify_const(FccTypeContext* context, FccTypeId base_type_id);

FccTypeId fcc_type_context_get_pointer(FccTypeContext* context, FccTypeId pointee_type_id);

FccTypeId fcc_type_context_get_array(FccTypeContext* context, FccTypeId element_type_id,
                                     size_t element_count, bool is_vla);

FccTypeId fcc_type_context_get_function(FccTypeContext* context, FccTypeId return_type_id,
                                        const FccTypeId* parameter_type_ids, size_t parameter_count,
                                        bool is_variadic);

FccTypeId fcc_type_context_create_record(FccTypeContext* context, FccTypeKind kind,
                                         const char* tag_name);

FccTypeId fcc_type_context_create_enum(FccTypeContext* context, const char* tag_name);

bool fcc_type_context_define_record(FccTypeContext* context, FccTypeId record_type_id,
                                    const FccTypeRecordFieldInput* fields, size_t field_count);

const char* fcc_type_name(FccTypeKind kind);

bool fcc_type_equals(FccTypeId left_type_id, FccTypeId right_type_id);

FccTypeId fcc_type_resolve_typedef(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_integer(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_arithmetic(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_unsigned_integer(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_object(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_scalar(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_pointer(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_array(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_function(const FccTypeContext* context, FccTypeId type_id);

bool fcc_type_is_complete(const FccTypeContext* context, FccTypeId type_id);

size_t fcc_type_record_field_count(const FccTypeContext* context, FccTypeId type_id);

const FccTypeRecordField* fcc_type_record_field_at(const FccTypeContext* context, FccTypeId type_id,
                                                   size_t field_index);

const FccTypeRecordField* fcc_type_record_find_field(const FccTypeContext* context,
                                                     FccTypeId type_id, const char* field_name);

bool fcc_type_is_const_qualified(const FccTypeContext* context, FccTypeId type_id);

FccTypeId fcc_type_decay_array(FccTypeContext* context, FccTypeId type_id);

FccTypeId fcc_type_get_pointee_type(const FccTypeContext* context, FccTypeId type_id);

FccTypeId fcc_type_get_function_return_type(const FccTypeContext* context, FccTypeId type_id);

size_t fcc_type_get_function_parameter_count(const FccTypeContext* context, FccTypeId type_id);

FccTypeId fcc_type_get_function_parameter_type(const FccTypeContext* context, FccTypeId type_id,
                                               size_t parameter_index);

size_t fcc_type_size_of(const FccTypeContext* context, FccTypeId type_id, bool* ok_out);

size_t fcc_type_alignment_of(const FccTypeContext* context, FccTypeId type_id, bool* ok_out);
